#include "fishing_wifi_settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <WiFi.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_random.h>
#endif

namespace paxg {
namespace wifi {

namespace {

// Default embedded HTML page for /wifi
static const char WIFI_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">\n"
"<title>PAXG ESP32-S3 Wi-Fi Setup &amp; Recovery</title>\n"
"<style>\n"
"body{background:#121212;color:#eee;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif;margin:0;padding:16px;text-align:center}\n"
".container{max-width:540px;margin:0 auto;background:#1e1e1e;border-radius:12px;padding:20px;box-shadow:0 4px 12px rgba(0,0,0,0.5)}\n"
"h1{margin:0 0 10px;font-size:20px;color:#ffd700;letter-spacing:1px}\n"
".nav-row{text-align:left;margin-bottom:15px}\n"
".nav-row a{color:#00b0ff;text-decoration:none;font-weight:bold;font-size:14px}\n"
".badge{display:inline-block;padding:8px 18px;border-radius:20px;font-weight:bold;font-size:15px;margin-bottom:15px}\n"
".badge-conn{background:#00c853;color:#000}\n"
".badge-ap{background:#ff9100;color:#000}\n"
".badge-busy{background:#00b0ff;color:#000}\n"
".badge-fail{background:#d50000;color:#fff}\n"
".card{background:#2a2a2a;padding:14px;border-radius:8px;text-align:left;margin-bottom:12px}\n"
".card .label{font-size:11px;color:#aaa;text-transform:uppercase;letter-spacing:0.5px}\n"
".card .val{font-size:16px;font-weight:bold;margin-top:4px;color:#fff;word-break:break-all}\n"
".info-box{background:#1b3039;border-left:4px solid #00b0ff;padding:12px;border-radius:4px;text-align:left;font-size:13px;line-height:1.4;margin-bottom:15px}\n"
".alert-box{padding:12px;border-radius:8px;font-weight:bold;margin-bottom:15px;display:none;text-align:left;font-size:14px}\n"
".alert-err{background:#b71c1c;color:#fff}\n"
".alert-ok{background:#1b5e20;color:#fff}\n"
"form{text-align:left}\n"
".form-group{margin-bottom:14px}\n"
"label{display:block;font-size:12px;color:#bbb;text-transform:uppercase;margin-bottom:6px;font-weight:bold}\n"
"input[type=\"text\"],input[type=\"password\"]{width:100%;box-sizing:border-box;padding:12px;border-radius:6px;border:1px solid #444;background:#161616;color:#fff;font-size:15px}\n"
"input:focus{outline:none;border-color:#00b0ff}\n"
"button{width:100%;padding:14px;border:none;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer;background:#00b0ff;color:#000;transition:opacity 0.2s}\n"
"button:disabled{opacity:0.5;cursor:not-allowed}\n"
".pin-hint{font-size:12px;color:#ffab40;margin-top:4px}\n"
".footer{margin-top:20px;font-size:11px;color:#666}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"container\">\n"
"<div class=\"nav-row\"><a href=\"/\">&larr; Back to Fishing Dashboard</a></div>\n"
"<h1>ESP32-S3 WI-FI SETTINGS</h1>\n"
"<div id=\"status-badge\" class=\"badge badge-busy\">CHECKING STATUS...</div>\n"
"<div id=\"msg-box\" class=\"alert-box\"></div>\n"
"<div class=\"card\">\n"
"<div class=\"label\">Current Network (SSID)</div>\n"
"<div id=\"cur-ssid\" class=\"val\">--</div>\n"
"</div>\n"
"<div class=\"card\">\n"
"<div class=\"label\">Station IP Address</div>\n"
"<div id=\"cur-ip\" class=\"val\">--</div>\n"
"</div>\n"
"<div id=\"ap-card\" class=\"card\" style=\"display:none\">\n"
"<div class=\"label\">Recovery Setup AP (WPA2)</div>\n"
"<div id=\"ap-ssid\" class=\"val\">--</div>\n"
"<div style=\"font-size:12px;color:#aaa;margin-top:4px\">AP IP: 192.168.4.1 | Password shown on device TFT</div>\n"
"</div>\n"
"<div class=\"info-box\">\n"
"<strong>Physical Security Gating:</strong> To change Wi-Fi settings over LAN or Setup AP, enter the 4-digit <strong>Device PIN</strong> displayed on the ESP32-S3 screen. Passwords are never logged or echoed.\n"
"</div>\n"
"<form id=\"wifi-form\" onsubmit=\"applyWifi(event)\">\n"
"<div class=\"form-group\">\n"
"<label for=\"ssid\">Target Wi-Fi SSID (max 32 chars)</label>\n"
"<input type=\"text\" id=\"ssid\" name=\"ssid\" maxlength=\"32\" required placeholder=\"Enter Wi-Fi network name\">\n"
"</div>\n"
"<div class=\"form-group\">\n"
"<label for=\"password\">Target Wi-Fi Password (WPA2, max 64 chars)</label>\n"
"<input type=\"password\" id=\"password\" name=\"password\" maxlength=\"64\" placeholder=\"Leave blank if open network\">\n"
"</div>\n"
"<div class=\"form-group\">\n"
"<label for=\"pin\">Device-Local Authorization PIN</label>\n"
"<input type=\"text\" id=\"pin\" name=\"pin\" maxlength=\"8\" required placeholder=\"4-digit PIN from S3 screen\">\n"
"<div class=\"pin-hint\">&bull; Look at the TFT screen on the ESP32-S3 for the current PIN</div>\n"
"</div>\n"
"<button type=\"submit\" id=\"btn-apply\">APPLY &amp; TEST CONNECTION</button>\n"
"</form>\n"
"<div class=\"footer\">Strict Fail-Closed | Dedicated Flash NVS | Setup AP HID Command Lockout</div>\n"
"</div>\n"
"<script>\n"
"let pollTimer=null;\n"
"function showMsg(text,isErr){\n"
"const b=document.getElementById('msg-box');\n"
"b.className='alert-box '+(isErr?'alert-err':'alert-ok');\n"
"b.innerHTML=text;\n"
"b.style.display='block';\n"
"}\n"
"function fetchStatus(){\n"
"fetch('/api/wifi/status').then(r=>r.json()).then(d=>{\n"
"const b=document.getElementById('status-badge');\n"
"const curSsid=document.getElementById('cur-ssid');\n"
"const curIp=document.getElementById('cur-ip');\n"
"const apCard=document.getElementById('ap-card');\n"
"curSsid.innerText=d.ssid||'(None / Setup AP)';\n"
"curIp.innerText=d.ip||'0.0.0.0';\n"
"if(d.ap_active){\n"
"apCard.style.display='block';\n"
"document.getElementById('ap-ssid').innerText=d.ap_ssid+' (Time left: '+d.setup_remaining_sec+'s)';\n"
"}else{\n"
"apCard.style.display='none';\n"
"}\n"
"if(d.state==='CONNECTED'){\n"
"b.className='badge badge-conn';b.innerText='CONNECTED';\n"
"}else if(d.state==='CONNECTING'||d.state==='CONNECTING_NEW'){\n"
"b.className='badge badge-busy';b.innerText='CONNECTING...';\n"
"}else if(d.state==='SETUP_AP'){\n"
"b.className='badge badge-ap';b.innerText='SETUP AP ACTIVE';\n"
"}else{\n"
"b.className='badge badge-fail';b.innerText=d.state||'DISCONNECTED';\n"
"}\n"
"}).catch(()=>{});\n"
"}\n"
"function applyWifi(e){\n"
"e.preventDefault();\n"
"const ssid=document.getElementById('ssid').value.trim();\n"
"const password=document.getElementById('password').value;\n"
"const pin=document.getElementById('pin').value.trim();\n"
"if(!ssid){alert('SSID is required');return;}\n"
"if(!pin){alert('Device PIN is required');return;}\n"
"const btn=document.getElementById('btn-apply');\n"
"btn.disabled=true;btn.innerText='CONNECTING IN BACKGROUND...';\n"
"showMsg('Applying new Wi-Fi credentials... Testing connection in background while keeping Setup AP reachable.',false);\n"
"fetch('/api/wifi/apply',{\n"
"method:'POST',\n"
"headers:{\n"
"'Content-Type':'application/json',\n"
"'X-Requested-With':'XMLHttpRequest',\n"
"'X-Setup-Auth':pin\n"
"},\n"
"body:JSON.stringify({ssid:ssid,password:password,pin:pin})\n"
"}).then(r=>r.json()).then(res=>{\n"
"if(!res.success){\n"
"btn.disabled=false;btn.innerText='APPLY & TEST CONNECTION';\n"
"showMsg('Apply Rejected: '+(res.reason||'Unknown error'),true);\n"
"}else{\n"
"let pollCount=0;\n"
"if(pollTimer)clearInterval(pollTimer);\n"
"pollTimer=setInterval(()=>{\n"
"fetch('/api/wifi/status').then(r=>r.json()).then(d=>{\n"
"pollCount++;\n"
"if(d.state==='CONNECTED'&&d.ssid===ssid){\n"
"clearInterval(pollTimer);\n"
"btn.disabled=false;btn.innerText='APPLY & TEST CONNECTION';\n"
"showMsg('SUCCESS: Connected to <b>'+d.ssid+'</b>!<br>New Device IP: <b>'+d.ip+'</b><br>Please reconnect your device to <b>'+d.ssid+'</b> and navigate to: <a href=\"http://'+d.ip+'/\" style=\"color:#fff;font-weight:bold\">http://'+d.ip+'/</a>',false);\n"
"fetchStatus();\n"
"}else if(d.state==='SETUP_AP'||d.last_error){\n"
"if(pollCount>5||d.last_error){\n"
"clearInterval(pollTimer);\n"
"btn.disabled=false;btn.innerText='APPLY & TEST CONNECTION';\n"
"showMsg('Connection to \"'+ssid+'\" failed ('+(d.last_error||'timeout')+'). Reverted to last working configuration. Setup AP remains active.',true);\n"
"fetchStatus();\n"
"}\n"
"}\n"
"}).catch(()=>{});\n"
"},1500);\n"
"}\n"
"}).catch(err=>{\n"
"btn.disabled=false;btn.innerText='APPLY & TEST CONNECTION';\n"
"showMsg('Request error: '+err,true);\n"
"});\n"
"}\n"
"setInterval(fetchStatus,2000);\n"
"fetchStatus();\n"
"</script>\n"
"</body>\n"
"</html>\n";

// Atomic NVS Storage Record
struct WifiBlob {
    uint32_t magic = 0x57494649; // 'WIFI'
    char ssid[33] = {0};
    char pass[65] = {0};
    uint32_t checksum = 0;
};

static uint32_t calcBlobChecksum(const WifiBlob& blob) {
    uint32_t sum = 0x12345678;
    for (size_t i = 0; i < sizeof(blob.ssid); ++i) {
        sum = (sum * 31) + static_cast<uint8_t>(blob.ssid[i]);
    }
    for (size_t i = 0; i < sizeof(blob.pass); ++i) {
        sum = (sum * 31) + static_cast<uint8_t>(blob.pass[i]);
    }
    return sum;
}

// Host mock driver for native builds
#if !defined(ARDUINO) && !defined(ESP_PLATFORM)
class MockWifiDriver : public IWifiDriver {
public:
    uint8_t mode = 0;
    bool connected = false;
    char staIp[32] = "192.168.1.100";
    char connectedSsid[33] = {0};
    bool apRunning = false;
    char apSsid[33] = {0};
    char apPass[17] = {0};

    void setMode(uint8_t m) override { mode = m; }
    void connectSta(const char* ssid, const char* password) override {
        (void)password;
        if (ssid) {
            std::strncpy(connectedSsid, ssid, sizeof(connectedSsid) - 1);
            connectedSsid[sizeof(connectedSsid) - 1] = '\0';
        }
        connected = true;
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

class MockNvsStorage : public INvsStorage {
public:
    WifiBlob blob;
    bool hasBlob = false;
    char legacySsid[33] = {0};
    char legacyPass[65] = {0};
    bool hasLegacy = false;
    bool failSave = false;

    bool loadCredentials(char* outSsid, size_t maxSsid, char* outPass, size_t maxPass) override {
        if (hasBlob && blob.magic == 0x57494649 && blob.checksum == calcBlobChecksum(blob)) {
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
        blob.checksum = calcBlobChecksum(blob);
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

class MockClock : public IClock {
public:
    uint32_t currentMs = 1000;
    uint32_t nowMs() override { return currentMs; }
    void advance(uint32_t ms) { currentMs += ms; }
};

static MockWifiDriver s_defaultDriver;
static MockNvsStorage s_defaultNvs;
static MockClock s_defaultClock;

#else

class EspWifiDriver : public IWifiDriver {
public:
    void setMode(uint8_t mode) override {
        if (mode == 0) WiFi.mode(WIFI_OFF);
        else if (mode == 1) WiFi.mode(WIFI_STA);
        else if (mode == 2) WiFi.mode(WIFI_AP);
        else if (mode == 3) WiFi.mode(WIFI_AP_STA);
    }

    void connectSta(const char* ssid, const char* password) override {
        WiFi.persistent(false); // Safeguard: never let Arduino auto-write to standard NVS
        if (password && password[0] != '\0') {
            WiFi.begin(ssid, password);
        } else {
            WiFi.begin(ssid);
        }
    }

    void startAp(const char* ssid, const char* password, const char* ip) override {
        (void)ip;
        IPAddress localIp(192, 168, 4, 1);
        IPAddress gateway(192, 168, 4, 1);
        IPAddress subnet(255, 255, 255, 0);
        WiFi.softAPConfig(localIp, gateway, subnet);
        WiFi.softAP(ssid, password); // Always WPA2 (minimum 8 chars password)
    }

    void stopAp() override {
        WiFi.softAPdisconnect(true);
    }

    bool isStaConnected() override {
        return (WiFi.status() == WL_CONNECTED && WiFi.localIP()[0] != 0);
    }

    const char* getStaIp() override {
        static char ipBuf[32];
        if (WiFi.status() == WL_CONNECTED) {
            std::strncpy(ipBuf, WiFi.localIP().toString().c_str(), sizeof(ipBuf) - 1);
            ipBuf[sizeof(ipBuf) - 1] = '\0';
            return ipBuf;
        }
        return "0.0.0.0";
    }

    void disconnectSta() override {
        WiFi.disconnect(false);
    }
};

class EspNvsStorage : public INvsStorage {
public:
    bool loadCredentials(char* outSsid, size_t maxSsid, char* outPass, size_t maxPass) override {
        nvs_handle_t handle;
        esp_err_t err = nvs_open("paxg_wifi", NVS_READONLY, &handle);
        if (err != ESP_OK) return false;

        // 1. Try atomic blob load
        WifiBlob blob;
        size_t reqBlob = sizeof(blob);
        err = nvs_get_blob(handle, "wifi_blob", &blob, &reqBlob);
        if (err == ESP_OK && reqBlob == sizeof(blob) && blob.magic == 0x57494649) {
            if (blob.checksum == calcBlobChecksum(blob) && blob.ssid[0] != '\0') {
                std::strncpy(outSsid, blob.ssid, maxSsid - 1);
                outSsid[maxSsid - 1] = '\0';
                std::strncpy(outPass, blob.pass, maxPass - 1);
                outPass[maxPass - 1] = '\0';
                nvs_close(handle);
                return true;
            }
        }

        // 2. Legacy fallback to separate string records
        size_t ssidLen = maxSsid;
        err = nvs_get_str(handle, "ssid", outSsid, &ssidLen);
        if (err == ESP_OK && ssidLen > 1) {
            size_t passLen = maxPass;
            err = nvs_get_str(handle, "pass", outPass, &passLen);
            if (err != ESP_OK) {
                outPass[0] = '\0';
            }
            nvs_close(handle);
            return true;
        }

        nvs_close(handle);
        return false;
    }

    bool saveCredentials(const char* ssid, const char* password) override {
        if (!ssid || ssid[0] == '\0') return false;

        nvs_handle_t handle;
        esp_err_t err = nvs_open("paxg_wifi", NVS_READWRITE, &handle);
        if (err != ESP_OK) return false;

        WifiBlob blob;
        blob.magic = 0x57494649;
        std::strncpy(blob.ssid, ssid, sizeof(blob.ssid) - 1);
        blob.ssid[sizeof(blob.ssid) - 1] = '\0';
        std::strncpy(blob.pass, password ? password : "", sizeof(blob.pass) - 1);
        blob.pass[sizeof(blob.pass) - 1] = '\0';
        blob.checksum = calcBlobChecksum(blob);

        err = nvs_set_blob(handle, "wifi_blob", &blob, sizeof(blob));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
        return (err == ESP_OK);
    }

    bool clearCredentials() override {
        nvs_handle_t handle;
        esp_err_t err = nvs_open("paxg_wifi", NVS_READWRITE, &handle);
        if (err != ESP_OK) return false;
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
        return true;
    }
};

class EspClock : public IClock {
public:
    uint32_t nowMs() override { return millis(); }
};

static EspWifiDriver s_defaultDriver;
static EspNvsStorage s_defaultNvs;
static EspClock s_defaultClock;

#endif

static WifiSettingsManager s_wifiMgr(&s_defaultDriver, &s_defaultNvs, &s_defaultClock);

} // namespace

const char* getWifiStateName(WifiState state) {
    switch (state) {
        case WifiState::Uninitialized:        return "UNINIT";
        case WifiState::ConnectingSta:        return "CONNECTING";
        case WifiState::ConnectedSta:         return "CONNECTED";
        case WifiState::StaFailed:            return "STA_FAILED";
        case WifiState::SetupApActive:        return "SETUP_AP";
        case WifiState::ConnectingNewSta:     return "CONNECTING_NEW";
        case WifiState::RestoringLastWorking: return "RESTORING";
        default:                              return "UNKNOWN";
    }
}

WifiSettingsManager::WifiSettingsManager(IWifiDriver* driver, INvsStorage* nvs, IClock* clock)
    : driver_(driver ? driver : &s_defaultDriver),
      nvs_(nvs ? nvs : &s_defaultNvs),
      clock_(clock ? clock : &s_defaultClock) {}

void WifiSettingsManager::generatePin() {
    uint32_t rnd = 0;
#if defined(ARDUINO) || defined(ESP_PLATFORM)
    rnd = esp_random();
#else
    rnd = static_cast<uint32_t>(rand());
#endif
    // Generate bounded 4-digit PIN in [1000, 9999]
    unsigned int val = (rnd % 9000) + 1000;
    std::snprintf(pin_, sizeof(pin_), "%04u", val);
}

void WifiSettingsManager::generateApCredentials() {
    uint32_t rnd = 0;
#if defined(ARDUINO) || defined(ESP_PLATFORM)
    rnd = esp_random();
#else
    rnd = static_cast<uint32_t>(rand());
#endif
    // Unique SSID e.g. PAXG-Setup-A1B2
    std::snprintf(status_.apSsid, sizeof(status_.apSsid), "PAXG-Setup-%04X", (unsigned int)(rnd & 0xFFFF));
    // Non-open WPA2 password: min 8 alphanumeric characters e.g. paxg4829
    unsigned int passSuffix = ((rnd >> 16) % 9000) + 1000;
    std::snprintf(apPassword_, sizeof(apPassword_), "paxg%04u", passSuffix);
    std::strncpy(status_.apIp, "192.168.4.1", sizeof(status_.apIp) - 1);
}

void WifiSettingsManager::enterSetupAp(uint32_t nowMs) {
    if (apPassword_[0] == '\0') {
        generateApCredentials();
    }
    if (pin_[0] == '\0') {
        generatePin();
    }

    // Keep STA mode active alongside AP (WIFI_AP_STA = 3) so STA reconnects can occur
    driver_->setMode(3);
    driver_->startAp(status_.apSsid, apPassword_, status_.apIp);

    status_.state = WifiState::SetupApActive;
    status_.isSetupAp = true;
    setupApDeadlineMs_ = nowMs + SETUP_AP_TIMEOUT_MS;
    status_.setupRemainingSec = SETUP_AP_TIMEOUT_MS / 1000;
}

void WifiSettingsManager::enterConnectingSta(const char* ssid, const char* password, uint32_t nowMs, bool isNewApply) {
    if (!ssid || ssid[0] == '\0') {
        enterSetupAp(nowMs);
        return;
    }

    isNewApply_ = isNewApply;
    std::strncpy(status_.currentSsid, ssid, sizeof(status_.currentSsid) - 1);
    status_.currentSsid[sizeof(status_.currentSsid) - 1] = '\0';

    if (pin_[0] == '\0') {
        generatePin();
    }

    if (isNewApply && status_.isSetupAp) {
        // Keep Setup AP reachable during connection attempt
        driver_->setMode(3); // WIFI_AP_STA
    } else {
        driver_->setMode(1); // WIFI_STA
    }

    stateDeadlineMs_ = nowMs + STA_CONNECT_TIMEOUT_MS;
    status_.state = isNewApply ? WifiState::ConnectingNewSta : WifiState::ConnectingSta;
    driver_->connectSta(ssid, password);
}

void WifiSettingsManager::enterRestoringLastWorking(uint32_t nowMs) {
    status_.state = WifiState::RestoringLastWorking;
    if (lastWorkingSsid_[0] != '\0') {
        enterConnectingSta(lastWorkingSsid_, lastWorkingPass_, nowMs, false);
    } else if (fallbackSsid_[0] != '\0') {
        enterConnectingSta(fallbackSsid_, fallbackPass_, nowMs, false);
    } else {
        enterSetupAp(nowMs);
    }
}

void WifiSettingsManager::init(const char* fallbackSsid, const char* fallbackPass) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fallbackSsid) {
        std::strncpy(fallbackSsid_, fallbackSsid, sizeof(fallbackSsid_) - 1);
        fallbackSsid_[sizeof(fallbackSsid_) - 1] = '\0';
    }
    if (fallbackPass) {
        std::strncpy(fallbackPass_, fallbackPass, sizeof(fallbackPass_) - 1);
        fallbackPass_[sizeof(fallbackPass_) - 1] = '\0';
    }

    generatePin();
    generateApCredentials();

    uint32_t now = clock_ ? clock_->nowMs() : 0;

    // Check dedicated NVS record first
    char nvsSsid[33] = {0};
    char nvsPass[65] = {0};
    if (nvs_->loadCredentials(nvsSsid, sizeof(nvsSsid), nvsPass, sizeof(nvsPass))) {
        enterConnectingSta(nvsSsid, nvsPass, now, false);
    } else if (fallbackSsid_[0] != '\0') {
        // Fall back to compiled credentials
        enterConnectingSta(fallbackSsid_, fallbackPass_, now, false);
    } else {
        // No stored or compiled credentials: enter Setup AP directly
        enterSetupAp(now);
    }
}

void WifiSettingsManager::update(uint32_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. Consume any pending apply staged by HTTP handler in owner loop
    if (pendingApply_.pending) {
        std::strncpy(candidateSsid_, pendingApply_.ssid, sizeof(candidateSsid_) - 1);
        candidateSsid_[sizeof(candidateSsid_) - 1] = '\0';
        std::strncpy(candidatePass_, pendingApply_.pass, sizeof(candidatePass_) - 1);
        candidatePass_[sizeof(candidatePass_) - 1] = '\0';
        pendingApply_.pending = false;

        enterConnectingSta(candidateSsid_, candidatePass_, nowMs, true);
        return;
    }

    if (status_.isSetupAp) {
        if (setupApDeadlineMs_ > nowMs) {
            status_.setupRemainingSec = (setupApDeadlineMs_ - nowMs + 999) / 1000;
        } else {
            status_.setupRemainingSec = 0;
            // Setup AP expired: re-attempt connection to last working config if available
            if (lastWorkingSsid_[0] != '\0' || fallbackSsid_[0] != '\0') {
                enterRestoringLastWorking(nowMs);
                return;
            } else {
                // Extend AP deadline if no network to restore
                setupApDeadlineMs_ = nowMs + SETUP_AP_TIMEOUT_MS;
            }
        }
    }

    switch (status_.state) {
        case WifiState::ConnectingSta:
        case WifiState::ConnectingNewSta: {
            if (driver_->isStaConnected()) {
                // Success: acquire DHCP IP
                const char* ip = driver_->getStaIp();
                if (ip) {
                    std::strncpy(status_.staIp, ip, sizeof(status_.staIp) - 1);
                    status_.staIp[sizeof(status_.staIp) - 1] = '\0';
                }
                status_.state = WifiState::ConnectedSta;
                status_.lastError[0] = '\0';

                // Save only successful new connection with DHCP to dedicated NVS
                if (isNewApply_) {
                    bool saved = nvs_->saveCredentials(candidateSsid_, candidatePass_);
                    if (!saved) {
                        // Crucial: Handle save failure as failure and restore previous working config!
                        std::strncpy(status_.lastError, "NVS_SAVE_FAILED", sizeof(status_.lastError) - 1);
                        isNewApply_ = false;
                        enterRestoringLastWorking(nowMs);
                        return;
                    }

                    std::strncpy(lastWorkingSsid_, candidateSsid_, sizeof(lastWorkingSsid_) - 1);
                    std::strncpy(lastWorkingPass_, candidatePass_, sizeof(lastWorkingPass_) - 1);
                    candidateSsid_[0] = '\0';
                    candidatePass_[0] = '\0';
                    isNewApply_ = false;

                    // Disable Setup AP once new STA confirmed
                    if (status_.isSetupAp) {
                        driver_->stopAp();
                        status_.isSetupAp = false;
                        status_.setupRemainingSec = 0;
                        driver_->setMode(1); // WIFI_STA only
                    }
                } else {
                    // Update last working baseline
                    std::strncpy(lastWorkingSsid_, status_.currentSsid, sizeof(lastWorkingSsid_) - 1);
                }
            } else if (nowMs >= stateDeadlineMs_) {
                // Connection attempt timed out
                if (isNewApply_) {
                    std::strncpy(status_.lastError, "CONNECTION_TIMEOUT", sizeof(status_.lastError) - 1);
                    isNewApply_ = false;
                    // Preserve last working config on failure; restore it
                    enterRestoringLastWorking(nowMs);
                } else {
                    // Boot connection failed: start time-bounded WPA2 Setup AP
                    status_.state = WifiState::StaFailed;
                    std::strncpy(status_.lastError, "BOOT_CONNECT_FAILED", sizeof(status_.lastError) - 1);
                    enterSetupAp(nowMs);
                }
            }
            break;
        }

        case WifiState::ConnectedSta: {
            if (!driver_->isStaConnected()) {
                // Lost STA connection
                status_.state = WifiState::StaFailed;
                std::strncpy(status_.lastError, "DISCONNECTED", sizeof(status_.lastError) - 1);
                // Attempt automatic reconnect or setup AP
                enterRestoringLastWorking(nowMs);
            }
            break;
        }

        default:
            break;
    }
}

WifiStatus WifiSettingsManager::getStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool WifiSettingsManager::isSetupApActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_.isSetupAp;
}

bool WifiSettingsManager::isSetupOrApplyActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_.isSetupAp ||
           status_.state == WifiState::ConnectingNewSta ||
           status_.state == WifiState::ConnectingSta ||
           status_.state == WifiState::RestoringLastWorking ||
           pendingApply_.pending;
}

bool WifiSettingsManager::verifyPin(const char* candidatePin) const {
    if (!candidatePin) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return (std::strcmp(candidatePin, pin_) == 0);
}

const char* WifiSettingsManager::getTftPin() const {
    return pin_;
}

const char* WifiSettingsManager::getTftApPassword() const {
    return apPassword_;
}

bool WifiSettingsManager::stagePendingApply(const char* ssid, const char* password) {
    if (!ssid || ssid[0] == '\0' || std::strlen(ssid) > WIFI_MAX_SSID_LEN) {
        return false;
    }
    if (password && std::strlen(password) > WIFI_MAX_PASS_LEN) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::strncpy(pendingApply_.ssid, ssid, sizeof(pendingApply_.ssid) - 1);
    pendingApply_.ssid[sizeof(pendingApply_.ssid) - 1] = '\0';
    std::strncpy(pendingApply_.pass, password ? password : "", sizeof(pendingApply_.pass) - 1);
    pendingApply_.pass[sizeof(pendingApply_.pass) - 1] = '\0';
    pendingApply_.pending = true;
    return true;
}

size_t WifiSettingsManager::buildWifiStatusJson(const WifiStatus& status, char* outBuf, size_t maxLen) {
    if (!outBuf || maxLen == 0) return 0;

    int written = std::snprintf(outBuf, maxLen,
        "{"
        "\"state\":\"%s\","
        "\"ssid\":\"%s\","
        "\"ip\":\"%s\","
        "\"ap_active\":%s,"
        "\"ap_ssid\":\"%s\","
        "\"ap_ip\":\"%s\","
        "\"setup_remaining_sec\":%u,"
        "\"last_error\":\"%s\""
        "}",
        getWifiStateName(status.state),
        status.currentSsid[0] ? status.currentSsid : "",
        status.staIp[0] ? status.staIp : "0.0.0.0",
        status.isSetupAp ? "true" : "false",
        status.apSsid[0] ? status.apSsid : "",
        status.apIp[0] ? status.apIp : "0.0.0.0",
        static_cast<unsigned int>(status.setupRemainingSec),
        status.lastError[0] ? status.lastError : ""
    );

    if (written < 0) return 0;
    return (static_cast<size_t>(written) < maxLen) ? static_cast<size_t>(written) : (maxLen - 1);
}

bool WifiSettingsManager::parseApplyJson(const char* json,
                                        char* outSsid, size_t maxSsid,
                                        char* outPass, size_t maxPass,
                                        char* outPin, size_t maxPin) {
    if (!json || !outSsid || !outPass || !outPin) return false;
    outSsid[0] = '\0';
    outPass[0] = '\0';
    outPin[0] = '\0';

    // Extract key values with bounded string extraction
    auto extractKey = [](const char* src, const char* key, char* dst, size_t maxDst) -> bool {
        const char* p = std::strstr(src, key);
        if (!p) return false;
        p += std::strlen(key);
        // Skip whitespace and colon
        while (*p && (*p == ' ' || *p == '\t' || *p == ':' || *p == '\"')) {
            if (*p == '\"') { p++; break; }
            p++;
        }
        size_t idx = 0;
        while (*p && *p != '\"' && *p != ',' && *p != '}' && idx < (maxDst - 1)) {
            dst[idx++] = *p++;
        }
        dst[idx] = '\0';
        return (idx > 0);
    };

    extractKey(json, "\"ssid\"", outSsid, maxSsid);
    extractKey(json, "\"password\"", outPass, maxPass);
    extractKey(json, "\"pin\"", outPin, maxPin);

    return (outSsid[0] != '\0' && outPin[0] != '\0');
}

const char* WifiSettingsManager::getWifiHtml() {
    return WIFI_HTML;
}

WifiSettingsManager& getWifiSettingsManager() {
    return s_wifiMgr;
}

bool isSetupApActive() {
    return s_wifiMgr.isSetupApActive();
}

bool isSetupOrApplyActive() {
    return s_wifiMgr.isSetupOrApplyActive();
}

bool isHidCommandAllowed(bool isFromApInterface) {
    // If incoming request is on Setup AP interface or Setup AP is active, block HID commands
    if (isFromApInterface || isSetupApActive() || isSetupOrApplyActive()) {
        return false;
    }
    return true;
}

} // namespace wifi
} // namespace paxg
