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
    adaptive_observation_t good = {.fps = 29.1, .camera_fps = 30, .latency_ms = 134, .worker_ms = 80};
    assert(adaptive_controller_init(&control, 2, 6) == 4);
    /* The old 180-second expiry forced a bad 4 -> 2 -> 6 exploration even
       while delivering camera FPS. Keep a smooth stream steady indefinitely. */
    for (unsigned long long second = 12; second < 1200; second++)
        assert(adaptive_controller_observe(&control, second * 1000000u, &good) == 4);
    assert(control.phase == ADAPT_HOLD);

    /* One low window is insufficient. Three sustained shortages can try more
       workers, but a contended trial rolls back after the first measurement. */
    adaptive_observation_t limited = {.fps = 24, .camera_fps = 30, .latency_ms = 170, .worker_ms = 80};
    adaptive_observation_t contention = {.fps = 22, .camera_fps = 30, .latency_ms = 240, .worker_ms = 100};
    assert(adaptive_controller_observe(&control, 1200000000u, &limited) == 4);
    assert(adaptive_controller_observe(&control, 1201000000u, &good) == 4);
    assert(adaptive_controller_observe(&control, 1202000000u, &limited) == 4);
    assert(adaptive_controller_observe(&control, 1203000000u, &limited) == 4);
    assert(adaptive_controller_observe(&control, 1204000000u, &limited) == 6);
    assert(control.phase == ADAPT_TRIAL);
    assert(adaptive_controller_observe(&control, 1205000000u, &contention) == 4);
    assert(control.best.fps == limited.fps); /* Bad trial is not the baseline. */
    for (unsigned long long second = 1206; second < 1600; second++)
        assert(adaptive_controller_observe(&control, second * 1000000u, &limited) == 4);
    assert(control.tested & adaptive_worker_bit(&control, 6));

    /* A real camera-rate change reopens a cached failure after three windows;
       elapsed wall time alone did not reopen it above. */
    adaptive_observation_t faster = {.fps = 30, .camera_fps = 60, .latency_ms = 170, .worker_ms = 80};
    adaptive_observation_t improved = {.fps = 40, .camera_fps = 60, .latency_ms = 180, .worker_ms = 100};
    assert(adaptive_controller_observe(&control, 1600000000u, &faster) == 4);
    assert(adaptive_controller_observe(&control, 1601000000u, &faster) == 4);
    assert(adaptive_controller_observe(&control, 1602000000u, &faster) == 4);
    assert(adaptive_controller_observe(&control, 1603000000u, &faster) == 4);
    assert(adaptive_controller_observe(&control, 1604000000u, &faster) == 6);
    assert(adaptive_controller_observe(&control, 1605000000u, &improved) == 6);
    assert(control.best_workers == 6 && control.phase == ADAPT_HOLD);

    /* A fast model with measured 25% spare capacity can safely try fewer
       workers. The selected lower pool must remain steady, not fixed at 4. */
    adaptive_observation_t light = {.fps = 30, .camera_fps = 30, .latency_ms = 70, .worker_ms = 40};
    adaptive_observation_t light_lower = {.fps = 30, .camera_fps = 30, .latency_ms = 60, .worker_ms = 38};
    assert(adaptive_controller_init(&control, 2, 6) == 4);
    assert(adaptive_controller_observe(&control, 12000000u, &light) == 4);
    assert(adaptive_controller_observe(&control, 13000000u, &light) == 4);
    assert(adaptive_controller_observe(&control, 14000000u, &light) == 2);
    assert(adaptive_controller_observe(&control, 15000000u, &light_lower) == 2);
    for (unsigned long long second = 16; second < 600; second++)
        assert(adaptive_controller_observe(&control, second * 1000000u, &light_lower) == 2);

    /* Unknown service time cannot justify a blind reduction. A nearly full
       smaller pool also lacks the safety margin needed to reduce workers. */
    good.worker_ms = 0;
    assert(adaptive_controller_init(&control, 2, 6) == 4);
    for (unsigned long long second = 1; second < 10; second++)
        assert(adaptive_controller_observe(&control, second * 1000000u, &good) == 4);
    good.worker_ms = 60;
    for (unsigned long long second = 10; second < 20; second++)
        assert(adaptive_controller_observe(&control, second * 1000000u, &good) == 4);

    /* Single-core/one-context platforms have no alternate pool to probe. */
    assert(adaptive_controller_init(&control, 1, 1) == 1);
    assert(adaptive_controller_observe(&control, 12000000u, &light) == 1);
    assert(control.phase == ADAPT_HOLD);
    puts("PASS: adaptive workers preserve smooth demand, require sustained evidence and cache failed trials");
}

static void check_adaptive_queue(void)
{
    pthread_mutex_init(&g_yolo.q_lock, NULL);
    pthread_cond_init(&g_yolo.q_not_empty, NULL);
    pthread_cond_init(&g_yolo.q_not_full, NULL);
    g_yolo.running = g_yolo.pool_running = 1;
    __atomic_store_n(&g_yolo.active_workers, 1, __ATOMIC_RELAXED);
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
    pthread_mutex_lock(&g_yolo.q_lock);
    __atomic_store_n(&g_yolo.active_workers, 2, __ATOMIC_RELAXED);
    pthread_cond_broadcast(&g_yolo.q_not_empty);
    pthread_mutex_unlock(&g_yolo.q_lock);
    assert(wait_completed(&parked, 500));
    assert(parked.status == 0 && parked.seq == 43);
    pthread_join(parked_thread, NULL);
    destroy_waiter(&active);
    destroy_waiter(&parked);

    /* A parked worker must exit on shutdown even when work is queued. */
    init_waiter(&parked, 1);
    __atomic_store_n(&g_yolo.active_workers, 1, __ATOMIC_RELAXED);
    assert(pthread_create(&parked_thread, NULL, pop_one, &parked) == 0);
    job.seq = 44;
    assert(yolo_queue_push(&job) == 0);
    assert(!wait_completed(&parked, 10));
    pthread_mutex_lock(&g_yolo.q_lock);
    g_yolo.pool_running = 0;
    pthread_cond_broadcast(&g_yolo.q_not_empty);
    pthread_mutex_unlock(&g_yolo.q_lock);
    assert(wait_completed(&parked, 500));
    assert(parked.status == -1);
    pthread_join(parked_thread, NULL);
    assert(g_yolo.q_count == 1);
    destroy_waiter(&parked);
    g_yolo.q_head = g_yolo.q_tail = g_yolo.q_count = 0;
    pthread_cond_destroy(&g_yolo.q_not_empty);
    pthread_cond_destroy(&g_yolo.q_not_full);
    pthread_mutex_destroy(&g_yolo.q_lock);
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
    assert(g_yolo.r_count == 0);
}

int main(void)
{
    check_adaptive_policy();
    check_adaptive_queue();
    pthread_mutex_init(&g_yolo.r_lock, NULL);
    pthread_cond_init(&g_yolo.r_not_empty, NULL);
    pthread_cond_init(&g_yolo.r_not_full, NULL);
    g_yolo.running = g_yolo.pool_running = 1;
    const unsigned long long a[] = {101, 103, 102};
    const unsigned long long b[] = {205, 199};
    const unsigned long long c[] = {300, 308, 302, 307, 301, 306, 305, 304};
    check_latest(a, 3, 103);
    check_latest(b, 2, 205);
    check_latest(c, 8, 308);
    yolo_result_item_t empty;
    assert(yolo_result_pop_latest(&empty, 2) == -1);
    puts("PASS: freshest sequence survives out-of-order worker completion and queue bursts");
    pthread_cond_destroy(&g_yolo.r_not_empty);
    pthread_cond_destroy(&g_yolo.r_not_full);
    pthread_mutex_destroy(&g_yolo.r_lock);
    frame_ring_t ring;
    frame_ring_init(&ring);
    frame_ring_set_yolo_active(&ring, 1);
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
