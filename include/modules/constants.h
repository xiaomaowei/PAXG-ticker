#pragma once

constexpr int BUTTON_DOWN = 0;
constexpr int BUTTON_UP = 14;

using Color = unsigned short;

// graphics
constexpr int row_height = 20;
constexpr int row_name_x_padding = 10;
constexpr int row_name_y_padding = 10;

// intervals
constexpr unsigned long TIME_UPDATE_INTERVAL = 5000UL;
constexpr unsigned long PRICE_UPDATE_INTERVAL = 60000UL;
constexpr unsigned long MODE0_UPDATE_INTERVAL = 60000UL;   // standard price
constexpr unsigned long MODE1_UPDATE_INTERVAL = 5000UL;    // clock
constexpr unsigned long MODE2_UPDATE_INTERVAL = 300000UL;  // weather
constexpr unsigned long MODE3_UPDATE_INTERVAL = 100UL;     // fishing dashboard UI (10Hz)
constexpr unsigned long FISHING_SIM_TICK_INTERVAL = 50UL;  // fishing sim logic tick (20Hz)
constexpr unsigned long FISHING_AUDIO_SAMPLE_RATE_HZ = 16000UL;  // 16kHz PCM
constexpr size_t FISHING_AUDIO_FRAME_SIZE = 128;                 // 128 samples per frame
constexpr unsigned long FISHING_AUDIO_FRAME_INTERVAL_MS = 8UL;   // Mathematically exact: 128 / 16000 = 8.0ms frame cadence
constexpr unsigned long ROTATION_INTERVAL = 15UL;          // PAXG badge rotation speed
constexpr int HOLD_TIME = 1000;                            // time needed to hold the button
constexpr unsigned long BUTTON_LONG_PRESS_MS = 800UL;      // long press duration threshold for mode gestures
constexpr unsigned long MODE_SWITCH_DEBOUNCE_MS = 50UL;    // contact bounce filter for mode switching

// Fishing HID Keyboard configurable key constants (safe placeholders)
constexpr uint8_t FISHING_KEY_CAST = '1';                  // Placeholder cast key
constexpr uint8_t FISHING_KEY_INTERACT = '2';              // Placeholder interact / reel key
constexpr unsigned long FISHING_KEY_PRESS_HOLD_MS = 60UL;  // Duration to hold key pressed across ticks
constexpr unsigned long FISHING_KEY_RELEASE_GAP_MS = 60UL; // Cooldown gap after release before next key
constexpr size_t FISHING_KEY_QUEUE_CAPACITY = 8;           // Bounded command queue depth
