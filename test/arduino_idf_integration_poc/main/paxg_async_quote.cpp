#include "paxg_async_quote.h"

#include <cstring>
#include <cstdlib>
#include <mutex>
#include <cctype>
#include <cmath>

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace paxg {
namespace market {

namespace {

std::mutex s_quoteMutex;
double s_price = 0.0;
double s_percentChange = 0.0;
uint32_t s_lastQuoteTimeMs = 0;
QuoteStatus s_quoteStatus = QuoteStatus::Unavailable;
size_t s_historyCount = 0;
double s_priceHistory[MAX_PRICE_HISTORY] = {0.0};

#if defined(ARDUINO) || defined(ESP_PLATFORM)
TaskHandle_t s_quoteTaskHandle = nullptr;
constexpr const char* BINANCE_PAXG_ENDPOINT = "https://api.binance.com/api/v3/ticker/24hr?symbol=PAXGUSDT";
constexpr uint32_t HTTP_TIMEOUT_MS = 5000;
constexpr uint32_t POLL_INTERVAL_SECONDS = 60;
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 15000;

// DigiCert Global Root G2 certificate for api.binance.com TLS authentication
const char* const BINANCE_ROOT_CA_PEM =
"-----BEGIN CERTIFICATE-----\n"
"MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
"MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
"d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
"MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
"MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
"b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
"9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
"2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
"1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
"q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
"tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
"vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
"BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
"5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
"1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
"NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
"Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
"8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
"pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
"MrY=\n"
"-----END CERTIFICATE-----\n";
#endif

// Lightweight, zero-allocation string span compatible across C++11 through C++23
struct JsonSpan {
    const char* ptr;
    size_t len;

    JsonSpan() : ptr(nullptr), len(0) {}
    JsonSpan(const char* p, size_t l) : ptr(p), len(l) {}

    const char* data() const { return ptr; }
    size_t size() const { return len; }

    bool equals(const char* s) const {
        if (!s || !ptr) return false;
        size_t sLen = std::strlen(s);
        return (len == sLen && std::strncmp(ptr, s, len) == 0);
    }
    bool operator==(const char* s) const { return equals(s); }
    bool operator!=(const char* s) const { return !equals(s); }
};

// Bounded strict recursive-descent JSON parser with zero dynamic allocation
class StrictJsonParser {
public:
    explicit StrictJsonParser(const char* json)
        : ptr_(json), end_(json ? json + std::strlen(json) : nullptr) {}

    bool skipWhitespace() {
        while (ptr_ < end_ && std::isspace(static_cast<unsigned char>(*ptr_))) {
            ptr_++;
        }
        return ptr_ < end_;
    }

    size_t remaining() const {
        return (ptr_ < end_) ? static_cast<size_t>(end_ - ptr_) : 0;
    }

    const char* current() const {
        return ptr_;
    }

    bool consumeChar(char expected) {
        if (ptr_ < end_ && *ptr_ == expected) {
            ptr_++;
            return true;
        }
        return false;
    }

    bool parseString(JsonSpan& outStr) {
        if (ptr_ >= end_ || *ptr_ != '"') return false;
        ptr_++; // skip opening quote
        const char* start = ptr_;
        while (ptr_ < end_) {
            char c = *ptr_;
            if (c == '"') {
                outStr = JsonSpan{start, static_cast<size_t>(ptr_ - start)};
                ptr_++; // skip closing quote
                return true;
            }
            if (c == '\\') {
                ptr_++;
                if (ptr_ >= end_) return false;
                char esc = *ptr_;
                if (esc == '"' || esc == '\\' || esc == '/' || esc == 'b' ||
                    esc == 'f' || esc == 'n' || esc == 'r' || esc == 't') {
                    ptr_++;
                } else if (esc == 'u') {
                    ptr_++;
                    for (int i = 0; i < 4; ++i) {
                        if (ptr_ >= end_ || !std::isxdigit(static_cast<unsigned char>(*ptr_))) {
                            return false;
                        }
                        ptr_++;
                    }
                } else {
                    return false; // invalid escape sequence
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                // unescaped control character in JSON string is illegal
                return false;
            } else {
                ptr_++;
            }
        }
        return false; // unclosed string
    }

    bool parseNumber(JsonSpan& outNum) {
        if (ptr_ >= end_) return false;
        const char* start = ptr_;
        if (*ptr_ == '-') ptr_++;
        if (ptr_ >= end_) return false;
        if (*ptr_ == '0') {
            ptr_++;
        } else if (*ptr_ >= '1' && *ptr_ <= '9') {
            while (ptr_ < end_ && std::isdigit(static_cast<unsigned char>(*ptr_))) {
                ptr_++;
            }
        } else {
            return false;
        }

        // fraction
        if (ptr_ < end_ && *ptr_ == '.') {
            ptr_++;
            if (ptr_ >= end_ || !std::isdigit(static_cast<unsigned char>(*ptr_))) {
                return false;
            }
            while (ptr_ < end_ && std::isdigit(static_cast<unsigned char>(*ptr_))) {
                ptr_++;
            }
        }

        // exponent
        if (ptr_ < end_ && (*ptr_ == 'e' || *ptr_ == 'E')) {
            ptr_++;
            if (ptr_ < end_ && (*ptr_ == '+' || *ptr_ == '-')) {
                ptr_++;
            }
            if (ptr_ >= end_ || !std::isdigit(static_cast<unsigned char>(*ptr_))) {
                return false;
            }
            while (ptr_ < end_ && std::isdigit(static_cast<unsigned char>(*ptr_))) {
                ptr_++;
            }
        }

        outNum = JsonSpan{start, static_cast<size_t>(ptr_ - start)};
        return true;
    }

    bool parseValue(int depth, JsonSpan* outVal = nullptr) {
        if (depth > 16) return false; // nesting recursion limit
        skipWhitespace();
        if (ptr_ >= end_) return false;

        char c = *ptr_;
        if (c == '"') {
            JsonSpan s;
            if (!parseString(s)) return false;
            if (outVal) *outVal = s;
            return true;
        }
        if (c == '{') {
            return parseObject(depth + 1);
        }
        if (c == '[') {
            return parseArray(depth + 1);
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            JsonSpan n;
            if (!parseNumber(n)) return false;
            if (outVal) *outVal = n;
            return true;
        }
        if (c == 't') {
            if (end_ - ptr_ >= 4 && std::strncmp(ptr_, "true", 4) == 0) {
                ptr_ += 4;
                return true;
            }
            return false;
        }
        if (c == 'f') {
            if (end_ - ptr_ >= 5 && std::strncmp(ptr_, "false", 5) == 0) {
                ptr_ += 5;
                return true;
            }
            return false;
        }
        if (c == 'n') {
            if (end_ - ptr_ >= 4 && std::strncmp(ptr_, "null", 4) == 0) {
                ptr_ += 4;
                return true;
            }
            return false;
        }
        return false;
    }

    bool parseArray(int depth) {
        if (!consumeChar('[')) return false;
        skipWhitespace();
        if (ptr_ < end_ && *ptr_ == ']') {
            ptr_++;
            return true;
        }
        while (true) {
            if (!parseValue(depth)) return false;
            skipWhitespace();
            if (ptr_ >= end_) return false;
            if (*ptr_ == ']') {
                ptr_++;
                return true;
            }
            if (*ptr_ == ',') {
                ptr_++;
                skipWhitespace();
                if (ptr_ < end_ && *ptr_ == ']') return false; // trailing comma rejected
            } else {
                return false;
            }
        }
    }

    bool parseObject(int depth) {
        if (!consumeChar('{')) return false;
        skipWhitespace();
        if (ptr_ < end_ && *ptr_ == '}') {
            ptr_++;
            return true;
        }
        while (true) {
            skipWhitespace();
            JsonSpan k;
            if (!parseString(k)) return false;
            skipWhitespace();
            if (!consumeChar(':')) return false;
            skipWhitespace();
            if (!parseValue(depth)) return false;
            skipWhitespace();
            if (ptr_ >= end_) return false;
            if (*ptr_ == '}') {
                ptr_++;
                return true;
            }
            if (*ptr_ == ',') {
                ptr_++;
                skipWhitespace();
                if (ptr_ < end_ && *ptr_ == '}') return false; // trailing comma rejected
            } else {
                return false;
            }
        }
    }

private:
    const char* ptr_;
    const char* end_;
};

bool isValidNumericToken(const char* str, size_t len) {
    if (!str || len == 0 || len >= 32) return false;
    bool hasDigit = false;
    bool hasDot = false;
    bool hasExp = false;
    for (size_t i = 0; i < len; ++i) {
        char c = str[i];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            hasDigit = true;
        } else if (c == '.') {
            if (hasDot || hasExp) return false;
            hasDot = true;
        } else if (c == 'e' || c == 'E') {
            if (hasExp || !hasDigit) return false;
            hasExp = true;
            hasDigit = false;
        } else if (c == '-' || c == '+') {
            if (i != 0 && str[i - 1] != 'e' && str[i - 1] != 'E') return false;
        } else {
            return false;
        }
    }
    return hasDigit;
}

} // namespace

MarketSessionsState evaluateMarketSessions(int dayOfWeek, int hour, int minute, bool timeValid) {
    MarketSessionsState state;
    if (!timeValid) {
        return state; // All flags false when time unavailable
    }

    state.timeValid = true;

    // Weekday check: Mon=1 through Fri=5 (0=Sun, 6=Sat)
    if (dayOfWeek >= 1 && dayOfWeek <= 5) {
        const int minutes = hour * 60 + minute;

        // UK Session: 09:00 - 17:29 (540..1049)
        if (minutes >= 540 && minutes <= 1049) {
            state.uk = true;
        }
        // US Session: 15:30 - 21:59 (930..1319)
        if (minutes >= 930 && minutes <= 1319) {
            state.us = true;
        }
        // Japan Session: 01:00 - 07:59 (60..479)
        if (minutes >= 60 && minutes <= 479) {
            state.japan = true;
        }
        // Off-hours / Moon: outside all sessions
        if (minutes > 1319 || minutes < 60) {
            state.moon = true;
        }
    } else {
        // Weekend: market closed, moon active
        state.moon = true;
    }

    return state;
}

bool parseBinanceTicker(const char* payload, double& outPrice, double& outPercentChange) {
    if (!payload) {
        return false;
    }

    StrictJsonParser parser(payload);
    parser.skipWhitespace();
    if (parser.remaining() == 0 || *parser.current() != '{') {
        return false; // Top-level must be a JSON object
    }
    parser.consumeChar('{');
    parser.skipWhitespace();

    JsonSpan symbolVal;
    JsonSpan lastPriceVal;
    JsonSpan percentChangeVal;
    bool hasSymbol = false;
    bool hasLastPrice = false;
    bool hasPercentChange = false;

    if (parser.remaining() > 0 && *parser.current() == '}') {
        parser.consumeChar('}');
    } else {
        while (true) {
            parser.skipWhitespace();
            JsonSpan key;
            if (!parser.parseString(key)) return false;
            parser.skipWhitespace();
            if (!parser.consumeChar(':')) return false;
            parser.skipWhitespace();

            JsonSpan val;
            if (!parser.parseValue(1, &val)) return false;

            if (key == "symbol" || key == "s") {
                if (hasSymbol) return false; // Duplicate key rejected
                symbolVal = val;
                hasSymbol = true;
            } else if (key == "lastPrice" || key == "c") {
                if (hasLastPrice) return false; // Duplicate key rejected
                lastPriceVal = val;
                hasLastPrice = true;
            } else if (key == "priceChangePercent" || key == "P") {
                if (hasPercentChange) return false; // Duplicate key rejected
                percentChangeVal = val;
                hasPercentChange = true;
            }

            parser.skipWhitespace();
            if (parser.remaining() == 0) return false;
            if (*parser.current() == '}') {
                parser.consumeChar('}');
                break;
            }
            if (*parser.current() == ',') {
                parser.consumeChar(',');
                parser.skipWhitespace();
                if (parser.remaining() > 0 && *parser.current() == '}') {
                    return false; // Trailing comma rejected
                }
            } else {
                return false;
            }
        }
    }

    // Must be no trailing non-whitespace characters after object closing brace
    parser.skipWhitespace();
    if (parser.remaining() != 0) {
        return false;
    }

    // Required fields must all be present
    if (!hasSymbol || !hasLastPrice || !hasPercentChange) {
        return false;
    }

    // Symbol must be exactly PAXGUSDT
    if (symbolVal != "PAXGUSDT") {
        return false;
    }

    // Validate lastPrice numeric value
    if (!isValidNumericToken(lastPriceVal.data(), lastPriceVal.size())) {
        return false;
    }
    char buf[64];
    if (lastPriceVal.size() >= sizeof(buf)) return false;
    std::memcpy(buf, lastPriceVal.data(), lastPriceVal.size());
    buf[lastPriceVal.size()] = '\0';
    char* endptr = nullptr;
    double p = std::strtod(buf, &endptr);
    if (endptr != (buf + lastPriceVal.size()) || std::isnan(p) || !std::isfinite(p) || p <= 0.0) {
        return false;
    }

    // Validate priceChangePercent numeric value
    if (!isValidNumericToken(percentChangeVal.data(), percentChangeVal.size())) {
        return false;
    }
    if (percentChangeVal.size() >= sizeof(buf)) return false;
    std::memcpy(buf, percentChangeVal.data(), percentChangeVal.size());
    buf[percentChangeVal.size()] = '\0';
    endptr = nullptr;
    double pct = std::strtod(buf, &endptr);
    if (endptr != (buf + percentChangeVal.size()) || std::isnan(pct) || !std::isfinite(pct)) {
        return false;
    }

    outPrice = p;
    outPercentChange = pct;
    return true;
}

QuoteStatus evaluateQuoteFreshness(uint32_t lastQuoteTimeMs, uint32_t nowMs, QuoteStatus currentStatus, uint32_t maxAgeMs) {
    if (currentStatus == QuoteStatus::Unavailable) {
        return QuoteStatus::Unavailable;
    }
    if ((nowMs - lastQuoteTimeMs) > maxAgeMs) {
        return QuoteStatus::Stale;
    }
    return currentStatus;
}

MarketQuoteSnapshot getMarketQuoteSnapshot(uint32_t nowMs) {
    std::lock_guard<std::mutex> lock(s_quoteMutex);
    MarketQuoteSnapshot snap;
    snap.price = s_price;
    snap.percentChange = s_percentChange;
    snap.lastQuoteTimeMs = s_lastQuoteTimeMs;
    snap.status = evaluateQuoteFreshness(s_lastQuoteTimeMs, nowMs, s_quoteStatus, DEFAULT_MAX_QUOTE_AGE_MS);
    snap.historyCount = s_historyCount;
    for (size_t i = 0; i < s_historyCount && i < MAX_PRICE_HISTORY; ++i) {
        snap.priceHistory[i] = s_priceHistory[i];
    }
    return snap;
}

void setMarketQuote(double price, double percentChange, uint32_t quoteTimeMs, QuoteStatus status) {
    std::lock_guard<std::mutex> lock(s_quoteMutex);
    s_price = price;
    s_percentChange = percentChange;
    s_lastQuoteTimeMs = quoteTimeMs;
    s_quoteStatus = status;

    if (status == QuoteStatus::Fresh) {
        if (s_historyCount < MAX_PRICE_HISTORY) {
            s_priceHistory[s_historyCount++] = price;
        } else {
            for (size_t i = 1; i < MAX_PRICE_HISTORY; ++i) {
                s_priceHistory[i - 1] = s_priceHistory[i];
            }
            s_priceHistory[MAX_PRICE_HISTORY - 1] = price;
        }
    }
}

void markMarketQuoteFetchFailed() {
    std::lock_guard<std::mutex> lock(s_quoteMutex);
    if (s_quoteStatus == QuoteStatus::Fresh) {
        s_quoteStatus = QuoteStatus::Stale;
    }
}

void resetMarketQuoteState() {
    std::lock_guard<std::mutex> lock(s_quoteMutex);
    s_price = 0.0;
    s_percentChange = 0.0;
    s_lastQuoteTimeMs = 0;
    s_quoteStatus = QuoteStatus::Unavailable;
    s_historyCount = 0;
    for (size_t i = 0; i < MAX_PRICE_HISTORY; ++i) {
        s_priceHistory[i] = 0.0;
    }
}

#if defined(ARDUINO) || defined(ESP_PLATFORM)
static void paxgQuoteWorkerTask(void* pvParameters) {
    (void)pvParameters;

    // Strict NVS preservation: never overwrite or commit Wi-Fi credentials to NVS flash
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);

#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
    const char* ssid = WIFI_SSID;
    const char* pass = WIFI_PASSWORD;
    if (ssid != nullptr && std::strlen(ssid) > 0) {
        WiFi.begin(ssid, pass);
    } else {
        WiFi.begin();
    }
#else
    WiFi.begin();
#endif

    uint32_t lastReconnectAttemptMs = 0;

    while (true) {
        const uint32_t nowMs = millis();
        if (WiFi.status() == WL_CONNECTED) {
            NetworkClientSecure client;
            // Authenticated TLS using DigiCert Global Root G2 certificate
            client.setCACert(BINANCE_ROOT_CA_PEM);

            HTTPClient http;
            http.setConnectTimeout(HTTP_TIMEOUT_MS);
            http.setTimeout(HTTP_TIMEOUT_MS);

            if (http.begin(client, BINANCE_PAXG_ENDPOINT)) {
                const int code = http.GET();
                if (code == 200) {
                    String payload = http.getString();
                    double price = 0.0;
                    double percent = 0.0;
                    if (parseBinanceTicker(payload.c_str(), price, percent)) {
                        setMarketQuote(price, percent, millis(), QuoteStatus::Fresh);
                    } else {
                        markMarketQuoteFetchFailed();
                    }
                } else {
                    markMarketQuoteFetchFailed();
                }
                http.end();
            } else {
                markMarketQuoteFetchFailed();
            }
        } else {
            markMarketQuoteFetchFailed();
            // Throttled reconnect attempt to avoid blocking or RF saturation
            if (nowMs - lastReconnectAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
                lastReconnectAttemptMs = nowMs;
                WiFi.reconnect();
            }
        }

        // Bounded sleep: 60s total, checked in 1s ticks so task remains responsive
        for (uint32_t s = 0; s < POLL_INTERVAL_SECONDS; ++s) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void startPaxgQuoteWorker() {
    if (s_quoteTaskHandle != nullptr) {
        return;
    }
    // Low priority (1), 6KB stack, pinned to Core 0 (leaving Core 1 for audio & real-time loop)
    xTaskCreatePinnedToCore(
        paxgQuoteWorkerTask,
        "paxg_quote_worker",
        6144,
        nullptr,
        1,
        &s_quoteTaskHandle,
        0
    );
}
#else
void startPaxgQuoteWorker() {
    // Host test stub
}
#endif

} // namespace market
} // namespace paxg
