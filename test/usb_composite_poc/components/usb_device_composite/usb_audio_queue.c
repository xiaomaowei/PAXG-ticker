#include "usb_audio_queue.h"
#include <string.h>

void usb_audio_queue_init(usb_audio_queue_t *q, size_t capacity)
{
    if (!q) {
        return;
    }
    memset(q, 0, sizeof(*q));
    if (capacity == 0 || capacity > USB_AUDIO_QUEUE_MAX_SLOTS) {
        capacity = USB_AUDIO_QUEUE_MAX_SLOTS;
    }
    q->capacity = capacity;
    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&q->stream_generation, 1, memory_order_relaxed);
    atomic_store_explicit(&q->drop_pending, false, memory_order_relaxed);
}

uint32_t usb_audio_queue_advance_generation(usb_audio_queue_t *q)
{
    if (!q) {
        return 0;
    }
    // Notice: tail is NEVER written here! Tail is exclusively owned by consumer.
    return (uint32_t)atomic_fetch_add_explicit(&q->stream_generation, 1, memory_order_acq_rel) + 1;
}

bool usb_audio_queue_enqueue(usb_audio_queue_t *q, const uint8_t *data, size_t len, uint32_t generation)
{
    if (!q || !data || len == 0 || len > USB_AUDIO_QUEUE_SLOT_SIZE || q->capacity == 0) {
        return false;
    }

    uint32_t tail = (uint32_t)atomic_load_explicit(&q->tail, memory_order_acquire);
    uint32_t head = (uint32_t)atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t count = head - tail;

    atomic_fetch_add_explicit(&q->stats.packets_in, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&q->stats.bytes_in, (uint32_t)len, memory_order_relaxed);

    if (count >= q->capacity) {
        // Queue is FULL: mark drop pending for the next post-gap packet
        atomic_store_explicit(&q->drop_pending, true, memory_order_relaxed);
        atomic_fetch_add_explicit(&q->stats.packets_dropped, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&q->stats.bytes_dropped, (uint32_t)len, memory_order_relaxed);
        return false;
    }

    uint32_t idx = head % q->capacity;
    memcpy(q->slots[idx].data, data, len);
    q->slots[idx].len = len;
    q->slots[idx].generation = generation;

    uint32_t flags = USB_AUDIO_SLOT_FLAG_NONE;
    if (atomic_exchange_explicit(&q->drop_pending, false, memory_order_relaxed)) {
        flags |= USB_AUDIO_SLOT_FLAG_DISCONTINUITY;
    }
    q->slots[idx].flags = flags;

    uint32_t new_depth = count + 1;
    atomic_store_explicit(&q->stats.current_depth, new_depth, memory_order_relaxed);

    uint32_t cur_max = (uint32_t)atomic_load_explicit(&q->stats.max_depth, memory_order_relaxed);
    while (new_depth > cur_max &&
           !atomic_compare_exchange_weak_explicit(&q->stats.max_depth, &cur_max, new_depth,
                                                  memory_order_relaxed, memory_order_relaxed)) {
    }

    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return true;
}

bool usb_audio_queue_dequeue_ext(usb_audio_queue_t *q, uint8_t *out_buf, size_t max_len, size_t *out_len, uint32_t *out_generation, uint32_t *out_flags)
{
    if (!q || !out_buf || max_len == 0 || q->capacity == 0) {
        return false;
    }

    uint32_t tail = (uint32_t)atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t head = (uint32_t)atomic_load_explicit(&q->head, memory_order_acquire);

    if (head == tail) {
        return false;
    }

    uint32_t idx = tail % q->capacity;
    size_t slot_len = q->slots[idx].len;

    // Reject undersized caller buffer without advancing tail and without losing data
    if (max_len < slot_len) {
        if (out_len) {
            *out_len = slot_len;
        }
        atomic_fetch_add_explicit(&q->stats.buffer_undersized_rejects, 1, memory_order_relaxed);
        return false;
    }

    memcpy(out_buf, q->slots[idx].data, slot_len);
    if (out_len) {
        *out_len = slot_len;
    }
    if (out_generation) {
        *out_generation = q->slots[idx].generation;
    }
    if (out_flags) {
        *out_flags = q->slots[idx].flags;
    }

    atomic_fetch_add_explicit(&q->stats.packets_out, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&q->stats.bytes_out, (uint32_t)slot_len, memory_order_relaxed);

    uint32_t rem = head - (tail + 1);
    atomic_store_explicit(&q->stats.current_depth, rem, memory_order_relaxed);

    // Tail is EXCLUSIVELY advanced here by the consumer!
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return true;
}

bool usb_audio_queue_dequeue(usb_audio_queue_t *q, uint8_t *out_buf, size_t max_len, size_t *out_len, uint32_t *out_generation)
{
    return usb_audio_queue_dequeue_ext(q, out_buf, max_len, out_len, out_generation, NULL);
}

size_t usb_audio_queue_count(const usb_audio_queue_t *q)
{
    if (!q) return 0;
    uint32_t head = (uint32_t)atomic_load_explicit(&q->head, memory_order_acquire);
    uint32_t tail = (uint32_t)atomic_load_explicit(&q->tail, memory_order_acquire);
    return (head >= tail) ? (size_t)(head - tail) : 0;
}

bool usb_audio_queue_is_empty(const usb_audio_queue_t *q)
{
    return usb_audio_queue_count(q) == 0;
}

bool usb_audio_queue_is_full(const usb_audio_queue_t *q)
{
    if (!q) return false;
    return usb_audio_queue_count(q) >= q->capacity;
}

void usb_audio_queue_reset_stats(usb_audio_queue_t *q)
{
    if (!q) return;
    atomic_store_explicit(&q->stats.packets_in, 0, memory_order_relaxed);
    atomic_store_explicit(&q->stats.bytes_in, 0, memory_order_relaxed);
    atomic_store_explicit(&q->stats.packets_out, 0, memory_order_relaxed);
    atomic_store_explicit(&q->stats.bytes_out, 0, memory_order_relaxed);
    atomic_store_explicit(&q->stats.packets_dropped, 0, memory_order_relaxed);
    atomic_store_explicit(&q->stats.bytes_dropped, 0, memory_order_relaxed);
    atomic_store_explicit(&q->stats.buffer_undersized_rejects, 0, memory_order_relaxed);
    atomic_store_explicit(&q->stats.stale_generation_drops, 0, memory_order_relaxed);
    atomic_store_explicit(&q->stats.max_depth, 0, memory_order_relaxed);
}

void usb_audio_queue_get_stats(const usb_audio_queue_t *q, usb_audio_queue_stats_snapshot_t *out_stats)
{
    if (!q || !out_stats) return;
    out_stats->packets_in = (uint32_t)atomic_load_explicit(&q->stats.packets_in, memory_order_relaxed);
    out_stats->bytes_in = (uint32_t)atomic_load_explicit(&q->stats.bytes_in, memory_order_relaxed);
    out_stats->packets_out = (uint32_t)atomic_load_explicit(&q->stats.packets_out, memory_order_relaxed);
    out_stats->bytes_out = (uint32_t)atomic_load_explicit(&q->stats.bytes_out, memory_order_relaxed);
    out_stats->packets_dropped = (uint32_t)atomic_load_explicit(&q->stats.packets_dropped, memory_order_relaxed);
    out_stats->bytes_dropped = (uint32_t)atomic_load_explicit(&q->stats.bytes_dropped, memory_order_relaxed);
    out_stats->buffer_undersized_rejects = (uint32_t)atomic_load_explicit(&q->stats.buffer_undersized_rejects, memory_order_relaxed);
    out_stats->stale_generation_drops = (uint32_t)atomic_load_explicit(&q->stats.stale_generation_drops, memory_order_relaxed);
    out_stats->current_depth = (uint32_t)atomic_load_explicit(&q->stats.current_depth, memory_order_relaxed);
    out_stats->max_depth = (uint32_t)atomic_load_explicit(&q->stats.max_depth, memory_order_relaxed);
}
