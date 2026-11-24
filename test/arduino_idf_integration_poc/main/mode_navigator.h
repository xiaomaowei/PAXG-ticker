#pragma once

#include <cstdint>
#include "live_fishing_controller.h"

namespace paxg {
namespace ui {

enum class ScreenMode : uint8_t {
    PaxgMarket = 0,
    RealFishing = 1,
};

/**
 * @brief Telemetry and display state snapshot for Real Fishing screen.
 *        Used for changed-region and differential state evaluation to suppress
 *        repetitive bus writes, eliminate LCD flicker, and prevent audio task starvation.
 */
struct FishingUiState {
    bool mounted{false};
    uint8_t audioState{0}; // 0=NoUsb, 1=NoAudio, 2=Silence, 3=AudioOk
    paxg::fishing::FishingState fsmState{paxg::fishing::FishingState::Idle};
    paxg::fishing::StopReason stopReason{paxg::fishing::StopReason::None};
    bool isArmed{false};
    unsigned long remSec{0};
    uint32_t casts{0};
    uint32_t bites{0};
    uint32_t reels{0};

    bool differsFrom(const FishingUiState& o) const {
        return mounted != o.mounted ||
               audioState != o.audioState ||
               fsmState != o.fsmState ||
               stopReason != o.stopReason ||
               isArmed != o.isArmed ||
               remSec != o.remSec ||
               casts != o.casts ||
               bites != o.bites ||
               reels != o.reels;
    }
};

/**
 * @brief Coordinates button debouncing, physical gestures, and fail-safe transitions
 *        between the Live PAXG Market and Real Fishing screens.
 *
 * Invariant Guarantees:
 * 1. Debounce: 50ms contact debounce on both physical buttons. Transient noise < 50ms
 *    is discarded without triggering state changes or commands.
 * 2. Stop-Then-Exit Semantics: When fishing is running or armed, pressing DOWN immediately
 *    triggers an emergency stop and disarms. Releasing that DOWN press DOES NOT exit to PAXG.
 *    Only a subsequent, separate DOWN press while already disarmed and idle exits to PAXG.
 * 3. Mode Entry/Release Safety: Entering Real Fishing initializes strictly in DISARMED
 *    and IDLE. The release of the DOWN button used to enter Real Fishing never emits
 *    keystrokes or alters controller arming.
 * 4. Zero Keystroke Leakage: While on the PAXG Market screen, the fishing controller
 *    is updated with (false, false), allowing background audio/USB telemetry servicing
 *    without any possibility of emitting HID keystrokes.
 */
class ModeNavigator {
public:
    static constexpr uint32_t DEBOUNCE_MS = 50UL;
    static constexpr uint32_t SHORT_PRESS_MAX_MS = 800UL;

    explicit ModeNavigator(ScreenMode initialMode = ScreenMode::PaxgMarket);

    /**
     * @brief Periodic tick from the application main loop.
     * @param rawUp Raw reading of Button UP (active true).
     * @param rawDown Raw reading of Button DOWN (active true).
     * @param nowMs Current monotonic millisecond timestamp.
     * @param controller Reference to the LiveFishingController.
     */
    void update(bool rawUp, bool rawDown, uint32_t nowMs, paxg::fishing::LiveFishingController& controller);

    ScreenMode getMode() const { return currentMode_; }
    bool isModeChanged() const { return modeChanged_; }
    void clearModeChanged() { modeChanged_ = false; }

    void switchToMode(ScreenMode target, paxg::fishing::LiveFishingController& controller);

    bool isDebouncedUp() const { return debouncedUp_; }
    bool isDebouncedDown() const { return debouncedDown_; }

private:
    ScreenMode currentMode_;
    bool modeChanged_ = true;

    // Debounce tracking for DOWN
    bool prevRawDown_ = false;
    uint32_t downChangeTime_ = 0;
    bool debouncedDown_ = false;
    uint32_t downPressTime_ = 0;
    bool downWasStopAction_ = false;

    // Debounce tracking for UP
    bool prevRawUp_ = false;
    uint32_t upChangeTime_ = 0;
    bool debouncedUp_ = false;
};

} // namespace ui
} // namespace paxg
