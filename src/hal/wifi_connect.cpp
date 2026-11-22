#include <TFT_eSPI.h>
#include <WiFi.h>
#include <modules/constants.h>
#include <modules/sprites.h>
#include <modules/utils.h>
#include <modules/variables.h>
#include <modules/wifi_connect.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

namespace
{

constexpr int WIFI_SCAN_PASSES = 3;
constexpr int STATUS_TEXT_X = 20;
constexpr int STATUS_TEXT_Y = 20;
constexpr unsigned long KEYBOARD_CONFIRM_DELAY_MS = 500UL;
constexpr int WIFI_CONNECT_ATTEMPTS = 20;
constexpr unsigned long WIFI_CONNECT_RETRY_DELAY_MS = 1000UL;
constexpr unsigned long MISSING_CREDENTIALS_MESSAGE_DELAY_MS = 1500UL;
constexpr unsigned long FAILURE_DIAGNOSTIC_DELAY_MS = 6000UL;

const std::vector<String> BOOT_MODES = {"Home", "Standard"};

std::atomic<bool> s_wifi_events_registered{false};
std::atomic<bool> s_sta_connected{false};
std::atomic<bool> s_sta_currently_connected{false};
std::atomic<bool> s_sta_got_ip{false};
std::atomic<bool> s_sta_disconnected{false};
std::atomic<int> s_disconnect_reason{-1};
std::atomic<int8_t> s_last_rssi{0};
std::atomic<uint32_t> s_sta_connected_count{0};
std::atomic<uint32_t> s_sta_disconnected_count{0};

void handle_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info)
{
    switch (event)
    {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            s_sta_connected.store(true, std::memory_order_relaxed);
            s_sta_currently_connected.store(true, std::memory_order_relaxed);
            s_sta_connected_count.fetch_add(1, std::memory_order_relaxed);
            break;

        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            s_sta_got_ip.store(true, std::memory_order_relaxed);
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        {
            s_sta_disconnected.store(true, std::memory_order_relaxed);
            s_sta_currently_connected.store(false, std::memory_order_relaxed);
            s_sta_disconnected_count.fetch_add(1, std::memory_order_relaxed);
            s_disconnect_reason.store(static_cast<int>(info.wifi_sta_disconnected.reason), std::memory_order_relaxed);
            s_last_rssi.store(info.wifi_sta_disconnected.rssi, std::memory_order_relaxed);
            break;
        }

        default:
            break;
    }
}

void register_wifi_events_once()
{
    bool expected = false;
    if (s_wifi_events_registered.compare_exchange_strong(expected, true))
    {
        WiFi.onEvent(handle_wifi_event);
    }
}

void reset_attempt_diagnostics()
{
    s_sta_connected.store(false, std::memory_order_relaxed);
    s_sta_currently_connected.store(false, std::memory_order_relaxed);
    s_sta_got_ip.store(false, std::memory_order_relaxed);
    s_sta_disconnected.store(false, std::memory_order_relaxed);
    s_disconnect_reason.store(-1, std::memory_order_relaxed);
    s_last_rssi.store(0, std::memory_order_relaxed);
    s_sta_connected_count.store(0, std::memory_order_relaxed);
    s_sta_disconnected_count.store(0, std::memory_order_relaxed);
}

const char* wifi_status_to_string(wl_status_t status)
{
    switch (status)
    {
        case WL_NO_SHIELD: return "WL_NO_SHIELD";
        case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
        case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
        case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
        case WL_CONNECTED: return "WL_CONNECTED";
        case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
        case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
        case WL_DISCONNECTED: return "WL_DISCONNECTED";
        default: return "WL_UNKNOWN";
    }
}

bool is_auth_failure_reason(int reason)
{
    switch (reason)
    {
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_NOT_AUTHED:
        case WIFI_REASON_ASSOC_NOT_AUTHED:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_802_1X_AUTH_FAILED:
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        case WIFI_REASON_AKMP_INVALID:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return true;
        default:
            return false;
    }
}

void show_status_screen(const String& message)
{
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(STATUS_TEXT_X, STATUS_TEXT_Y);
    tft.print(message);
}

void show_failure_diagnostics(const char* mode_name, const char* action_message)
{
    wl_status_t status = WiFi.status();
    bool connected = s_sta_connected.load(std::memory_order_relaxed);
    bool currently_connected = s_sta_currently_connected.load(std::memory_order_relaxed);
    bool got_ip = s_sta_got_ip.load(std::memory_order_relaxed);
    int reason = s_disconnect_reason.load(std::memory_order_relaxed);
    int8_t last_rssi = s_last_rssi.load(std::memory_order_relaxed);
    uint32_t conn_count = s_sta_connected_count.load(std::memory_order_relaxed);
    uint32_t disc_count = s_sta_disconnected_count.load(std::memory_order_relaxed);

    const char* status_str = wifi_status_to_string(status);
    const char* reason_name = "unknown";
    if (reason >= 0)
    {
        const char* name = WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(reason));
        if (name != nullptr && std::strlen(name) > 0)
        {
            reason_name = name;
        }
        else
        {
            reason_name = "UNRECOGNIZED";
        }
    }

    const char* stage_str = "Unknown stage";
    const char* diag_str = "Unknown failure";

    if (currently_connected && !got_ip)
    {
        stage_str = "Connected to AP (waiting IP)";
        diag_str = "DHCP pending/timeout (no IP)";
    }
    else if (disc_count > 0 && is_auth_failure_reason(reason))
    {
        stage_str = "AP auth / key handshake";
        diag_str = "Auth or handshake failed";
    }
    else if (disc_count > 0 && reason == WIFI_REASON_NO_AP_FOUND)
    {
        stage_str = "AP scan / probe";
        diag_str = "AP not found (SSID/band mismatch)";
    }
    else if (disc_count > 0 && reason == WIFI_REASON_BEACON_TIMEOUT)
    {
        stage_str = "AP beacon monitoring";
        diag_str = "Beacon timeout (signal lost)";
    }
    else if (disc_count > 0 && connected)
    {
        stage_str = "Associated to AP then dropped";
        diag_str = reason_name;
    }
    else if (disc_count > 0)
    {
        stage_str = "AP association stage";
        diag_str = reason_name;
    }
    else if (!connected && disc_count == 0)
    {
        stage_str = "No AP event recorded";
        diag_str = "Connection timed out (unknown)";
    }

    // Serial diagnostics without credentials
    Serial.printf("[WiFi] Failure in %s mode\n", mode_name);
    Serial.printf("[WiFi]   WiFi.status(): %d (%s)\n", static_cast<int>(status), status_str);
    if (reason >= 0)
    {
        Serial.printf("[WiFi]   ESP disconnect reason: %d (%s)\n", reason, reason_name);
    }
    else
    {
        Serial.println("[WiFi]   ESP disconnect reason: unknown (none recorded)");
    }
    Serial.printf("[WiFi]   Events: conn=%u, disc=%u, got_ip=%d, rssi=%d dBm\n",
                  static_cast<unsigned>(conn_count),
                  static_cast<unsigned>(disc_count),
                  got_ip ? 1 : 0,
                  static_cast<int>(last_rssi));
    Serial.printf("[WiFi]   Stage: %s\n", stage_str);
    Serial.printf("[WiFi]   Diagnosis: %s\n", diag_str);

    // TFT LCD 320x170 layout
    tft.fillScreen(TFT_BLACK);

    tft.setTextSize(2);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(10, 8);
    tft.printf("WiFi Failed: %s", mode_name);

    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    tft.setCursor(10, 32);
    tft.printf("WiFi.status: %d (%s)", static_cast<int>(status), status_str);

    tft.setCursor(10, 48);
    if (reason >= 0)
    {
        tft.printf("ESP reason:  %d (%s)", reason, reason_name);
    }
    else
    {
        tft.print("ESP reason:  unknown (none recorded)");
    }

    tft.setCursor(10, 64);
    if (disc_count > 0 && last_rssi != 0)
    {
        tft.printf("Events:      conn=%u disc=%u rssi=%d dBm",
                   static_cast<unsigned>(conn_count),
                   static_cast<unsigned>(disc_count),
                   static_cast<int>(last_rssi));
    }
    else
    {
        tft.printf("Events:      conn=%u disc=%u got_ip=%d",
                   static_cast<unsigned>(conn_count),
                   static_cast<unsigned>(disc_count),
                   got_ip ? 1 : 0);
    }

    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(10, 80);
    tft.printf("Stage:       %s", stage_str);

    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(10, 96);
    tft.printf("Diagnosis:   %s", diag_str);

    tft.setTextColor(TFT_SILVER, TFT_BLACK);
    tft.setCursor(10, 116);
    tft.print(action_message);

    tft.setCursor(10, 132);
    tft.print("Diagnostic hold: 6s (readable)");

    // Restore default text styling
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    delay(FAILURE_DIAGNOSTIC_DELAY_MS);
}

}  // namespace

std::vector<String> get_available_wifis()
{
    std::vector<String> result;

    for (int i = 0; i < WIFI_SCAN_PASSES; i++)
    {
        tft.print(".");
        int numNetworks = WiFi.scanNetworks();

        for (int i = 0; i < numNetworks; i++)
        {
            String wifiName = WiFi.SSID(i);
            if (std::find(result.begin(), result.end(), wifiName) == result.end())
            {
                result.push_back(wifiName);
            }
        }
    }

    return result;
}

String choose_wifi()
{
    show_status_screen("Loading networks");

    const std::vector<String> wifis = get_available_wifis();
    if (wifis.empty())
    {
        show_status_screen("No WiFi networks found");
        delay(MISSING_CREDENTIALS_MESSAGE_DELAY_MS);
        return "";
    }

    unsigned selected = row_choice(wifis);
    if (selected >= wifis.size())
    {
        selected = 0;
    }
    return wifis[selected];
}

void user_input_wifi()
{
    while (true)
    {
        String ssid = choose_wifi();
        if (ssid.isEmpty())
        {
            continue;
        }

        String password = "";
        if (!keyboard_input(password))
        {
            show_status_screen("Input cancelled / timed out");
            delay(MISSING_CREDENTIALS_MESSAGE_DELAY_MS);
            continue;
        }
        delay(KEYBOARD_CONFIRM_DELAY_MS);

        register_wifi_events_once();
        reset_attempt_diagnostics();

        WiFi.begin(ssid, password);
        show_status_screen("Connecting to WiFi");

        for (int i = 0; WiFi.status() != WL_CONNECTED && i < WIFI_CONNECT_ATTEMPTS; ++i)
        {
            delay(WIFI_CONNECT_RETRY_DELAY_MS);
            tft.print(".");
        }

        if (WiFi.status() == WL_CONNECTED)
        {
            tft.fillScreen(TFT_BLACK);
            return;
        }
        show_failure_diagnostics("Standard", "Retrying network selection...");
    }
}

void fast_connect_wifi()
{
    show_status_screen("");

    if (std::strlen(WIFI_SSID) == 0 || std::strlen(WIFI_PASSWORD) == 0)
    {
        tft.print("No .env WiFi credentials found");
        delay(MISSING_CREDENTIALS_MESSAGE_DELAY_MS);
        user_input_wifi();
        return;
    }

    register_wifi_events_once();
    reset_attempt_diagnostics();

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    show_status_screen("Connecting to WiFi");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < WIFI_CONNECT_ATTEMPTS)
    {
        delay(WIFI_CONNECT_RETRY_DELAY_MS);
        tft.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        tft.fillScreen(TFT_BLACK);
    }
    else
    {
        show_failure_diagnostics("Home", "Falling back to manual setup...");
        user_input_wifi();
    }
}

void connect_wifi()
{
    int selected = row_choice(BOOT_MODES);

    switch (selected)
    {
        case 0:
            fast_connect_wifi();
            break;
        case 1:
            user_input_wifi();
            break;
    }
    tft.fillScreen(TFT_BLACK);
}
