#include <TFT_eSPI.h>
#include <modules/app_state.h>
#include <modules/constants.h>
#include <modules/modes/fishing_mode.h>
#include <modules/sprites.h>
#include <modules/variables.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{

// Palette: Dark / Blue-Cyan Theme
constexpr uint16_t COLOR_BG = 0x0005;               // Ultra-dark background
constexpr uint16_t COLOR_PANEL_BG = 0x0113;         // Deep navy-cyan card background
constexpr uint16_t COLOR_BORDER = 0x0338;           // Soft cyan card border
constexpr uint16_t COLOR_BORDER_BRIGHT = TFT_CYAN;  // 0x07FF vibrant cyan
constexpr uint16_t COLOR_TEXT_DIM = 0x9CD3;         // Dim cyan-grey
constexpr uint16_t COLOR_BAR_BG = 0x018C;           // Progress bar unfilled track
constexpr uint16_t COLOR_PILL_STOPPED = 0x2945;     // Slate grey
constexpr uint16_t COLOR_PILL_PAUSED = 0x5A80;      // Dark amber
constexpr uint16_t COLOR_PILL_RUNNING = 0x0324;     // Dark forest cyan

// State durations (milliseconds)
constexpr unsigned long CASTING_DURATION_MS = 1500UL;
constexpr unsigned long REELING_DURATION_MS = 1200UL;
constexpr unsigned long BITE_SPIKE_WINDOW_MS = 700UL;

// Audio history sparkline samples
constexpr int AUDIO_HISTORY_COUNT = 24;
float audioHistory[AUDIO_HISTORY_COUNT] = {0.0f};
int audioHistoryIndex = 0;
unsigned long lastHistorySampleTime = 0;
constexpr unsigned long HISTORY_SAMPLE_INTERVAL_MS = 80UL;

unsigned long lastSimTick = 0;

float generate_synthetic_noise()
{
    // Ambient sound floor with small sinus drift and random jitter
    const float phase = static_cast<float>(currentMillis % 4000) / 4000.0f * 6.2831853f;
    const float wave = (std::sin(phase) + 1.0f) * 0.05f;                 // 0.00 to 0.10
    const float jitter = (static_cast<float>(rand() % 100) / 100.0f) * 0.08f;  // 0.00 to 0.08
    return 0.12f + wave + jitter;                                       // ~0.12 to 0.30
}

void record_audio_sample(float level)
{
    if (currentMillis - lastHistorySampleTime >= HISTORY_SAMPLE_INTERVAL_MS)
    {
        audioHistory[audioHistoryIndex] = level;
        audioHistoryIndex = (audioHistoryIndex + 1) % AUDIO_HISTORY_COUNT;
        lastHistorySampleTime = currentMillis;
    }
}

}  // namespace

void fishing_mode_init()
{
    if (fishing_display.getPointer() == nullptr)
    {
        fishing_display.createSprite(320, 170);
    }

    fishingState = FishingState::Stopped;
    fishingStateStartTime = 0;
    fishingSessionStartTime = 0;
    fishingTotalUptimeMs = 0;
    fishingSimReelCmds = 0;
    fishingSimTimeouts = 0;
    fishingSimAudioLevel = 0.05f;
    fishingSimAudioThreshold = 0.75f;
    fishingAudioDetected = false;
    fishingNextBiteDelayMs = 0;
    fishingWaitMaxMs = 20000UL;

    for (int i = 0; i < AUDIO_HISTORY_COUNT; ++i)
    {
        audioHistory[i] = 0.05f;
    }
}

void fishing_toggle_start_pause()
{
    if (fishingState == FishingState::Stopped)
    {
        // Start simulation
        fishingState = FishingState::Casting;
        fishingStateStartTime = currentMillis;
        fishingSessionStartTime = currentMillis;
        fishingTotalUptimeMs = 0;
        fishingSimReelCmds = 0;
        fishingSimTimeouts = 0;
        fishingAudioDetected = false;
    }
    else if (fishingState == FishingState::Paused)
    {
        // Resume simulation
        fishingState = FishingState::Casting;
        fishingStateStartTime = currentMillis;
        fishingAudioDetected = false;
    }
    else
    {
        // Pause active simulation
        fishingState = FishingState::Paused;
        fishingAudioDetected = false;
        fishingSimAudioLevel = 0.05f;
    }
}

void fishing_stop_reset()
{
    if (fishingState != FishingState::Stopped)
    {
        // First press: Stop simulation
        fishingState = FishingState::Stopped;
        fishingAudioDetected = false;
        fishingSimAudioLevel = 0.05f;
    }
    else
    {
        // Second press when already stopped: Reset counters
        fishingSimReelCmds = 0;
        fishingSimTimeouts = 0;
        fishingTotalUptimeMs = 0;
    }
}

void fishing_sim_tick()
{
    if (lastSimTick == 0)
    {
        lastSimTick = currentMillis;
        return;
    }

    const unsigned long delta = currentMillis - lastSimTick;
    if (delta < FISHING_SIM_TICK_INTERVAL)
    {
        return;
    }
    lastSimTick = currentMillis;

    // Track active session uptime
    if (fishingState != FishingState::Stopped && fishingState != FishingState::Paused)
    {
        fishingTotalUptimeMs += delta;
    }

    switch (fishingState)
    {
        case FishingState::Stopped:
        {
            fishingSimAudioLevel = 0.05f;
            fishingAudioDetected = false;
            break;
        }

        case FishingState::Paused:
        {
            fishingSimAudioLevel = 0.05f;
            fishingAudioDetected = false;
            break;
        }

        case FishingState::Casting:
        {
            fishingSimAudioLevel = generate_synthetic_noise() * 0.7f;
            fishingAudioDetected = false;

            if (currentMillis - fishingStateStartTime >= CASTING_DURATION_MS)
            {
                // Transition to WaitingBite; pick synthetic bite schedule (4.5s to 15.0s)
                fishingState = FishingState::WaitingBite;
                fishingStateStartTime = currentMillis;
                fishingNextBiteDelayMs = 4500UL + (rand() % 10500UL);
            }
            break;
        }

        case FishingState::WaitingBite:
        {
            const unsigned long waitElapsed = currentMillis - fishingStateStartTime;

            // Check if bite sound window is triggered
            if (waitElapsed >= fishingNextBiteDelayMs &&
                waitElapsed < (fishingNextBiteDelayMs + BITE_SPIKE_WINDOW_MS))
            {
                // Synthetic bite audio burst (exceeds threshold)
                fishingSimAudioLevel = 0.82f + (static_cast<float>(rand() % 16) / 100.0f);
                if (fishingSimAudioLevel >= fishingSimAudioThreshold)
                {
                    fishingAudioDetected = true;
                    // Trigger simulated reel command and transition
                    fishingSimReelCmds++;
                    fishingState = FishingState::Reeling;
                    fishingStateStartTime = currentMillis;
                }
            }
            else if (waitElapsed >= fishingWaitMaxMs)
            {
                // Cast timed out (20s reached with no bite event)
                fishingSimTimeouts++;
                fishingAudioDetected = false;
                fishingState = FishingState::Casting;
                fishingStateStartTime = currentMillis;
            }
            else
            {
                // Normal ambient noise floor
                fishingSimAudioLevel = generate_synthetic_noise();
                fishingAudioDetected = false;
            }
            break;
        }

        case FishingState::Reeling:
        {
            fishingAudioDetected = false;
            fishingSimAudioLevel = 0.22f + (static_cast<float>(rand() % 10) / 100.0f);

            if (currentMillis - fishingStateStartTime >= REELING_DURATION_MS)
            {
                // Cycle back to next cast
                fishingState = FishingState::Casting;
                fishingStateStartTime = currentMillis;
            }
            break;
        }
    }

    record_audio_sample(fishingSimAudioLevel);
}

void fishing_render()
{
    fishing_display.fillSprite(COLOR_BG);

    // =========================================================================
    // 1. Header Bar (y: 2, height: 22)
    // =========================================================================
    fishing_display.drawRoundRect(2, 2, 316, 22, 3, COLOR_BORDER);

    fishing_display.setTextSize(1);
    fishing_display.setTextColor(COLOR_BORDER_BRIGHT);
    fishing_display.setCursor(8, 7);
    fishing_display.print("WOW FISHING");

    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(84, 7);
    fishing_display.print("[SIMULATION]");

    // State Pill
    if (fishingState == FishingState::Stopped)
    {
        fishing_display.fillRoundRect(164, 4, 60, 18, 2, COLOR_PILL_STOPPED);
        fishing_display.setTextColor(TFT_LIGHTGREY);
        fishing_display.setCursor(170, 7);
        fishing_display.print("STOPPED");
    }
    else if (fishingState == FishingState::Paused)
    {
        fishing_display.fillRoundRect(164, 4, 56, 18, 2, COLOR_PILL_PAUSED);
        fishing_display.setTextColor(TFT_YELLOW);
        fishing_display.setCursor(172, 7);
        fishing_display.print("PAUSED");
    }
    else
    {
        fishing_display.fillRoundRect(164, 4, 62, 18, 2, COLOR_PILL_RUNNING);
        fishing_display.setTextColor(TFT_GREEN);
        fishing_display.setCursor(170, 7);
        fishing_display.print("RUNNING");
    }

    // Explicit Hardware Status Flag
    fishing_display.setTextColor(TFT_ORANGE);
    fishing_display.setCursor(232, 7);
    fishing_display.print("USB: UNAVAIL");

    // =========================================================================
    // 2. Telemetry Cards (y: 26, height: 75)
    // =========================================================================
    // Left Card: Simulated Reel Count & Timeouts
    fishing_display.fillRoundRect(2, 26, 156, 75, 3, COLOR_PANEL_BG);
    fishing_display.drawRoundRect(2, 26, 156, 75, 3, COLOR_BORDER);

    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(8, 31);
    fishing_display.print("SIM REEL CMDS");

    char reelBuf[16];
    std::snprintf(reelBuf, sizeof(reelBuf), "%u", fishingSimReelCmds);
    fishing_display.setTextSize(3);
    fishing_display.setTextColor(COLOR_BORDER_BRIGHT);
    fishing_display.setCursor(10, 43);
    fishing_display.print(reelBuf);

    fishing_display.setTextSize(1);
    fishing_display.setTextColor(TFT_YELLOW);
    fishing_display.setCursor(8, 72);
    fishing_display.print("Simulated (no catch data)");

    char timeoutBuf[24];
    std::snprintf(timeoutBuf, sizeof(timeoutBuf), "Timeouts: %u", fishingSimTimeouts);
    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(8, 85);
    fishing_display.print(timeoutBuf);

    // Right Card: Phase, Wait Progress & Session Uptime
    fishing_display.fillRoundRect(162, 26, 156, 75, 3, COLOR_PANEL_BG);
    fishing_display.drawRoundRect(162, 26, 156, 75, 3, COLOR_BORDER);

    // State Display
    fishing_display.setCursor(168, 31);
    switch (fishingState)
    {
        case FishingState::Stopped:
            fishing_display.setTextColor(TFT_LIGHTGREY);
            fishing_display.print("STATE: IDLE");
            break;
        case FishingState::Paused:
            fishing_display.setTextColor(TFT_YELLOW);
            fishing_display.print("STATE: PAUSED");
            break;
        case FishingState::Casting:
            fishing_display.setTextColor(0x5D3F);  // Sky blue
            fishing_display.print("STATE: CASTING...");
            break;
        case FishingState::WaitingBite:
            fishing_display.setTextColor(COLOR_BORDER_BRIGHT);
            fishing_display.print("STATE: WAITING BITE");
            break;
        case FishingState::Reeling:
            fishing_display.setTextColor(TFT_GREEN);
            fishing_display.print("STATE: REELING! [BITE]");
            break;
    }

    // Wait Timer / Cast Timer
    char waitBuf[32];
    if (fishingState == FishingState::WaitingBite)
    {
        const float waitSec = (currentMillis - fishingStateStartTime) / 1000.0f;
        std::snprintf(waitBuf, sizeof(waitBuf), "Wait: %.1fs / 20.0s", waitSec);
        fishing_display.setTextColor(TFT_WHITE);
        fishing_display.setCursor(168, 44);
        fishing_display.print(waitBuf);

        // Visual Wait Progress Bar (w: 142, h: 4)
        fishing_display.fillRect(168, 55, 142, 4, COLOR_BAR_BG);
        const int progressW = static_cast<int>((waitSec / 20.0f) * 142.0f);
        if (progressW > 0)
        {
            fishing_display.fillRect(168, 55, std::min(progressW, 142), 4, COLOR_BORDER_BRIGHT);
        }
    }
    else if (fishingState == FishingState::Casting)
    {
        const float castSec = (currentMillis - fishingStateStartTime) / 1000.0f;
        std::snprintf(waitBuf, sizeof(waitBuf), "Cast: %.1fs / 1.5s", castSec);
        fishing_display.setTextColor(COLOR_TEXT_DIM);
        fishing_display.setCursor(168, 44);
        fishing_display.print(waitBuf);

        fishing_display.fillRect(168, 55, 142, 4, COLOR_BAR_BG);
        const int progressW = static_cast<int>((castSec / 1.5f) * 142.0f);
        if (progressW > 0)
        {
            fishing_display.fillRect(168, 55, std::min(progressW, 142), 4, 0x5D3F);
        }
    }
    else
    {
        fishing_display.setTextColor(COLOR_TEXT_DIM);
        fishing_display.setCursor(168, 44);
        fishing_display.print("Wait: --");
        fishing_display.fillRect(168, 55, 142, 4, COLOR_BAR_BG);
    }

    // Session Uptime
    char upBuf[24];
    const unsigned long uptimeSec = fishingTotalUptimeMs / 1000UL;
    std::snprintf(upBuf, sizeof(upBuf), "Uptime: %02lu:%02lu", (uptimeSec / 60) % 60, uptimeSec % 60);
    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(168, 64);
    fishing_display.print(upBuf);

    // USB Physical States Explicit Notice
    fishing_display.setTextColor(TFT_RED);
    fishing_display.setCursor(168, 77);
    fishing_display.print("USB HID: NO KEYSTROKES");

    fishing_display.setTextColor(0x7BEF);  // Darker grey
    fishing_display.setCursor(168, 88);
    fishing_display.print("UAC: NOT CONNECTED");

    // =========================================================================
    // 3. Simulated Audio Detector & Waveform (y: 103, height: 49)
    // =========================================================================
    fishing_display.fillRoundRect(2, 103, 316, 49, 3, COLOR_PANEL_BG);
    fishing_display.drawRoundRect(2, 103, 316, 49, 3, COLOR_BORDER);

    // Audio Section Header
    char audioBuf[48];
    const int audioPct = static_cast<int>(fishingSimAudioLevel * 100.0f);
    const int threshPct = static_cast<int>(fishingSimAudioThreshold * 100.0f);
    std::snprintf(audioBuf, sizeof(audioBuf), "SIM AUDIO: %d%%  THRESH: %d%% (SYNTHETIC)",
                  audioPct, threshPct);

    fishing_display.setTextColor(TFT_WHITE);
    fishing_display.setCursor(8, 108);
    fishing_display.print(audioBuf);

    // Audio Level Meter Bar (x: 8, y: 122, w: 180, h: 12)
    fishing_display.drawRect(8, 122, 180, 12, COLOR_BORDER);
    fishing_display.fillRect(9, 123, 178, 10, COLOR_BAR_BG);

    const int meterWidth = static_cast<int>(fishingSimAudioLevel * 178.0f);
    const uint16_t meterColor = (fishingSimAudioLevel >= fishingSimAudioThreshold) ? TFT_RED : COLOR_BORDER_BRIGHT;
    if (meterWidth > 0)
    {
        fishing_display.fillRect(9, 123, std::min(meterWidth, 178), 10, meterColor);
    }

    // Threshold Marker Tick (vertical bar at 75%)
    const int thresholdX = 9 + static_cast<int>(fishingSimAudioThreshold * 178.0f);
    fishing_display.drawFastVLine(thresholdX, 120, 16, TFT_WHITE);

    // Mini Synthetic Signal Sparkline (x: 198, y: 114, w: 114, h: 32)
    fishing_display.drawRect(198, 114, 114, 32, COLOR_BORDER);
    fishing_display.fillRect(199, 115, 112, 30, COLOR_BG);

    // Plot audio history samples
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

    // =========================================================================
    // 4. Footer Hint Bar (y: 156, height: 12)
    // =========================================================================
    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(14, 157);
    fishing_display.print("HOLD UP: Start/Pause");

    fishing_display.setTextColor(TFT_WHITE);
    fishing_display.setCursor(154, 157);
    fishing_display.print("|");

    fishing_display.setTextColor(COLOR_TEXT_DIM);
    fishing_display.setCursor(168, 157);
    fishing_display.print("HOLD DOWN: Stop/Reset");

    // Push composite frame to LCD (single DMA/parallel block transfer - zero flicker)
    fishing_display.pushSprite(0, 0);
}
