/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "control/car_sport_ctrl.h"
#include "can/can_socket.h"
#include "can/can_motor.h"
#include "core/log.h"
#include "watchdog/watchdog.h"
#include "http/http_internal.h"
#include <math.h>
#include <sched.h>
#include <time.h>
#include <sys/utsname.h>

enum {
    ENABLED,
    CLASS,
    SPEED,
    TURN_DELTA,
    DEAD,
    TIMEOUT,
    VP,
    VI,
    VD,
    TP,
    TI,
    TD,
    WP,
    WI,
    WD,
    SEED_X,
    SEED_Y,
    MANUAL,
    SETPOINT,
    AREA,
    WHEEL_I_LIMIT,
    N
};
static const char *keys[N] = {"enabled",        "class_id",       "max_speed",
                              "max_turn_delta", "deadband",       "timeout_ms",
                              "speed_kp",       "speed_ki",       "speed_kd",
                              "turn_kp",        "turn_ki",        "turn_kd",
                              "wheel_kp",       "wheel_ki",       "wheel_kd",
                              "seed_x",         "seed_y",         "manual",
                              "setpoint",       "reference_area", "wheel_integral_limit"};
static const double defaults[N] = {0,    0, .3, .15, .04, 300, .4, 0,   0, .15, 0,
                                   .003, 1, .1, 0,   .5,  .5,  0,  .15, 0, 1};
/* Hard caps for the external agent source. These are deliberately tighter than
 * the profile limits a human can set from the UI, and are enforced here rather
 * than in the HTTP layer so that a compromised caller cannot widen them. */
#define AGENT_V_CAP      0.30  /* m/s */
#define AGENT_STEER_CAP  0.15  /* half differential, m/s */
#define AGENT_TTL_MIN_MS 100u
#define AGENT_TTL_MAX_MS 500u
/* How long a fresh command may wait for the decoder's first wheel frame
 * before the missing feedback is treated as an actuator fault. */
#define AGENT_FEEDBACK_GRACE_MS 500ull
/* 租约到期后先停车、再等这段时间：HTTP 只有一条事件循环线程，一次几秒的阻塞
 * 不等于执行器失联，不该要求操作员到现场重新 arm。超过宽限仍未续租才锁存。 */
#define AGENT_LEASE_GRACE_MS    1500ull
static struct {
    pthread_mutex_t lock, feedback_lock, save_lock;
    pthread_cond_t changed;
    atomic_int ready;
    int running, fifo, priority, rt_kernel, rt_error, selected, tracked;
    int motor_running, stop_required;
    app_ctx_t *app;
    follow_motor_ops_t motor;
    double cfg[N], ex, ey, area_error, left_target, right_target, left_output, right_output;
    double measured_left, measured_right, cx, cy;
    unsigned long long feedback_ms, accepted, processed, previous_ms;
    unsigned long generation, handled, capture_epoch;
    yolo_event_meta_t frame;
    follow_pid_t pid[4];
    const char *reason;
    /* External agent source: a leased velocity/steering command that replaces
     * the vision path for as long as it stays fresh. */
    struct {
        int state;
        double v, steer, traveled_m;
        unsigned long long expiry_ms, accepted_ms, accepted;
        char fault[32];
    } ag;
} g;
static unsigned long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
#ifdef ADCBENCH
/* Loop-back actuator for the bench build only. A real CAN decoder publishes
 * measured wheel speeds continuously; this mock republishes its last command
 * on every control tick so the freshness interlock behaves the same. */
static double bench_left, bench_right;
static int bench_send(app_ctx_t *app, double left, double right)
{
    (void)app;
    bench_left = left;
    bench_right = right;
    return 0;
}
static int bench_stop(app_ctx_t *app)
{
    (void)app;
    bench_left = bench_right = 0;
    return 0;
}
static const follow_motor_ops_t g_bench_motor = {bench_send, bench_stop};
static void bench_publish(void) { (void)follow_feedback(bench_left, bench_right, now_ms()); }
#endif

static int valid(const double *v)
{
    for (int i = 0; i < N; i++)
        if (!isfinite(v[i]) || v[i] < 0 || v[i] > 1000)
            return 0;
    for (int i = VP; i <= WD; i++)
        if (v[i] > 100)
            return 0;
    return v[VI] == 0 && v[TI] == 0 && v[WD] == 0 && v[ENABLED] <= 1 &&
           floor(v[ENABLED]) == v[ENABLED] && v[CLASS] <= 99 && floor(v[CLASS]) == v[CLASS] &&
           v[SPEED] > .0 && v[SPEED] <= 2 && v[TURN_DELTA] > 0 && v[TURN_DELTA] <= 4 &&
           v[DEAD] <= .5 && v[TIMEOUT] >= 100 && v[TIMEOUT] <= 1000 && v[SEED_X] <= 1 &&
           v[SEED_Y] <= 1 && v[MANUAL] <= 1 && floor(v[MANUAL]) == v[MANUAL] && v[SETPOINT] <= 2 &&
           (!v[MANUAL] || v[SETPOINT] <= v[SPEED]) && v[AREA] <= 1 && v[WHEEL_I_LIMIT] <= 1;
}
static void reset(void)
{
    memset(g.pid, 0, sizeof(g.pid));
    g.previous_ms = 0;
    g.tracked = 0;
    g.cx = g.cfg[SEED_X];
    g.cy = g.cfg[SEED_Y];
    g.left_target = g.right_target = g.left_output = g.right_output = 0;
    g.selected = -1;
}
static int stop_motor_if_needed(void)
{
    if (!g.motor.stop || (!g.motor_running && !g.stop_required))
        return 0;
    g.stop_required = 1;
    if (g.motor.stop(g.app) < 0)
        return -1;
    g.motor_running = g.stop_required = 0;
    return 0;
}
static void finish_motor_stop(void)
{
    const unsigned long long deadline = now_ms() + 100;
    unsigned attempts = 0;
    for (;;) {
        pthread_mutex_lock(&g.lock);
        int failed = stop_motor_if_needed() < 0;
        pthread_mutex_unlock(&g.lock);
        if (!failed)
            return;
        if (++attempts >= 100 || now_ms() >= deadline) {
            LOG_ERROR("car control: final motor stop remains pending after %u retries; "
                      "closing transport", attempts);
            return;
        }
        /* The worker has joined. Only its owner waits here, without holding
         * the controller lock or blocking a real-time control iteration. */
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
}
static void halt(const char *reason)
{
    g.left_target = g.right_target = g.left_output = g.right_output = 0;
    memset(g.pid, 0, sizeof(g.pid));
    g.previous_ms = 0;
    g.selected = -1;
    g.reason = reason;
    if (stop_motor_if_needed() < 0)
        g.reason = "motor_stop_failed";
}
static void stop_wheels(const char *reason)
{
    memset(&g.pid[2], 0, 2 * sizeof(g.pid[0]));
    g.left_output = g.right_output = 0;
    g.reason = reason;
    if (stop_motor_if_needed() < 0)
        g.reason = "motor_stop_failed";
}
/* A fault in the agent source is latched, not transient: the profile is
 * switched off so that neither the agent nor the vision path can resume
 * motion until an operator clears it explicitly. */
static void agent_latch(const char *why)
{
    g.ag.state = AGENT_MOTION_FAULT;
    g.ag.expiry_ms = 0;
    g.cfg[ENABLED] = 0;
    snprintf(g.ag.fault, sizeof(g.ag.fault), "%s", why);
    halt(why);
}
/* External agent source: same actuator chain as the vision path, but the
 * left/right setpoints come from a bounded command instead of a detection. */
static void agent_tick(unsigned long long now, int can_enabled)
{
    double left, right;
    unsigned long long stamp;
#ifdef ADCBENCH
    can_enabled = 1; /* bench build: the mock adapter replaces the CAN transport */
#endif
    pthread_mutex_lock(&g.feedback_lock);
    left = g.measured_left;
    right = g.measured_right;
    stamp = g.feedback_ms;
    pthread_mutex_unlock(&g.feedback_lock);

    /* Preview stays visible even when blocked, so a bench run without a motor
     * can still show what would have been commanded. */
    follow_mix(g.ag.v, g.ag.steer, g.cfg[SPEED], &g.left_target, &g.right_target);

    if (!g.ag.expiry_ms) {
        /* No outstanding command at all: hold still without latching. */
        stop_wheels("agent_idle");
        g.ag.state = AGENT_MOTION_IDLE;
        return;
    }
    if (now > g.ag.expiry_ms) {
        /* The wheels stop on the very first missed lease, but the latch waits
         * out a grace window: a blocked HTTP thread jitters the 8 Hz pulse
         * train, and that must not turn into a state only a human can clear.
         * Past the window the caller really is gone, so latch as before. */
        stop_wheels("lease_expired");
        if (now - g.ag.expiry_ms >= AGENT_LEASE_GRACE_MS)
            agent_latch("lease_expired");
        return;
    }
    /* Transport-level preconditions first: they do not depend on feedback, so a
     * caller that keeps refreshing the lease must not be able to skip them. */
    const char *hard = !g.motor.send ? "adapter_not_connected"
                       : !g.fifo      ? "fifo_unavailable"
                       : !can_enabled ? "can_disabled"
                                      : NULL;
    if (hard) {
        stop_wheels(hard);
        agent_latch(hard);
        return;
    }
    if (!stamp) {
        /* The decoder has not produced its first frame yet. Hold, do not latch:
         * a real bus needs a few milliseconds before the first wheel frame. */
        if (now - g.ag.accepted_ms < AGENT_FEEDBACK_GRACE_MS) {
            stop_wheels("awaiting_feedback");
            return;
        }
        stop_wheels("stale_wheel_feedback");
        agent_latch("stale_wheel_feedback");
        return;
    }
    if (now >= stamp && now - stamp > (unsigned long long)g.cfg[TIMEOUT]) {
        stop_wheels("stale_wheel_feedback");
        agent_latch("stale_wheel_feedback");
        return;
    }
    double dt = g.previous_ms && now > g.previous_ms ? (now - g.previous_ms) / 1000.0 : 0;
    if (dt <= 0 || dt > .5) {
        /* First iteration after a command, or a stalled worker. Keep the
         * setpoints but do not integrate a bogus dt into the wheel loops. */
        g.previous_ms = now;
        if (g.motor_running)
            stop_wheels("priming");
        return;
    }
    g.previous_ms = now;
    follow_gains_t wheel_pi = {g.cfg[WP], g.cfg[WI], 0};
    g.left_output = follow_pid_step_limited(&g.pid[2], wheel_pi, g.left_target - left, dt, 1,
                                            g.cfg[WHEEL_I_LIMIT]);
    g.right_output = follow_pid_step_limited(&g.pid[3], wheel_pi, g.right_target - right, dt, 1,
                                             g.cfg[WHEEL_I_LIMIT]);
    g.stop_required = 1;
    if (g.motor.send(g.app, g.left_output, g.right_output) < 0) {
        halt("motor_send_failed");
        agent_latch("motor_send_failed");
        return;
    }
    g.motor_running = 1;
    g.stop_required = 0;
    g.reason = "agent_active";
    g.ag.traveled_m += (fabs(g.left_target) + fabs(g.right_target)) * .5 * dt;
}
static void tick(unsigned long long now, int can_enabled)
{
#ifdef ADCBENCH
    bench_publish();
#endif
    if (!g.app->running) {
        halt("shutdown");
        return;
    }
    if (!g.cfg[ENABLED]) {
        /* The UI stop switch also retires an outstanding agent command. */
        if (g.ag.state == AGENT_MOTION_ACTIVE) {
            g.ag.state = AGENT_MOTION_IDLE;
            g.ag.expiry_ms = 0;
        }
        halt("disabled");
        return;
    }
    if (g.ag.state == AGENT_MOTION_FAULT) {
        /* Latched until an operator resets it; blocks both sources. */
        halt(g.ag.fault[0] ? g.ag.fault : "agent_fault");
        return;
    }
    if (g.ag.state == AGENT_MOTION_ACTIVE) {
        agent_tick(now, can_enabled);
        return;
    }
    if (!g.frame.capture_monotonic_ms || now < g.frame.capture_monotonic_ms ||
        now - g.frame.capture_monotonic_ms > g.cfg[TIMEOUT]) {
        halt("stale_detection");
        return;
    }
    double left, right;
    unsigned long long stamp;
    pthread_mutex_lock(&g.feedback_lock);
    left = g.measured_left;
    right = g.measured_right;
    stamp = g.feedback_ms;
    pthread_mutex_unlock(&g.feedback_lock);
    /* Publication rejects future feedback. A sample arriving after this tick's
     * timestamp was taken is new, rather than stale due to unsigned subtraction. */
    const char *interlock = !g.motor.send  ? "adapter_not_connected"
                            : !g.fifo      ? "fifo_unavailable"
                            : !can_enabled ? "can_disabled"
                            : (!stamp || (now >= stamp && now - stamp > g.cfg[TIMEOUT]))
                                ? "stale_wheel_feedback"
                                : NULL;
    /* Check actuator safety at idle wakes too, without resetting vision timing
     * or preventing target/setpoint preview when no motor is connected. */
    if (g.motor.send && interlock)
        stop_wheels(interlock);
    if (g.handled == g.generation)
        return; /* PID follows new inference timestamps, not polling ticks. */
    g.handled = g.generation;
    g.processed = g.frame.seq;
    double best = .35 * .35;
    int target = -1;
    for (int i = 0; i < g.frame.count; i++) {
        yolo_event_object_t *d = &g.frame.objects[i];
        if (d->cls != (int)g.cfg[CLASS] || !isfinite(d->x1) || !isfinite(d->x2) ||
            !isfinite(d->y1) || !isfinite(d->y2) || d->x1 < 0 || d->y1 < 0 ||
            d->x2 >= g.frame.width || d->y2 >= g.frame.height || d->x2 <= d->x1 || d->y2 <= d->y1)
            continue;
        double x = (d->x1 + d->x2) * .5 / g.frame.width, y = (d->y1 + d->y2) * .5 / g.frame.height;
        double distance = (x - g.cx) * (x - g.cx) + (y - g.cy) * (y - g.cy);
        if (distance < best) {
            best = distance;
            target = i;
        }
    }
    if (target < 0) {
        halt("target_lost");
        return;
    }
    yolo_event_object_t *d = &g.frame.objects[target];
    g.cx = (d->x1 + d->x2) * .5 / g.frame.width;
    g.cy = (d->y1 + d->y2) * .5 / g.frame.height;
    g.tracked = 1;
    g.selected = target;
    double area = (d->x2 - d->x1) * (d->y2 - d->y1) / (g.frame.width * (double)g.frame.height);
    if (g.cfg[AREA] <= 0)
        g.cfg[AREA] = area;
    g.area_error = fmax(-1, fmin(1, (g.cfg[AREA] - area) / g.cfg[AREA]));
    g.ex = 2 * (g.cx - .5);
    g.ey = 2 * (.5 - g.cy);
    if (fabs(g.ex) < g.cfg[DEAD])
        g.ex = 0;
    if (fabs(g.ey) < g.cfg[DEAD])
        g.ey = 0;
    /* The steering UI's center deadband must not change automatic speed. */
    if (fabs(g.area_error) < .04)
        g.area_error = 0;
    double dt = g.previous_ms && g.frame.capture_monotonic_ms > g.previous_ms
                    ? (g.frame.capture_monotonic_ms - g.previous_ms) / 1000.0
                    : 0;
    g.previous_ms = g.frame.capture_monotonic_ms;
    if (dt <= 0 || dt > .5) {
        halt("priming");
        g.previous_ms = g.frame.capture_monotonic_ms;
        return;
    }
    double v = g.cfg[MANUAL]
                   ? g.cfg[SETPOINT]
                   : fmax(0, follow_pid_step(&g.pid[0], (follow_gains_t){g.cfg[VP], 0, g.cfg[VD]},
                                             g.area_error, dt, g.cfg[SPEED]));
    double correction = follow_pid_step(&g.pid[1], (follow_gains_t){g.cfg[TP], 0, g.cfg[TD]}, g.ex,
                                        dt, g.cfg[TURN_DELTA]);
    follow_mix(v, correction, g.cfg[SPEED], &g.left_target, &g.right_target);
    /* No synthetic feedback and no assumed motor encoding. Preview keeps wheel
     * setpoints visible; actuator output requires every interlock below. */
    if (interlock) {
        if (!g.motor.send)
            stop_wheels(interlock);
        return;
    }
    follow_gains_t wheel_pi = {g.cfg[WP], g.cfg[WI], 0};
    g.left_output = follow_pid_step_limited(&g.pid[2], wheel_pi, g.left_target - left, dt, 1,
                                            g.cfg[WHEEL_I_LIMIT]);
    g.right_output = follow_pid_step_limited(&g.pid[3], wheel_pi, g.right_target - right, dt, 1,
                                             g.cfg[WHEEL_I_LIMIT]);
    /* A transport can fail after one wheel's frame was accepted. Always try
     * the documented stop sequence if any command pair fails partway. */
    g.stop_required = 1;
    if (g.motor.send(g.app, g.left_output, g.right_output) < 0)
        halt("motor_send_failed");
    else {
        g.motor_running = 1;
        g.stop_required = 0;
        g.reason = "active";
    }
}
void *follow_task(void *arg)
{
    app_ctx_t *app = arg;
    (void)pthread_setname_np(pthread_self(), "follow_pid");
    if (watchdog_register_thread(pthread_self(), "car_sport_ctrl", 3, 3) < 0) {
        app->fatal = 1;
        app->running = 0;
        return NULL;
    }
    int policy = 0;
    struct sched_param param = {0};
    int err = pthread_getschedparam(pthread_self(), &policy, &param);
    if (!err && policy != SCHED_FIFO) err = ENOTSUP;
    pthread_mutex_lock(&g.lock);
    g.fifo = !err;
    g.priority = err ? 0 : param.sched_priority;
    g.rt_error = err;
    pthread_mutex_unlock(&g.lock);
    unsigned long observed = 0;
    for (;;) {
        watchdog_feed_self("car_sport_ctrl");
        pthread_mutex_lock(&g.lock);
        if (!g.running || !g.app->running) {
            halt("shutdown");
            pthread_mutex_unlock(&g.lock);
            break;
        }
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_nsec += 50000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_nsec -= 1000000000;
            deadline.tv_sec++;
        }
        /* Separate wake consumption from PID acceptance: stale/lost frames
         * must also lead back to a blocking wait, especially under FIFO. */
        if (observed == g.generation || !g.cfg[ENABLED])
            pthread_cond_timedwait(&g.changed, &g.lock, &deadline);
        observed = g.generation;
        pthread_mutex_unlock(&g.lock);
        can_motor_poll(g.app);
        /* Never acquire can_mutex while holding the feedback lock. */
        pthread_mutex_lock(&g.app->can_mutex);
        int can_enabled = g.app->cfg->args.can_enabled;
        pthread_mutex_unlock(&g.app->can_mutex);
        pthread_mutex_lock(&g.lock);
        tick(now_ms(), can_enabled);
        pthread_mutex_unlock(&g.lock);
    }
    if (app->running) app->fatal = 1;
    app->running = 0;
    watchdog_unregister_self("car_sport_ctrl");
    return NULL;
}
void follow_invalidate_feedback(void)
{
    if (!atomic_load(&g.ready))
        return;
    pthread_mutex_lock(&g.feedback_lock);
    g.feedback_ms = 0;
    pthread_mutex_unlock(&g.feedback_lock);
}

int follow_agent_cmd(double v, double steer, unsigned ttl_ms)
{
    if (!atomic_load(&g.ready))
        return -1;
    if (!isfinite(v) || !isfinite(steer) || v < 0 || v > AGENT_V_CAP || fabs(steer) > AGENT_STEER_CAP ||
        ttl_ms < AGENT_TTL_MIN_MS || ttl_ms > AGENT_TTL_MAX_MS)
        return -2;
    pthread_mutex_lock(&g.lock);
    if (!g.cfg[ENABLED] || g.ag.state == AGENT_MOTION_FAULT) {
        pthread_mutex_unlock(&g.lock);
        return -3;
    }
    unsigned long long now = now_ms();
    g.ag.v = v;
    g.ag.steer = steer;
    g.ag.expiry_ms = now + ttl_ms;
    g.ag.accepted_ms = now;
    g.ag.state = AGENT_MOTION_ACTIVE;
    g.ag.accepted++;
    /* Start the wheel loops from rest instead of integrating the gap since the
     * previous source last ran; agent_tick consumes the zero dt itself. */
    if (!g.previous_ms)
        g.previous_ms = now;
    pthread_cond_signal(&g.changed);
    pthread_mutex_unlock(&g.lock);
    return 0;
}

void follow_agent_cancel(int disable_profile)
{
    if (!atomic_load(&g.ready))
        return;
    /* disable_profile=1（stop/estop/disarm）：结束智能体运动绝不能把控制权交回
     * 视觉路径——开着 profile 的话，智能体一停止说话车就会去追目标。所以关掉
     * profile，由操作员显式重开。
     * disable_profile=0（release）：只放弃租约。一条计划正常跑完后调用它，下一次
     * plan 才能接着跑，否则每次结束都要人点一次"启用"。 */
    if (disable_profile)
        (void)follow_force_disable("agent_stopped");
    pthread_mutex_lock(&g.lock);
    g.ag.state = AGENT_MOTION_IDLE;
    g.ag.expiry_ms = 0;
    g.ag.v = g.ag.steer = 0;
    g.stop_required = 1;
    pthread_mutex_unlock(&g.lock);
}

void follow_agent_reset(void)
{
    if (!atomic_load(&g.ready))
        return;
    pthread_mutex_lock(&g.lock);
    g.ag.state = AGENT_MOTION_IDLE;
    g.ag.expiry_ms = 0;
    g.ag.v = g.ag.steer = 0;
    g.ag.traveled_m = 0;
    g.ag.accepted = 0;
    g.ag.fault[0] = 0;
    g.stop_required = 1;
    halt("agent_idle");
    pthread_mutex_unlock(&g.lock);
}

int follow_agent_snapshot(agent_motion_t *out)
{
    if (!out || !atomic_load(&g.ready) || pthread_mutex_trylock(&g.lock))
        return -1;
    unsigned long long now = now_ms();
    *out = (agent_motion_t){.state = g.ag.state,
                            .v = g.ag.v,
                            .steer = g.ag.steer,
                            .traveled_m = g.ag.traveled_m,
                            .accepted = g.ag.accepted,
                            .enabled = g.cfg[ENABLED] != 0,
                            .motor_bound = g.motor.send != NULL};
    snprintf(out->fault, sizeof(out->fault), "%s", g.ag.fault);
    snprintf(out->reason, sizeof(out->reason), "%s", g.reason ? g.reason : "");
    out->expires_in_ms = g.ag.expiry_ms && g.ag.expiry_ms > now ? g.ag.expiry_ms - now : 0;
    pthread_mutex_unlock(&g.lock);
    if (g.app && g.app->cfg) {
        pthread_mutex_lock(&g.app->can_mutex);
        out->can_enabled = g.app->cfg->args.can_enabled;
        pthread_mutex_unlock(&g.app->can_mutex);
    }
    int transport = out->can_enabled;
#ifdef ADCBENCH
    transport = 1; /* the mock adapter stands in for the CAN transport */
#endif
    out->actuation_ready = out->motor_bound && out->enabled && g.fifo && transport;
    return 0;
}

int follow_profile_enabled(void)
{
    if (!atomic_load(&g.ready))
        return 0;
    return g.cfg[ENABLED] != 0;
}

int follow_force_disable(const char *reason)
{
    if (!atomic_load(&g.ready))
        return -1;
    /* Same lock order as the settings endpoint: save_lock, then lock. */
    pthread_mutex_lock(&g.save_lock);
    pthread_mutex_lock(&g.lock);
    g.cfg[ENABLED] = 0;
    g.ag.state = AGENT_MOTION_IDLE;
    g.ag.expiry_ms = 0;
    g.stop_required = 1;
    halt(reason ? reason : "disabled");
    double values[N];
    memcpy(values, g.cfg, sizeof(values));
    pthread_mutex_unlock(&g.lock);
    /* Persist the disable so that a service restart cannot bring the profile
     * back up enabled with a stale credential still valid. */
    FILE *f = fopen("config/follow.conf.tmp", "w");
    int failed = !f;
    if (f) {
        for (int i = 1; i < N; i++)
            if (fprintf(f, "%s %.9g\n", keys[i], values[i]) < 0) failed = 1;
        if (fclose(f)) failed = 1;
    }
    if (failed || rename("config/follow.conf.tmp", "config/follow.conf"))
        remove("config/follow.conf.tmp");
    pthread_mutex_unlock(&g.save_lock);
    return 0;
}

int follow_telemetry_snapshot(follow_telemetry_t *out)
{
    if (!out || !atomic_load(&g.ready) || pthread_mutex_trylock(&g.lock))
        return -1;
    if (pthread_mutex_trylock(&g.feedback_lock)) {
        pthread_mutex_unlock(&g.lock);
        return -1;
    }
    unsigned long long now = now_ms();
    int fresh = g.feedback_ms && now >= g.feedback_ms &&
                now - g.feedback_ms <= (unsigned long long)g.cfg[TIMEOUT];
    *out = (follow_telemetry_t){
        .speed_target = (g.left_target + g.right_target) * .5,
        .speed_actual = fresh ? (g.measured_left + g.measured_right) * .5 : NAN,
        .steering_target = (g.left_target - g.right_target) * .5,
        .steering_actual = fresh ? (g.measured_left - g.measured_right) * .5 : NAN,
        .control_seq = g.processed,
        .feedback_ms = g.feedback_ms,
        .feedback_valid = fresh,
        .feedback_valid_until_ms = g.feedback_ms
                                      ? g.feedback_ms + (unsigned long long)g.cfg[TIMEOUT] : 0,
    };
    pthread_mutex_unlock(&g.feedback_lock);
    pthread_mutex_unlock(&g.lock);
    return 0;
}
int follow_init(void *app)
{
    if (atomic_load(&g.ready))
        return -1;
    g.app = app;
    g.reason = "disabled";
    memcpy(g.cfg, defaults, sizeof(defaults));
    /* PI mutex bounds priority inversion between HTTP and the FIFO worker. */
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&g.lock, &ma);
    pthread_mutex_init(&g.feedback_lock, &ma);
    pthread_mutex_init(&g.save_lock, &ma);
    pthread_mutexattr_destroy(&ma);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&g.changed, &ca);
    pthread_condattr_destroy(&ca);
    FILE *f = fopen("config/follow.conf", "r");
    if (f) {
        double loaded[N];
        memcpy(loaded, g.cfg, sizeof(loaded));
        char key[48];
        double value;
        double legacy_yaw = 1, legacy_track = .3, legacy_kp = 1, legacy_kd = .02;
        int has_correction = 0;
        while (fscanf(f, "%47s %lf", key, &value) == 2) {
            if (!strcmp(key, "max_yaw") && isfinite(value) && value > 0 && value <= 4)
                legacy_yaw = value;
            if (!strcmp(key, "wheel_track") && isfinite(value) && value >= .05 && value <= 2)
                legacy_track = value;
            if (!strcmp(key, "max_turn_delta"))
                has_correction = 1;
            if (!strcmp(key, "turn_kp"))
                legacy_kp = value;
            if (!strcmp(key, "turn_kd"))
                legacy_kd = value;
            for (int i = 1; i < N; i++)
                if (!strcmp(key, keys[i]))
                    loaded[i] = value;
        }
        fclose(f);
        if (!has_correction) {
            /* Convert legacy rad/s gains and limit to wheel-speed correction. */
            loaded[TURN_DELTA] = legacy_yaw * legacy_track * .5;
            loaded[TP] = legacy_kp * legacy_track * .5;
            loaded[TD] = legacy_kd * legacy_track * .5;
        }
        /* Retire the former automatic-speed integral gain without discarding
         * the remaining saved settings from an older configuration. */
        loaded[VI] = 0;
        if (valid(loaded))
            memcpy(g.cfg, loaded, sizeof(loaded));
    }
    /* Mainline RT kernels may omit the legacy sysfs flag. Their running
       kernel version still advertises PREEMPT_RT independently of the release name. */
    struct utsname kernel;
    g.rt_kernel = uname(&kernel) == 0 && strstr(kernel.version, "PREEMPT_RT") != NULL;
    f = fopen("/sys/kernel/realtime", "r");
    if (f) {
        int realtime;
        if (fscanf(f, "%d", &realtime) == 1) g.rt_kernel = realtime == 1;
        fclose(f);
    }
    reset();
    memset(&g.ag, 0, sizeof(g.ag));
    g.motor = (follow_motor_ops_t){0};
    g.motor_running = g.stop_required = 0;
    g.running = 1;
    atomic_store(&g.ready, 1);
    /* The profile remains disabled unless explicitly configured. Binding occurs
     * before the controller worker starts; the FIFO interlock stays mandatory. */
    (void)can_motor_init(g.app);
#ifdef ADCBENCH
    /* Test-only build (-DADCBENCH): a loop-back actuator that echoes the
     * commanded wheel speeds as measured feedback. It exists so the agent
     * capability layer can be exercised end to end without a motor, and is
     * never compiled into the deployed binary. */
    if (!g.motor.send) {
        follow_bind_motor(&g_bench_motor);
    }
    LOG_INFO("car control: ADCBENCH build, mock actuator bound");
#endif
    return 0;
}
void follow_destroy(void *app)
{
    (void)app;
    if (!atomic_exchange(&g.ready, 0))
        return;
    pthread_mutex_lock(&g.lock);
    g.running = 0;
    pthread_cond_signal(&g.changed);
    pthread_mutex_unlock(&g.lock);
    /* A transient nonblocking send failure must not discard the final stop.
     * Retry on the owner thread before closing transport, with a bounded wait. */
    finish_motor_stop();
    can_motor_cleanup();
    pthread_cond_destroy(&g.changed);
    pthread_mutex_destroy(&g.lock);
    pthread_mutex_destroy(&g.feedback_lock);
    pthread_mutex_destroy(&g.save_lock);
}
int follow_bind_motor(const follow_motor_ops_t *ops)
{
    if (!atomic_load(&g.ready) || !ops || !ops->send || !ops->stop)
        return -1;
    pthread_mutex_lock(&g.lock);
    int rc = g.cfg[ENABLED] || g.motor.send ? -1 : 0;
    if (!rc) {
        g.motor = *ops;
        g.stop_required = 1;
    }
    pthread_mutex_unlock(&g.lock);
    return rc;
}
int follow_feedback(double left, double right, unsigned long long stamp)
{
    unsigned long long now = now_ms();
    if (!atomic_load(&g.ready) || !isfinite(left) || !isfinite(right) || fabs(left) > 20 ||
        fabs(right) > 20 || !stamp || stamp > now)
        return -1;
    pthread_mutex_lock(&g.feedback_lock);
    int rc = stamp <= g.feedback_ms ? -1 : 0;
    if (!rc) {
        g.measured_left = left;
        g.measured_right = right;
        g.feedback_ms = stamp;
    }
    pthread_mutex_unlock(&g.feedback_lock);
    return rc;
}
void follow_submit(const yolo_event_meta_t *frame)
{
    if (!atomic_load(&g.ready) || !frame || frame->count < 0 || frame->count > YOLO_MAX_DETS ||
        frame->width <= 0 || frame->height <= 0)
        return;
    if (pthread_mutex_trylock(&g.lock))
        return;
    if (frame->capture_monotonic_ms > g.frame.capture_monotonic_ms) {
        /* Capture restart may reset its sequence counter. Require explicit
         * re-enable rather than carrying a motor command into a new session. */
        if (frame->seq <= g.accepted) {
            g.capture_epoch++;
            g.cfg[ENABLED] = 0;
            reset();
        }
        g.frame = *frame;
        g.accepted = frame->seq;
        g.generation++;
        pthread_cond_signal(&g.changed);
    }
    pthread_mutex_unlock(&g.lock);
}
void http_follow_get(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req)
{
    (void)app;
    (void)method;
    (void)uri;
    (void)req;
    if (!atomic_load(&g.ready)) {
        http_err(fd, 503, "Unavailable", "Controller starting");
        return;
    }
    struct {
        double cfg[N], ex, ey, area_error, left_target, right_target, left_output, right_output;
        int fifo, priority, rt_kernel, rt_error, selected, adapter;
        unsigned long long processed;
        const char *reason;
        yolo_event_meta_t frame;
    } s;
    pthread_mutex_lock(&g.lock);
    memcpy(s.cfg, g.cfg, sizeof(s.cfg));
    s.ex = g.ex;
    s.ey = g.ey;
    s.area_error = g.area_error;
    s.left_target = g.left_target;
    s.right_target = g.right_target;
    s.left_output = g.left_output;
    s.right_output = g.right_output;
    s.fifo = g.fifo;
    s.priority = g.priority;
    s.rt_kernel = g.rt_kernel;
    s.rt_error = g.rt_error;
    s.selected = g.selected;
    s.adapter = g.motor.send != NULL;
    s.processed = g.processed;
    s.reason = g.reason;
    s.frame = g.frame;
    pthread_mutex_unlock(&g.lock);
    can_motor_status_t motor_status;
    can_motor_get_status(&motor_status);
    char motor_error[1024], motor_interface[IFNAMSIZ * 6 + 1];
    http_json_escape(motor_status.error, motor_error, sizeof(motor_error));
    http_json_escape(motor_status.interface_name, motor_interface, sizeof(motor_interface));
    /* Polling the UI must not hold the controller lock during JSON encoding. */
    char json[32768];
    int off = 0;
    off += snprintf(json + off, sizeof(json) - off, "{\"config\":{");
    for (int i = 0; i < N; i++)
        off += snprintf(json + off, sizeof(json) - off, "%s\"%s\":%.6f", i ? "," : "", keys[i],
                        s.cfg[i]);
    off += snprintf(json + off, sizeof(json) - off,
                    "},\"status\":{\"reason\":\"%s\",\"fifo\":%s,\"priority\":%d,\"rt_kernel\":%s,"
                    "\"rt_error\":%d,\"adapter\":%s,\"seq\":%llu,\"selected\":%d,\"error_x\":%.5f,"
                    "\"error_y\":%.5f,\"area_error\":%.5f,\"left_target\":%.5f,\"right_target\":%."
                    "5f,\"left_output\":%.5f,\"right_output\":%.5f},\"targets\":[",
                    s.reason, s.fifo ? "true" : "false", s.priority,
                    s.rt_kernel ? "true" : "false", s.rt_error, s.adapter ? "true" : "false",
                    s.processed, s.selected, s.ex, s.ey, s.area_error, s.left_target,
                    s.right_target, s.left_output, s.right_output);
    for (int i = 0; i < s.frame.count; i++) {
        yolo_event_object_t *d = &s.frame.objects[i];
        char name[600];
        http_json_escape(d->name, name, sizeof(name));
        off +=
            snprintf(json + off, sizeof(json) - off,
                     "%s{\"class_id\":%d,\"name\":\"%s\",\"x\":%.5f,\"y\":%.5f,\"area\":%.7f}",
                     i ? "," : "", d->cls, name, (d->x1 + d->x2) * .5 / s.frame.width,
                     (d->y1 + d->y2) * .5 / s.frame.height,
                     (d->x2 - d->x1) * (d->y2 - d->y1) / (s.frame.width * (double)s.frame.height));
    }
    off +=
        snprintf(json + off, sizeof(json) - off,
                 "],\"motor\":{\"configured\":%s,\"online\":%s,\"interface\":\"%s\","
                 "\"tx_frames\":%llu,\"tx_errors\":%llu,\"rx_frames\":%llu,"
                 "\"decode_errors\":%llu,\"error\":\"%s\"}}",
                 motor_status.configured ? "true" : "false", motor_status.online ? "true" : "false",
                 motor_interface, motor_status.tx_frames, motor_status.tx_errors,
                 motor_status.rx_frames, motor_status.decode_errors, motor_error);
    http_ok_json(fd, json, (size_t)off);
}
/* Apply only posted settings; runtime-learned reference and capture-stop state
 * are owned by the inference path. Ordinary tuning never restarts tracking. */
static void apply_settings(const double *values, const unsigned char *present)
{
    unsigned char changed[N] = {0};
    for (int i = 0; i < N; i++) {
        if (!present[i])
            continue;
        changed[i] = g.cfg[i] != values[i];
        g.cfg[i] = values[i];
    }
    int new_target = present[CLASS] || present[SEED_X] || present[SEED_Y];
    if (new_target || changed[ENABLED]) {
        halt(g.cfg[ENABLED] ? "settings_changed" : "disabled");
        reset();
        return;
    }
    if (changed[VP] || changed[VI] || changed[VD] || changed[AREA] || changed[MANUAL])
        memset(&g.pid[0], 0, sizeof(g.pid[0]));
    if (changed[TP] || changed[TI] || changed[TD] || changed[TURN_DELTA] || changed[DEAD])
        memset(&g.pid[1], 0, sizeof(g.pid[1]));
    if (changed[WP] || changed[WI] || changed[WD] || changed[SPEED] || changed[MANUAL])
        memset(&g.pid[2], 0, 2 * sizeof(g.pid[0]));
    if (changed[WHEEL_I_LIMIT]) {
        double bound = g.cfg[WI] > 0 ? g.cfg[WHEEL_I_LIMIT] / g.cfg[WI] : 0;
        for (int i = 2; i <= 3; i++)
            g.pid[i].integral = fmax(-bound, fmin(bound, g.pid[i].integral));
    }
}

void http_follow_post(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body)
{
    (void)app;
    (void)method;
    (void)uri;
    if (!atomic_load(&g.ready)) {
        http_err(fd, 503, "Unavailable", "Controller starting");
        return;
    }
    if (!body) {
        http_err(fd, 400, "Bad Request", NULL);
        return;
    }
    http_form_field_t fields[32];
    int n = http_form_parse(body, fields, 32);
    if (n <= 0 || n == 32) {
        http_err(fd, 400, "Bad Request", "Invalid tracking form");
        return;
    }
    double requested[N] = {0}, values[N];
    unsigned char present[N] = {0};
    for (int i = 0; i < n; i++) {
        int key = -1;
        for (int k = 0; k < N; k++)
            if (!strcmp(fields[i].key, keys[k])) {
                key = k;
                break;
            }
        if (key < 0 || present[key]) {
            http_err(fd, 400, "Bad Request", "Unknown or duplicate tracking parameter");
            return;
        }
        char *end;
        errno = 0;
        requested[key] = strtod(fields[i].val, &end);
        if (errno || end == fields[i].val || *end || !isfinite(requested[key])) {
            http_err(fd, 400, "Bad Request", "Invalid numeric tracking parameter");
            return;
        }
        present[key] = 1;
    }

    /* Serialize saves separately; the control worker never takes save_lock. */
    pthread_mutex_lock(&g.save_lock);
    pthread_mutex_lock(&g.lock);
    memcpy(values, g.cfg, sizeof(values));
    for (int i = 0; i < N; i++)
        if (present[i])
            values[i] = requested[i];
    if (!valid(values)) {
        pthread_mutex_unlock(&g.lock);
        pthread_mutex_unlock(&g.save_lock);
        http_err(fd, 400, "Bad Request", "Tracking parameters outside limits");
        return;
    }
    unsigned long capture_epoch = g.capture_epoch;
    if (present[ENABLED] && !values[ENABLED]) {
        /* A valid explicit stop does not wait for storage to finish. */
        g.cfg[ENABLED] = 0;
        g.stop_required = 1;
        halt("disabled");
    }
    pthread_mutex_unlock(&g.lock);

    /* All formatting, buffered writes and close/flush happen off the control
     * lock. Only the final atomic rename and runtime commit need that lock. */
    FILE *f = fopen("config/follow.conf.tmp", "w");
    int failed = !f;
    if (f) {
        for (int i = 1; i < N; i++)
            if (fprintf(f, "%s %.9g\n", keys[i], values[i]) < 0)
                failed = 1;
        if (fclose(f))
            failed = 1;
    }
    if (failed) {
        remove("config/follow.conf.tmp");
        pthread_mutex_unlock(&g.save_lock);
        http_err(fd, 500, "Error", "Cannot save tracking settings");
        return;
    }

    pthread_mutex_lock(&g.lock);
    int session_sensitive = present[CLASS] || present[SEED_X] || present[SEED_Y] || present[AREA] ||
                            (present[ENABLED] && values[ENABLED]);
    if (session_sensitive && capture_epoch != g.capture_epoch) {
        pthread_mutex_unlock(&g.lock);
        remove("config/follow.conf.tmp");
        pthread_mutex_unlock(&g.save_lock);
        http_err(fd, 409, "Conflict", "Camera session changed; select target and retry");
        return;
    }
    if (rename("config/follow.conf.tmp", "config/follow.conf")) {
        pthread_mutex_unlock(&g.lock);
        remove("config/follow.conf.tmp");
        pthread_mutex_unlock(&g.save_lock);
        http_err(fd, 500, "Error", "Cannot commit tracking settings");
        return;
    }
    apply_settings(values, present);
    pthread_cond_signal(&g.changed);
    pthread_mutex_unlock(&g.lock);
    pthread_mutex_unlock(&g.save_lock);
    http_ok_text(fd, "saved");
}

/* PID and differential-drive primitives. */
static double clamp(double v, double n)
{
    return fmax(-n, fmin(n, v));
}
double follow_pid_step(follow_pid_t *p, follow_gains_t g, double e, double dt, double limit)
{
    return follow_pid_step_limited(p, g, e, dt, limit, limit);
}

double follow_pid_step_limited(follow_pid_t *p, follow_gains_t g, double e, double dt, double limit,
                               double integral_limit)
{
    if (!p || !isfinite(e) || !isfinite(dt) || dt <= 0 || dt > .5 || !isfinite(limit) ||
        limit <= 0 || !isfinite(integral_limit) || integral_limit < 0 || !isfinite(g.kp) ||
        !isfinite(g.ki) || !isfinite(g.kd))
        return 0;
    double derivative = p->valid ? (e - p->previous) / dt : 0;
    p->derivative += (dt / (.05 + dt)) * (derivative - p->derivative);
    double accumulator_limit = g.ki > 0 ? fmin(limit, integral_limit) / g.ki : 0;
    /* Enforce a lowered limit even when output saturation blocks integration. */
    p->integral = clamp(p->integral, accumulator_limit);
    double integral = clamp(p->integral + e * dt, accumulator_limit);
    double output = g.kp * e + g.ki * integral + g.kd * p->derivative;
    /* Conditional integration prevents saturation from winding up the loop. */
    if (fabs(output) <= limit || e * output < 0)
        p->integral = integral;
    p->previous = e;
    p->valid = 1;
    return clamp(g.kp * e + g.ki * p->integral + g.kd * p->derivative, limit);
}
void follow_mix(double speed, double correction, double limit, double *l, double *r)
{
    if (!l || !r)
        return;
    *l = *r = 0;
    if (!isfinite(speed) || !isfinite(correction) || !isfinite(limit) || limit <= 0)
        return;
    /* Positive image-X error means target right: left wheel runs faster. */
    *l = speed + correction;
    *r = speed - correction;
    double peak = fmax(fabs(*l), fabs(*r));
    if (peak > limit) {
        *l *= limit / peak;
        *r *= limit / peak;
    }
}
