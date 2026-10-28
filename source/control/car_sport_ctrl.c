/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "control/car_sport_ctrl.h"
#include "can/can_socket.h"
#include "core/cpu_affinity.h"
#include "http/http_internal.h"
#include <math.h>
#include <sched.h>
#include <time.h>

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
    N
};
static const char *keys[N] = {
    "enabled",  "class_id", "max_speed", "max_turn_delta", "deadband", "timeout_ms",    "speed_kp",
    "speed_ki", "speed_kd", "turn_kp",   "turn_ki",        "turn_kd",  "wheel_kp",      "wheel_ki",
    "wheel_kd", "seed_x",   "seed_y",    "manual",         "setpoint", "reference_area"};
static const double defaults[N] = {0, 0,    .3, .15, .04, 300, .4, 0, 0,   .15,
                                   0, .003, 1,  .1,  0,   .5,  .5, 0, .15, 0};
static struct {
    pthread_mutex_t lock, feedback_lock;
    pthread_cond_t changed;
    pthread_t thread;
    atomic_int ready;
    int running, fifo, rt_kernel, rt_error, selected, tracked;
    app_ctx_t *app;
    follow_motor_ops_t motor;
    double cfg[N], ex, ey, area_error, left_target, right_target, left_output, right_output;
    double measured_left, measured_right, cx, cy;
    unsigned long long feedback_ms, accepted, processed, previous_ms;
    unsigned long generation, handled;
    yolo_event_meta_t frame;
    follow_pid_t pid[4];
    const char *reason;
} g;
static unsigned long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
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
           (!v[MANUAL] || v[SETPOINT] <= v[SPEED]) && v[AREA] <= 1;
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
static void halt(const char *reason)
{
    g.left_target = g.right_target = g.left_output = g.right_output = 0;
    memset(g.pid, 0, sizeof(g.pid));
    g.previous_ms = 0;
    g.selected = -1;
    g.reason = reason;
    if (g.motor.stop && g.motor.stop(g.app) < 0)
        g.reason = "motor_stop_failed";
}
static void tick(unsigned long long now, int can_enabled)
{
    if (!g.app->running) {
        halt("shutdown");
        return;
    }
    if (!g.cfg[ENABLED]) {
        halt("disabled");
        return;
    }
    if (!g.frame.capture_monotonic_ms || now < g.frame.capture_monotonic_ms ||
        now - g.frame.capture_monotonic_ms > g.cfg[TIMEOUT]) {
        halt("stale_detection");
        return;
    }
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
    if (fabs(g.area_error) < g.cfg[DEAD])
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
    double left, right;
    unsigned long long stamp;
    pthread_mutex_lock(&g.feedback_lock);
    left = g.measured_left;
    right = g.measured_right;
    stamp = g.feedback_ms;
    pthread_mutex_unlock(&g.feedback_lock);
    /* No synthetic feedback and no assumed motor encoding. Preview keeps wheel
     * setpoints visible; actuator output requires every interlock below. */
    const char *reason = !g.motor.send  ? "adapter_not_connected"
                         : !g.fifo      ? "fifo_unavailable"
                         : !can_enabled ? "can_disabled"
                         : (!stamp || now < stamp || now - stamp > g.cfg[TIMEOUT])
                             ? "stale_wheel_feedback"
                             : NULL;
    if (reason) {
        memset(&g.pid[2], 0, 2 * sizeof(g.pid[0]));
        g.left_output = g.right_output = 0;
        g.reason = reason;
        if (g.motor.stop && g.motor.stop(g.app) < 0)
            g.reason = "motor_stop_failed";
        return;
    }
    follow_gains_t wheel_pi = {g.cfg[WP], g.cfg[WI], 0};
    g.left_output = follow_pid_step(&g.pid[2], wheel_pi, g.left_target - left, dt, 1);
    g.right_output = follow_pid_step(&g.pid[3], wheel_pi, g.right_target - right, dt, 1);
    if (g.motor.send(g.app, g.left_output, g.right_output) < 0)
        halt("motor_send_failed");
    else
        g.reason = "active";
}
static void *worker(void *unused)
{
    (void)unused;
    pthread_setname_np(pthread_self(), "follow_pid");
    cpu_bind_little();
    struct sched_param p = {.sched_priority = 20};
    int err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
    pthread_mutex_lock(&g.lock);
    g.fifo = !err;
    g.rt_error = err;
    pthread_mutex_unlock(&g.lock);
    unsigned long observed = 0;
    for (;;) {
        pthread_mutex_lock(&g.lock);
        if (!g.running) {
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
        /* Never acquire can_mutex while holding the feedback lock. */
        pthread_mutex_lock(&g.app->can_mutex);
        int can_enabled = g.app->cfg->args.can_enabled;
        pthread_mutex_unlock(&g.app->can_mutex);
        pthread_mutex_lock(&g.lock);
        tick(now_ms(), can_enabled);
        pthread_mutex_unlock(&g.lock);
    }
    return NULL;
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
    f = fopen("/sys/kernel/realtime", "r");
    if (f) {
        fscanf(f, "%d", &g.rt_kernel);
        fclose(f);
    }
    reset();
    g.running = 1;
    atomic_store(&g.ready, 1);
    if (pthread_create(&g.thread, NULL, worker, NULL)) {
        atomic_store(&g.ready, 0);
        g.running = 0;
        return -1;
    }
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
    pthread_join(g.thread, NULL);
    pthread_cond_destroy(&g.changed);
    pthread_mutex_destroy(&g.lock);
    pthread_mutex_destroy(&g.feedback_lock);
}
int follow_bind_motor(const follow_motor_ops_t *ops)
{
    if (!atomic_load(&g.ready) || !ops || !ops->send || !ops->stop)
        return -1;
    pthread_mutex_lock(&g.lock);
    int rc = g.cfg[ENABLED] || g.motor.send ? -1 : 0;
    if (!rc)
        g.motor = *ops;
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
    char json[32768];
    int off = 0;
    pthread_mutex_lock(&g.lock);
    off += snprintf(json + off, sizeof(json) - off, "{\"config\":{");
    for (int i = 0; i < N; i++)
        off += snprintf(json + off, sizeof(json) - off, "%s\"%s\":%.6f", i ? "," : "", keys[i],
                        g.cfg[i]);
    off += snprintf(json + off, sizeof(json) - off,
                    "},\"status\":{\"reason\":\"%s\",\"fifo\":%s,\"priority\":%d,\"rt_kernel\":%s,"
                    "\"rt_error\":%d,\"adapter\":%s,\"seq\":%llu,\"selected\":%d,\"error_x\":%.5f,"
                    "\"error_y\":%.5f,\"area_error\":%.5f,\"left_target\":%.5f,\"right_target\":%."
                    "5f,\"left_output\":%.5f,\"right_output\":%.5f},\"targets\":[",
                    g.reason, g.fifo ? "true" : "false", g.fifo ? 20 : 0,
                    g.rt_kernel ? "true" : "false", g.rt_error, g.motor.send ? "true" : "false",
                    g.processed, g.selected, g.ex, g.ey, g.area_error, g.left_target,
                    g.right_target, g.left_output, g.right_output);
    for (int i = 0; i < g.frame.count; i++) {
        yolo_event_object_t *d = &g.frame.objects[i];
        char name[600];
        http_json_escape(d->name, name, sizeof(name));
        off +=
            snprintf(json + off, sizeof(json) - off,
                     "%s{\"class_id\":%d,\"name\":\"%s\",\"x\":%.5f,\"y\":%.5f,\"area\":%.7f}",
                     i ? "," : "", d->cls, name, (d->x1 + d->x2) * .5 / g.frame.width,
                     (d->y1 + d->y2) * .5 / g.frame.height,
                     (d->x2 - d->x1) * (d->y2 - d->y1) / (g.frame.width * (double)g.frame.height));
    }
    pthread_mutex_unlock(&g.lock);
    off += snprintf(json + off, sizeof(json) - off, "]}");
    http_ok_json(fd, json, (size_t)off);
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
    double values[N];
    if (n <= 0 || n == 32) {
        http_err(fd, 400, "Bad Request", "Invalid tracking form");
        return;
    }
    pthread_mutex_lock(&g.lock);
    memcpy(values, g.cfg, sizeof(values));
    for (int i = 0; i < n; i++) {
        int found = 0;
        for (int k = 0; k < N; k++)
            if (!strcmp(fields[i].key, keys[k])) {
                char *end;
                errno = 0;
                values[k] = strtod(fields[i].val, &end);
                found = 1;
                if (errno || end == fields[i].val || *end)
                    found = 0;
                break;
            }
        if (!found) {
            pthread_mutex_unlock(&g.lock);
            http_err(fd, 400, "Bad Request", "Unknown or invalid tracking parameter");
            return;
        }
    }
    if (!valid(values)) {
        pthread_mutex_unlock(&g.lock);
        http_err(fd, 400, "Bad Request", "Tracking parameters outside limits");
        return;
    }
    FILE *f = fopen("config/follow.conf.tmp", "w");
    int failed = !f;
    if (f) {
        for (int i = 1; i < N; i++)
            if (fprintf(f, "%s %.9g\n", keys[i], values[i]) < 0)
                failed = 1;
        if (fclose(f))
            failed = 1;
        if (!failed && rename("config/follow.conf.tmp", "config/follow.conf"))
            failed = 1;
    }
    if (failed) {
        remove("config/follow.conf.tmp");
        pthread_mutex_unlock(&g.lock);
        http_err(fd, 500, "Error", "Cannot save tracking settings");
        return;
    }
    double previous_x = g.cx, previous_y = g.cy;
    int preserve_target =
        g.tracked && !http_form_find(fields, n, "seed_x") && !http_form_find(fields, n, "class_id");
    halt("settings_changed");
    memcpy(g.cfg, values, sizeof(values));
    reset();
    if (preserve_target) {
        g.cx = previous_x;
        g.cy = previous_y;
    }
    pthread_cond_signal(&g.changed);
    pthread_mutex_unlock(&g.lock);
    http_ok_text(fd, "saved");
}

/* PID and differential-drive primitives. */
static double clamp(double v, double n)
{
    return fmax(-n, fmin(n, v));
}
double follow_pid_step(follow_pid_t *p, follow_gains_t g, double e, double dt, double limit)
{
    if (!p || !isfinite(e) || !isfinite(dt) || dt <= 0 || dt > .5 || !isfinite(limit) ||
        limit <= 0 || !isfinite(g.kp) || !isfinite(g.ki) || !isfinite(g.kd))
        return 0;
    double derivative = p->valid ? (e - p->previous) / dt : 0;
    p->derivative += (dt / (.05 + dt)) * (derivative - p->derivative);
    double integral = clamp(p->integral + e * dt, g.ki > 0 ? limit / g.ki : 0);
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
