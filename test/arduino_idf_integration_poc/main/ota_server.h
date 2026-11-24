/**
 * @file ota_server.h
 * @brief paxg::ota — Standalone HTTP OTA server, ESP-IDF 5.x, port 8080.
 *
 * Lifecycle
 * ---------
 * 1. init(hooks)      — once, NVS already open. Provisions token, fails closed.
 * 2. start(netif)     — on STA "got IP". Binds 8080 on STA private IP. Idempotent.
 * 3. stop()           — on STA disconnect. Tears down httpd. Safe if already stopped.
 *                       Contract: stop() does NOT touch any in-flight OTA handle;
 *                       upload state is local to the HTTP server task and is cleaned
 *                       up when the socket closes. Owner must NOT call stop() while
 *                       expecting a concurrent esp_ota_write to complete gracefully.
 * 4. tick()           — from owner loop ≥ every 500 ms. Services deferred restart.
 *
 * Token
 * -----
 * getToken() → LCD-only reveal. NEVER log. nullptr until init() succeeds.
 *
 * Boot validation
 * ---------------
 * Owner calls esp_ota_mark_app_valid_cancel_rollback() / _invalid_rollback_and_reboot()
 * after genuine health checks. Use bootState() / pendingValidation() to gate.
 */

#pragma once
#include <cstdint>
#include <cstddef>
#include <esp_netif.h>
#include <esp_err.h>

namespace paxg {
namespace ota {

static constexpr uint16_t kOtaHttpPort  = 8080;
static constexpr uint16_t kOtaCtrlPort  = 32769; ///< distinct from fishing 32768
static constexpr size_t   kLogRingCap   = 32;
static constexpr size_t   kTokenBytes   = 32;     ///< 256-bit
static constexpr size_t   kTokenHexLen  = 64;
static constexpr const char* kVersionPrefix = "ota1-";
static constexpr uint32_t kExpectedChipId   = 9;  ///< ESP32-S3

enum class BootState : uint8_t {
    Unknown = 0, Valid, PendingValidation
};

enum class OtaPhase : uint8_t {
    Idle = 0, Receiving, Verifying, Done, Failed
};

// ---------------------------------------------------------------------------
// Hook types
// ---------------------------------------------------------------------------

/** Called before OTA writes start. Must ack within timeoutMs; false aborts. */
using MaintenanceBeginFn = bool (*)(uint32_t timeoutMs);
/** Called after OTA completes or fails. success=false if aborted/error. */
using MaintenanceLeaveFn = void (*)(bool success);
/** Progress during streaming (optional). */
using ProgressFn = void (*)(size_t written, size_t total);
/** Owner arranges safe restart within ~10 s; called after 202 sent.
 *  If null the module calls esp_restart() directly. */
using RequestRestartFn = void (*)();

/**
 * @brief Thread-safe health/telemetry snapshot filled by the owner.
 *
 * healthy         — owner asserts overall system health (not pendingValidation).
 * usbHidReady     — HID device enumerated and ready.
 * usbAudioStreaming — UAC audio stream active.
 * totalPsram      — total installed PSRAM bytes (0 if none).
 * operatingMode   — human-readable mode string, e.g. "fishing", "setup", "idle".
 */
struct HealthSnapshot {
    bool     healthy            = false;
    int8_t   rssi               = 0;
    char     localIp[32]        = {};
    uint32_t uptimeSec          = 0;
    uint32_t freeHeap           = 0;
    uint32_t minFreeHeap        = 0;
    uint32_t freePsram          = 0;
    uint32_t totalPsram         = 0;
    bool     usbMounted         = false;
    bool     usbHidReady        = false;
    bool     usbAudioStreaming  = false;
    char     wifiMode[8]        = {};   ///< "STA", "AP", "APSTA"
    char     activeSlot[8]      = {};
    char     bootStateTxt[24]   = {};
    char     operatingMode[24]  = {};
};
using SnapshotHealthFn = void (*)(HealthSnapshot* out);

struct RuntimeHooks {
    MaintenanceBeginFn  maintenanceBegin  = nullptr; ///< required
    MaintenanceLeaveFn  maintenanceLeave  = nullptr;
    ProgressFn          progress          = nullptr;
    RequestRestartFn    requestRestart    = nullptr;
    SnapshotHealthFn    snapshotHealth    = nullptr;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/** Init; NVS must be open. Provisions/loads token. Fails closed. */
esp_err_t init(const RuntimeHooks& hooks);

/** Start server bound to STA private IP. Safe on reconnect. */
esp_err_t start(esp_netif_t* netif);

/** Stop server. Upload state is local to HTTP task; stop() does not abort it.
 *  Any partial write is abandoned when the httpd socket closes. */
void stop();

/** Service deferred restart from owner loop (≥ 500 ms cadence). */
void tick();

bool isRunning();
bool isReady();
BootState bootState();
bool pendingValidation();

/** LCD-only token reveal. nullptr if init() not yet succeeded. Never log. */
const char* getToken();

/** Append to bounded OTA event log. No credentials. Safe from any task. */
void logEvent(const char* msg);

} // namespace ota
} // namespace paxg

// ota_dashboard.h must declare:
//   namespace paxg { namespace ota { extern const char kDashboardHtml[]; } }
// Included only by ota_server.cpp.
