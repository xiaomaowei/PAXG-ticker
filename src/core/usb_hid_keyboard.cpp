#include <USB.h>
#include <USBHIDKeyboard.h>
#include <modules/app_state.h>
#include <modules/constants.h>
#include <modules/hal/usb_hid_keyboard.h>
#include <modules/modes/fishing_mode.h>

#include <atomic>

namespace
{

// Stock Arduino ESP32 USBHIDKeyboard instance (standard descriptors, no evasion/anticheat modification)
static USBHIDKeyboard keyboard;

enum class KeyState : uint8_t
{
    Idle,
    Pressed,
    Cooldown
};

static uint8_t keyQueue[FISHING_KEY_QUEUE_CAPACITY] = {0};
static size_t queueHead = 0;
static size_t queueTail = 0;
static size_t queueCount = 0;

static KeyState keyState = KeyState::Idle;
static uint8_t activeKey = 0;
static unsigned long activeKeyTimestamp = 0;

// Thread-safe asynchronous signal for pending disconnect or suspend from the USB event task.
// Cross-thread mutation of queue/keyboard state is strictly avoided by deferring consumption
// to usb_hid_keyboard_tick on the Arduino main loop.
static std::atomic<bool> s_pendingDisconnect{false};

/**
 * Hardware / State Safety Gate:
 * Transmission is permitted IF AND ONLY IF:
 * 1. USB device is actually mounted on host (_started && tinyusb_device_mounted)
 * 2. Current display mode is DisplayMode::Fishing
 * 3. Explicit armed flag (fishingArmed) is enabled by the user
 * 4. Fishing state machine is actively in an execution state (Casting, Waiting, BiteDetected, Reeling)
 *
 * Absolutely NO HID transmission is permitted if unmounted, in PAXG, Clock, Weather,
 * Idle, Paused, Cooldown, Timeout, or Error states.
 */
bool is_gating_permitted()
{
    if (!usb_hid_is_connected())
    {
        return false;
    }
    if (mode != DisplayMode::Fishing)
    {
        return false;
    }
    if (!fishingArmed)
    {
        return false;
    }
    if (fishingState != FishingState::Casting &&
        fishingState != FishingState::Waiting &&
        fishingState != FishingState::BiteDetected &&
        fishingState != FishingState::Reeling)
    {
        return false;
    }
    return true;
}

static void usb_event_callback(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == ARDUINO_USB_EVENTS)
    {
        // Only treat disconnect and suspend events. Resume or mount events do NOT re-arm.
        if (event_id == ARDUINO_USB_STOPPED_EVENT || event_id == ARDUINO_USB_SUSPEND_EVENT)
        {
            // Thread-safe signal only; do NOT mutate queue or call releaseAll directly from USB event task
            s_pendingDisconnect.store(true, std::memory_order_release);
        }
    }
}

}  // namespace

void usb_hid_keyboard_init()
{
    USB.onEvent(usb_event_callback);
    keyboard.begin();
    USB.begin();
}

void usb_hid_keyboard_clear_and_release_all()
{
    queueHead = 0;
    queueTail = 0;
    queueCount = 0;

    keyState = KeyState::Idle;
    activeKey = 0;
    activeKeyTimestamp = 0;

    keyboard.releaseAll();
}

bool usb_hid_keyboard_enqueue(uint8_t key)
{
    if (key == 0)
    {
        return false;
    }
    if (!is_gating_permitted())
    {
        return false;
    }
    if (queueCount >= FISHING_KEY_QUEUE_CAPACITY)
    {
        return false;  // Bounded buffer drop
    }

    keyQueue[queueTail] = key;
    queueTail = (queueTail + 1) % FISHING_KEY_QUEUE_CAPACITY;
    queueCount++;
    return true;
}

void usb_hid_keyboard_tick()
{
    // 1. Consume thread-safe asynchronous disconnect/suspend signal on the Arduino loop task
    if (s_pendingDisconnect.exchange(false, std::memory_order_acq_rel))
    {
        // Disconnect forces auto-disarm to prevent automated key transmissions upon reconnect
        fishingArmed = false;
        if (mode == DisplayMode::Fishing && fishingState != FishingState::Idle && fishingState != FishingState::Paused)
        {
            fishing_trigger_error("USB DISCONNECTED");
        }
        usb_hid_keyboard_clear_and_release_all();
        return;
    }

    // 2. Strict safety check: if safety conditions fail at any point (e.g. unmounted, disarmed,
    // paused, stopped, mode switched), immediately purge the queue and release all keys.
    if (!is_gating_permitted())
    {
        if (queueCount > 0 || keyState != KeyState::Idle || activeKey != 0)
        {
            usb_hid_keyboard_clear_and_release_all();
        }
        return;
    }

    const unsigned long now = millis();

    switch (keyState)
    {
        case KeyState::Idle:
        {
            if (queueCount > 0)
            {
                activeKey = keyQueue[queueHead];
                queueHead = (queueHead + 1) % FISHING_KEY_QUEUE_CAPACITY;
                queueCount--;

                keyboard.press(activeKey);
                activeKeyTimestamp = now;
                keyState = KeyState::Pressed;
                fishingLiveKeySentCount++;
            }
            break;
        }

        case KeyState::Pressed:
        {
            // Guaranteed separate service tick: hold key down until FISHING_KEY_PRESS_HOLD_MS expires
            if (now - activeKeyTimestamp >= FISHING_KEY_PRESS_HOLD_MS)
            {
                keyboard.release(activeKey);
                activeKeyTimestamp = now;
                keyState = KeyState::Cooldown;
            }
            break;
        }

        case KeyState::Cooldown:
        {
            // Guaranteed separate service tick: inter-key gap before next key can be pressed
            if (now - activeKeyTimestamp >= FISHING_KEY_RELEASE_GAP_MS)
            {
                activeKey = 0;
                keyState = KeyState::Idle;
            }
            break;
        }
    }
}

bool usb_hid_is_connected()
{
    // The framework's ESPUSB bool operator checks (_started && tinyusb_device_mounted)
    return static_cast<bool>(USB);
}

size_t usb_hid_queue_depth()
{
    return queueCount;
}
