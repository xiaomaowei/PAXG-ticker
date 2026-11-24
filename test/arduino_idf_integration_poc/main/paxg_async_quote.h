#pragma once

#include <cstdint>
#include <cstddef>

namespace paxg {
namespace market {

enum class QuoteStatus : uint8_t {
    Unavailable = 0,
    Fresh = 1,
    Stale = 2,
};

inline const char* getQuoteStatusName(QuoteStatus status) {
    switch (status) {
        case QuoteStatus::Fresh: return "LIVE";
        case QuoteStatus::Stale: return "STALE";
        case QuoteStatus::Unavailable: default: return "UNAVAIL";
    }
}

constexpr size_t MAX_PRICE_HISTORY = 10;
constexpr uint32_t DEFAULT_MAX_QUOTE_AGE_MS = 180000UL; // 3 minutes threshold for stale quote

struct MarketQuoteSnapshot {
    double price = 0.0;
    double percentChange = 0.0;
    uint32_t lastQuoteTimeMs = 0;
    QuoteStatus status = QuoteStatus::Unavailable;
    size_t historyCount = 0;
    double priceHistory[MAX_PRICE_HISTORY] = {0.0};
};

/**
 * @brief Market session flags based on synchronized time.
 */
struct MarketSessionsState {
    bool timeValid = false;
    bool uk = false;
    bool us = false;
    bool japan = false;
    bool moon = false;
};

/**
 * @brief Pure helper to evaluate active market sessions from synchronized time.
 *        If time is invalid or unsynchronized, all session flags are false (never fabricated).
 * @param dayOfWeek 0=Sunday, 1=Monday, ..., 5=Friday, 6=Saturday
 * @param hour Hour of day (0..23)
 * @param minute Minute of hour (0..59)
 * @param timeValid True if time has been synchronized with NTP / network RTC
 */
MarketSessionsState evaluateMarketSessions(int dayOfWeek, int hour, int minute, bool timeValid);

/**
 * @brief Pure helper to parse Binance 24hr ticker JSON payload.
 *        Zero dynamic memory allocation, thread-safe, native-testable.
 *        Extracts and rigorously validates 'lastPrice' and 'priceChangePercent'.
 *        Rejects malformed JSON, wrong symbol, nonfinite/NaN/inf values, and trailing garbage.
 */
bool parseBinanceTicker(const char* payload, double& outPrice, double& outPercentChange);

/**
 * @brief Pure helper to evaluate quote freshness based on age and previous status.
 *        Never falsely marks a stale or unavailable quote as current.
 */
QuoteStatus evaluateQuoteFreshness(uint32_t lastQuoteTimeMs, uint32_t nowMs, QuoteStatus currentStatus, uint32_t maxAgeMs = DEFAULT_MAX_QUOTE_AGE_MS);

/**
 * @brief Get current thread-safe snapshot of market quote.
 */
MarketQuoteSnapshot getMarketQuoteSnapshot(uint32_t nowMs);

/**
 * @brief Thread-safe update of quote snapshot.
 */
void setMarketQuote(double price, double percentChange, uint32_t quoteTimeMs, QuoteStatus status);

/**
 * @brief Mark current quote as fetch failed (transitions Fresh -> Stale, Unavailable remains Unavailable).
 */
void markMarketQuoteFetchFailed();

/**
 * @brief Reset market quote state (for tests and teardown).
 */
void resetMarketQuoteState();

/**
 * @brief Initialize the bounded async FreeRTOS worker task for Wi-Fi and market quotes.
 *        Runs as low-priority task with authenticated TLS and non-blocking timeouts.
 */
void startPaxgQuoteWorker();

} // namespace market
} // namespace paxg
