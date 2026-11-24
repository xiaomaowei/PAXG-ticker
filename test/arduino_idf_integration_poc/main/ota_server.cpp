/**
 * @file ota_server.cpp
 * @brief paxg::ota HTTP OTA server implementation (ESP-IDF 5.x).
 *
 * Design notes
 * ------------
 * Upload state is stack-local to handle_update() — no cross-task abort race.
 * stop() calls httpd_stop() which closes all sockets; the HTTP task unwinds
 * normally (recv returns <=0) and cleans up its local UploadCtx via RAII.
 * tick() only checks the deferred-restart flag; it does NOT touch upload state.
 *
 * Prefix policy: 288 bytes (esp_image_header_t + first segment header +
 * esp_app_desc_t) are accumulated across fragmented TCP reads before any
 * esp_ota_begin() call.  Chip ID is read from esp_image_header_t.chip_id.
 *
 * Power-loss sequencing
 * ---------------------
 * On any error after esp_ota_begin(): esp_ota_abort() → maintenanceLeave(false).
 * Boot partition is set only after esp_ota_end() succeeds and SHA-256 matches.
 * 202 is sent before requestRestart()/esp_restart(). If send fails the restart
 * still happens (g_rebootPend already set); the HTTP connection is simply lost.
 */

#include "ota_server.h"

#if __has_include("ota_dashboard.h")
#  include "ota_dashboard.h"
#else
namespace paxg { namespace ota {
static const char kDashboardHtml[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>PAXG OTA</title></head>"
    "<body><p>Dashboard pending.</p></body></html>";
} }
#endif

#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_image_format.h>
#include <esp_netif.h>
#include <nvs.h>
#include <esp_http_server.h>
#include <lwip/sockets.h>

#include <mbedtls/sha256.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr const char* TAG           = "paxg_ota";
static constexpr const char* kNvsNs        = "paxg_ota";
static constexpr const char* kNvsTokenKey  = "token";
static constexpr size_t      kWriteBlock   = 4096;
// Prefix size: esp_image_header_t(4) + esp_image_segment_header_t(8) + esp_app_desc_t(256) = 268
// Round up to 288 to cover any padding and ensure full descriptor read.
static constexpr size_t      kPrefixBytes  = 288;
static constexpr uint32_t    kTotalDeadMs  = 120000;
static constexpr uint32_t    kIdleDeadMs   = 15000;
static constexpr uint32_t    kRestartDelMs = 2000;
static constexpr uint32_t    kMaintTmoMs   = 5000;
static constexpr size_t      kLogEntryLen  = 128;
static constexpr size_t      kJsonBuf      = 1200;

// Offset of esp_app_desc_t within firmware image:
// esp_image_header_t (4 bytes) + esp_image_segment_header_t (8 bytes) = 12 bytes
static constexpr size_t kAppDescOffset = 12;

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------
namespace {

bool              g_ready      = false;
bool              g_running    = false;
std::atomic_bool  g_rebootPend{false};
std::atomic_bool  g_deferRst{false};
std::atomic<uint32_t> g_deferRstUs{0};

char g_tokenHex[paxg::ota::kTokenHexLen + 1] = {};
char g_deviceId[32] = {};
char g_bootId[32]   = {};

paxg::ota::RuntimeHooks g_hooks = {};

httpd_handle_t g_server = nullptr;
esp_netif_ip_info_t g_staIp = {};
esp_netif_t* g_staNetif = nullptr;

// Log ring
SemaphoreHandle_t g_logMtx = nullptr;
struct LogEntry { char msg[kLogEntryLen]; int64_t tsUs; };
LogEntry  g_log[paxg::ota::kLogRingCap] = {};
uint32_t  g_logHead  = 0;
uint32_t  g_logCount = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void bytes_to_hex(const uint8_t* b, size_t n, char* out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i*2]   = h[b[i] >> 4];
        out[i*2+1] = h[b[i] & 0xf];
    }
    out[n*2] = '\0';
}

static bool ct_memeq(const char* a, const char* b, size_t n) {
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) acc |= (uint8_t)a[i] ^ (uint8_t)b[i];
    return acc == 0;
}

static bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool valid_sha256_hex(const char* s) {
    if (!s || strlen(s) != 64) return false;
    for (int i = 0; i < 64; i++) if (!is_hex_char(s[i])) return false;
    return true;
}

static bool ip_is_private(uint32_t ip) {
    uint8_t b[4]; memcpy(b, &ip, 4);
    return (b[0] == 10) ||
           (b[0] == 172 && b[1] >= 16 && b[1] <= 31) ||
           (b[0] == 192 && b[1] == 168) ||
           (b[0] == 169 && b[1] == 254);
}

/** Verify this request arrived on the current, live private STA IPv4 address. */
static bool on_sta_iface(httpd_req_t* req) {
    esp_netif_t* netif = g_staNetif;
    if (!netif || !esp_netif_is_netif_up(netif)) return false;
    esp_netif_ip_info_t sta{};
    if (esp_netif_get_ip_info(netif, &sta) != ESP_OK ||
        !sta.ip.addr || !ip_is_private(sta.ip.addr)) return false;
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &len) != 0) return false;
    if (addr.ss_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in local{};
        memcpy(&local, &addr, sizeof(local));
        return local.sin_addr.s_addr == sta.ip.addr;
    }
#if CONFIG_LWIP_IPV6
    // ESP-IDF's dual-stack HTTP listener reports IPv4 as ::ffff:a.b.c.d.
    if (addr.ss_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
        struct sockaddr_in6 local{};
        memcpy(&local, &addr, sizeof(local));
        constexpr uint8_t mappedPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        const uint8_t* ip = local.sin6_addr.s6_addr;
        return memcmp(ip, mappedPrefix, sizeof(mappedPrefix)) == 0 &&
               memcmp(ip + sizeof(mappedPrefix), &sta.ip.addr, sizeof(sta.ip.addr)) == 0;
    }
#endif
    return false;
}

/** Constant-time Bearer token check. */
static bool auth_ok(httpd_req_t* req) {
    char hdr[80];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    if (strncmp(hdr, "Bearer ", 7) != 0) return false;
    const char* tok = hdr + 7;
    if (strlen(tok) != paxg::ota::kTokenHexLen) return false;
    bool ok = ct_memeq(tok, g_tokenHex, paxg::ota::kTokenHexLen);
    memset(hdr, 0, sizeof(hdr));
    return ok;
}

static void json_error(httpd_req_t* req, int status, const char* code, const char* detail) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\",\"detail\":\"%s\"}", code, detail);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const char* sl = "500 Internal Server Error";
    if      (status == 400) sl = "400 Bad Request";
    else if (status == 401) sl = "401 Unauthorized";
    else if (status == 403) sl = "403 Forbidden";
    else if (status == 408) sl = "408 Request Timeout";
    else if (status == 409) sl = "409 Conflict";
    else if (status == 503) sl = "503 Service Unavailable";
    httpd_resp_set_status(req, sl);
    httpd_resp_sendstr(req, buf);
}

static esp_err_t update_reject(httpd_req_t* req, int status, const char* code, const char* detail) {
    httpd_resp_set_hdr(req, "Connection", "close");
    json_error(req, status, code, detail);
    return ESP_FAIL;
}

static void json_ok(httpd_req_t* req, const char* json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_sendstr(req, json);
}

static esp_err_t json_202(httpd_req_t* req, const char* json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, json);
}

/** Schedule deferred restart. 202 must be sent before this is called. */
static void schedule_restart() {
    g_deferRstUs.store((uint32_t)esp_timer_get_time(), std::memory_order_relaxed);
    g_rebootPend.store(true, std::memory_order_release);
    g_deferRst.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// RAII OTA context — stack-local in handle_update
// ---------------------------------------------------------------------------
struct UploadCtx {
    esp_ota_handle_t  handle   = 0;
    bool              began    = false;  // esp_ota_begin called
    bool              maint    = false;  // maintenanceBegin called
    mbedtls_sha256_context sha;
    bool              shaInit  = false;

    ~UploadCtx() {
        if (shaInit)  { mbedtls_sha256_free(&sha); shaInit = false; }
        if (began && handle) {
            esp_ota_abort(handle);
            handle = 0; began = false;
        }
        if (maint && g_hooks.maintenanceLeave) {
            g_hooks.maintenanceLeave(false);
            maint = false;
        }
    }
    void commit() {
        // Called on success path; prevents destructor from aborting/leaving-false.
        handle = 0; began = false; maint = false;
    }
};

// ---------------------------------------------------------------------------
// GET /
// ---------------------------------------------------------------------------
static esp_err_t handle_root(httpd_req_t* req) {
    if (!on_sta_iface(req)) { json_error(req, 403, "forbidden", "not_sta"); return ESP_OK; }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, paxg::ota::kDashboardHtml);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /api/version
// ---------------------------------------------------------------------------
static esp_err_t handle_version(httpd_req_t* req) {
    if (!g_ready)         { json_error(req, 503, "not_ready",    "init_failed"); return ESP_OK; }
    if (!on_sta_iface(req)) { json_error(req, 403, "forbidden",  "not_sta");     return ESP_OK; }
    if (!auth_ok(req))    { json_error(req, 401, "unauthorized", "bad_token");   return ESP_OK; }

    const esp_app_desc_t* d = esp_app_get_description();
    char elfSha[65] = {};
    bytes_to_hex((const uint8_t*)d->app_elf_sha256, 32, elfSha);

    const esp_partition_t* running  = esp_ota_get_running_partition();
    const esp_partition_t* inactive = esp_ota_get_next_update_partition(nullptr);

#ifndef OTA_GIT_COMMIT
#define OTA_GIT_COMMIT "unknown"
#endif
    char buf[kJsonBuf];
    snprintf(buf, sizeof(buf),
        "{\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"project_name\":\"%s\",\"version\":\"%s\","
        "\"git_commit\":\"%s\",\"elf_sha256\":\"%s\","
        "\"chip_id\":%" PRIu32 ",\"active_partition\":\"%s\","
        "\"ota_max_size\":%" PRIu32 "}",
        g_deviceId, g_bootId,
        d->project_name, d->version,
        OTA_GIT_COMMIT, elfSha,
        (uint32_t)paxg::ota::kExpectedChipId,
        running  ? running->label  : "unknown",
        inactive ? inactive->size  : 0u);
    json_ok(req, buf);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /api/health
// ---------------------------------------------------------------------------
static esp_err_t handle_health(httpd_req_t* req) {
    if (!g_ready)         { json_error(req, 503, "not_ready",    "init_failed"); return ESP_OK; }
    if (!on_sta_iface(req)) { json_error(req, 403, "forbidden",  "not_sta");     return ESP_OK; }
    if (!auth_ok(req))    { json_error(req, 401, "unauthorized", "bad_token");   return ESP_OK; }

    paxg::ota::HealthSnapshot s{};
    if (g_hooks.snapshotHealth) g_hooks.snapshotHealth(&s);

    // Defensive fallbacks
    if (s.uptimeSec  == 0) s.uptimeSec  = (uint32_t)(esp_timer_get_time() / 1000000LL);
    if (s.freeHeap   == 0) s.freeHeap   = esp_get_free_heap_size();
    if (s.minFreeHeap== 0) s.minFreeHeap= esp_get_minimum_free_heap_size();

    // healthy = hook says healthy AND not pending bootloader validation
    bool healthy = s.healthy && !paxg::ota::pendingValidation();

    const esp_partition_t* running = esp_ota_get_running_partition();
    paxg::ota::BootState bs = paxg::ota::bootState();
    const char* bsTxt = (bs == paxg::ota::BootState::PendingValidation)
                        ? "pending_validation" : "valid";

    char buf[kJsonBuf];
    snprintf(buf, sizeof(buf),
        "{\"healthy\":%s,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"telemetry\":{"
        "\"rssi\":%" PRId8 ",\"local_ip\":\"%s\","
        "\"uptime_sec\":%" PRIu32 ",\"free_heap\":%" PRIu32 ","
        "\"min_free_heap\":%" PRIu32 ",\"free_psram\":%" PRIu32 ","
        "\"total_psram\":%" PRIu32 ","
        "\"usb_mounted\":%s,\"usb_hid_ready\":%s,\"usb_audio_streaming\":%s,"
        "\"wifi_mode\":\"%s\",\"active_slot\":\"%s\","
        "\"boot_state\":\"%s\",\"operating_mode\":\"%s\""
        "}}",
        healthy ? "true" : "false",
        g_deviceId, g_bootId,
        s.rssi, s.localIp,
        s.uptimeSec, s.freeHeap, s.minFreeHeap, s.freePsram, s.totalPsram,
        s.usbMounted        ? "true" : "false",
        s.usbHidReady       ? "true" : "false",
        s.usbAudioStreaming  ? "true" : "false",
        s.wifiMode[0] ? s.wifiMode : "STA",
        running ? running->label : "unknown",
        bsTxt,
        s.operatingMode[0] ? s.operatingMode : "unknown");
    json_ok(req, buf);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /api/logs
// ---------------------------------------------------------------------------
static esp_err_t handle_logs(httpd_req_t* req) {
    if (!g_ready)         { json_error(req, 503, "not_ready",    "init_failed"); return ESP_OK; }
    if (!on_sta_iface(req)) { json_error(req, 403, "forbidden",  "not_sta");     return ESP_OK; }
    if (!auth_ok(req))    { json_error(req, 401, "unauthorized", "bad_token");   return ESP_OK; }

    // Max: kLogRingCap*(kLogEntryLen+40) + framing
    static char lbuf[paxg::ota::kLogRingCap * (kLogEntryLen + 40) + 8];
    size_t pos = 0;
    lbuf[pos++] = '[';

    if (xSemaphoreTake(g_logMtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint32_t count = g_logCount < paxg::ota::kLogRingCap
                         ? g_logCount : (uint32_t)paxg::ota::kLogRingCap;
        uint32_t start = g_logCount >= paxg::ota::kLogRingCap ? g_logHead : 0;
        for (uint32_t i = 0; i < count; i++) {
            const LogEntry& e = g_log[(start + i) % paxg::ota::kLogRingCap];
            if (i > 0 && pos < sizeof(lbuf) - 2) lbuf[pos++] = ',';
            int n = snprintf(lbuf + pos, sizeof(lbuf) - pos - 4,
                             "{\"ts\":%" PRId64 ",\"msg\":\"", e.tsUs / 1000);
            if (n > 0) pos += (size_t)n;
            for (const char* p = e.msg; *p && pos < sizeof(lbuf) - 4; p++) {
                if (*p == '"' || *p == '\\') lbuf[pos++] = '\\';
                lbuf[pos++] = *p;
            }
            if (pos < sizeof(lbuf) - 2) { lbuf[pos++] = '"'; lbuf[pos++] = '}'; }
        }
        xSemaphoreGive(g_logMtx);
    }
    lbuf[pos++] = ']'; lbuf[pos] = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_sendstr(req, lbuf);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /api/update
// ---------------------------------------------------------------------------
static esp_err_t handle_update(httpd_req_t* req) {
    const int64_t uploadStartUs = esp_timer_get_time();
    // 1. Init check
    if (!g_ready) return update_reject(req, 503, "not_ready", "init_failed");
    // 2. Interface check
    if (!on_sta_iface(req)) return update_reject(req, 403, "forbidden", "not_sta");
    // 3. Auth (before conflict gating)
    if (!auth_ok(req)) return update_reject(req, 401, "unauthorized", "bad_token");
    // 4. Conflict gating
    if (g_rebootPend) return update_reject(req, 409, "conflict", "reboot_pending");
    if (paxg::ota::pendingValidation()) {
        return update_reject(req, 409, "conflict", "pending_validation");
    }

    // 5. Health gate
    {
        paxg::ota::HealthSnapshot hs{};
        if (g_hooks.snapshotHealth) g_hooks.snapshotHealth(&hs);
        if (!hs.healthy) {
            return update_reject(req, 503, "not_ready", "system_unhealthy");
        }
    }

    // 6. Headers
    char ctype[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) != ESP_OK
        || strncmp(ctype, "application/octet-stream", 24) != 0) {
        return update_reject(req, 400, "bad_request", "content_type");
    }

    size_t contentLen = req->content_len;
    if (contentLen < kPrefixBytes || contentLen > 8u * 1024 * 1024) {
        return update_reject(req, 400, "bad_request", "content_length");
    }

    char expectSha[65] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Firmware-SHA256",
                                    expectSha, sizeof(expectSha)) != ESP_OK
        || !valid_sha256_hex(expectSha)) {
        return update_reject(req, 400, "bad_request", "x_firmware_sha256");
    }
    for (int i = 0; i < 64; i++)
        if (expectSha[i] >= 'A' && expectSha[i] <= 'F') expectSha[i] += 32;

    // 7. Partition selection; reject same address as running
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) return update_reject(req, 503, "no_partition", "unavailable");
    {
        const esp_partition_t* running = esp_ota_get_running_partition();
        if (running && running->address == part->address) {
            return update_reject(req, 409, "conflict", "same_partition");
        }
    }
    if (contentLen > part->size) {
        return update_reject(req, 400, "bad_request", "image_too_large");
    }

    // 8. Accumulate full prefix before esp_ota_begin
    // Receive exactly kPrefixBytes into a stack buffer before any flash write.
    uint8_t prefix[kPrefixBytes];
    {
        size_t pGot = 0;
        while (pGot < kPrefixBytes) {
            if (esp_timer_get_time() - uploadStartUs > (int64_t)kTotalDeadMs * 1000 || !on_sta_iface(req))
                return update_reject(req, 408, "timeout", "prefix_deadline_or_sta_lost");
            int r = httpd_req_recv(req, (char*)(prefix + pGot),
                                   (int)(kPrefixBytes - pGot));
            if (r <= 0) {
                return update_reject(req, 400, "bad_request", "recv_prefix");
            }
            pGot += (size_t)r;
        }
    }

    // 9. Validate image header (magic + chip_id)
    // esp_image_header_t layout (packed, IDF 5.x):
    //   uint8_t  magic           [0]
    //   uint8_t  segment_count   [1]
    //   uint8_t  spi_mode        [2]
    //   uint8_t  spi_speed:4, spi_size:4 [3]
    //   uint32_t entry_addr      [4..7]
    //   uint8_t  wp_pin          [8]
    //   uint8_t  spi_pin_drv[3]  [9..11]
    //   uint16_t chip_id         [12..13]  ← ESP_CHIP_ID_ESP32S3 = 0x0009
    //   uint8_t  min_chip_rev    [14]
    //   uint8_t  min_chip_rev_full[15..16]
    //   uint8_t  max_chip_rev_full[17..18]
    //   uint8_t  reserved[8]     [19..26]
    //   uint8_t  hash_appended   [27]
    if (prefix[0] != 0xE9u) {
        return update_reject(req, 400, "bad_image", "bad_magic");
    }
    // chip_id little-endian at byte 12
    uint16_t chipId = (uint16_t)(prefix[12] | (prefix[13] << 8));
    if (chipId != (uint16_t)paxg::ota::kExpectedChipId) {
        return update_reject(req, 400, "bad_image", "chip_id_mismatch");
    }

    // 10. Validate esp_app_desc_t at kAppDescOffset
    if (kAppDescOffset + sizeof(esp_app_desc_t) > kPrefixBytes) {
        // Should never happen with kPrefixBytes=288 and sizeof(esp_app_desc_t)≤256
        return update_reject(req, 500, "internal", "prefix_size");
    }
    const esp_app_desc_t* imgDesc =
        reinterpret_cast<const esp_app_desc_t*>(prefix + kAppDescOffset);
    if (imgDesc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return update_reject(req, 400, "bad_image", "bad_desc_magic");
    }
    if (strncmp(imgDesc->project_name, "esp32s3_arduino_idf_poc",
                sizeof(imgDesc->project_name)) != 0) {
        return update_reject(req, 400, "bad_image", "project_mismatch");
    }
    if (strncmp(imgDesc->version, paxg::ota::kVersionPrefix,
                strlen(paxg::ota::kVersionPrefix)) != 0) {
        return update_reject(req, 400, "bad_image", "version_prefix");
    }

    // 11. Maintenance begin (bounded owner ack)
    UploadCtx ctx;
    if (g_hooks.maintenanceBegin) {
        bool ack = g_hooks.maintenanceBegin(kMaintTmoMs);
        if (!ack) {
            return update_reject(req, 503, "not_ready", "maint_ack_timeout");
        }
        ctx.maint = true;
    }

    // 12. esp_ota_begin
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ctx.handle) != ESP_OK) {
        return update_reject(req, 503, "ota_error", "begin_failed"); // destructor calls maintenanceLeave(false)
    }
    ctx.began = true;

    // 13. SHA-256 init + write prefix
    mbedtls_sha256_init(&ctx.sha); ctx.shaInit = true;
    if (mbedtls_sha256_starts(&ctx.sha, 0) != 0 ||
        mbedtls_sha256_update(&ctx.sha, prefix, kPrefixBytes) != 0)
        return update_reject(req, 500, "crypto_error", "sha_init_failed");

    if (esp_ota_write(ctx.handle, prefix, kPrefixBytes) != ESP_OK) {
        return update_reject(req, 500, "ota_error", "write_failed"); // destructor aborts
    }

    paxg::ota::logEvent("OTA upload started");

    // 14. Stream remainder
    int64_t startUs = uploadStartUs;
    int64_t lastUs  = esp_timer_get_time();
    size_t  received = kPrefixBytes;
    size_t  remaining = contentLen - kPrefixBytes;

    uint8_t buf[kWriteBlock];
    while (remaining > 0) {
        int64_t nowUs = esp_timer_get_time();
        if (nowUs - startUs > (int64_t)kTotalDeadMs * 1000 || !on_sta_iface(req)) {
            paxg::ota::logEvent("OTA upload total deadline");
            return update_reject(req, 408, "timeout", "upload_deadline_or_sta_lost");
        }
        if (nowUs - lastUs > (int64_t)kIdleDeadMs * 1000) {
            paxg::ota::logEvent("OTA upload idle timeout");
            return update_reject(req, 408, "timeout", "idle_timeout");
        }

        int toRead = (int)(remaining < kWriteBlock ? remaining : kWriteBlock);
        int got = httpd_req_recv(req, (char*)buf, toRead);
        if (got <= 0) {
            paxg::ota::logEvent("OTA recv error");
            return update_reject(req, 400, "bad_request", "recv_error");
        }
        if (!on_sta_iface(req))
            return update_reject(req, 408, "timeout", "sta_lost");
        lastUs   = esp_timer_get_time();
        received += (size_t)got;

        if (mbedtls_sha256_update(&ctx.sha, buf, (size_t)got) != 0)
            return update_reject(req, 500, "crypto_error", "sha_update_failed");
        if (esp_ota_write(ctx.handle, buf, (size_t)got) != ESP_OK) {
            paxg::ota::logEvent("OTA write error");
            return update_reject(req, 500, "ota_error", "write_failed");
        }
        remaining -= (size_t)got;
        if (g_hooks.progress) g_hooks.progress(received, contentLen);
    }

    if (received != contentLen) {
        paxg::ota::logEvent("OTA length mismatch");
        return update_reject(req, 400, "bad_request", "length_mismatch");
    }
    if (!on_sta_iface(req)) return update_reject(req, 408, "timeout", "sta_lost");

    // 15. Digest check
    uint8_t digest[32];
    if (mbedtls_sha256_finish(&ctx.sha, digest) != 0)
        return update_reject(req, 500, "crypto_error", "sha_finish_failed");
    mbedtls_sha256_free(&ctx.sha); ctx.shaInit = false;
    char actualSha[65];
    bytes_to_hex(digest, 32, actualSha);
    if (!ct_memeq(actualSha, expectSha, 64)) {
        paxg::ota::logEvent("OTA SHA256 mismatch");
        return update_reject(req, 400, "bad_image", "sha256_mismatch");
    }

    // 16. esp_ota_end (also does IDF internal image verify)
    esp_err_t endErr = esp_ota_end(ctx.handle);
    ctx.handle = 0; ctx.began = false; // prevent destructor abort
    if (endErr != ESP_OK) {
        paxg::ota::logEvent("OTA end failed");
        return update_reject(req, 400, "bad_image", "ota_end_failed");
    }

    esp_partition_pos_t pos{part->address, part->size};
    esp_image_metadata_t metadata{};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &metadata) != ESP_OK ||
        metadata.image_len != contentLen) {
        paxg::ota::logEvent("OTA image verification/length mismatch");
        return update_reject(req, 400, "bad_image", "image_length_or_verify_failed");
    }

    // 17. Set boot partition
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        paxg::ota::logEvent("OTA set_boot_partition failed");
        return update_reject(req, 500, "ota_error", "set_boot_failed");
    }
    paxg::ota::logEvent("OTA verified; boot partition updated");

    // 18. Maintenance leave success; commit ctx (no abort/leave-false in destructor)
    if (g_hooks.maintenanceLeave) {
        g_hooks.maintenanceLeave(true);
        ctx.maint = false;
    }
    ctx.commit();

    // 19. Send 202 then arrange restart
    char resp[200];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"accepted\",\"boot_id\":\"%s\","
             "\"restart_after_ms\":%" PRIu32 "}",
             g_bootId, kRestartDelMs);
    const esp_err_t sent = json_202(req, resp);
    schedule_restart();
    if (sent != ESP_OK) paxg::ota::logEvent("OTA 202 response send failed");
    return sent;
}

// ---------------------------------------------------------------------------
// POST /api/rollback
// ---------------------------------------------------------------------------
static esp_err_t handle_rollback(httpd_req_t* req) {
    if (!g_ready)         { json_error(req, 503, "not_ready",    "init_failed"); return ESP_OK; }
    if (!on_sta_iface(req)) { json_error(req, 403, "forbidden",  "not_sta");     return ESP_OK; }
    if (!auth_ok(req))    { json_error(req, 401, "unauthorized", "bad_token");   return ESP_OK; }
    if (g_rebootPend)     { json_error(req, 409, "conflict",     "reboot_pending"); return ESP_OK; }
    // Reject rollback attempt if currently pending validation (would mask problem)
    if (paxg::ota::pendingValidation()) {
        json_error(req, 409, "conflict", "pending_validation"); return ESP_OK;
    }

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* target  = nullptr;

    // Find a valid, compatible, non-running OTA partition
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (it) {
        const esp_partition_t* p = esp_partition_get(it);
        if (p && running && p->address != running->address) {
            esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
            if (esp_ota_get_state_partition(p, &st) == ESP_OK
                && st == ESP_OTA_IMG_VALID) {
                // Verify compatible project name from stored descriptor
                esp_app_desc_t pd{};
                esp_ota_get_partition_description(p, &pd);
                if (strncmp(pd.project_name, "esp32s3_arduino_idf_poc",
                            sizeof(pd.project_name)) == 0
                    && strncmp(pd.version, paxg::ota::kVersionPrefix,
                               strlen(paxg::ota::kVersionPrefix)) == 0) {
                    target = p;
                    break;
                }
            }
        }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    if (!target) {
        json_error(req, 409, "rollback_unavailable", "no_valid_target"); return ESP_OK;
    }

    // Maintenance before changing boot selection
    UploadCtx ctx; // reuse RAII for maintenanceLeave
    if (g_hooks.maintenanceBegin) {
        if (!g_hooks.maintenanceBegin(kMaintTmoMs)) {
            json_error(req, 503, "not_ready", "maint_ack_timeout"); return ESP_OK;
        }
        ctx.maint = true;
    }

    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        json_error(req, 500, "ota_error", "set_boot_failed"); return ESP_OK;
    }
    paxg::ota::logEvent("Rollback: boot partition set");

    if (g_hooks.maintenanceLeave) { g_hooks.maintenanceLeave(true); ctx.maint = false; }
    ctx.commit();

    char resp[200];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"rollback_scheduled\","
             "\"target_partition\":\"%s\","
             "\"restart_after_ms\":%" PRIu32 "}",
             target->label, kRestartDelMs);
    const esp_err_t sent = json_202(req, resp);
    schedule_restart();
    if (sent != ESP_OK) paxg::ota::logEvent("rollback 202 response send failed");
    return sent;
}

// ---------------------------------------------------------------------------
// POST /api/reboot
// ---------------------------------------------------------------------------
static esp_err_t handle_reboot(httpd_req_t* req) {
    if (!g_ready)         { json_error(req, 503, "not_ready",    "init_failed"); return ESP_OK; }
    if (!on_sta_iface(req)) { json_error(req, 403, "forbidden",  "not_sta");     return ESP_OK; }
    if (!auth_ok(req))    { json_error(req, 401, "unauthorized", "bad_token");   return ESP_OK; }
    if (g_rebootPend)     { json_error(req, 409, "conflict",     "reboot_pending"); return ESP_OK; }

    paxg::ota::logEvent("Reboot requested via /api/reboot");
    char resp[200];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"reboot_scheduled\",\"boot_id\":\"%s\","
             "\"restart_after_ms\":%" PRIu32 "}",
             g_bootId, kRestartDelMs);
    const esp_err_t sent = json_202(req, resp);
    schedule_restart();
    if (sent != ESP_OK) paxg::ota::logEvent("reboot 202 response send failed");
    return sent;
}

// ---------------------------------------------------------------------------
// Route table
// ---------------------------------------------------------------------------
static const httpd_uri_t kRoutes[] = {
    { "/",             HTTP_GET,  handle_root,     nullptr },
    { "/api/version",  HTTP_GET,  handle_version,  nullptr },
    { "/api/health",   HTTP_GET,  handle_health,   nullptr },
    { "/api/logs",     HTTP_GET,  handle_logs,     nullptr },
    { "/api/update",   HTTP_POST, handle_update,   nullptr },
    { "/api/rollback", HTTP_POST, handle_rollback, nullptr },
    { "/api/reboot",   HTTP_POST, handle_reboot,   nullptr },
};

static esp_err_t register_routes(httpd_handle_t srv) {
    for (auto& r : kRoutes) {
        if (httpd_register_uri_handler(srv, &r) != ESP_OK) {
            ESP_LOGE(TAG, "failed to register route %s", r.uri);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

} // anonymous namespace

// ===========================================================================
// Public API
// ===========================================================================
namespace paxg {
namespace ota {

esp_err_t init(const RuntimeHooks& hooks) {
    if (g_ready) return ESP_OK;
    if (!hooks.maintenanceBegin) return ESP_ERR_INVALID_ARG;

    g_hooks = hooks;

    if (!g_logMtx) {
        g_logMtx = xSemaphoreCreateMutex();
        if (!g_logMtx) return ESP_ERR_NO_MEM;
    }

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    bytes_to_hex(mac, 6, g_deviceId);

    uint32_t rnd = esp_random();
    int64_t  ts  = esp_timer_get_time();
    snprintf(g_bootId, sizeof(g_bootId), "%08" PRIx32 "%08" PRIx64, rnd, (uint64_t)ts);

    // Token provisioning
    nvs_handle_t nvsH = 0;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &nvsH);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err)); return ESP_FAIL; }

    char stored[kTokenHexLen + 1] = {};
    size_t storedLen = sizeof(stored);
    bool needGen = false;

    err = nvs_get_str(nvsH, kNvsTokenKey, stored, &storedLen);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        needGen = true;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_str: %s", esp_err_to_name(err));
        nvs_close(nvsH); return ESP_FAIL;
    } else {
        needGen = (storedLen - 1 != kTokenHexLen);
        if (!needGen) {
            for (size_t i = 0; i < kTokenHexLen; i++)
                if (!is_hex_char(stored[i])) { needGen = true; break; }
        }
    }

    if (needGen) {
        uint8_t raw[kTokenBytes];
        esp_fill_random(raw, kTokenBytes);
        uint8_t acc = 0; for (auto b : raw) acc |= b;
        if (acc == 0) {
            ESP_LOGE(TAG, "RNG degenerate"); nvs_close(nvsH); return ESP_FAIL;
        }
        bytes_to_hex(raw, kTokenBytes, g_tokenHex);
        memset(raw, 0, sizeof(raw));

        err = nvs_set_str(nvsH, kNvsTokenKey, g_tokenHex);
        if (err != ESP_OK || (err = nvs_commit(nvsH)) != ESP_OK) {
            memset(g_tokenHex, 0, sizeof(g_tokenHex));
            ESP_LOGE(TAG, "nvs write: %s", esp_err_to_name(err));
            nvs_close(nvsH); return ESP_FAIL;
        }
        logEvent("token provisioned");
    } else {
        memcpy(g_tokenHex, stored, kTokenHexLen + 1);
    }
    memset(stored, 0, sizeof(stored));
    nvs_close(nvsH);

    g_ready = true;
    logEvent("paxg::ota ready");
    return ESP_OK;
}

esp_err_t start(esp_netif_t* netif) {
    if (!g_ready || !netif) return ESP_ERR_INVALID_STATE;
    if (g_running)          return ESP_OK;
    if (g_rebootPend)       return ESP_ERR_INVALID_STATE;

    if (!esp_netif_is_netif_up(netif) || esp_netif_get_ip_info(netif, &g_staIp) != ESP_OK) {
        ESP_LOGE(TAG, "start: STA interface is down");
        return ESP_FAIL;
    }
    if (g_staIp.ip.addr == 0 || !ip_is_private(g_staIp.ip.addr)) {
        ESP_LOGE(TAG, "start: invalid STA IP");
        return ESP_FAIL;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = kOtaHttpPort;
    cfg.ctrl_port         = kOtaCtrlPort;
    cfg.task_priority     = tskIDLE_PRIORITY + 2;
    cfg.stack_size        = 8192;
    cfg.max_open_sockets  = 4;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.max_uri_handlers  = 8;
    cfg.lru_purge_enable  = true;

    g_staNetif = netif;
    if (httpd_start(&g_server, &cfg) != ESP_OK) {
        g_staNetif = nullptr;
        ESP_LOGE(TAG, "httpd_start failed"); return ESP_FAIL;
    }
    if (register_routes(g_server) != ESP_OK) {
        httpd_stop(g_server); g_server = nullptr;
        g_staNetif = nullptr;
        ESP_LOGE(TAG, "route registration failed"); return ESP_FAIL;
    }

    g_running = true;
    logEvent("OTA server started :8080");
    return ESP_OK;
}

void stop() {
    // Contract: stop() does NOT touch in-flight OTA handle — that state is
    // stack-local in handle_update(). httpd_stop() closes all sockets;
    // handle_update() receives <=0 from httpd_req_recv and its UploadCtx
    // destructor runs esp_ota_abort + maintenanceLeave(false) automatically.
    if (!g_running) return;
    httpd_stop(g_server);
    g_server  = nullptr;
    g_staNetif = nullptr;
    g_running = false;
    logEvent("OTA server stopped");
}

void tick() {
    if (!g_ready) return;
    if (!g_rebootPend.load(std::memory_order_acquire) ||
        !g_deferRst.load(std::memory_order_acquire)) return;
    uint32_t nowUs = (uint32_t)esp_timer_get_time();
    if ((uint32_t)(nowUs - g_deferRstUs.load(std::memory_order_relaxed)) >
        kRestartDelMs * 1000u && g_deferRst.exchange(false, std::memory_order_acq_rel)) {
        if (g_hooks.requestRestart) g_hooks.requestRestart();
        else esp_restart();
    }
}

bool isRunning() { return g_running; }
bool isReady()   { return g_ready;   }

BootState bootState() {
    const esp_partition_t* p = esp_ota_get_running_partition();
    if (!p) return BootState::Unknown;
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(p, &st) != ESP_OK) return BootState::Unknown;
    if (st == ESP_OTA_IMG_PENDING_VERIFY) return BootState::PendingValidation;
    if (st == ESP_OTA_IMG_VALID) return BootState::Valid;
    return BootState::Unknown;
}

bool pendingValidation() { return bootState() == BootState::PendingValidation; }

const char* getToken() { return g_ready ? g_tokenHex : nullptr; }

void logEvent(const char* msg) {
    if (!msg || !g_logMtx) return;
    if (xSemaphoreTake(g_logMtx, pdMS_TO_TICKS(20)) != pdTRUE) return;
    LogEntry& e = g_log[g_logHead];
    strncpy(e.msg, msg, kLogEntryLen - 1);
    e.msg[kLogEntryLen - 1] = '\0';
    e.tsUs = esp_timer_get_time();
    g_logHead = (g_logHead + 1) % paxg::ota::kLogRingCap;
    g_logCount++;
    xSemaphoreGive(g_logMtx);
}

} // namespace ota
} // namespace paxg
