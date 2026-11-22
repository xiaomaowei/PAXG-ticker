#pragma once

#ifdef __cplusplus
#include <cstdint>
#include <cstddef>
#include <atomic>
using std::atomic_uint_fast32_t;
using std::atomic_bool;
using std::memory_order_relaxed;
using std::memory_order_acquire;
using std::memory_order_release;
using std::memory_order_acq_rel;
using std::atomic_store_explicit;
using std::atomic_load_explicit;
using std::atomic_fetch_add_explicit;
using std::atomic_exchange_explicit;
using std::atomic_compare_exchange_weak_explicit;
#else
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef USB_AUDIO_QUEUE_MAX_SLOTS
#define USB_AUDIO_QUEUE_MAX_SLOTS 8
#endif

#ifndef USB_AUDIO_QUEUE_SLOT_SIZE
#define USB_AUDIO_QUEUE_SLOT_SIZE 2156
#endif

/**
 * @brief Slot attribute flags.
 */
enum {
    USB_AUDIO_SLOT_FLAG_NONE = 0,
    USB_AUDIO_SLOT_FLAG_DISCONTINUITY = (1 << 0), /*!< Packet preceded by an overflow drop */
};

/**
 * @brief Telemetry statistics for audio queue monitoring.
 */
typedef struct {
    atomic_uint_fast32_t packets_in;
    atomic_uint_fast32_t bytes_in;
    atomic_uint_fast32_t packets_out;
    atomic_uint_fast32_t bytes_out;
    atomic_uint_fast32_t packets_dropped;
    atomic_uint_fast32_t bytes_dropped;
    atomic_uint_fast32_t buffer_undersized_rejects;
    atomic_uint_fast32_t stale_generation_drops;
    atomic_uint_fast32_t current_depth;
    atomic_uint_fast32_t max_depth;
} usb_audio_queue_stats_t;

/**
 * @brief Single bounded audio packet slot.
 */
typedef struct {
    size_t len;
    uint32_t generation;
    uint32_t flags;
    uint8_t data[USB_AUDIO_QUEUE_SLOT_SIZE];
} usb_audio_slot_t;

/**
 * @brief Lock-free Single-Producer Single-Consumer (SPSC) bounded audio ring queue.
 *
 * Strict Storage Ownership Rules:
 * - `head` is EXCLUSIVELY written by the producer task.
 * - `tail` is EXCLUSIVELY written by the consumer task.
 * - Producer / lifecycle tasks NEVER write or reset `tail`.
 * - Stream restarts and close events advance `stream_generation`.
 * - Stale slots from prior generations are retired exclusively by the consumer advancing `tail`.
 */
typedef struct {
    usb_audio_slot_t slots[USB_AUDIO_QUEUE_MAX_SLOTS];
    size_t capacity;
    atomic_uint_fast32_t head;
    atomic_uint_fast32_t tail;
    atomic_uint_fast32_t stream_generation;
    atomic_bool drop_pending;
    usb_audio_queue_stats_t stats;
} usb_audio_queue_t;

/**
 * @brief Snapshot of queue telemetry statistics (plain integers).
 */
typedef struct {
    uint32_t packets_in;
    uint32_t bytes_in;
    uint32_t packets_out;
    uint32_t bytes_out;
    uint32_t packets_dropped;
    uint32_t bytes_dropped;
    uint32_t buffer_undersized_rejects;
    uint32_t stale_generation_drops;
    uint32_t current_depth;
    uint32_t max_depth;
} usb_audio_queue_stats_snapshot_t;

/**
 * @brief Initialize the audio ring queue.
 */
void usb_audio_queue_init(usb_audio_queue_t *q, size_t capacity);

/**
 * @brief Advance stream generation epoch counter.
 */
uint32_t usb_audio_queue_advance_generation(usb_audio_queue_t *q);

/**
 * @brief Reset cumulative telemetry statistics.
 */
void usb_audio_queue_reset_stats(usb_audio_queue_t *q);

/**
 * @brief Enqueue a packet from the USB receiver (wait-free, strictly non-blocking).
 */
bool usb_audio_queue_enqueue(usb_audio_queue_t *q, const uint8_t *data, size_t len, uint32_t generation);

/**
 * @brief Dequeue a packet with extended slot flags.
 */
bool usb_audio_queue_dequeue_ext(usb_audio_queue_t *q, uint8_t *out_buf, size_t max_len, size_t *out_len, uint32_t *out_generation, uint32_t *out_flags);

/**
 * @brief Dequeue a packet into consumer-owned storage (backwards compatible wrapper).
 */
bool usb_audio_queue_dequeue(usb_audio_queue_t *q, uint8_t *out_buf, size_t max_len, size_t *out_len, uint32_t *out_generation);

/**
 * @brief Query number of currently queued packets.
 */
size_t usb_audio_queue_count(const usb_audio_queue_t *q);

/**
 * @brief Check if queue is empty.
 */
bool usb_audio_queue_is_empty(const usb_audio_queue_t *q);

/**
 * @brief Check if queue is full.
 */
bool usb_audio_queue_is_full(const usb_audio_queue_t *q);

/**
 * @brief Retrieve a snapshot of queue telemetry statistics.
 */
void usb_audio_queue_get_stats(const usb_audio_queue_t *q, usb_audio_queue_stats_snapshot_t *out_stats);

#ifdef __cplusplus
}
#endif
