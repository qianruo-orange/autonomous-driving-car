/* Exercise the production result queue with out-of-order worker completion.
 * Unused model/worker sections are discarded by the standalone test linker. */
#include "ai/rknn_yolo.c"
#include <assert.h>

static void check_latest(const unsigned long long *seqs, size_t count,
                         unsigned long long expected)
{
    for (size_t i = 0; i < count; i++) {
        yolo_result_item_t item = {0};
        item.seq = seqs[i];
        assert(yolo_result_push(&item) == 0);
    }
    yolo_result_item_t latest;
    assert(yolo_result_pop_latest(&latest, 5) == 0);
    assert(latest.seq == expected);
    assert(g_ai.r_count == 0);
}

int main(void)
{
    pthread_mutex_init(&g_ai.r_lock, NULL);
    pthread_cond_init(&g_ai.r_not_empty, NULL);
    pthread_cond_init(&g_ai.r_not_full, NULL);
    g_ai.running = g_ai.pool_running = 1;
    const unsigned long long a[] = {101, 103, 102};
    const unsigned long long b[] = {205, 199};
    const unsigned long long c[] = {300, 308, 302, 307, 301, 306, 305, 304};
    check_latest(a, 3, 103);
    check_latest(b, 2, 205);
    check_latest(c, 8, 308);
    yolo_result_item_t empty;
    assert(yolo_result_pop_latest(&empty, 2) == -1);
    puts("PASS: freshest sequence survives out-of-order worker completion and queue bursts");
    pthread_cond_destroy(&g_ai.r_not_empty);
    pthread_cond_destroy(&g_ai.r_not_full);
    pthread_mutex_destroy(&g_ai.r_lock);
    frame_ring_t ring;
    frame_ring_init(&ring);
    frame_ring_set_ai_active(&ring, 1);
    frame_ring_set_rec_active(&ring, 1);
    frame_ring_lock(&ring);
    ring.capture_fps_num = 30;
    ring.capture_fps_den = 1;
    for (int i = 0; i < 2; i++) {
        frame_slot_t *slot = frame_ring_produce_slot_locked(&ring);
        assert(slot);
        slot->raw.buf = malloc(1);
        assert(slot->raw.buf);
        slot->raw.len = slot->raw.cap = 1;
        slot->raw_present = slot->infer_done = 1;
        frame_ring_produce_commit_locked(&ring);
        assert(slot->capture_ms > 0);
        frame_ring_buf_take_locked(&ring, FRAME_RING_POOL_NV12, 16, &slot->nv12);
        frame_ring_display_commit_locked(&ring, slot);
    }
    frame_slot_t *encoded = frame_ring_encode_pick_locked(&ring, 0);
    assert(encoded && encoded->seq == 2);
    frame_ring_encode_advance_locked(&ring, encoded);
    frame_ring_unlock(&ring);
    frame_ring_stats_t stats;
    frame_ring_stats(&ring, &stats);
    assert(stats.capture_fps == 30);
    assert(stats.display_seq == 2 && stats.displayed_frames == 2);
    assert(stats.encoded_frames == 1 && stats.encode_replaced == 1);
    assert(stats.processing_age_max_ms >= stats.processing_age_ms);
    frame_ring_destroy(&ring);
    puts("PASS: stage metrics distinguish rendered frames, encoded frames and latest-frame replacement");
    return 0;
}
