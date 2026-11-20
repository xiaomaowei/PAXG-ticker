#include <TFT_eSPI.h>
#include <modules/app_controller.h>
#include <modules/app_state.h>
#include <modules/constants.h>
#include <modules/display_setup.h>
#include <modules/hal/brightness_control.h>
#include <modules/modes/clock_mode.h>
#include <modules/modes/fishing_mode.h>
#include <modules/modes/paxg_mode.h>
#include <modules/modes/weather_mode.h>
#include <modules/sprites.h>
#include <modules/variables.h>
#include <modules/wifi_connect.h>
#include <stdlib.h>
#include <time.h>

#ifndef APP_TIMEZONE
#define APP_TIMEZONE ""
#endif

namespace
{

constexpr int MODE_COUNT = 4;
constexpr unsigned long SERIAL_BAUD_RATE = 115200UL;
constexpr const char* NTP_SERVER = "pool.ntp.org";

// Button debounce and gesture tracking state
bool buttonUpPhysicalPrev = false;
bool buttonDownPhysicalPrev = false;

bool buttonUpDebounced = false;
bool buttonDownDebounced = false;

unsigned long buttonUpDebounceMillis = 0;
unsigned long buttonDownDebounceMillis = 0;

unsigned long buttonUpPressStartTime = 0;
unsigned long buttonDownPressStartTime = 0;

bool buttonUpLongPressHandled = false;
bool buttonDownLongPressHandled = false;

void update_mode_variables()
{
    changed_mode = false;
    lastModeUpdate = currentMillis;
}

/**
 * Button Handling with Debounce and Release Semantics:
 *
 * - Debounce: 50ms stable interval filters physical contact chatter.
 * - Mode Navigation Outside Fishing:
 *   On button release outside Fishing mode, navigate mode forward (Button UP) or
 *   backward (Button DOWN) as before, regardless of how long the button was held.
 * - In-Mode Gestures (Fishing Mode only):
 *   * Short Press (< 800ms, on release) -> Navigate mode forward/backward as usual.
 *   * Long Press (Hold >= 800ms)        -> Trigger Fishing action immediately:
 *       - Button UP: Start / Pause / Resume simulation.
 *       - Button DOWN: Stop simulation (1st hold) / Reset counters (2nd hold).
 *     Sets buttonUpLongPressHandled / buttonDownLongPressHandled to suppress mode
 *     cycling on the subsequent release exclusively for the in-mode gesture.
 * - Setup Non-Interference:
 *   Wi-Fi configuration and keyboard input execute in app_setup() and do not use app_loop() buttons.
 */
void handle_mode_switch_buttons()
{
    const bool rawUpPressed = (digitalRead(BUTTON_UP) == LOW);
    const bool rawDownPressed = (digitalRead(BUTTON_DOWN) == LOW);

    // --- Debounce & Process Button UP (GPIO 14) ---
    if (rawUpPressed != buttonUpPhysicalPrev)
    {
        buttonUpDebounceMillis = currentMillis;
        buttonUpPhysicalPrev = rawUpPressed;
    }

    if ((currentMillis - buttonUpDebounceMillis) >= MODE_SWITCH_DEBOUNCE_MS)
    {
        if (rawUpPressed != buttonUpDebounced)
        {
            buttonUpDebounced = rawUpPressed;
            if (buttonUpDebounced)
            {
                // Press edge
                buttonUpPressStartTime = currentMillis;
                buttonUpLongPressHandled = false;
            }
            else
            {
                // Release edge:
                // Outside Fishing mode, navigate as before regardless of hold length.
                // In Fishing mode, navigate only if in-mode long-press gesture was not handled.
                if (mode != DisplayMode::Fishing || !buttonUpLongPressHandled)
                {
                    mode = static_cast<DisplayMode>((static_cast<int>(mode) + 1) % MODE_COUNT);
                    changed_mode = true;
                    tft.fillScreen(TFT_BLACK);
                }
                buttonUpLongPressHandled = false;
            }
        }
    }

    // Long-press hold detection for Button UP: active exclusively in Fishing mode
    if (mode == DisplayMode::Fishing && buttonUpDebounced && !buttonUpLongPressHandled)
    {
        if ((currentMillis - buttonUpPressStartTime) >= BUTTON_LONG_PRESS_MS)
        {
            fishing_toggle_start_pause();
            buttonUpLongPressHandled = true;
        }
    }

    // --- Debounce & Process Button DOWN (GPIO 0) ---
    if (rawDownPressed != buttonDownPhysicalPrev)
    {
        buttonDownDebounceMillis = currentMillis;
        buttonDownPhysicalPrev = rawDownPressed;
    }

    if ((currentMillis - buttonDownDebounceMillis) >= MODE_SWITCH_DEBOUNCE_MS)
    {
        if (rawDownPressed != buttonDownDebounced)
        {
            buttonDownDebounced = rawDownPressed;
            if (buttonDownDebounced)
            {
                // Press edge
                buttonDownPressStartTime = currentMillis;
                buttonDownLongPressHandled = false;
            }
            else
            {
                // Release edge:
                // Outside Fishing mode, navigate as before regardless of hold length.
                // In Fishing mode, navigate only if in-mode long-press gesture was not handled.
                if (mode != DisplayMode::Fishing || !buttonDownLongPressHandled)
                {
                    mode = static_cast<DisplayMode>((static_cast<int>(mode) - 1 + MODE_COUNT) % MODE_COUNT);
                    changed_mode = true;
                    tft.fillScreen(TFT_BLACK);
                }
                buttonDownLongPressHandled = false;
            }
        }
    }

    // Long-press hold detection for Button DOWN: active exclusively in Fishing mode
    if (mode == DisplayMode::Fishing && buttonDownDebounced && !buttonDownLongPressHandled)
    {
        if ((currentMillis - buttonDownPressStartTime) >= BUTTON_LONG_PRESS_MS)
        {
            fishing_stop_reset();
            buttonDownLongPressHandled = true;
        }
    }
}

void render_active_mode()
{
    // Each mode has its own cadence to keep UI responsive while avoiding unnecessary redraws.
    switch (mode)
    {
        case DisplayMode::PAXG:
            paxg_logo_rotation();
            if (currentMillis - lastModeUpdate >= MODE0_UPDATE_INTERVAL || changed_mode)
            {
                paxg_render();
                update_mode_variables();
            }
            break;
        case DisplayMode::Clock:
            if (currentMillis - lastModeUpdate >= MODE1_UPDATE_INTERVAL || changed_mode)
            {
                if (changed_mode)
                {
                    clock_update_time();
                }
                clock_render();
                update_mode_variables();
            }
            break;
        case DisplayMode::Weather:
            if (currentMillis - lastModeUpdate >= MODE2_UPDATE_INTERVAL || changed_mode)
            {
                weather_render();
                update_mode_variables();
            }
            break;
        case DisplayMode::Fishing:
            fishing_sim_tick();
            if (currentMillis - lastModeUpdate >= MODE3_UPDATE_INTERVAL || changed_mode)
            {
                fishing_render();
                update_mode_variables();
            }
            break;
    }
}

}  // namespace

void app_setup()
{
    Serial.begin(SERIAL_BAUD_RATE);
    Serial.println("PAXG ticker starting up");
    initialize_display();
    initialize_input();
    initialize_sprites();
    configure_sprite_swap_bytes();

    connect_wifi();
    // ESP32 helper that applies TZ rules and starts NTP sync in one call.
    configTzTime(APP_TIMEZONE, NTP_SERVER);
    paxg_update_price();
    clock_update_time();
    adjust_brightness();
    paxg_sync_chart();
    fishing_mode_init();
}

void app_loop()
{
    currentMillis = millis();
    handle_mode_switch_buttons();

    // Data updates run independently from mode rendering, so mode switches stay snappy.
    if (currentMillis - lastPriceUpdate >= PRICE_UPDATE_INTERVAL)
    {
        paxg_update_price();
    }

    if (currentMillis - lastTimeUpdate >= TIME_UPDATE_INTERVAL)
    {
        clock_update_time();
        paxg_sync_chart();
    }

    // Apply smooth non-blocking brightness transitions continuously.
    adjust_brightness();

    render_active_mode();
}
