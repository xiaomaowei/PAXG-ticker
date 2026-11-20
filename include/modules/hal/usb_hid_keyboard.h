#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Initializes the stock USB HID Keyboard subsystem alongside USB composite stack at boot.
 * Registers USB lifecycle event handlers for mount, unmount, and suspend.
 */
void usb_hid_keyboard_init();

/**
 * Nonblocking service tick for the USB HID Keyboard command queue.
 * Consumes thread-safe asynchronous disconnect/suspend signals on the Arduino loop,
 * enforces auto-disarm (fishingArmed=false), and services separate-tick press and release.
 * Strictly gated behind host-mounted state AND Fishing mode AND explicit user armed state AND running substate.
 */
void usb_hid_keyboard_tick();

/**
 * Enqueue a key press into the bounded command queue.
 * Rejected if host is not mounted, not in Fishing mode, not armed, or not in running state (Casting/WaitingBite/Reeling).
 *
 * @param key ASCII character or standard HID key code.
 * @return true if enqueued, false if rejected by safety gates or queue full.
 */
bool usb_hid_keyboard_enqueue(uint8_t key);

/**
 * Immediately clears all queued commands and calls releaseAll() on the stock keyboard interface.
 * Executed on disarm, pause, stop, mode switch, or USB disconnect/suspend consumption.
 */
void usb_hid_keyboard_clear_and_release_all();

/**
 * Checks if the USB device is mounted on a host according to the TinyUSB driver
 * (_started && tinyusb_device_mounted). Returns false if host is disconnected or suspended.
 */
bool usb_hid_is_connected();

/**
 * Returns the current depth of the bounded key queue.
 */
size_t usb_hid_queue_depth();
