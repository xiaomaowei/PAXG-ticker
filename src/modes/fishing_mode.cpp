#include <TFT_eSPI.h>
#include <modules/app_state.h>
#include <modules/audio/audio_detector.h>
#include <modules/audio/synthetic_audio_samples.h>
#include <modules/constants.h>
#include <modules/fishing_settings.h>
#include <modules/fishing_state_machine.h>
#include <modules/fishing_types.h>
#include <modules/hal/usb_hid_keyboard.h>
#include <modules/modes/fishing_mode.h>
#include <modules/sprites.h>
#include <modules/variables.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{

// Palette: Dark / Cyan-Navy High-Contrast Theme
constexpr uint16_t COLOR_BG = 0x0005;               // Ultra-dark background
constexpr uint16_t COLOR_PANEL_BG = 0x0113;         // Deep navy-cyan card background
constexpr uint16_t COLOR_BORDER = 0x0338;           // Soft cyan card border
constexpr uint16_t COLOR_BORDER_BRIGHT = TFT_CYAN;  // Vibrant cyan
constexpr uint16_t COLOR_TEXT_DIM = 0x9CD3;         // Dim cyan-grey
constexpr uint16_t COLOR_BAR_BG = 0x018C;           // Progress bar unfilled track
constexpr uint16_t COLOR_ROW_HIGHLIGHT = 0x0276;    // Highlight for selected settings row

// State durations (milliseconds)
constexpr unsigned long CASTING_DURATION_MS = 1500UL;
constexpr unsigned long BITE_DETECTED_DURATION_MS = 300UL;
constexpr unsigned long REELING_DURATION_MS = 1200UL;
constexpr unsigned long TIMEOUT_DISPLAY_DURATION_MS = 1500UL;

// Audio history sparkline samples
constexpr int AUDIO_HISTORY_COUNT = 48;
float audioHistory[AUDIO_HISTORY_COUNT] = {0.0f};
int audioHistoryIndex = 0;
unsigned long lastHistorySampleTime = 0;
constexpr unsigned long HISTORY_SAMPLE_INTERVAL_MS = 50UL;

// Finite State Machine instance
paxg::fishing::FishingStateMachine s_fsm(paxg::fishing::FishingState::Idle);

// Modular PCM Audio Detector and Synthetic Audio Stream
// Declarations: 16kHz PCM mono with 128-sample frames (8.0ms exact duration)
paxg::audio::PcmAudioDetector s_detector;
paxg::audio::AudioDetectorConfig s_audioConfig;
paxg::fishing::FishingSettingsData s_settings;
int16_t s_pcmFrame[FISHING_AUDIO_FRAME_SIZE];
uint32_t s_synthNoiseSeed = 1337;
unsigned long s_castCycleCounter = 0;
unsigned long lastAudioFrameTime = 0;

// Settings menu state
int s_settingsSelectedRow = 0;
constexpr int SETTINGS_ROW_COUNT = 6;

void record_audio_sample(float level)
{
    if (currentMillis - lastHistorySampleTime >= HISTORY_SAMPLE_INTERVAL_MS)
    {
        audioHistory[audioHistoryIndex] = level;
        audioHistoryIndex = (audioHistoryIndex + 1) % AUDIO_HISTORY_COUNT;
        lastHistorySampleTime = currentMillis;
    }
}

void apply_settings_to_detector()
{
    s_audioConfig.absoluteThreshold = s_settings.absoluteThreshold;
    s_audioConfig.frameSize = FISHING_AUDIO_FRAME_SIZE;
    s_audioConfig.sampleRate = FISHING_AUDIO_SAMPLE_RATE_HZ;
    s_audioConfig.thresholdRatio = 2.5f;
    s_audioConfig.debounceFrames = 2;
    s_audioConfig.cooldownFrames = 25;
    s_detector.updateConfig(s_audioConfig);

    fishingWaitMaxMs = s_settings.waitTimeoutMs;
    fishingCooldownMs = s_settings.cooldownMs;
    fishingCastKey = s_settings.castKey;
    fishingInteractKey = s_settings.interactKey;
}

void process_single_audio_frame_and_fsm()
{
    // Accumulate session uptime based on exact 8ms frame time
    if (fishingState != FishingState::Idle && fishingState != FishingState::Paused && fishingState != FishingState::Error)
    {
        fishingTotalUptimeMs += FISHING_AUDIO_FRAME_INTERVAL_MS;
    }

    switch (fishingState)
    {
        case FishingState::Idle:
        case FishingState::Paused:
        case FishingState::Error:
        {
            paxg::audio::SyntheticAudioGenerator::generateSilence(s_pcmFrame, FISHING_AUDIO_FRAME_SIZE);
            s_detector.processSamples(s_pcmFrame, FISHING_AUDIO_FRAME_SIZE);
            fishingSimAudioLevel = 0.02f;
            fishingAudioDetected = false;
            break;
        }

        case FishingState::Casting:
        {
            paxg::audio::SyntheticAudioGenerator::generateNoise(s_pcmFrame, FISHING_AUDIO_FRAME_SIZE, 200, s_synthNoiseSeed++);
            s_detector.processSamples(s_pcmFrame, FISHING_AUDIO_FRAME_SIZE);
            fishingSimAudioLevel = s_detector.getMetrics().rms / 5000.0f;
            fishingAudioDetected = false;

            if (currentMillis - fishingStateStartTime >= CASTING_DURATION_MS)
            {
                if (s_fsm.handleEvent(paxg::fishing::FishingEvent::CastComplete))
                {
                    fishingState = s_fsm.getState();
                    fishingStateStartTime = currentMillis;
                    s_castCycleCounter++;

                    // Deterministic timeout reachability: every 4th cast schedules bite past timeout window
                    if ((s_castCycleCounter % 4) == 0)
                    {
                        fishingNextBiteDelayMs = s_settings.waitTimeoutMs + 4000UL;
                    }
                    else
                    {
                        fishingNextBiteDelayMs = 4500UL + (rand() % 9500UL);  // 4.5s to 14.0s
                    }
                }
            }
            break;
        }

        case FishingState::Waiting:
        {
            const unsigned long waitElapsed = currentMillis - fishingStateStartTime;

            // Generate synthetic PCM frame: bite splash burst vs baseline ambient noise
            if (waitElapsed >= fishingNextBiteDelayMs &&
                waitElapsed < (fishingNextBiteDelayMs + 300UL))
            {
                paxg::audio::SyntheticAudioGenerator::generateBurst(s_pcmFrame, FISHING_AUDIO_FRAME_SIZE, 250, 6500, 0, FISHING_AUDIO_FRAME_SIZE, s_synthNoiseSeed++);
            }
            else
            {
                paxg::audio::SyntheticAudioGenerator::generateNoise(s_pcmFrame, FISHING_AUDIO_FRAME_SIZE, 280, s_synthNoiseSeed++);
            }

            s_detector.processSamples(s_pcmFrame, FISHING_AUDIO_FRAME_SIZE);
            const paxg::audio::AudioDetectorMetrics m = s_detector.getMetrics();
            fishingSimAudioLevel = std::min(1.0f, m.rms / 5000.0f);
            fishingSimAudioThreshold = std::min(1.0f, m.effectiveThreshold / 5000.0f);

            if (s_detector.isTriggered())
            {
                s_detector.clearTrigger();
                if (s_fsm.handleEvent(paxg::fishing::FishingEvent::BiteDetected))
                {
                    fishingState = s_fsm.getState();
                    fishingStateStartTime = currentMillis;
                    fishingAudioDetected = true;
                    fishingSimReelCmds++;
                    if (fishingArmed)
                    {
                        usb_hid_keyboard_enqueue(s_settings.interactKey);
                    }
                }
            }
            else if (waitElapsed >= s_settings.waitTimeoutMs)
            {
                if (s_fsm.handleEvent(paxg::fishing::FishingEvent::BiteTimeout))
                {
                    fishingState = s_fsm.getState();
                    fishingStateStartTime = currentMillis;
                    fishingSimTimeouts++;
                    fishingAudioDetected = false;
                }
            }
            break;
        }

        case FishingState::BiteDetected:
        {
            paxg::audio::SyntheticAudioGenerator::generateNoise(s_pcmFrame, FISHING_AUDIO_FRAME_SIZE, 500, s_synthNoiseSeed++);
            s_detector.processSamples(s_pcmFrame, FISHING_AUDIO_FRAME_SIZE);
            fishingSimAudioLevel = 0.90f;
            fishingAudioDetected = true;

            if (currentMillis - fishingStateStartTime >= BITE_DETECTED_DURATION_MS)
            {
                if (s_fsm.handleEvent(paxg::fishing::FishingEvent::ReelTrigger))
                {
                    fishingState = s_fsm.getState();
                    fishingStateStartTime = currentMillis;
                }
            }
            break;
        }

        case FishingState::Reeling:
        {
            paxg::audio::SyntheticAudioGenerator::generateNoise(s_pcmFrame, FISHING_AUDIO_FRAME_SIZE, 400, s_synthNoiseSeed++);
            s_detector.processSamples(s_pcmFrame, FISHING_AUDIO_FRAME_SIZE);
            fishingAudioDetected = false;
            fishingSimAudioLevel = 0.35f;

            if (currentMillis - fishingStateStartTime >= REELING_DURATION_MS)
            {
                if (s_fsm.handleEvent(paxg::fishing::FishingEvent::ReelComplete))
                {
                    fishingState = s_fsm.getState();
                    fishingStateStartTime = currentMillis;
                }
            }
            break;
        }

        case FishingState::Cooldown:
        {
            paxg::audio::SyntheticAudioGenerator::generateNoise(s_pcmFrame, FISHING_AUDIO_FRAME_SIZE, 150, s_synthNoiseSeed++);
            s_detector.processSamples(s_pcmFrame, FISHING_AUDIO_FRAME_SIZE);
            fishingAudioDetected = false;
            fishingSimAudioLevel = 0.08f;

            if (currentMillis - fishingStateStartTime >= s_settings.cooldownMs)
            {
                if (s_fsm.handleEvent(paxg::fishing::FishingEvent::CooldownComplete))
                {
                    fishingState = s_fsm.getState();
                    fishingStateStartTime = currentMillis;
                    if (fishingArmed)
                    {
                        usb_hid_keyboard_enqueue(s_settings.castKey);
                    }
                }
            }
            break;
        }

        case FishingState::Timeout:
        {
            paxg::audio::SyntheticAudioGenerator::generateNoise(s_pcmFrame, FISHING_AUDIO_FRAME_SIZE, 150, s_synthNoiseSeed++);
            s_detector.processSamples(s_pcmFrame, FISHING_AUDIO_FRAME_SIZE);
            fishingAudioDetected = false;
            fishingSimAudioLevel = 0.05f;

            if (currentMillis - fishingStateStartTime >= TIMEOUT_DISPLAY_DURATION_MS)
            {
                if (s_fsm.handleEvent(paxg::fishing::FishingEvent::TimeoutComplete))
                {
                    fishingState = s_fsm.getState();
                    fishingStateStartTime = currentMillis;
                }
            }
            break;
        }
    }
}

}  // namespace

void fishing_mode_init()
{
    if (fishing_display.getPointer() == nullptr)
    {
        fishing_display.createSprite(320, 170);
    }

    paxg::fishing::loadPersistentSettings(s_settings);

    s_audioConfig.sampleRate = FISHING_AUDIO_SAMPLE_RATE_HZ;
    s_audioConfig.frameSize = FISHING_AUDIO_FRAME_SIZE;
    s_audioConfig.noiseFloorAlpha = 0.05f;
    s_audioConfig.thresholdRatio = 2.5f;
    s_audioConfig.absoluteThreshold = s_settings.absoluteThreshold;
    s_audioConfig.debounceFrames = 2;
    s_audioConfig.cooldownFrames = 25;
    s_detector.init(s_audioConfig);

    s_fsm.reset(paxg::fishing::FishingState::Idle);
    fishingState = s_fsm.getState();
    fishingCurrentPage = FishingPage::Dashboard;
    fishingStateStartTime = 0;
    fishingSessionStartTime = 0;
    fishingTotalUptimeMs = 0;
    fishingSimReelCmds = 0;
    fishingSimTimeouts = 0;
    fishingSimAudioLevel = 0.02f;
    fishingSimAudioThreshold = 0.30f;
    fishingAudioDetected = false;
    fishingNextBiteDelayMs = 0;
    fishingWaitMaxMs = s_settings.waitTimeoutMs;
    fishingCooldownMs = s_settings.cooldownMs;
    fishingArmed = false;
    fishingLiveKeySentCount = 0;
    fishingCastKey = s_settings.castKey;
    fishingInteractKey = s_settings.interactKey;
    fishingErrorMessage[0] = '\0';
    s_castCycleCounter = 0;
    s_settingsSelectedRow = 0;
    lastAudioFrameTime = 0;

    for (int i = 0; i < AUDIO_HISTORY_COUNT; ++i)
    {
        audioHistory[i] = 0.02f;
    }
}

void fishing_toggle_arm()
{
    if (!usb_hid_is_connected())
    {
        fishingArmed = false;
        usb_hid_keyboard_clear_and_release_all();
        return;
    }

    fishingArmed = !fishingArmed;
    if (!fishingArmed)
    {
        usb_hid_keyboard_clear_and_release_all();
    }
}

void fishing_exit_cleanup()
{
    fishingArmed = false;
    usb_hid_keyboard_clear_and_release_all();
}

void fishing_trigger_error(const char* msg)
{
    if (s_fsm.handleEvent(paxg::fishing::FishingEvent::Error))
    {
        fishingState = s_fsm.getState();
        fishingAudioDetected = false;
        fishingArmed = false;
        if (msg)
        {
            std::snprintf(fishingErrorMessage, sizeof(fishingErrorMessage), "%s", msg);
        }
        usb_hid_keyboard_clear_and_release_all();
    }
}

void fishing_toggle_start_pause()
{
    if (fishingState == FishingState::Idle)
    {
        // Start simulation from Idle
        if (s_fsm.handleEvent(paxg::fishing::FishingEvent::Start))
        {
            fishingState = s_fsm.getState();
            fishingStateStartTime = currentMillis;
            fishingSessionStartTime = currentMillis;
            fishingTotalUptimeMs = 0;
            fishingSimReelCmds = 0;
            fishingSimTimeouts = 0;
            fishingAudioDetected = false;
            s_detector.reset();
            s_castCycleCounter = 1;
            lastAudioFrameTime = currentMillis;
            if (fishingArmed)
            {
                usb_hid_keyboard_enqueue(s_settings.castKey);
            }
        }
    }
    else if (fishingState == FishingState::Paused)
    {
        // Resume simulation
        if (s_fsm.handleEvent(paxg::fishing::FishingEvent::Resume))
        {
            fishingState = s_fsm.getState();
            fishingStateStartTime = currentMillis;
            fishingAudioDetected = false;
            lastAudioFrameTime = currentMillis;
            if (fishingArmed)
            {
                usb_hid_keyboard_enqueue(s_settings.castKey);
            }
        }
    }
    else
    {
        // Pause active simulation: clear queue, release keys
        if (s_fsm.handleEvent(paxg::fishing::FishingEvent::Pause))
        {
            fishingState = s_fsm.getState();
            fishingAudioDetected = false;
            fishingSimAudioLevel = 0.02f;
            usb_hid_keyboard_clear_and_release_all();
        }
    }
}

void fishing_stop_reset()
{
    if (fishingState != FishingState::Idle)
    {
        // First action: Stop running simulation, disarm and release keys
        if (s_fsm.handleEvent(paxg::fishing::FishingEvent::Stop))
        {
            fishingState = s_fsm.getState();
            fishingAudioDetected = false;
            fishingSimAudioLevel = 0.02f;
            fishingArmed = false;
            fishingErrorMessage[0] = '\0';
            usb_hid_keyboard_clear_and_release_all();
            s_detector.reset();
        }
    }
    else
    {
        // Second action when already in Idle: Reset telemetry counters
        s_fsm.handleEvent(paxg::fishing::FishingEvent::Stop);
        fishingState = s_fsm.getState();
        fishingSimReelCmds = 0;
        fishingSimTimeouts = 0;
        fishingTotalUptimeMs = 0;
        fishingLiveKeySentCount = 0;
        fishingArmed = false;
        fishingErrorMessage[0] = '\0';
        usb_hid_keyboard_clear_and_release_all();
        s_detector.reset();
    }
}

void fishing_next_page()
{
    const int next = (static_cast<int>(fishingCurrentPage) + 1) % 3;
    fishingCurrentPage = static_cast<FishingPage>(next);
}

void fishing_set_page(paxg::fishing::FishingPage page)
{
    fishingCurrentPage = page;
}

void fishing_settings_next_item()
{
    s_settingsSelectedRow = (s_settingsSelectedRow + 1) % SETTINGS_ROW_COUNT;
}

void fishing_settings_adjust_value()
{
    switch (s_settingsSelectedRow)
    {
        case 0:  // Absolute Threshold
            s_settings.absoluteThreshold += paxg::fishing::STEP_THRESHOLD;
            if (s_settings.absoluteThreshold > paxg::fishing::MAX_THRESHOLD)
            {
                s_settings.absoluteThreshold = paxg::fishing::MIN_THRESHOLD;
            }
            apply_settings_to_detector();
            paxg::fishing::savePersistentSettings(s_settings);
            break;

        case 1:  // Wait Timeout
            s_settings.waitTimeoutMs += paxg::fishing::STEP_WAIT_TIMEOUT_MS;
            if (s_settings.waitTimeoutMs > paxg::fishing::MAX_WAIT_TIMEOUT_MS)
            {
                s_settings.waitTimeoutMs = paxg::fishing::MIN_WAIT_TIMEOUT_MS;
            }
            apply_settings_to_detector();
            paxg::fishing::savePersistentSettings(s_settings);
            break;

        case 2:  // Cooldown
            s_settings.cooldownMs += paxg::fishing::STEP_COOLDOWN_MS;
            if (s_settings.cooldownMs > paxg::fishing::MAX_COOLDOWN_MS)
            {
                s_settings.cooldownMs = paxg::fishing::MIN_COOLDOWN_MS;
            }
            apply_settings_to_detector();
            paxg::fishing::savePersistentSettings(s_settings);
            break;

        case 3:  // Cast Key
        {
            size_t idx = 0;
            for (size_t i = 0; i < paxg::fishing::ALLOWED_KEYS_COUNT; ++i)
            {
                if (s_settings.castKey == paxg::fishing::ALLOWED_KEYS[i])
                {
                    idx = (i + 1) % paxg::fishing::ALLOWED_KEYS_COUNT;
                    break;
                }
            }
            s_settings.castKey = paxg::fishing::ALLOWED_KEYS[idx];
            apply_settings_to_detector();
            paxg::fishing::savePersistentSettings(s_settings);
            break;
        }

        case 4:  // Interact Key
        {
            size_t idx = 0;
            for (size_t i = 0; i < paxg::fishing::ALLOWED_KEYS_COUNT; ++i)
            {
                if (s_settings.interactKey == paxg::fishing::ALLOWED_KEYS[i])
                {
                    idx = (i + 1) % paxg::fishing::ALLOWED_KEYS_COUNT;
                    break;
                }
            }
            s_settings.interactKey = paxg::fishing::ALLOWED_KEYS[idx];
            apply_settings_to_detector();
            paxg::fishing::savePersistentSettings(s_settings);
            break;
        }

        case 5:  // Return to Dashboard
            fishingCurrentPage = FishingPage::Dashboard;
            break;
    }
}

void fishing_settings_reset_defaults()
{
    paxg::fishing::resetPersistentSettings(s_settings);
    apply_settings_to_detector();
}

void fishing_sim_tick()
{
    usb_hid_keyboard_tick();

    // Exact 16kHz PCM audio frame timing: 128 samples / 16000Hz = 8.0ms per frame
    if (lastAudioFrameTime == 0)
    {
        lastAudioFrameTime = currentMillis;
    }

    // Process all elapsed 8ms frames without drift; limit catch-up to max 4 frames (32ms) to avoid blocking
    unsigned int framesProcessed = 0;
    while ((currentMillis - lastAudioFrameTime) >= FISHING_AUDIO_FRAME_INTERVAL_MS && framesProcessed < 4)
    {
        lastAudioFrameTime += FISHING_AUDIO_FRAME_INTERVAL_MS;
        framesProcessed++;
        process_single_audio_frame_and_fsm();
    }

    record_audio_sample(fishingSimAudioLevel);
}

// =============================================================================
// Page 1: Dashboard (Low-flicker 320x170 via fishing_display sprite)
// =============================================================================
static void render_page_dashboard()
{
    fishing_display.fillSprite(COLOR_BG);

    // 1. Header Bar (y: 2, h: 22)
    fishing_display.drawRoundRect(2, 2, 316, 22, 3, COLOR_BORDER);
    fishing_display.setTextSize(1);
    fishing_display.setTextColor(COLOR_BORDER_BRIGHT);
    fishing_display.setCursor(8, 7);
    fishing_display.print("WOW FISHING [1/3 DASH]");

    // State Pill
    uint16_t pillBg = COLOR_BORDER;
    uint16_t pillText = TFT_WHITE;
    const char* pillStr = paxg::fishing::getFishingStateName(fishingState);

    switch (fishingState)
    {
        case FishingState::Idle:
            pillBg = 0x2945;  // Slate grey
            pillText = TFT_LIGHTGREY;
            break;
        case FishingState::Paused:
            pillBg = 0x5A80;  // Amber
            pillText = TFT_YELLOW;
            break;
        case FishingState::Casting:
            pillBg = 0x028A;  // Slate cyan
            pillText = 0x5D3F;
            break;
        case FishingState::Waiting:
            pillBg = 0x0324;  // Deep cyan
            pillText = COLOR_BORDER_BRIGHT;
            break;
        case FishingState::BiteDetected:
        case FishingState::Reeling:
            pillBg = 0x0440;  // Vibrant green
            pillText = TFT_GREEN;
            break;
        case FishingState::Cooldown:
            pillBg = 0x39E0;  // Amber brown
            pillText = TFT_ORANGE;
            break;
        case FishingState::Timeout:
            pillBg = 0x6180;  // Dark orange
            pillText = TFT_YELLOW;
            break;
        case FishingState::Error:
            pillBg = 0x8800;  // Deep red
            pillText = TFT_RED;
            break;
    }

    fishing_display.fillRoundRect(160, 4, 68, 18, 2, pillBg);
    fishing_display.setTextColor(pillText);
    fishing_display.setCursor(164, 7);
    fishing_display.print(pillStr);

    // USB Host Mount status
    if (usb_hid_is_connected())
    {
        fishing_display.setTextColor(COLOR_BORDER_BRIGHT);
        fishing_display.setCursor(236, 7);
        fishing_display.print("USB:MOUNT");
    }
    else
    {
        fishing_display.setTextColor(TFT_ORANGE);
        fishing_display.setCursor(236, 7);
        fishing_display.print("USB:DISCON");
    }

    // 2. Middle Cards (y: 26, h: 74)
    // Left Card: Simulated Reel Count & Timeouts / Keys
    fishing_display.fillRoundRect(2, 26, 156, 74, 3, COLOR_PANEL_BG);
    fishing_display.drawRoundRect(2, 26, 156, 74, 3, COLOR_BORDER);

    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(8, 30);
    fishing_display.print("SIM REEL CMDS");

    char reelBuf[16];
    std::snprintf(reelBuf, sizeof(reelBuf), "%u", fishingSimReelCmds);
    fishing_display.setTextSize(3);
    fishing_display.setTextColor(COLOR_BORDER_BRIGHT);
    fishing_display.setCursor(10, 42);
    fishing_display.print(reelBuf);

    fishing_display.setTextSize(1);
    fishing_display.setTextColor(TFT_YELLOW);
    fishing_display.setCursor(8, 70);
    fishing_display.print("Simulated (no catch data)");

    char toBuf[36];
    std::snprintf(toBuf, sizeof(toBuf), "TO: %u | Live: %u", fishingSimTimeouts, fishingLiveKeySentCount);
    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(8, 83);
    fishing_display.print(toBuf);

    // Right Card: State & Wait Progress & Hardware Status
    fishing_display.fillRoundRect(162, 26, 156, 74, 3, COLOR_PANEL_BG);
    fishing_display.drawRoundRect(162, 26, 156, 74, 3, COLOR_BORDER);

    // State Display
    char stateLine[32];
    std::snprintf(stateLine, sizeof(stateLine), "STATE: %s", pillStr);
    fishing_display.setTextColor(pillText);
    fishing_display.setCursor(168, 30);
    fishing_display.print(stateLine);

    // Dynamic Timer / Progress Bar
    char waitBuf[32];
    if (fishingState == FishingState::Waiting)
    {
        const float waitSec = (currentMillis - fishingStateStartTime) / 1000.0f;
        const float maxSec = s_settings.waitTimeoutMs / 1000.0f;
        std::snprintf(waitBuf, sizeof(waitBuf), "Wait: %.1fs / %.1fs", waitSec, maxSec);
        fishing_display.setTextColor(TFT_WHITE);
        fishing_display.setCursor(168, 43);
        fishing_display.print(waitBuf);

        fishing_display.fillRect(168, 54, 142, 4, COLOR_BAR_BG);
        const int progressW = static_cast<int>((waitSec / maxSec) * 142.0f);
        if (progressW > 0)
        {
            fishing_display.fillRect(168, 54, std::min(progressW, 142), 4, COLOR_BORDER_BRIGHT);
        }
    }
    else if (fishingState == FishingState::Casting)
    {
        const float castSec = (currentMillis - fishingStateStartTime) / 1000.0f;
        std::snprintf(waitBuf, sizeof(waitBuf), "Cast: %.1fs / 1.5s", castSec);
        fishing_display.setTextColor(COLOR_TEXT_DIM);
        fishing_display.setCursor(168, 43);
        fishing_display.print(waitBuf);

        fishing_display.fillRect(168, 54, 142, 4, COLOR_BAR_BG);
        const int progressW = static_cast<int>((castSec / 1.5f) * 142.0f);
        if (progressW > 0)
        {
            fishing_display.fillRect(168, 54, std::min(progressW, 142), 4, 0x5D3F);
        }
    }
    else if (fishingState == FishingState::Cooldown)
    {
        const float cdSec = (currentMillis - fishingStateStartTime) / 1000.0f;
        const float maxCd = s_settings.cooldownMs / 1000.0f;
        std::snprintf(waitBuf, sizeof(waitBuf), "CD: %.1fs / %.1fs", cdSec, maxCd);
        fishing_display.setTextColor(TFT_ORANGE);
        fishing_display.setCursor(168, 43);
        fishing_display.print(waitBuf);

        fishing_display.fillRect(168, 54, 142, 4, COLOR_BAR_BG);
        const int progressW = static_cast<int>((cdSec / maxCd) * 142.0f);
        if (progressW > 0)
        {
            fishing_display.fillRect(168, 54, std::min(progressW, 142), 4, TFT_ORANGE);
        }
    }
    else
    {
        fishing_display.setTextColor(COLOR_TEXT_DIM);
        fishing_display.setCursor(168, 43);
        fishing_display.print("Wait: --");
        fishing_display.fillRect(168, 54, 142, 4, COLOR_BAR_BG);
    }

    // Session Uptime
    char upBuf[24];
    const unsigned long uptimeSec = fishingTotalUptimeMs / 1000UL;
    std::snprintf(upBuf, sizeof(upBuf), "Uptime: %02lu:%02lu", (uptimeSec / 60) % 60, uptimeSec % 60);
    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(168, 62);
    fishing_display.print(upBuf);

    // HID Safety status
    if (!usb_hid_is_connected())
    {
        fishing_display.setTextColor(0x7BEF);
        fishing_display.setCursor(168, 73);
        fishing_display.print("HID: UNAVAILABLE");
    }
    else if (!fishingArmed)
    {
        fishing_display.setTextColor(TFT_LIGHTGREY);
        fishing_display.setCursor(168, 73);
        fishing_display.print("HID: DISARMED");
    }
    else
    {
        fishing_display.setTextColor(TFT_RED);
        fishing_display.setCursor(168, 73);
        fishing_display.print("HID: ARMED [KEYS ON]");
    }

    fishing_display.setTextColor(0x7BEF);
    fishing_display.setCursor(168, 85);
    fishing_display.print("PCM: SYNTH (No UAC)");

    // 3. Audio & Waveform Card (y: 102, h: 50)
    fishing_display.fillRoundRect(2, 102, 316, 50, 3, COLOR_PANEL_BG);
    fishing_display.drawRoundRect(2, 102, 316, 50, 3, COLOR_BORDER);

    const paxg::audio::AudioDetectorMetrics m = s_detector.getMetrics();
    char audioBuf[64];
    std::snprintf(audioBuf, sizeof(audioBuf), "SYNTH PCM DETECTOR  RMS: %u  THR: %u", static_cast<unsigned int>(m.rms),
                  static_cast<unsigned int>(m.effectiveThreshold));
    fishing_display.setTextColor(fishingAudioDetected ? TFT_GREEN : COLOR_BORDER_BRIGHT);
    fishing_display.setCursor(8, 106);
    fishing_display.print(audioBuf);

    // Audio Level Meter Bar (x: 8, y: 120, w: 180, h: 10)
    fishing_display.drawRect(8, 120, 180, 10, COLOR_BORDER);
    fishing_display.fillRect(9, 121, 178, 8, COLOR_BAR_BG);
    const int meterW = static_cast<int>(fishingSimAudioLevel * 178.0f);
    const uint16_t mColor = (fishingSimAudioLevel >= fishingSimAudioThreshold) ? TFT_RED : COLOR_BORDER_BRIGHT;
    if (meterW > 0)
    {
        fishing_display.fillRect(9, 121, std::min(meterW, 178), 8, mColor);
    }

    const int threshX = 9 + static_cast<int>(fishingSimAudioThreshold * 178.0f);
    fishing_display.drawFastVLine(std::min(threshX, 186), 118, 14, TFT_WHITE);

    // Diagnostics readout under bar
    char diagBuf[64];
    std::snprintf(diagBuf, sizeof(diagBuf), "Peak: %5u | Buf: %u/2048 (SYNTH)", static_cast<unsigned int>(m.peak),
                  static_cast<unsigned int>(s_detector.getBufferedSampleCount()));
    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(8, 134);
    fishing_display.print(diagBuf);

    // Mini Sparkline (x: 198, y: 114, w: 114, h: 32)
    fishing_display.drawRect(198, 114, 114, 32, COLOR_BORDER);
    fishing_display.fillRect(199, 115, 112, 30, COLOR_BG);

    for (int i = 0; i < AUDIO_HISTORY_COUNT - 1; ++i)
    {
        const int idx1 = (audioHistoryIndex + i) % AUDIO_HISTORY_COUNT;
        const int idx2 = (audioHistoryIndex + i + 1) % AUDIO_HISTORY_COUNT;

        const int x1 = 200 + i * (110 / (AUDIO_HISTORY_COUNT - 1));
        const int x2 = 200 + (i + 1) * (110 / (AUDIO_HISTORY_COUNT - 1));

        const int y1 = 143 - static_cast<int>(audioHistory[idx1] * 26.0f);
        const int y2 = 143 - static_cast<int>(audioHistory[idx2] * 26.0f);

        const uint16_t waveColor = (audioHistory[idx2] >= fishingSimAudioThreshold) ? TFT_YELLOW : COLOR_BORDER_BRIGHT;
        fishing_display.drawLine(x1, y1, x2, y2, waveColor);
    }

    // 4. Footer Hint Bar (y: 154, h: 14)
    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(6, 156);
    fishing_display.print("UP:Page/Hold:Run | DN:Exit/Hold:Stop | BOTH:Arm");
}

// =============================================================================
// Page 2: Audio Monitor (Deep telemetry & synthetic stream diagnostics)
// =============================================================================
static void render_page_audio_monitor()
{
    fishing_display.fillSprite(COLOR_BG);

    // 1. Header (y: 2, h: 22)
    fishing_display.drawRoundRect(2, 2, 316, 22, 3, COLOR_BORDER);
    fishing_display.setTextSize(1);
    fishing_display.setTextColor(COLOR_BORDER_BRIGHT);
    fishing_display.setCursor(8, 7);
    fishing_display.print("AUDIO MONITOR [2/3 AUD]");

    fishing_display.setTextColor(TFT_YELLOW);
    fishing_display.setCursor(160, 7);
    fishing_display.print("[SYNTH PCM16]");

    fishing_display.setTextColor(0x7BEF);
    fishing_display.setCursor(244, 7);
    fishing_display.print("NO UAC");

    // 2. Waveform & Scope Window (y: 26, h: 62)
    fishing_display.fillRoundRect(2, 26, 316, 62, 3, COLOR_PANEL_BG);
    fishing_display.drawRoundRect(2, 26, 316, 62, 3, COLOR_BORDER);

    // Plot full-width sparkline
    for (int i = 0; i < AUDIO_HISTORY_COUNT - 1; ++i)
    {
        const int idx1 = (audioHistoryIndex + i) % AUDIO_HISTORY_COUNT;
        const int idx2 = (audioHistoryIndex + i + 1) % AUDIO_HISTORY_COUNT;

        const int x1 = 6 + i * (304 / (AUDIO_HISTORY_COUNT - 1));
        const int x2 = 6 + (i + 1) * (304 / (AUDIO_HISTORY_COUNT - 1));

        const int y1 = 82 - static_cast<int>(audioHistory[idx1] * 48.0f);
        const int y2 = 82 - static_cast<int>(audioHistory[idx2] * 48.0f);

        const uint16_t waveColor = (audioHistory[idx2] >= fishingSimAudioThreshold) ? TFT_RED : COLOR_BORDER_BRIGHT;
        fishing_display.drawLine(x1, y1, x2, y2, waveColor);
    }

    // Horizontal threshold line across scope
    const int scopeThreshY = 82 - static_cast<int>(fishingSimAudioThreshold * 48.0f);
    fishing_display.drawFastHLine(6, std::max(scopeThreshY, 28), 304, TFT_WHITE);

    // Overlay text on scope
    const paxg::audio::AudioDetectorMetrics m = s_detector.getMetrics();
    char scopeText[64];
    std::snprintf(scopeText, sizeof(scopeText), "RMS: %5.0f | PEAK: %5u | NOISE: %5.0f", m.rms, m.peak, m.noiseFloor);
    fishing_display.setTextColor(TFT_WHITE);
    fishing_display.setCursor(8, 30);
    fishing_display.print(scopeText);

    // 3. Telemetry Matrix (y: 90, h: 62)
    fishing_display.fillRoundRect(2, 90, 156, 62, 3, COLOR_PANEL_BG);
    fishing_display.drawRoundRect(2, 90, 156, 62, 3, COLOR_BORDER);

    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(8, 94);
    fishing_display.print("DETECTOR CONFIG");

    char cfgBuf1[32];
    std::snprintf(cfgBuf1, sizeof(cfgBuf1), "Abs Thresh: %ld", static_cast<long>(s_settings.absoluteThreshold));
    fishing_display.setCursor(8, 106);
    fishing_display.print(cfgBuf1);

    char cfgBuf2[32];
    std::snprintf(cfgBuf2, sizeof(cfgBuf2), "Dyn Thresh: %.0f", m.effectiveThreshold);
    fishing_display.setCursor(8, 118);
    fishing_display.print(cfgBuf2);

    char cfgBuf3[32];
    std::snprintf(cfgBuf3, sizeof(cfgBuf3), "Debounce: %u frames", static_cast<unsigned int>(s_audioConfig.debounceFrames));
    fishing_display.setCursor(8, 130);
    fishing_display.print(cfgBuf3);

    char cfgBuf4[32];
    std::snprintf(cfgBuf4, sizeof(cfgBuf4), "Cooldown: %u frames", static_cast<unsigned int>(s_audioConfig.cooldownFrames));
    fishing_display.setCursor(8, 142);
    fishing_display.print(cfgBuf4);

    // Right Box: Metrics & Telemetry
    fishing_display.fillRoundRect(162, 90, 156, 62, 3, COLOR_PANEL_BG);
    fishing_display.drawRoundRect(162, 90, 156, 62, 3, COLOR_BORDER);

    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(168, 94);
    fishing_display.print("BUFFER & EVENTS");

    char mBuf1[32];
    std::snprintf(mBuf1, sizeof(mBuf1), "Triggers: %u", static_cast<unsigned int>(m.triggerCount));
    fishing_display.setTextColor(m.triggerCount > 0 ? TFT_GREEN : COLOR_TEXT_DIM);
    fishing_display.setCursor(168, 106);
    fishing_display.print(mBuf1);

    char mBuf2[32];
    std::snprintf(mBuf2, sizeof(mBuf2), "RingBuf : %u / 2048", static_cast<unsigned int>(s_detector.getBufferedSampleCount()));
    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(168, 118);
    fishing_display.print(mBuf2);

    char mBuf3[32];
    std::snprintf(mBuf3, sizeof(mBuf3), "Frames  : %llu", static_cast<unsigned long long>(m.totalFramesProcessed));
    fishing_display.setCursor(168, 130);
    fishing_display.print(mBuf3);

    fishing_display.setTextColor(0x7BEF);
    fishing_display.setCursor(168, 142);
    fishing_display.print("SRC: SYNTH (NO UAC)");

    // 4. Footer Hint Bar (y: 154, h: 14)
    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(6, 156);
    fishing_display.print("UP:Page/Hold:Run | DN:Exit/Hold:Stop | BOTH:Arm");
}

// =============================================================================
// Page 3: Settings (Interactive NVS preferences adjustment)
// =============================================================================
static void render_page_settings()
{
    fishing_display.fillSprite(COLOR_BG);

    // 1. Header (y: 2, h: 22)
    fishing_display.drawRoundRect(2, 2, 316, 22, 3, COLOR_BORDER);
    fishing_display.setTextSize(1);
    fishing_display.setTextColor(COLOR_BORDER_BRIGHT);
    fishing_display.setCursor(8, 7);
    fishing_display.print("FISHING SETTINGS [3/3 SET]");

    fishing_display.setTextColor(TFT_GREEN);
    fishing_display.setCursor(174, 7);
    fishing_display.print("[NVS SAVED]");

    fishing_display.setTextColor(TFT_LIGHTGREY);
    fishing_display.setCursor(252, 7);
    fishing_display.print("DISARM DEF");

    // 2. Settings Menu Card (y: 26, h: 126)
    fishing_display.fillRoundRect(2, 26, 316, 126, 3, COLOR_PANEL_BG);
    fishing_display.drawRoundRect(2, 26, 316, 126, 3, COLOR_BORDER);

    const char* rowLabels[SETTINGS_ROW_COUNT] = {
        "1. Threshold RMS",
        "2. Wait Timeout",
        "3. Cooldown Time",
        "4. Cast Key",
        "5. Interact Key",
        "6. [ RETURN TO DASHBOARD ]"
    };

    char valBuf[SETTINGS_ROW_COUNT][36];
    std::snprintf(valBuf[0], sizeof(valBuf[0]), "%ld  (500-5000)", static_cast<long>(s_settings.absoluteThreshold));
    std::snprintf(valBuf[1], sizeof(valBuf[1]), "%lds  (5-30s)", static_cast<long>(s_settings.waitTimeoutMs / 1000UL));
    std::snprintf(valBuf[2], sizeof(valBuf[2]), "%.1fs  (0.5-5.0s)", s_settings.cooldownMs / 1000.0f);
    if (s_settings.castKey == ' ')
    {
        std::snprintf(valBuf[3], sizeof(valBuf[3]), "SPACE  (0x%02X)", s_settings.castKey);
    }
    else
    {
        std::snprintf(valBuf[3], sizeof(valBuf[3]), "'%c'  (0x%02X)", s_settings.castKey, s_settings.castKey);
    }
    if (s_settings.interactKey == ' ')
    {
        std::snprintf(valBuf[4], sizeof(valBuf[4]), "SPACE  (0x%02X)", s_settings.interactKey);
    }
    else
    {
        std::snprintf(valBuf[4], sizeof(valBuf[4]), "'%c'  (0x%02X)", s_settings.interactKey, s_settings.interactKey);
    }
    std::snprintf(valBuf[5], sizeof(valBuf[5]), "");

    for (int r = 0; r < SETTINGS_ROW_COUNT; ++r)
    {
        const int rowY = 30 + r * 20;
        const bool isSelected = (r == s_settingsSelectedRow);

        if (isSelected)
        {
            fishing_display.fillRoundRect(6, rowY - 2, 308, 18, 2, COLOR_ROW_HIGHLIGHT);
            fishing_display.drawRoundRect(6, rowY - 2, 308, 18, 2, COLOR_BORDER_BRIGHT);
            fishing_display.setTextColor(COLOR_BORDER_BRIGHT);
        }
        else
        {
            fishing_display.setTextColor(COLOR_TEXT_DIM);
        }

        fishing_display.setCursor(12, rowY + 3);
        fishing_display.print(isSelected ? "> " : "  ");
        fishing_display.print(rowLabels[r]);

        if (valBuf[r][0] != '\0')
        {
            fishing_display.setTextColor(isSelected ? TFT_WHITE : COLOR_TEXT_DIM);
            fishing_display.setCursor(170, rowY + 3);
            fishing_display.print(valBuf[r]);
        }
    }

    // 4. Footer Hint Bar (y: 154, h: 14)
    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(6, 156);
    fishing_display.print("UP:Sel/Hold:Dash | DN:Adjust/Hold:Reset | BOTH:Arm");
}

void fishing_render()
{
    switch (fishingCurrentPage)
    {
        case FishingPage::Dashboard:
            render_page_dashboard();
            break;
        case FishingPage::AudioMonitor:
            render_page_audio_monitor();
            break;
        case FishingPage::Settings:
            render_page_settings();
            break;
    }

    // DMA push frame to LCD panel
    fishing_display.pushSprite(0, 0);
}
