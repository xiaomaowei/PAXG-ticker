#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>

#include "paxg_async_quote.h"
#include "live_fishing_controller.h"
#include "mode_navigator.h"

using namespace paxg::market;
using namespace paxg::fishing;
using namespace paxg::ui;

// Mock implementations for controller test
class MockHid : public IHidKeyboard {
public:
    bool mounted = true;
    bool ready = true;
    int pressCount = 0;
    int releaseCount = 0;
    uint8_t lastKeycode = 0;

    bool isMounted() override { return mounted; }
    bool isReady() override { return ready; }
    bool press(uint8_t, uint8_t keycode) override {
        pressCount++;
        lastKeycode = keycode;
        return true;
    }
    bool release() override {
        releaseCount++;
        return true;
    }
};

class MockClock : public IClock {
public:
    uint32_t currentMs = 1000;
    uint32_t nowMs() override { return currentMs; }
    void advance(uint32_t ms) { currentMs += ms; }
};

class MockAudioStream : public IAudioStream {
public:
    uint64_t packets = 100;
    uint64_t discontinuities = 0;
    uint32_t queueDrops = 0;
    bool triggered = false;
    float rms = 100.0f;

    uint64_t getPacketsReceived() const override { return packets; }
    uint64_t getDiscontinuities() const override { return discontinuities; }
    uint32_t getQueueDrops() const override { return queueDrops; }
    bool isTriggered() const override { return triggered; }
    void clearTrigger() override { triggered = false; }
    float getRms() const override { return rms; }
};

// 1. Valid ticker parsing
void test_parse_binance_ticker_valid() {
    printf("[RUN]  test_parse_binance_ticker_valid\n");
    const char* json = "{\"symbol\":\"PAXGUSDT\",\"priceChange\":\"12.50000000\",\"priceChangePercent\":\"0.475\","
                       "\"weightedAvgPrice\":\"2640.12\",\"prevClosePrice\":\"2635.00\",\"lastPrice\":\"2647.50000000\"}";
    double price = 0.0;
    double percent = 0.0;
    bool ok = parseBinanceTicker(json, price, percent);
    assert(ok);
    assert(std::fabs(price - 2647.50) < 0.001);
    assert(std::fabs(percent - 0.475) < 0.001);
    printf("  [PASS] test_parse_binance_ticker_valid\n");
}

// 2. Negative percentage and varied whitespace
void test_parse_binance_ticker_negative_percent() {
    printf("[RUN]  test_parse_binance_ticker_negative_percent\n");
    const char* json = "{\n  \"symbol\": \"PAXGUSDT\",\n  \"priceChangePercent\": \"-1.854\",\n  \"lastPrice\": \"2610.25\"\n}";
    double price = 0.0;
    double percent = 0.0;
    bool ok = parseBinanceTicker(json, price, percent);
    assert(ok);
    assert(std::fabs(price - 2610.25) < 0.001);
    assert(std::fabs(percent - (-1.854)) < 0.001);
    printf("  [PASS] test_parse_binance_ticker_negative_percent\n");
}

// 3. Malformed JSON and missing fields
void test_parse_binance_ticker_malformed() {
    printf("[RUN]  test_parse_binance_ticker_malformed\n");
    double price = 0.0;
    double percent = 0.0;
    // Null or empty
    assert(!parseBinanceTicker(nullptr, price, percent));
    assert(!parseBinanceTicker("", price, percent));
    assert(!parseBinanceTicker("{}", price, percent));

    // Wrong symbol
    assert(!parseBinanceTicker("{\"symbol\":\"BTCUSDT\",\"lastPrice\":\"50000.0\",\"priceChangePercent\":\"2.0\"}", price, percent));

    // Non-object JSON
    assert(!parseBinanceTicker("[\"PAXGUSDT\", 2650.0]", price, percent));

    // Missing lastPrice
    assert(!parseBinanceTicker("{\"symbol\":\"PAXGUSDT\",\"priceChangePercent\":\"0.5\"}", price, percent));

    // Missing priceChangePercent
    assert(!parseBinanceTicker("{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"2600.0\"}", price, percent));

    printf("  [PASS] test_parse_binance_ticker_malformed\n");
}

// 4. Rejection of nonfinite numeric strings, NaN, inf, and trailing garbage
void test_parse_binance_ticker_nonfinite_and_trailing_garbage() {
    printf("[RUN]  test_parse_binance_ticker_nonfinite_and_trailing_garbage\n");
    double price = 0.0;
    double percent = 0.0;

    // "NaN" and "nan"
    assert(!parseBinanceTicker("{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"NaN\",\"priceChangePercent\":\"0.5\"}", price, percent));
    assert(!parseBinanceTicker("{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"2650.0\",\"priceChangePercent\":\"nan\"}", price, percent));

    // "inf" and "Infinity"
    assert(!parseBinanceTicker("{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"inf\",\"priceChangePercent\":\"0.5\"}", price, percent));
    assert(!parseBinanceTicker("{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"2650.0\",\"priceChangePercent\":\"-Infinity\"}", price, percent));

    // Trailing garbage / letters
    assert(!parseBinanceTicker("{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"2650.00xyz\",\"priceChangePercent\":\"0.5\"}", price, percent));
    assert(!parseBinanceTicker("{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"2650.00\",\"priceChangePercent\":\"0.5pct\"}", price, percent));

    // Multiple decimal points
    assert(!parseBinanceTicker("{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"2650..00\",\"priceChangePercent\":\"0.5\"}", price, percent));

    // Negative or zero price
    assert(!parseBinanceTicker("{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"-2650.0\",\"priceChangePercent\":\"0.5\"}", price, percent));
    assert(!parseBinanceTicker("{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"0.00\",\"priceChangePercent\":\"0.5\"}", price, percent));

    printf("  [PASS] test_parse_binance_ticker_nonfinite_and_trailing_garbage\n");
}

// 5. Strict JSON validation: embedded key strings, nested misleading objects, malformed syntax, and trailing commas
void test_parse_binance_ticker_strict_json_validation() {
    printf("[RUN]  test_parse_binance_ticker_strict_json_validation\n");
    double price = 0.0;
    double percent = 0.0;

    // A. Embedded key string lookalike inside string value (description contains "symbol:PAXGUSDT")
    // Top-level symbol is BTCUSDT, so parser must reject
    const char* embeddedStrReject =
        "{\"description\":\"symbol:PAXGUSDT lastPrice:2600.00 priceChangePercent:0.50\","
        "\"symbol\":\"BTCUSDT\",\"lastPrice\":\"50000.0\",\"priceChangePercent\":\"1.0\"}";
    assert(!parseBinanceTicker(embeddedStrReject, price, percent));

    // B. Embedded lookalike inside comment string where symbol IS PAXGUSDT
    // Top-level lastPrice and priceChangePercent must be correctly extracted, NOT the ones inside comment
    const char* embeddedCommentValid =
        "{\"symbol\":\"PAXGUSDT\","
        "\"comment\":\"lastPrice:9999.00 priceChangePercent:99.99\","
        "\"lastPrice\":\"2640.50\",\"priceChangePercent\":\"0.75\"}";
    assert(parseBinanceTicker(embeddedCommentValid, price, percent));
    assert(std::fabs(price - 2640.50) < 0.001);
    assert(std::fabs(percent - 0.75) < 0.001);

    // C. Nested misleading object where inner object has symbol:PAXGUSDT but top-level has BTCUSDT
    const char* nestedMisleading =
        "{\"symbol\":\"BTCUSDT\","
        "\"related\":{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"2600.00\",\"priceChangePercent\":\"0.50\"},"
        "\"lastPrice\":\"50000.0\",\"priceChangePercent\":\"1.0\"}";
    assert(!parseBinanceTicker(nestedMisleading, price, percent));

    // D. Misleading config object before symbol
    const char* nestedBefore =
        "{\"config\":{\"symbol\":\"PAXGUSDT\"},\"symbol\":\"ETHUSDT\",\"lastPrice\":\"3000.0\",\"priceChangePercent\":\"2.0\"}";
    assert(!parseBinanceTicker(nestedBefore, price, percent));

    // E. Trailing garbage after valid JSON object
    const char* trailingJunk =
        "{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"2600.00\",\"priceChangePercent\":\"0.50\"} trailing_junk";
    assert(!parseBinanceTicker(trailingJunk, price, percent));

    const char* trailingSemi =
        "{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"2600.00\",\"priceChangePercent\":\"0.50\"};";
    assert(!parseBinanceTicker(trailingSemi, price, percent));

    // F. Syntax errors in nested/sibling structures
    const char* trailingCommaArray =
        "{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"2600.00\",\"priceChangePercent\":\"0.50\",\"tags\":[\"gold\",]}";
    assert(!parseBinanceTicker(trailingCommaArray, price, percent));

    const char* unclosedNestedObj =
        "{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"2600.00\",\"priceChangePercent\":\"0.50\",\"nested\":{\"k\":}}";
    assert(!parseBinanceTicker(unclosedNestedObj, price, percent));

    const char* unclosedString =
        "{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"2600.00\",\"priceChangePercent\":\"0.50\",\"err\":\"unclosed}";
    assert(!parseBinanceTicker(unclosedString, price, percent));

    // G. Trailing comma in top-level object
    const char* trailingCommaObj =
        "{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"2600.00\",\"priceChangePercent\":\"0.50\",}";
    assert(!parseBinanceTicker(trailingCommaObj, price, percent));

    // H. Duplicate top-level keys
    const char* duplicateKeys =
        "{\"symbol\":\"PAXGUSDT\",\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"2600.00\",\"priceChangePercent\":\"0.50\"}";
    assert(!parseBinanceTicker(duplicateKeys, price, percent));

    // I. Missing colon
    const char* missingColon =
        "{\"symbol\" \"PAXGUSDT\",\"lastPrice\":\"2600.00\",\"priceChangePercent\":\"0.50\"}";
    assert(!parseBinanceTicker(missingColon, price, percent));

    // J. Unquoted key
    const char* unquotedKey =
        "{symbol:\"PAXGUSDT\",\"lastPrice\":\"2600.00\",\"priceChangePercent\":\"0.50\"}";
    assert(!parseBinanceTicker(unquotedKey, price, percent));

    // K. Top-level array containing valid ticker object
    const char* arrayNotObj =
        "[{\"symbol\":\"PAXGUSDT\",\"lastPrice\":\"2600.00\",\"priceChangePercent\":\"0.50\"}]";
    assert(!parseBinanceTicker(arrayNotObj, price, percent));

    // L. Valid numeric literals (unquoted numbers)
    const char* unquotedNumbers =
        "{\"symbol\":\"PAXGUSDT\",\"lastPrice\":2642.75,\"priceChangePercent\":-0.65}";
    assert(parseBinanceTicker(unquotedNumbers, price, percent));
    assert(std::fabs(price - 2642.75) < 0.001);
    assert(std::fabs(percent - (-0.65)) < 0.001);

    printf("  [PASS] test_parse_binance_ticker_strict_json_validation\n");
}

// 6. Quote freshness evaluation
void test_quote_freshness_evaluation() {
    printf("[RUN]  test_quote_freshness_evaluation\n");
    const uint32_t maxAge = 180000; // 3 minutes

    // Unavailable always remains Unavailable
    assert(evaluateQuoteFreshness(1000, 2000, QuoteStatus::Unavailable, maxAge) == QuoteStatus::Unavailable);

    // Fresh within maxAge
    assert(evaluateQuoteFreshness(1000, 1000 + 60000, QuoteStatus::Fresh, maxAge) == QuoteStatus::Fresh);
    assert(evaluateQuoteFreshness(1000, 1000 + 179999, QuoteStatus::Fresh, maxAge) == QuoteStatus::Fresh);

    // Stale when exceeding maxAge
    assert(evaluateQuoteFreshness(1000, 1000 + 180001, QuoteStatus::Fresh, maxAge) == QuoteStatus::Stale);
    assert(evaluateQuoteFreshness(1000, 1000 + 500000, QuoteStatus::Fresh, maxAge) == QuoteStatus::Stale);

    // Stale remains Stale
    assert(evaluateQuoteFreshness(1000, 2000, QuoteStatus::Stale, maxAge) == QuoteStatus::Stale);

    printf("  [PASS] test_quote_freshness_evaluation\n");
}

// 6. Market quote snapshot history and failure transition
void test_quote_state_and_failure_transition() {
    printf("[RUN]  test_quote_state_and_failure_transition\n");
    resetMarketQuoteState();

    // Initial state is Unavailable
    MarketQuoteSnapshot snap0 = getMarketQuoteSnapshot(1000);
    assert(snap0.status == QuoteStatus::Unavailable);
    assert(snap0.historyCount == 0);

    // Set fresh quote
    setMarketQuote(2650.0, 0.5, 2000, QuoteStatus::Fresh);
    MarketQuoteSnapshot snap1 = getMarketQuoteSnapshot(2500);
    assert(snap1.status == QuoteStatus::Fresh);
    assert(std::fabs(snap1.price - 2650.0) < 0.001);
    assert(std::fabs(snap1.percentChange - 0.5) < 0.001);
    assert(snap1.historyCount == 1);
    assert(std::fabs(snap1.priceHistory[0] - 2650.0) < 0.001);

    // Simulate fetch failure: transitions Fresh -> Stale
    markMarketQuoteFetchFailed();
    MarketQuoteSnapshot snap2 = getMarketQuoteSnapshot(3000);
    assert(snap2.status == QuoteStatus::Stale);
    assert(std::fabs(snap2.price - 2650.0) < 0.001);

    // Recover with new fresh quote
    setMarketQuote(2655.0, 0.7, 4000, QuoteStatus::Fresh);
    MarketQuoteSnapshot snap3 = getMarketQuoteSnapshot(4500);
    assert(snap3.status == QuoteStatus::Fresh);
    assert(snap3.historyCount == 2);
    assert(std::fabs(snap3.priceHistory[1] - 2655.0) < 0.001);

    printf("  [PASS] test_quote_state_and_failure_transition\n");
}

// 7. Market sessions evaluation: unsynchronized time omits all session icons (never fabricated)
void test_evaluate_market_sessions_time_invalid() {
    printf("[RUN]  test_evaluate_market_sessions_time_invalid\n");
    // When timeValid == false, all session indicators must be false
    for (int day = 0; day <= 6; ++day) {
        for (int h = 0; h < 24; h += 4) {
            const auto s = evaluateMarketSessions(day, h, 0, false);
            assert(!s.timeValid);
            assert(!s.uk);
            assert(!s.us);
            assert(!s.japan);
            assert(!s.moon);
        }
    }
    printf("  [PASS] test_evaluate_market_sessions_time_invalid\n");
}

// 8. Market sessions evaluation: accurate session hours on synchronized time
void test_evaluate_market_sessions_schedule() {
    printf("[RUN]  test_evaluate_market_sessions_schedule\n");

    // UK Session (09:00 - 17:29 UTC on Mon=1)
    const auto sUk = evaluateMarketSessions(1, 10, 0, true);
    assert(sUk.timeValid);
    assert(sUk.uk);
    assert(!sUk.japan);
    assert(!sUk.moon);

    // US Session (15:30 - 21:59 UTC on Wed=3)
    const auto sUs = evaluateMarketSessions(3, 16, 0, true);
    assert(sUs.timeValid);
    assert(sUs.us);
    assert(sUs.uk); // 16:00 overlaps UK & US
    assert(!sUs.japan);
    assert(!sUs.moon);

    // Japan Session (01:00 - 07:59 UTC on Thu=4)
    const auto sJp = evaluateMarketSessions(4, 3, 0, true);
    assert(sJp.timeValid);
    assert(sJp.japan);
    assert(!sJp.uk);
    assert(!sJp.us);
    assert(!sJp.moon);

    // Off-hours on weekday (e.g. 23:00 UTC on Tue=2) -> Moon active
    const auto sNight = evaluateMarketSessions(2, 23, 0, true);
    assert(sNight.timeValid);
    assert(!sNight.uk);
    assert(!sNight.us);
    assert(!sNight.japan);
    assert(sNight.moon);

    // Weekend (Sun=0, Sat=6) -> Moon active, all market icons closed
    const auto sSun = evaluateMarketSessions(0, 12, 0, true);
    assert(sSun.timeValid);
    assert(!sSun.uk && !sSun.us && !sSun.japan);
    assert(sSun.moon);

    const auto sSat = evaluateMarketSessions(6, 15, 0, true);
    assert(sSat.timeValid);
    assert(!sSat.uk && !sSat.us && !sSat.japan);
    assert(sSat.moon);

    printf("  [PASS] test_evaluate_market_sessions_schedule\n");
}

// 9. ModeNavigator contact bounce suppression (< 50ms pulse ignored)
void test_mode_navigator_contact_bounce_suppression() {
    printf("[RUN]  test_mode_navigator_contact_bounce_suppression\n");
    MockHid hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController ctrl(&hid, &clock, &audio);
    ctrl.init();

    ModeNavigator nav(ScreenMode::PaxgMarket);
    uint32_t now = 1000;

    // Transient bounce on DOWN: 30ms pulse (< 50ms DEBOUNCE_MS)
    nav.update(false, true, now, ctrl);
    now += 30;
    nav.update(false, false, now, ctrl);
    now += 100;
    nav.update(false, false, now, ctrl);

    // Mode must NOT have changed
    assert(nav.getMode() == ScreenMode::PaxgMarket);
    assert(!nav.isDebouncedDown());

    printf("  [PASS] test_mode_navigator_contact_bounce_suppression\n");
}

// 10. ModeNavigator short press DOWN switches from PAXG Market to Real Fishing
void test_mode_navigator_switch_paxg_to_fishing() {
    printf("[RUN]  test_mode_navigator_switch_paxg_to_fishing\n");
    MockHid hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController ctrl(&hid, &clock, &audio);
    ctrl.init();

    ModeNavigator nav(ScreenMode::PaxgMarket);
    uint32_t now = 1000;

    // Press DOWN for 100ms (> 50ms debounce, < 800ms short press)
    nav.update(false, true, now, ctrl);
    now += 60; // Debounce confirmed
    nav.update(false, true, now, ctrl);
    assert(nav.isDebouncedDown());

    now += 100; // Release DOWN
    nav.update(false, false, now, ctrl);
    now += 60; // Debounce release confirmed
    nav.update(false, false, now, ctrl);

    // Mode must have transitioned to RealFishing
    assert(nav.getMode() == ScreenMode::RealFishing);
    assert(nav.isModeChanged());
    nav.clearModeChanged();
    assert(!nav.isModeChanged());

    // Controller must be strictly Disarmed and Idle
    assert(!ctrl.isArmed());
    assert(ctrl.getState() == FishingState::Idle);
    assert(hid.pressCount == 0);

    printf("  [PASS] test_mode_navigator_switch_paxg_to_fishing\n");
}

// 11. ModeNavigator stop-then-exit invariant:
//     Pressing DOWN while running STOPS the controller; releasing that press DOES NOT exit mode.
//     Only a subsequent DOWN press when already idle exits to PAXG.
void test_mode_navigator_stop_then_exit() {
    printf("[RUN]  test_mode_navigator_stop_then_exit\n");
    MockHid hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController ctrl(&hid, &clock, &audio);
    ctrl.init();
    ctrl.update(false, false);

    ModeNavigator nav(ScreenMode::RealFishing);
    uint32_t now = 1000;

    // 1. Arm controller manually and start fishing
    assert(ctrl.requestArmToggle());
    audio.packets += 20;
    clock.advance(10);
    ctrl.update(false, false);
    assert(ctrl.requestStart());
    assert(ctrl.getState() == FishingState::Casting);
    assert(ctrl.isArmed());

    // 2. Press DOWN while running -> Emergency Stop
    nav.update(false, true, now, ctrl);
    now += 60; // Confirm 50ms debounce in navigator
    clock.advance(60);
    nav.update(false, true, now, ctrl);
    now += 60; // Confirm 50ms stop debounce in controller
    clock.advance(60);
    nav.update(false, true, now, ctrl);

    // Controller must now be stopped and disarmed
    assert(!ctrl.isArmed());
    assert(ctrl.getState() == FishingState::Idle);
    assert(ctrl.getLastStopReason() == StopReason::UserStopped);

    // 3. Release DOWN -> Mode must REMAIN RealFishing (stop-then-exit invariant!)
    now += 100;
    clock.advance(100);
    nav.update(false, false, now, ctrl);
    now += 60; // Confirm release debounce
    clock.advance(60);
    nav.update(false, false, now, ctrl);

    assert(nav.getMode() == ScreenMode::RealFishing); // Still in RealFishing!

    // 4. Now that controller is already Idle and Disarmed, a second DOWN press triggers mode exit
    now += 100;
    clock.advance(100);
    nav.update(false, true, now, ctrl);
    now += 60;
    clock.advance(60);
    nav.update(false, true, now, ctrl);

    // Release second DOWN press
    now += 100;
    clock.advance(100);
    nav.update(false, false, now, ctrl);
    now += 60;
    clock.advance(60);
    nav.update(false, false, now, ctrl);

    // Mode must now transition to PaxgMarket
    assert(nav.getMode() == ScreenMode::PaxgMarket);
    assert(!ctrl.isArmed());
    assert(ctrl.getState() == FishingState::Idle);

    printf("  [PASS] test_mode_navigator_stop_then_exit\n");
}

// 12. ModeNavigator dual-button arming and post-arm release requirement
void test_mode_navigator_dual_button_arm_and_release() {
    printf("[RUN]  test_mode_navigator_dual_button_arm_and_release\n");
    MockHid hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController ctrl(&hid, &clock, &audio);
    ctrl.init();
    ctrl.update(false, false);

    ModeNavigator nav(ScreenMode::RealFishing);
    uint32_t now = 1000;

    // Hold both buttons for 1000ms
    nav.update(true, true, now, ctrl);
    now += 60;
    nav.update(true, true, now, ctrl);
    now += 1000;
    clock.advance(1000);
    nav.update(true, true, now, ctrl);

    // Controller should now be armed
    assert(ctrl.isArmed());

    // Holding UP while releasing DOWN must NOT trigger START (requires post-arm full release)
    now += 100;
    clock.advance(100);
    nav.update(true, false, now, ctrl);
    now += 60;
    nav.update(true, false, now, ctrl);
    assert(ctrl.getState() == FishingState::Idle); // Cannot start yet!

    // Release both buttons
    now += 100;
    clock.advance(100);
    nav.update(false, false, now, ctrl);
    now += 60;
    nav.update(false, false, now, ctrl);

    // Now press UP alone -> starts fishing
    audio.packets += 20;
    now += 100;
    clock.advance(100);
    nav.update(true, false, now, ctrl);
    now += 60; // Debounce UP in navigator -> debouncedUp_ becomes true
    clock.advance(60);
    nav.update(true, false, now, ctrl);
    now += 60; // Debounce UP in controller -> START_PRESS_DURATION_MS
    clock.advance(60);
    nav.update(true, false, now, ctrl);
    assert(ctrl.getState() == FishingState::Casting);

    printf("  [PASS] test_mode_navigator_dual_button_arm_and_release\n");
}

// 13. ModeNavigator mode entry and button release guarantees no accidental keypress
void test_mode_navigator_mode_entry_release_no_accidental_keys() {
    printf("[RUN]  test_mode_navigator_mode_entry_release_no_accidental_keys\n");
    MockHid hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController ctrl(&hid, &clock, &audio);
    ctrl.init();

    ModeNavigator nav(ScreenMode::PaxgMarket);
    uint32_t now = 1000;

    // Switch to RealFishing
    nav.switchToMode(ScreenMode::RealFishing, ctrl);
    assert(nav.getMode() == ScreenMode::RealFishing);
    assert(!ctrl.isArmed());
    assert(ctrl.getState() == FishingState::Idle);

    // Simulate button release after switching
    for (int i = 0; i < 20; ++i) {
        now += 50;
        clock.advance(50);
        nav.update(false, false, now, ctrl);
        assert(!ctrl.isArmed());
        assert(ctrl.getState() == FishingState::Idle);
        assert(hid.pressCount == 0);
    }

    printf("  [PASS] test_mode_navigator_mode_entry_release_no_accidental_keys\n");
}

// 14. Persistent stop reason and countdown accuracy
void test_persistent_stop_reason_and_countdown() {
    printf("[RUN]  test_persistent_stop_reason_and_countdown\n");
    MockHid hid;
    MockClock clock;
    MockAudioStream audio;
    LiveFishingController ctrl(&hid, &clock, &audio);
    ctrl.init();
    ctrl.update(false, false);

    // When in Idle, countdown remaining is strictly 0
    assert(ctrl.getWaitRemainingMs() == 0);

    // Arm and start
    assert(ctrl.requestArmToggle());
    audio.packets += 20;
    clock.advance(10);
    ctrl.update(false, false);
    assert(ctrl.requestStart());
    assert(ctrl.getState() == FishingState::Casting);

    // Advance through Casting (1500ms) with regular audio packets to Waiting
    for (int i = 0; i < 160; ++i) {
        clock.advance(10);
        audio.packets += 1;
        ctrl.update(false, false);
    }
    assert(ctrl.getState() == FishingState::Waiting);

    // Countdown in Waiting is positive and tracks remaining duration
    uint32_t rem = ctrl.getWaitRemainingMs();
    assert(rem > 0 && rem <= 30000);

    // Stop with UserStopped
    ctrl.requestStop(StopReason::UserStopped);
    assert(ctrl.getState() == FishingState::Idle);
    assert(ctrl.getLastStopReason() == StopReason::UserStopped);
    assert(std::strcmp(getStopReasonName(ctrl.getLastStopReason()), "USER_STOP") == 0);
    assert(ctrl.getWaitRemainingMs() == 0);

    printf("  [PASS] test_persistent_stop_reason_and_countdown\n");
}

// 15. Concurrent read/write stress on market quote state
void test_concurrency_thread_safety() {
    printf("[RUN]  test_concurrency_thread_safety\n");
    resetMarketQuoteState();

    std::atomic<bool> running{true};
    std::thread writer([&]() {
        for (int i = 0; i < 500; ++i) {
            setMarketQuote(2600.0 + (i % 50), 0.1 * (i % 10), 1000 + i * 100, QuoteStatus::Fresh);
            if (i % 20 == 0) {
                markMarketQuoteFetchFailed();
            }
        }
        running.store(false);
    });

    std::thread reader([&]() {
        while (running.load()) {
            MarketQuoteSnapshot snap = getMarketQuoteSnapshot(50000);
            assert(snap.price >= 0.0);
        }
    });

    writer.join();
    reader.join();
    printf("  [PASS] test_concurrency_thread_safety\n");
}

// 17. FishingUiState changed-region / differential update evaluation
void test_fishing_ui_state_changed_evaluation() {
    printf("[RUN]  test_fishing_ui_state_changed_evaluation\n");

    FishingUiState s1;
    s1.mounted = true;
    s1.audioState = 3;
    s1.fsmState = FishingState::Waiting;
    s1.stopReason = StopReason::None;
    s1.isArmed = true;
    s1.remSec = 30;
    s1.casts = 5;
    s1.bites = 4;
    s1.reels = 4;

    FishingUiState s2 = s1;
    // Identical states must NOT differ (redraw suppressed, bus idle)
    assert(!s2.differsFrom(s1));

    // Countdown second tick must trigger redraw
    s2.remSec = 29;
    assert(s2.differsFrom(s1));
    s2.remSec = 30;

    // FSM state change must trigger redraw
    s2.fsmState = FishingState::BiteDetected;
    assert(s2.differsFrom(s1));
    s2.fsmState = FishingState::Waiting;

    // Arm state toggle must trigger redraw
    s2.isArmed = false;
    assert(s2.differsFrom(s1));
    s2.isArmed = true;

    // Stop reason change must trigger redraw
    s2.stopReason = StopReason::UserStopped;
    assert(s2.differsFrom(s1));
    s2.stopReason = StopReason::None;

    // USB / Audio disconnect must trigger redraw
    s2.mounted = false;
    assert(s2.differsFrom(s1));
    s2.mounted = true;

    s2.audioState = 1;
    assert(s2.differsFrom(s1));
    s2.audioState = 3;

    // Counts increment must trigger redraw
    s2.casts = 6;
    assert(s2.differsFrom(s1));
    s2.casts = 5;

    s2.bites = 5;
    assert(s2.differsFrom(s1));
    s2.bites = 4;

    s2.reels = 5;
    assert(s2.differsFrom(s1));

    printf("  [PASS] test_fishing_ui_state_changed_evaluation\n");
}

int main() {
    printf("=======================================================================\n");
    printf("PAXG Market & Mode Switching Integration Native Tests\n");
    printf("=======================================================================\n");

    test_parse_binance_ticker_valid();
    test_parse_binance_ticker_negative_percent();
    test_parse_binance_ticker_malformed();
    test_parse_binance_ticker_nonfinite_and_trailing_garbage();
    test_parse_binance_ticker_strict_json_validation();
    test_quote_freshness_evaluation();
    test_quote_state_and_failure_transition();
    test_evaluate_market_sessions_time_invalid();
    test_evaluate_market_sessions_schedule();
    test_mode_navigator_contact_bounce_suppression();
    test_mode_navigator_switch_paxg_to_fishing();
    test_mode_navigator_stop_then_exit();
    test_mode_navigator_dual_button_arm_and_release();
    test_mode_navigator_mode_entry_release_no_accidental_keys();
    test_persistent_stop_reason_and_countdown();
    test_concurrency_thread_safety();
    test_fishing_ui_state_changed_evaluation();

    printf("=======================================================================\n");
    printf("Results: 17/17 passed (0 failed)\n");
    printf("=======================================================================\n");
    return 0;
}
