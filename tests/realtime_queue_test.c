/* Exercise the production result queue with out-of-order worker completion.
 * Unused model/worker sections are discarded by the standalone test linker. */
#include "yolo/rknn_yolo.c"
#include <assert.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t done;
    int index, completed, status;
    unsigned long long seq;
} queue_waiter_t;

static void *pop_one(void *arg)
{
    queue_waiter_t *waiter = arg;
    yolo_job_t job = {0};
    int status = yolo_queue_pop(&job, waiter->index);
    pthread_mutex_lock(&waiter->lock);
    waiter->status = status;
    waiter->seq = job.seq;
    waiter->completed = 1;
    pthread_cond_signal(&waiter->done);
    pthread_mutex_unlock(&waiter->lock);
    return NULL;
}

static void init_waiter(queue_waiter_t *waiter, int index)
{
    memset(waiter, 0, sizeof(*waiter));
    waiter->index = index;
    pthread_mutex_init(&waiter->lock, NULL);
    pthread_cond_init(&waiter->done, NULL);
}

static int wait_completed(queue_waiter_t *waiter, int timeout_ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += (long)timeout_ms * 1000000L;
    until.tv_sec += until.tv_nsec / 1000000000L;
    until.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&waiter->lock);
    while (!waiter->completed)
        if (pthread_cond_timedwait(&waiter->done, &waiter->lock, &until)) break;
    int done = waiter->completed;
    pthread_mutex_unlock(&waiter->lock);
    return done;
}

static void destroy_waiter(queue_waiter_t *waiter)
{
    pthread_cond_destroy(&waiter->done);
    pthread_mutex_destroy(&waiter->lock);
}

static void check_adaptive_policy(void)
{
    adaptive_controller_t control;
    adaptive_observation_t good = {.fps = 29.1, .camera_fps = 30, .latency_ms = 134};
    adaptive_observation_t too_few = {.fps = 26.5, .camera_fps = 30, .latency_ms = 172};
    adaptive_observation_t contention = {.fps = 25.67, .camera_fps = 30, .latency_ms = 220};
    assert(adaptive_controller_init(&control, 2, 6) == 4);
    assert(adaptive_controller_observe(&control, 12000000u, &good) == 2);
    assert(control.phase == ADAPT_TRIAL);
    assert(adaptive_controller_observe(&control, 17500000u, &too_few) == 4);
    assert(control.phase == ADAPT_BASELINE);
    assert(adaptive_controller_observe(&control, 23000000u, &good) == 6);
    assert(adaptive_controller_observe(&control, 28500000u, &contention) == 4);
    assert(control.phase == ADAPT_HOLD);
    assert(adaptive_controller_observe(&control, 33000000u, &good) == 4);
    assert(control.phase == ADAPT_HOLD);
    adaptive_observation_t changed = {.fps = 20, .camera_fps = 30, .latency_ms = 300};
    assert(adaptive_controller_observe(&control, 38000000u, &changed) == 4);
    assert(adaptive_controller_observe(&control, 43000000u, &changed) == 4);
    assert(adaptive_controller_observe(&control, 100000000u, &changed) == 4);
    assert(control.phase == ADAPT_HOLD && control.unstable_votes == 0);
    adaptive_observation_t noise = {.fps = 25, .camera_fps = 30, .latency_ms = 180};
    assert(adaptive_controller_observe(&control, 149000000u, &noise) == 4);
    assert(adaptive_controller_observe(&control, 153000000u, &noise) == 4);
    assert(adaptive_controller_observe(&control, 157000000u, &noise) == 4);
    assert(control.phase == ADAPT_HOLD && control.unstable_votes == 0);
    assert(adaptive_controller_observe(&control, 161000000u, &changed) == 4);
    assert(adaptive_controller_observe(&control, 165000000u, &changed) == 4);
    assert(adaptive_controller_observe(&control, 169000000u, &changed) == 2);
    assert(control.phase == ADAPT_TRIAL);

    /* A faster/light model can retain the lower count; it is not fixed at 4. */
    adaptive_observation_t light = {.fps = 30, .camera_fps = 30, .latency_ms = 70};
    adaptive_observation_t light_lower = {.fps = 30, .camera_fps = 30, .latency_ms = 60};
    assert(adaptive_controller_init(&control, 2, 6) == 4);
    assert(adaptive_controller_observe(&control, 12000000u, &light) == 2);
    assert(adaptive_controller_observe(&control, 17500000u, &light_lower) == 2);
    assert(adaptive_controller_observe(&control, 23000000u, &light_lower) == 6);
    assert(adaptive_controller_observe(&control, 28500000u, &light_lower) == 2);
    assert(control.phase == ADAPT_HOLD);
    assert(adaptive_controller_observe(&control, 220000000u, &light_lower) == 4);
    assert(control.phase == ADAPT_TRIAL);

    /* A useful throughput improvement can select the larger count. */
    adaptive_observation_t limited = {.fps = 20, .camera_fps = 30, .latency_ms = 140};
    adaptive_observation_t improved = {.fps = 28, .camera_fps = 30, .latency_ms = 150};
    assert(adaptive_controller_init(&control, 2, 6) == 4);
    assert(adaptive_controller_observe(&control, 12000000u, &limited) == 2);
    assert(adaptive_controller_observe(&control, 17500000u, &too_few) == 2);
    assert(adaptive_controller_observe(&control, 23000000u, &too_few) == 6);
    assert(adaptive_controller_observe(&control, 28500000u, &improved) == 6);
    assert(control.phase == ADAPT_HOLD);
    assert(adaptive_controller_init(&control, 1, 1) == 1);
    assert(adaptive_controller_observe(&control, 12000000u, &light) == 1);
    assert(control.phase == ADAPT_HOLD);
    puts("PASS: measured adaptive trials roll back contention, select useful concurrency and hold without oscillation");
}

static void check_adaptive_queue(void)
{
    pthread_mutex_init(&g_ai.q_lock, NULL);
    pthread_cond_init(&g_ai.q_not_empty, NULL);
    pthread_cond_init(&g_ai.q_not_full, NULL);
    g_ai.running = g_ai.pool_running = 1;
    __atomic_store_n(&g_ai.active_workers, 1, __ATOMIC_RELAXED);
    queue_waiter_t active, parked;
    init_waiter(&parked, 1);
    init_waiter(&active, 0);
    pthread_t active_thread, parked_thread;
    assert(pthread_create(&parked_thread, NULL, pop_one, &parked) == 0);
    usleep(10000);
    assert(pthread_create(&active_thread, NULL, pop_one, &active) == 0);
    usleep(10000);
    yolo_job_t job = {.seq = 42};
    assert(yolo_queue_push(&job) == 0);
    assert(wait_completed(&active, 500));
    assert(active.status == 0 && active.seq == 42);
    assert(!wait_completed(&parked, 10));
    pthread_join(active_thread, NULL);

    /* No active thread remains: queued work must stay parked until scale-up. */
    job.seq = 43;
    assert(yolo_queue_push(&job) == 0);
    assert(!wait_completed(&parked, 10));
    pthread_mutex_lock(&g_ai.q_lock);
    __atomic_store_n(&g_ai.active_workers, 2, __ATOMIC_RELAXED);
    pthread_cond_broadcast(&g_ai.q_not_empty);
    pthread_mutex_unlock(&g_ai.q_lock);
    assert(wait_completed(&parked, 500));
    assert(parked.status == 0 && parked.seq == 43);
    pthread_join(parked_thread, NULL);
    destroy_waiter(&active);
    destroy_waiter(&parked);

    /* A parked worker must exit on shutdown even when work is queued. */
    init_waiter(&parked, 1);
    __atomic_store_n(&g_ai.active_workers, 1, __ATOMIC_RELAXED);
    assert(pthread_create(&parked_thread, NULL, pop_one, &parked) == 0);
    job.seq = 44;
    assert(yolo_queue_push(&job) == 0);
    assert(!wait_completed(&parked, 10));
    pthread_mutex_lock(&g_ai.q_lock);
    g_ai.pool_running = 0;
    pthread_cond_broadcast(&g_ai.q_not_empty);
    pthread_mutex_unlock(&g_ai.q_lock);
    assert(wait_completed(&parked, 500));
    assert(parked.status == -1);
    pthread_join(parked_thread, NULL);
    assert(g_ai.q_count == 1);
    destroy_waiter(&parked);
    g_ai.q_head = g_ai.q_tail = g_ai.q_count = 0;
    pthread_cond_destroy(&g_ai.q_not_empty);
    pthread_cond_destroy(&g_ai.q_not_full);
    pthread_mutex_destroy(&g_ai.q_lock);
    puts("PASS: parked workers do not steal wakeups; scale-up and shutdown release queue waiters");
}

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
    check_adaptive_policy();
    check_adaptive_queue();
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
    frame_ring_unlock(&ring);
    assert(frame_ring_wait_encode(&ring, 0, 2) == 1);
    frame_ring_lock(&ring);
    frame_slot_t *encoded = frame_ring_encode_pick_locked(&ring, 0);
    assert(encoded && encoded->seq == 2);
    frame_ring_encode_advance_locked(&ring, encoded);
    frame_ring_unlock(&ring);
    assert(frame_ring_wait_encode(&ring, 2, 2) == 0);
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
