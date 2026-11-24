#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>

namespace paxg {
namespace wifi {

/**
 * @brief Discrete operational states of the Wi-Fi lifecycle & recovery manager.
 */
enum class WifiState : uint8_t {
    Uninitialized = 0,
    ConnectingSta,          // Connecting to stored NVS or compiled fallback network
    ConnectedSta,           // Successfully connected to STA with valid DHCP IP
    StaFailed,              // STA connection failed or timed out
    SetupApActive,          // Time-bounded WPA2 Setup AP active (device-generated password on TFT)
    ConnectingNewSta,       // Attempting new STA credentials while keeping Setup AP reachable
    RestoringLastWorking    // New credentials failed/timed out; restoring previous working config
};

const char* getWifiStateName(WifiState state);

/**
 * @brief Thread-safe Wi-Fi telemetry snapshot (strictly contains NO passwords or PIN).
 */
struct WifiStatus {
    WifiState state = WifiState::Uninitialized;
    char currentSsid[33] = {0};
    char staIp[32] = {0};
    char apSsid[33] = {0};
    char apIp[32] = {0};
    uint32_t setupRemainingSec = 0;
    bool isSetupAp = false;
    char lastError[32] = {0};
};

/**
 * @brief Result of applying new Wi-Fi credentials.
 */
struct ApplyResult {
    bool success = false;
    char reason[32] = {0};
};

/**
 * @brief Hardware Wi-Fi abstraction interface for isolation and host testing.
 */
class IWifiDriver {
public:
    virtual ~IWifiDriver() = default;
    virtual void setMode(uint8_t mode) = 0; // 0=OFF, 1=STA, 2=AP, 3=AP_STA
    virtual void connectSta(const char* ssid, const char* password) = 0;
    virtual void startAp(const char* ssid, const char* password, const char* ip) = 0;
    virtual void stopAp() = 0;
    virtual bool isStaConnected() = 0;
    virtual const char* getStaIp() = 0;
    virtual void disconnectSta() = 0;
};

/**
 * @brief NVS storage abstraction interface for isolated unit testing.
 */
class INvsStorage {
public:
    virtual ~INvsStorage() = default;
    virtual bool loadCredentials(char* outSsid, size_t maxSsid, char* outPass, size_t maxPass) = 0;
    virtual bool saveCredentials(const char* ssid, const char* password) = 0;
    virtual bool clearCredentials() = 0;
};

/**
 * @brief Monotonic clock interface.
 */
class IClock {
public:
    virtual ~IClock() = default;
    virtual uint32_t nowMs() = 0;
};

/**
 * @brief Bounded Wi-Fi Settings & Recovery Manager.
 */
class WifiSettingsManager {
public:
    static constexpr uint32_t STA_CONNECT_TIMEOUT_MS = 15000;    // 15 seconds STA connection timeout
    static constexpr uint32_t SETUP_AP_TIMEOUT_MS = 300000;       // 5 minutes (300s) time-bounded Setup AP
    static constexpr size_t WIFI_MAX_SSID_LEN = 32;
    static constexpr size_t WIFI_MAX_PASS_LEN = 64;
    static constexpr size_t WIFI_PIN_LEN = 4;

    WifiSettingsManager(IWifiDriver* driver = nullptr,
                        INvsStorage* nvs = nullptr,
                        IClock* clock = nullptr);

    /**
     * @brief Initialize manager with optional compiled fallback credentials.
     */
    void init(const char* fallbackSsid = nullptr, const char* fallbackPass = nullptr);

    /**
     * @brief Periodic non-blocking state machine update (runs strictly in owner loop).
     */
    void update(uint32_t nowMs);

    /**
     * @brief Retrieve thread-safe synchronized public status snapshot (never leaks secrets).
     */
    WifiStatus getStatus() const;

    /**
     * @brief Check if Setup AP is currently active.
     */
    bool isSetupApActive() const;

    /**
     * @brief Check if Setup AP or connection attempt is in progress (prevents start).
     */
    bool isSetupOrApplyActive() const;

    /**
     * @brief Validate candidate PIN against device-local PIN (constant-time check).
     */
    bool verifyPin(const char* candidatePin) const;

    /**
     * @brief Retrieve device-local PIN strictly for display on local physical S3 TFT.
     */
    const char* getTftPin() const;

    /**
     * @brief Retrieve device-generated AP password strictly for display on local physical S3 TFT.
     */
    const char* getTftApPassword() const;

    /**
     * @brief Thread-safe stage of a validated pending apply request to be executed by owner loop.
     */
    bool stagePendingApply(const char* ssid, const char* password);

    /**
     * @brief Build JSON status response (strictly omits all passwords and PIN).
     */
    static size_t buildWifiStatusJson(const WifiStatus& status, char* outBuf, size_t maxLen);

    /**
     * @brief Bounded JSON extractor for POST /api/wifi/apply payload.
     */
    static bool parseApplyJson(const char* json,
                               char* outSsid, size_t maxSsid,
                               char* outPass, size_t maxPass,
                               char* outPin, size_t maxPin);

    /**
     * @brief Returns HTML content for /wifi settings page.
     */
    static const char* getWifiHtml();

private:
    void generatePin();
    void generateApCredentials();
    void enterSetupAp(uint32_t nowMs);
    void enterConnectingSta(const char* ssid, const char* password, uint32_t nowMs, bool isNewApply);
    void enterRestoringLastWorking(uint32_t nowMs);

    mutable std::mutex mutex_;

    IWifiDriver* driver_ = nullptr;
    INvsStorage* nvs_ = nullptr;
    IClock* clock_ = nullptr;

    char fallbackSsid_[33] = {0};
    char fallbackPass_[65] = {0};

    char lastWorkingSsid_[33] = {0};
    char lastWorkingPass_[65] = {0};

    char candidateSsid_[33] = {0};
    char candidatePass_[65] = {0};

    char pin_[9] = {0};
    char apPassword_[17] = {0};

    struct PendingApply {
        bool pending = false;
        char ssid[33] = {0};
        char pass[65] = {0};
    } pendingApply_;

    WifiStatus status_;
    uint32_t stateDeadlineMs_ = 0;
    uint32_t setupApDeadlineMs_ = 0;
    bool isNewApply_ = false;
};

/**
 * @brief Singleton accessor for the global Wi-Fi settings manager.
 */
WifiSettingsManager& getWifiSettingsManager();

/**
 * @brief Global check if Setup AP is active.
 */
bool isSetupApActive();

/**
 * @brief Global check if Setup AP or apply is active.
 */
bool isSetupOrApplyActive();

/**
 * @brief Helper to determine if incoming HTTP request is allowed to issue HID commands.
 */
bool isHidCommandAllowed(bool isFromApInterface);

} // namespace wifi
} // namespace paxg
