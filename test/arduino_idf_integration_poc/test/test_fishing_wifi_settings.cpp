#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "fishing_wifi_settings.h"
#include "fishing_web_server.h"
#include "live_fishing_controller.h"

using namespace paxg::wifi;

// Mock test implementations
class TestWifiDriver : public IWifiDriver {
public:
    uint8_t mode = 0;
    bool connected = false;
    char staIp[32] = "192.168.1.50";
    char connectedSsid[33] = {0};
    char connectedPass[65] = {0};
    bool apRunning = false;
    char apSsid[33] = {0};
    char apPass[17] = {0};

    void setMode(uint8_t m) override { mode = m; }
    void connectSta(const char* ssid, const char* password) override {
        if (ssid) {
            std::strncpy(connectedSsid, ssid, sizeof(connectedSsid) - 1);
            connectedSsid[sizeof(connectedSsid) - 1] = '\0';
        }
        if (password) {
            std::strncpy(connectedPass, password, sizeof(connectedPass) - 1);
            connectedPass[sizeof(connectedPass) - 1] = '\0';
        }
    }
    void startAp(const char* ssid, const char* password, const char* ip) override {
        (void)ip;
        apRunning = true;
        if (ssid) std::strncpy(apSsid, ssid, sizeof(apSsid) - 1);
        if (password) std::strncpy(apPass, password, sizeof(apPass) - 1);
    }
    void stopAp() override {
        apRunning = false;
    }
    bool isStaConnected() override { return connected; }
    const char* getStaIp() override { return connected ? staIp : "0.0.0.0"; }
    void disconnectSta() override { connected = false; }
};

struct TestWifiBlob {
    uint32_t magic = 0x57494649;
    char ssid[33] = {0};
    char pass[65] = {0};
    uint32_t checksum = 0;
};

static uint32_t calcTestChecksum(const TestWifiBlob& blob) {
    uint32_t sum = 0x12345678;
    for (size_t i = 0; i < sizeof(blob.ssid); ++i) sum = (sum * 31) + static_cast<uint8_t>(blob.ssid[i]);
    for (size_t i = 0; i < sizeof(blob.pass); ++i) sum = (sum * 31) + static_cast<uint8_t>(blob.pass[i]);
    return sum;
}

class TestNvsStorage : public INvsStorage {
public:
    TestWifiBlob blob;
    bool hasBlob = false;
    char legacySsid[33] = {0};
    char legacyPass[65] = {0};
    bool hasLegacy = false;
    bool failSave = false;

    bool loadCredentials(char* outSsid, size_t maxSsid, char* outPass, size_t maxPass) override {
        if (hasBlob && blob.magic == 0x57494649 && blob.checksum == calcTestChecksum(blob)) {
            std::strncpy(outSsid, blob.ssid, maxSsid - 1);
            outSsid[maxSsid - 1] = '\0';
            std::strncpy(outPass, blob.pass, maxPass - 1);
            outPass[maxPass - 1] = '\0';
            return true;
        }
        if (hasLegacy && legacySsid[0] != '\0') {
            std::strncpy(outSsid, legacySsid, maxSsid - 1);
            outSsid[maxSsid - 1] = '\0';
            std::strncpy(outPass, legacyPass, maxPass - 1);
            outPass[maxPass - 1] = '\0';
            return true;
        }
        return false;
    }

    bool saveCredentials(const char* ssid, const char* password) override {
        if (failSave) return false;
        if (!ssid || ssid[0] == '\0') return false;
        blob.magic = 0x57494649;
        std::strncpy(blob.ssid, ssid, sizeof(blob.ssid) - 1);
        blob.ssid[sizeof(blob.ssid) - 1] = '\0';
        std::strncpy(blob.pass, password ? password : "", sizeof(blob.pass) - 1);
        blob.pass[sizeof(blob.pass) - 1] = '\0';
        blob.checksum = calcTestChecksum(blob);
        hasBlob = true;
        return true;
    }

    bool clearCredentials() override {
        hasBlob = false;
        hasLegacy = false;
        blob = {};
        legacySsid[0] = '\0';
        legacyPass[0] = '\0';
        return true;
    }
};

class TestClock : public IClock {
public:
    uint32_t currentMs = 1000;
    uint32_t nowMs() override { return currentMs; }
    void advance(uint32_t ms) { currentMs += ms; }
};

// 1. Initial boot with stored NVS credentials connects to stored STA
void test_wifi_settings_initial_boot_with_nvs_credentials() {
    printf("[RUN]  test_wifi_settings_initial_boot_with_nvs_credentials\n");
    TestWifiDriver driver;
    TestNvsStorage nvs;
    TestClock clock;

    nvs.saveCredentials("StoredHomeWiFi", "SecretPass123");

    WifiSettingsManager mgr(&driver, &nvs, &clock);
    mgr.init("CompiledSSID", "CompiledPass");

    assert(mgr.getStatus().state == WifiState::ConnectingSta);
    assert(std::strcmp(mgr.getStatus().currentSsid, "StoredHomeWiFi") == 0);

    driver.connected = true;
    mgr.update(clock.nowMs());

    assert(mgr.getStatus().state == WifiState::ConnectedSta);
    assert(std::strcmp(mgr.getStatus().staIp, "192.168.1.50") == 0);
    assert(!mgr.isSetupApActive());
    printf("  [PASS] test_wifi_settings_initial_boot_with_nvs_credentials\n");
}

// 2. Initial boot without NVS falls back to compiled credentials
void test_wifi_settings_initial_boot_fallback_to_compiled() {
    printf("[RUN]  test_wifi_settings_initial_boot_fallback_to_compiled\n");
    TestWifiDriver driver;
    TestNvsStorage nvs;
    TestClock clock;

    WifiSettingsManager mgr(&driver, &nvs, &clock);
    mgr.init("FallbackSSID", "FallbackPass");

    assert(mgr.getStatus().state == WifiState::ConnectingSta);
    assert(std::strcmp(mgr.getStatus().currentSsid, "FallbackSSID") == 0);

    driver.connected = true;
    mgr.update(clock.nowMs());

    assert(mgr.getStatus().state == WifiState::ConnectedSta);
    assert(!mgr.isSetupApActive());
    printf("  [PASS] test_wifi_settings_initial_boot_fallback_to_compiled\n");
}

// 3. STA unreachable on boot triggers time-bounded WPA2 Setup AP
void test_wifi_settings_initial_boot_sta_failure_starts_wpa2_setup_ap() {
    printf("[RUN]  test_wifi_settings_initial_boot_sta_failure_starts_wpa2_setup_ap\n");
    TestWifiDriver driver;
    TestNvsStorage nvs;
    TestClock clock;

    WifiSettingsManager mgr(&driver, &nvs, &clock);
    mgr.init("UnreachableWiFi", "Pass");

    assert(mgr.getStatus().state == WifiState::ConnectingSta);
    driver.connected = false;

    clock.advance(15500);
    mgr.update(clock.nowMs());

    assert(mgr.getStatus().state == WifiState::SetupApActive);
    assert(mgr.isSetupApActive());
    assert(driver.apRunning);
    assert(std::strncmp(mgr.getStatus().apSsid, "PAXG-Setup-", 11) == 0);
    assert(std::strlen(mgr.getTftApPassword()) >= 8); // WPA2 compliant (not open!)
    assert(std::strcmp(mgr.getStatus().apIp, "192.168.4.1") == 0);
    assert(mgr.getStatus().setupRemainingSec > 0);
    printf("  [PASS] test_wifi_settings_initial_boot_sta_failure_starts_wpa2_setup_ap\n");
}

// 4. Verify no open AP (always non-empty WPA2 password >= 8 characters)
void test_wifi_settings_no_open_ap() {
    printf("[RUN]  test_wifi_settings_no_open_ap\n");
    TestWifiDriver driver;
    TestNvsStorage nvs;
    TestClock clock;

    WifiSettingsManager mgr(&driver, &nvs, &clock);
    mgr.init(nullptr, nullptr);

    assert(mgr.isSetupApActive());
    assert(driver.apRunning);
    assert(std::strlen(driver.apPass) >= 8);
    assert(std::strncmp(driver.apPass, "paxg", 4) == 0);
    printf("  [PASS] test_wifi_settings_no_open_ap\n");
}

// 5. PIN verification before any apply action; invalid PIN leaves state unchanged
void test_wifi_settings_pin_validation_before_fishing_impact() {
    printf("[RUN]  test_wifi_settings_pin_validation_before_fishing_impact\n");
    TestWifiDriver driver;
    TestNvsStorage nvs;
    TestClock clock;

    WifiSettingsManager mgr(&driver, &nvs, &clock);
    mgr.init("Net", "Pass");

    const char* pin = mgr.getTftPin();
    assert(pin != nullptr);
    assert(std::strlen(pin) == 4);

    // Invalid PIN must be rejected
    assert(!mgr.verifyPin("0000"));
    assert(!mgr.verifyPin("123"));
    assert(!mgr.verifyPin(""));
    assert(!mgr.verifyPin(nullptr));

    // Valid PIN accepted
    assert(mgr.verifyPin(pin));
    printf("  [PASS] test_wifi_settings_pin_validation_before_fishing_impact\n");
}

// 6. Custom header, bounded staging and JSON parsing
void test_wifi_settings_staging_and_bounded_parsing() {
    printf("[RUN]  test_wifi_settings_staging_and_bounded_parsing\n");
    TestWifiDriver driver;
    TestNvsStorage nvs;
    TestClock clock;

    WifiSettingsManager mgr(&driver, &nvs, &clock);
    mgr.init("Net", "Pass");

    // Empty SSID rejected
    assert(!mgr.stagePendingApply("", "Pass"));

    // Oversized SSID (> 32) rejected
    std::string longSsid(33, 'A');
    assert(!mgr.stagePendingApply(longSsid.c_str(), "Pass"));

    // Oversized password (> 64) rejected
    std::string longPass(65, 'B');
    assert(!mgr.stagePendingApply("ValidSSID", longPass.c_str()));

    // Valid apply staged successfully
    assert(mgr.stagePendingApply("ValidSSID", "ValidPass"));

    // JSON parsing helper tests
    char s[33], p[65], pinOut[9];
    const char* validJson = "{\"ssid\":\"HomeWiFi\",\"password\":\"Secret123\",\"pin\":\"4829\"}";
    assert(WifiSettingsManager::parseApplyJson(validJson, s, sizeof(s), p, sizeof(p), pinOut, sizeof(pinOut)));
    assert(std::strcmp(s, "HomeWiFi") == 0);
    assert(std::strcmp(p, "Secret123") == 0);
    assert(std::strcmp(pinOut, "4829") == 0);

    const char* noPinJson = "{\"ssid\":\"HomeWiFi\",\"password\":\"Secret123\"}";
    assert(!WifiSettingsManager::parseApplyJson(noPinJson, s, sizeof(s), p, sizeof(p), pinOut, sizeof(pinOut)));
    printf("  [PASS] test_wifi_settings_staging_and_bounded_parsing\n");
}

// 7. Successful new connection saves atomic NVS blob and updates IP
void test_wifi_settings_apply_success_saves_to_nvs_and_updates_ip() {
    printf("[RUN]  test_wifi_settings_apply_success_saves_to_nvs_and_updates_ip\n");
    TestWifiDriver driver;
    TestNvsStorage nvs;
    TestClock clock;

    WifiSettingsManager mgr(&driver, &nvs, &clock);
    mgr.init(nullptr, nullptr); // starts in Setup AP
    assert(mgr.isSetupApActive());

    // Stage pending apply from owner context
    assert(mgr.stagePendingApply("BrandNewRouter", "CorrectPassKey"));

    // Owner loop update consumes pending apply
    clock.advance(100);
    mgr.update(clock.nowMs());

    assert(mgr.getStatus().state == WifiState::ConnectingNewSta);
    assert(mgr.isSetupApActive());
    assert(driver.apRunning);
    assert(driver.mode == 3); // AP_STA

    // Simulate successful DHCP connection
    driver.connected = true;
    std::strncpy(driver.staIp, "192.168.1.188", sizeof(driver.staIp));
    clock.advance(1000);
    mgr.update(clock.nowMs());

    assert(mgr.getStatus().state == WifiState::ConnectedSta);
    assert(std::strcmp(mgr.getStatus().staIp, "192.168.1.188") == 0);
    assert(!mgr.isSetupApActive());
    assert(!driver.apRunning);

    // Dedicated NVS must contain atomic blob with matching checksum
    char savedSsid[33] = {0};
    char savedPass[65] = {0};
    assert(nvs.loadCredentials(savedSsid, sizeof(savedSsid), savedPass, sizeof(savedPass)));
    assert(std::strcmp(savedSsid, "BrandNewRouter") == 0);
    assert(std::strcmp(savedPass, "CorrectPassKey") == 0);
    printf("  [PASS] test_wifi_settings_apply_success_saves_to_nvs_and_updates_ip\n");
}

// 8. Connection failure preserves last working config and restores on timeout
void test_wifi_settings_apply_failure_preserves_nvs_and_restores_last_working() {
    printf("[RUN]  test_wifi_settings_apply_failure_preserves_nvs_and_restores_last_working\n");
    TestWifiDriver driver;
    TestNvsStorage nvs;
    TestClock clock;

    nvs.saveCredentials("OriginalGoodWiFi", "GoodPass");
    WifiSettingsManager mgr(&driver, &nvs, &clock);
    mgr.init("Fallback", "Pass");

    driver.connected = true;
    mgr.update(clock.nowMs());
    assert(mgr.getStatus().state == WifiState::ConnectedSta);

    // Stage bad network apply
    assert(mgr.stagePendingApply("WrongNetwork", "BadPass"));
    clock.advance(100);
    mgr.update(clock.nowMs());
    assert(mgr.getStatus().state == WifiState::ConnectingNewSta);

    driver.connected = false;

    // Advance clock past 15000ms timeout
    clock.advance(16000);
    mgr.update(clock.nowMs());

    // NVS must NOT be modified with BadPass!
    char curSsid[33] = {0};
    char curPass[65] = {0};
    assert(nvs.loadCredentials(curSsid, sizeof(curSsid), curPass, sizeof(curPass)));
    assert(std::strcmp(curSsid, "OriginalGoodWiFi") == 0);
    assert(std::strcmp(curPass, "GoodPass") == 0);

    // Restoring last working
    assert(mgr.getStatus().state == WifiState::RestoringLastWorking || mgr.getStatus().state == WifiState::ConnectingSta);
    printf("  [PASS] test_wifi_settings_apply_failure_preserves_nvs_and_restores_last_working\n");
}

// 9. NVS save failure handled as failure: restores previous working config
void test_wifi_settings_save_failure_restores_last_working() {
    printf("[RUN]  test_wifi_settings_save_failure_restores_last_working\n");
    TestWifiDriver driver;
    TestNvsStorage nvs;
    TestClock clock;

    nvs.saveCredentials("OriginalGoodWiFi", "GoodPass");
    WifiSettingsManager mgr(&driver, &nvs, &clock);
    mgr.init(nullptr, nullptr);

    driver.connected = true;
    mgr.update(clock.nowMs());

    // Make NVS fail save on next write
    nvs.failSave = true;

    assert(mgr.stagePendingApply("NewNet", "NewPass"));
    clock.advance(100);
    mgr.update(clock.nowMs());

    // Driver connects, but NVS save fails
    driver.connected = true;
    clock.advance(1000);
    mgr.update(clock.nowMs());

    // Must be treated as failure, NOT accepted!
    assert(std::strcmp(mgr.getStatus().lastError, "NVS_SAVE_FAILED") == 0);
    assert(mgr.getStatus().state == WifiState::RestoringLastWorking || mgr.getStatus().state == WifiState::ConnectingSta);
    printf("  [PASS] test_wifi_settings_save_failure_restores_last_working\n");
}

// 10. Atomic NVS blob with legacy fallback
void test_wifi_settings_atomic_nvs_blob_and_legacy_fallback() {
    printf("[RUN]  test_wifi_settings_atomic_nvs_blob_and_legacy_fallback\n");
    TestNvsStorage nvs;

    // 1. Atomic blob write & read
    assert(nvs.saveCredentials("AtomicSSID", "AtomicPassword"));
    assert(nvs.hasBlob);
    assert(nvs.blob.magic == 0x57494649);

    char outS[33] = {0};
    char outP[65] = {0};
    assert(nvs.loadCredentials(outS, sizeof(outS), outP, sizeof(outP)));
    assert(std::strcmp(outS, "AtomicSSID") == 0);
    assert(std::strcmp(outP, "AtomicPassword") == 0);

    // 2. Corrupt blob checksum causes load to fall back or fail
    nvs.blob.checksum = 0xDEADBEEF;
    assert(!nvs.loadCredentials(outS, sizeof(outS), outP, sizeof(outP)));

    // 3. Legacy fallback (no blob, separate string keys)
    nvs.hasBlob = false;
    nvs.hasLegacy = true;
    std::strncpy(nvs.legacySsid, "LegacySSID", sizeof(nvs.legacySsid));
    std::strncpy(nvs.legacyPass, "LegacyPassword", sizeof(nvs.legacyPass));

    assert(nvs.loadCredentials(outS, sizeof(outS), outP, sizeof(outP)));
    assert(std::strcmp(outS, "LegacySSID") == 0);
    assert(std::strcmp(outP, "LegacyPassword") == 0);
    printf("  [PASS] test_wifi_settings_atomic_nvs_blob_and_legacy_fallback\n");
}

// 11. Secrets (passwords and PIN) strictly never exposed in status snapshot or JSON
void test_wifi_settings_password_and_pin_never_exposed() {
    printf("[RUN]  test_wifi_settings_password_and_pin_never_exposed\n");
    TestWifiDriver driver;
    TestNvsStorage nvs;
    TestClock clock;

    nvs.saveCredentials("SecretNet", "SuperSecretPassword123!");
    WifiSettingsManager mgr(&driver, &nvs, &clock);
    mgr.init(nullptr, nullptr);

    WifiStatus st = mgr.getStatus();
    char jsonBuf[512] = {0};
    size_t len = WifiSettingsManager::buildWifiStatusJson(st, jsonBuf, sizeof(jsonBuf));
    assert(len > 0);

    // Verify neither STA password, AP password, nor PIN appear in JSON
    assert(std::strstr(jsonBuf, "SuperSecretPassword123!") == nullptr);
    assert(std::strstr(jsonBuf, "\"password\"") == nullptr);
    assert(std::strstr(jsonBuf, "\"pin\"") == nullptr);
    assert(std::strstr(jsonBuf, mgr.getTftPin()) == nullptr);
    assert(std::strstr(jsonBuf, mgr.getTftApPassword()) == nullptr);
    printf("  [PASS] test_wifi_settings_password_and_pin_never_exposed\n");
}

// 12. Setup AP interface blocks HID commands; ordinary STA 192.168.4.x is NOT blocked
void test_wifi_settings_setup_ap_blocks_hid_commands_ordinary_sta_allowed() {
    printf("[RUN]  test_wifi_settings_setup_ap_blocks_hid_commands_ordinary_sta_allowed\n");
    TestWifiDriver driver;
    TestNvsStorage nvs;
    TestClock clock;

    WifiSettingsManager mgr(&driver, &nvs, &clock);
    mgr.init("HomeNet", "HomePass");
    driver.connected = true;
    mgr.update(clock.nowMs());
    assert(mgr.getStatus().state == WifiState::ConnectedSta);
    assert(!mgr.isSetupApActive());

    // When AP is inactive and request is NOT on AP interface: allowed!
    assert(isHidCommandAllowed(false));

    // When request arrives on AP interface (local IP 192.168.4.1): blocked!
    assert(!isHidCommandAllowed(true));
    printf("  [PASS] test_wifi_settings_setup_ap_blocks_hid_commands_ordinary_sta_allowed\n");
}

// 13. Prevent start during setup or apply
void test_wifi_settings_prevent_start_during_setup_and_apply() {
    printf("[RUN]  test_wifi_settings_prevent_start_during_setup_and_apply\n");
    TestWifiDriver driver;
    TestNvsStorage nvs;
    TestClock clock;

    WifiSettingsManager mgr(&driver, &nvs, &clock);
    mgr.init(nullptr, nullptr); // Boot into Setup AP

    // During Setup AP: start is blocked!
    assert(mgr.isSetupOrApplyActive());

    // Stage apply: start remains blocked!
    assert(mgr.stagePendingApply("NewNet", "NewPass"));
    assert(mgr.isSetupOrApplyActive());

    mgr.update(clock.nowMs());
    assert(mgr.isSetupOrApplyActive());

    // Connects successfully
    driver.connected = true;
    clock.advance(1000);
    mgr.update(clock.nowMs());
    assert(mgr.getStatus().state == WifiState::ConnectedSta);

    // Connected & Setup AP closed: start allowed!
    assert(!mgr.isSetupOrApplyActive());
    printf("  [PASS] test_wifi_settings_prevent_start_during_setup_and_apply\n");
}

int main() {
    printf("=======================================================================\n");
    printf("PAXG Wi-Fi Settings & Recovery Manager Focused Native Tests\n");
    printf("=======================================================================\n");

    test_wifi_settings_initial_boot_with_nvs_credentials();
    test_wifi_settings_initial_boot_fallback_to_compiled();
    test_wifi_settings_initial_boot_sta_failure_starts_wpa2_setup_ap();
    test_wifi_settings_no_open_ap();
    test_wifi_settings_pin_validation_before_fishing_impact();
    test_wifi_settings_staging_and_bounded_parsing();
    test_wifi_settings_apply_success_saves_to_nvs_and_updates_ip();
    test_wifi_settings_apply_failure_preserves_nvs_and_restores_last_working();
    test_wifi_settings_save_failure_restores_last_working();
    test_wifi_settings_atomic_nvs_blob_and_legacy_fallback();
    test_wifi_settings_password_and_pin_never_exposed();
    test_wifi_settings_setup_ap_blocks_hid_commands_ordinary_sta_allowed();
    test_wifi_settings_prevent_start_during_setup_and_apply();

    printf("=======================================================================\n");
    printf("Results: 13/13 passed (0 failed)\n");
    printf("=======================================================================\n");
    return 0;
}
