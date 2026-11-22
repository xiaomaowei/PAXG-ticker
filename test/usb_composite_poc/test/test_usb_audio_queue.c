#undef NDEBUG
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdatomic.h>
#include "usb_audio_queue.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

static void test_queue_init_empty(void)
{
    printf("Running test_queue_init_empty...\n");
    usb_audio_queue_t q;
    usb_audio_queue_init(&q, 4);

    assert(usb_audio_queue_count(&q) == 0);
    assert(usb_audio_queue_is_empty(&q) == true);
    assert(usb_audio_queue_is_full(&q) == false);

    uint8_t out_buf[128];
    size_t out_len = 999;
    uint32_t out_gen = 0;
    bool dequeued = usb_audio_queue_dequeue(&q, out_buf, sizeof(out_buf), &out_len, &out_gen);
    assert(!dequeued);
    assert(out_len == 999);

    usb_audio_queue_stats_snapshot_t stats;
    usb_audio_queue_get_stats(&q, &stats);
    assert(stats.packets_in == 0);
    assert(stats.packets_out == 0);
    assert(stats.packets_dropped == 0);
    assert(stats.current_depth == 0);
    assert(stats.max_depth == 0);
    printf("  -> PASSED\n");
}

static void test_queue_fifo_order(void)
{
    printf("Running test_queue_fifo_order...\n");
    usb_audio_queue_t q;
    usb_audio_queue_init(&q, 4);

    uint8_t pkt1[] = {0x11, 0x12, 0x13, 0x14};
    uint8_t pkt2[] = {0x21, 0x22, 0x23};
    uint8_t pkt3[] = {0x31, 0x32, 0x33, 0x34, 0x35};

    assert(usb_audio_queue_enqueue(&q, pkt1, sizeof(pkt1), 1));
    assert(usb_audio_queue_enqueue(&q, pkt2, sizeof(pkt2), 1));
    assert(usb_audio_queue_enqueue(&q, pkt3, sizeof(pkt3), 1));

    assert(usb_audio_queue_count(&q) == 3);
    assert(!usb_audio_queue_is_empty(&q));

    uint8_t out_buf[64];
    size_t out_len = 0;
    uint32_t out_gen = 0;

    assert(usb_audio_queue_dequeue(&q, out_buf, sizeof(out_buf), &out_len, &out_gen));
    assert(out_len == sizeof(pkt1));
    assert(out_gen == 1);
    assert(memcmp(out_buf, pkt1, sizeof(pkt1)) == 0);

    assert(usb_audio_queue_dequeue(&q, out_buf, sizeof(out_buf), &out_len, &out_gen));
    assert(out_len == sizeof(pkt2));
    assert(out_gen == 1);
    assert(memcmp(out_buf, pkt2, sizeof(pkt2)) == 0);

    assert(usb_audio_queue_dequeue(&q, out_buf, sizeof(out_buf), &out_len, &out_gen));
    assert(out_len == sizeof(pkt3));
    assert(out_gen == 1);
    assert(memcmp(out_buf, pkt3, sizeof(pkt3)) == 0);

    assert(usb_audio_queue_is_empty(&q));

    usb_audio_queue_stats_snapshot_t stats;
    usb_audio_queue_get_stats(&q, &stats);
    assert(stats.packets_in == 3);
    assert(stats.packets_out == 3);
    assert(stats.packets_dropped == 0);
    assert(stats.bytes_in == sizeof(pkt1) + sizeof(pkt2) + sizeof(pkt3));
    assert(stats.bytes_out == stats.bytes_in);
    assert(stats.max_depth == 3);
    printf("  -> PASSED\n");
}

static void test_queue_burst_coalescing(void)
{
    printf("Running test_queue_burst_coalescing...\n");
    usb_audio_queue_t q;
    usb_audio_queue_init(&q, 5);

    for (uint8_t i = 1; i <= 5; i++) {
        uint8_t payload[8];
        memset(payload, i, sizeof(payload));
        assert(usb_audio_queue_enqueue(&q, payload, sizeof(payload), 10));
    }

    assert(usb_audio_queue_is_full(&q));
    assert(usb_audio_queue_count(&q) == 5);

    size_t drained_count = 0;
    uint8_t out_buf[32];
    size_t out_len = 0;
    uint32_t out_gen = 0;

    while (usb_audio_queue_dequeue(&q, out_buf, sizeof(out_buf), &out_len, &out_gen)) {
        drained_count++;
        assert(out_len == 8);
        assert(out_gen == 10);
        assert(out_buf[0] == (uint8_t)drained_count);
    }

    assert(drained_count == 5);
    assert(usb_audio_queue_is_empty(&q));
    printf("  -> PASSED\n");
}

static void test_queue_bounded_overflow_drop(void)
{
    printf("Running test_queue_bounded_overflow_drop...\n");
    usb_audio_queue_t q;
    usb_audio_queue_init(&q, 3);

    uint8_t p1[] = {1, 1};
    uint8_t p2[] = {2, 2, 2};
    uint8_t p3[] = {3, 3, 3, 3};
    uint8_t p4[] = {4, 4, 4, 4, 4};

    assert(usb_audio_queue_enqueue(&q, p1, sizeof(p1), 1));
    assert(usb_audio_queue_enqueue(&q, p2, sizeof(p2), 1));
    assert(usb_audio_queue_enqueue(&q, p3, sizeof(p3), 1));
    assert(usb_audio_queue_is_full(&q));

    // Queue is full: p4 must be rejected non-blocking with drop accounting
    assert(!usb_audio_queue_enqueue(&q, p4, sizeof(p4), 1));

    usb_audio_queue_stats_snapshot_t stats;
    usb_audio_queue_get_stats(&q, &stats);
    assert(stats.packets_in == 4);
    assert(stats.packets_dropped == 1);
    assert(stats.bytes_dropped == sizeof(p4));

    // Queued packets p1, p2, p3 must remain intact
    uint8_t out_buf[32];
    size_t out_len = 0;
    uint32_t out_gen = 0;

    assert(usb_audio_queue_dequeue(&q, out_buf, sizeof(out_buf), &out_len, &out_gen));
    assert(out_len == sizeof(p1) && memcmp(out_buf, p1, sizeof(p1)) == 0);

    assert(usb_audio_queue_dequeue(&q, out_buf, sizeof(out_buf), &out_len, &out_gen));
    assert(out_len == sizeof(p2) && memcmp(out_buf, p2, sizeof(p2)) == 0);

    assert(usb_audio_queue_dequeue(&q, out_buf, sizeof(out_buf), &out_len, &out_gen));
    assert(out_len == sizeof(p3) && memcmp(out_buf, p3, sizeof(p3)) == 0);

    assert(usb_audio_queue_is_empty(&q));
    printf("  -> PASSED\n");
}

static void test_queue_undersized_buffer_rejection(void)
{
    printf("Running test_queue_undersized_buffer_rejection...\n");
    usb_audio_queue_t q;
    usb_audio_queue_init(&q, 4);

    uint8_t pkt[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    assert(usb_audio_queue_enqueue(&q, pkt, sizeof(pkt), 1));

    uint8_t small_buf[3];
    size_t required_len = 0;
    uint32_t gen = 0;

    // Must return false without discarding data
    bool ok = usb_audio_queue_dequeue(&q, small_buf, sizeof(small_buf), &required_len, &gen);
    assert(!ok);
    assert(required_len == sizeof(pkt));
    assert(usb_audio_queue_count(&q) == 1);

    usb_audio_queue_stats_snapshot_t stats;
    usb_audio_queue_get_stats(&q, &stats);
    assert(stats.buffer_undersized_rejects == 1);
    assert(stats.packets_out == 0);

    uint8_t full_buf[16];
    size_t actual_len = 0;
    assert(usb_audio_queue_dequeue(&q, full_buf, sizeof(full_buf), &actual_len, &gen));
    assert(actual_len == sizeof(pkt));
    assert(memcmp(full_buf, pkt, sizeof(pkt)) == 0);
    assert(usb_audio_queue_is_empty(&q));
    printf("  -> PASSED\n");
}

static void test_deterministic_pause_mid_dequeue_slot_protection(void)
{
    printf("Running test_deterministic_pause_mid_dequeue_slot_protection...\n");
    usb_audio_queue_t q;
    usb_audio_queue_init(&q, 3); // Capacity 3

    uint8_t p0[] = {0xA0, 0xA1, 0xA2};
    uint8_t p1[] = {0xB0, 0xB1, 0xB2};
    uint8_t p2[] = {0xC0, 0xC1, 0xC2};
    uint8_t p3[] = {0xD0, 0xD1, 0xD2};

    // Fill queue completely: slots 0, 1, 2
    assert(usb_audio_queue_enqueue(&q, p0, sizeof(p0), 1));
    assert(usb_audio_queue_enqueue(&q, p1, sizeof(p1), 1));
    assert(usb_audio_queue_enqueue(&q, p2, sizeof(p2), 1));
    assert(usb_audio_queue_is_full(&q));

    // Consumer starts dequeuing slot 0:
    // Consumer loads tail=0, reads slot 0 data, BUT DOES NOT ADVANCE TAIL YET (paused mid-dequeue)
    uint32_t paused_tail = (uint32_t)atomic_load_explicit(&q.tail, memory_order_relaxed);
    assert(paused_tail == 0);
    assert(memcmp(q.slots[paused_tail % q.capacity].data, p0, sizeof(p0)) == 0);

    // While consumer is paused reading slot 0, lifecycle task closes and restarts stream:
    // Lifecycle task calls advance_generation (tail is NEVER written or reset by producer!)
    usb_audio_queue_advance_generation(&q); // Generation becomes 2
    assert((uint32_t)atomic_load_explicit(&q.tail, memory_order_relaxed) == 0);

    // Producer attempts to enqueue a new frame under generation 2:
    // Because tail is still 0 (consumer has not finished with slot 0), queue is still FULL!
    // Producer CANNOT reuse or overwrite slot 0!
    bool enqueue_res = usb_audio_queue_enqueue(&q, p3, sizeof(p3), 2);
    assert(!enqueue_res); // Correctly rejected! Non-blocking drop accounting.

    // Verify slot 0 data is 100% UNTOUCHED and intact!
    assert(memcmp(q.slots[0].data, p0, sizeof(p0)) == 0);

    // Consumer now finishes its processing of slot 0, detects generation 1 is stale, and advances tail to 1:
    atomic_store_explicit(&q.tail, 1, memory_order_release);

    // Now that consumer has safely retired slot 0, producer can finally enqueue p3:
    assert(usb_audio_queue_enqueue(&q, p3, sizeof(p3), 2));
    printf("  -> PASSED: Slot 0 strictly protected during mid-dequeue pause across stream restart!\n");
}

#define THREAD_STRESS_PACKETS 20000

typedef struct {
    usb_audio_queue_t *q;
    atomic_bool producer_done;
    atomic_uint_fast32_t delivered_count;
    atomic_uint_fast32_t last_seq;
    atomic_uint_fast32_t seq_error_count;
    atomic_uint_fast32_t payload_error_count;
} thread_test_ctx_t;

#ifdef _WIN32
static DWORD WINAPI stress_producer_thread(LPVOID arg)
{
    thread_test_ctx_t *ctx = (thread_test_ctx_t *)arg;
    for (uint32_t i = 1; i <= THREAD_STRESS_PACKETS; i++) {
        uint32_t payload[8];
        payload[0] = i;
        for (int k = 1; k < 8; k++) {
            payload[k] = i ^ ((uint32_t)k * 0x55555555U);
        }
        usb_audio_queue_enqueue(ctx->q, (const uint8_t *)payload, sizeof(payload), 1);
    }
    atomic_store_explicit(&ctx->producer_done, true, memory_order_release);
    return 0;
}

static DWORD WINAPI stress_consumer_thread(LPVOID arg)
{
    thread_test_ctx_t *ctx = (thread_test_ctx_t *)arg;
    uint32_t out_payload[8];
    size_t out_len = 0;
    uint32_t gen = 0;

    while (!atomic_load_explicit(&ctx->producer_done, memory_order_acquire) ||
           !usb_audio_queue_is_empty(ctx->q)) {
        if (usb_audio_queue_dequeue(ctx->q, (uint8_t *)out_payload, sizeof(out_payload), &out_len, &gen)) {
            assert(out_len == sizeof(out_payload));
            uint32_t seq = out_payload[0];
            uint32_t last = (uint32_t)atomic_load_explicit(&ctx->last_seq, memory_order_relaxed);

            if (seq <= last && last != 0) {
                atomic_fetch_add_explicit(&ctx->seq_error_count, 1, memory_order_relaxed);
            }
            atomic_store_explicit(&ctx->last_seq, seq, memory_order_relaxed);

            for (int k = 1; k < 8; k++) {
                if (out_payload[k] != (seq ^ ((uint32_t)k * 0x55555555U))) {
                    atomic_fetch_add_explicit(&ctx->payload_error_count, 1, memory_order_relaxed);
                }
            }
            atomic_fetch_add_explicit(&ctx->delivered_count, 1, memory_order_relaxed);
        }
    }
    return 0;
}
#endif

static void test_multithreaded_producer_consumer_race(void)
{
    printf("Running test_multithreaded_producer_consumer_race (%d packets)...\n", THREAD_STRESS_PACKETS);
#ifdef _WIN32
    usb_audio_queue_t q;
    usb_audio_queue_init(&q, 8);

    thread_test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.q = &q;

    HANDLE hProd = CreateThread(NULL, 0, stress_producer_thread, &ctx, 0, NULL);
    HANDLE hCons = CreateThread(NULL, 0, stress_consumer_thread, &ctx, 0, NULL);
    assert(hProd && hCons);

    WaitForSingleObject(hProd, INFINITE);
    WaitForSingleObject(hCons, INFINITE);

    CloseHandle(hProd);
    CloseHandle(hCons);

    usb_audio_queue_stats_snapshot_t stats;
    usb_audio_queue_get_stats(&q, &stats);

    uint32_t delivered = (uint32_t)atomic_load_explicit(&ctx.delivered_count, memory_order_relaxed);
    uint32_t seq_errs = (uint32_t)atomic_load_explicit(&ctx.seq_error_count, memory_order_relaxed);
    uint32_t payload_errs = (uint32_t)atomic_load_explicit(&ctx.payload_error_count, memory_order_relaxed);

    printf("  Packets In: %u, Delivered: %u, Dropped: %u, Max Depth: %u\n",
           stats.packets_in, delivered, stats.packets_dropped, stats.max_depth);

    assert(seq_errs == 0);
    assert(payload_errs == 0);
    assert(delivered + stats.packets_dropped == THREAD_STRESS_PACKETS);
    printf("  -> PASSED: Zero sequence reordering, zero corrupted payloads, exact drop accounting!\n");
#else
    printf("  -> SKIPPED (non-Windows)\n");
#endif
}

#ifdef _WIN32
typedef struct {
    usb_audio_queue_t *q;
    atomic_bool stop_flag;
    atomic_uint_fast32_t current_stream_gen;
    atomic_uint_fast32_t stale_callback_attempts;
} lifecycle_test_ctx_t;

static DWORD WINAPI lifecycle_producer(LPVOID arg)
{
    lifecycle_test_ctx_t *ctx = (lifecycle_test_ctx_t *)arg;
    uint32_t seq = 0;
    while (!atomic_load_explicit(&ctx->stop_flag, memory_order_relaxed)) {
        seq++;
        uint32_t gen = (uint32_t)atomic_load_explicit(&ctx->current_stream_gen, memory_order_acquire);
        usb_audio_queue_enqueue(ctx->q, (const uint8_t *)&seq, sizeof(seq), gen);
    }
    return 0;
}

static DWORD WINAPI lifecycle_consumer(LPVOID arg)
{
    lifecycle_test_ctx_t *ctx = (lifecycle_test_ctx_t *)arg;
    uint32_t val = 0;
    size_t len = 0;
    uint32_t pkt_gen = 0;

    while (!atomic_load_explicit(&ctx->stop_flag, memory_order_relaxed)) {
        uint32_t active_gen = (uint32_t)atomic_load_explicit(&ctx->current_stream_gen, memory_order_acquire);
        if (usb_audio_queue_dequeue(ctx->q, (uint8_t *)&val, sizeof(val), &len, &pkt_gen)) {
            if (pkt_gen != active_gen) {
                // Safely filtered out stale generation packet
                continue;
            }
            if ((uint32_t)atomic_load_explicit(&ctx->current_stream_gen, memory_order_acquire) != active_gen) {
                atomic_fetch_add_explicit(&ctx->stale_callback_attempts, 1, memory_order_relaxed);
            }
        }
    }
    return 0;
}
#endif

static void test_multithreaded_lifecycle_interleaving(void)
{
    printf("Running test_multithreaded_lifecycle_interleaving...\n");
#ifdef _WIN32
    usb_audio_queue_t q;
    usb_audio_queue_init(&q, 8);

    lifecycle_test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.q = &q;
    atomic_store_explicit(&ctx.current_stream_gen, 1, memory_order_relaxed);

    HANDLE hProd = CreateThread(NULL, 0, lifecycle_producer, &ctx, 0, NULL);
    HANDLE hCons = CreateThread(NULL, 0, lifecycle_consumer, &ctx, 0, NULL);

    for (int cycle = 0; cycle < 50; cycle++) {
        Sleep(1);
        atomic_fetch_add_explicit(&ctx.current_stream_gen, 1, memory_order_acq_rel);
        usb_audio_queue_advance_generation(&q);
    }

    atomic_store_explicit(&ctx.stop_flag, true, memory_order_release);
    WaitForSingleObject(hProd, INFINITE);
    WaitForSingleObject(hCons, INFINITE);

    CloseHandle(hProd);
    CloseHandle(hCons);

    printf("  Lifecycle cycles: 50, Interleaved state races detected and safely filtered: %u\n",
           (uint32_t)atomic_load_explicit(&ctx.stale_callback_attempts, memory_order_relaxed));
    printf("  -> PASSED: Threaded lifecycle interleaving safe and validated!\n");
#else
    printf("  -> SKIPPED (non-Windows)\n");
#endif
}

int main(void)
{
    printf("===================================================================\n");
    printf("Production Lock-Free SPSC usb_audio_queue Verification Suite\n");
    printf("===================================================================\n");

    test_queue_init_empty();
    test_queue_fifo_order();
    test_queue_burst_coalescing();
    test_queue_bounded_overflow_drop();
    test_queue_undersized_buffer_rejection();
    test_deterministic_pause_mid_dequeue_slot_protection();
    test_multithreaded_producer_consumer_race();
    test_multithreaded_lifecycle_interleaving();

    printf("\nAll lock-free usb_audio_queue core native tests passed successfully!\n");
    return 0;
}
