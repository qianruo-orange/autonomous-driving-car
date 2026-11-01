/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise the production lifecycle without starting hardware or services.
 * Build with function/data sections so the unused application entry is dropped:
 * cc -std=c11 -Wall -Wextra -ffunction-sections -fdata-sections -Iinclude \
 *    tests/lifecycle_test.c -Wl,--gc-sections -pthread -o /tmp/car-lifecycle-test
 */
#define _GNU_SOURCE
#define main application_main
#include "../source/core/main.c"
#undef main
#include "../source/watchdog/watchdog.c"
#include <assert.h>
#include <sys/wait.h>

static time_t fake_now;
static int notification_result, stop_on_sleep, wait_calls, drain_on_wait;
static app_ctx_t *test_app;
static int destructors[2], destructor_count;

/* Replace clocks, waits, logging, and systemd with deterministic test doubles. */
time_t time(time_t *out)
{
    if (out) *out = fake_now;
    return fake_now;
}

unsigned sleep(unsigned seconds)
{
    fake_now += seconds;
    if (stop_on_sleep) test_app->running = 0;
    return 0;
}

int usleep(useconds_t usec)
{
    (void)usec;
    if (++wait_calls == drain_on_wait) g_app.threads_running = 0;
    return 0;
}

void sync(void) {}
int sd_notify(int unset_environment, const char *state)
{
    (void)unset_environment;
    (void)state;
    return notification_result;
}

void _log_info(const char *file, int line, const char *format, ...)
{
    (void)file; (void)line; (void)format;
}

void _log_error(const char *file, int line, const char *format, ...)
{
    (void)file; (void)line; (void)format;
}

/* Only module descriptors are retained; these workers are never started. */
#define INIT_STUB(name) int name(void *arg) { (void)arg; return 0; }
#define DTOR_STUB(name) void name(void *arg) { (void)arg; }
#define TASK_STUB(name) void *name(void *arg) { (void)arg; return NULL; }
INIT_STUB(can_init)
INIT_STUB(tcp_init)
INIT_STUB(http_server_start)
INIT_STUB(follow_init)
INIT_STUB(ano_init)
INIT_STUB(video_stream_init)
INIT_STUB(rknn_yolo_init)
INIT_STUB(video_rec_init)
DTOR_STUB(can_cleanup)
DTOR_STUB(tcp_cleanup)
DTOR_STUB(http_server_stop)
DTOR_STUB(follow_destroy)
DTOR_STUB(ano_destroy)
DTOR_STUB(video_stream_shutdown)
DTOR_STUB(rknn_yolo_destroy)
DTOR_STUB(video_rec_destroy)
TASK_STUB(can_recv_task)
TASK_STUB(can_send_task)
TASK_STUB(tcp_task)
TASK_STUB(http_server_task)
TASK_STUB(video_stream_task)
TASK_STUB(rknn_yolo_task)
TASK_STUB(video_rec_task)

static void reset_watchdog(app_ctx_t *app)
{
    memset(app, 0, sizeof(*app));
    app->running = 1;
    memset(g_wd_slots, 0, sizeof(g_wd_slots));
    fake_now = 100;
    wd_notify_sec = 0;
    notification_result = 1;
    stop_on_sleep = wait_calls = drain_on_wait = 0;
    test_app = app;
}

static void record_first(void *app)
{
    (void)app;
    destructors[destructor_count++] = 1;
}

static void record_second(void *app)
{
    (void)app;
    destructors[destructor_count++] = 2;
}

static void forbidden_destructor(void *app)
{
    (void)app;
    _Exit(88);
}

static void forbidden_atexit(void)
{
    _Exit(89);
}

int main(void)
{
    app_ctx_t app;
    reset_watchdog(&app);
    assert(watchdog_register_thread(pthread_self(), "stalled", 1, 1) >= 0);
    watchdog_task(&app);
    assert(!app.running && app.fatal);
    puts("PASS: missed heartbeat requests failure exit");

    reset_watchdog(&app);
    wd_notify_sec = 1;
    notification_result = -1;
    watchdog_task(&app);
    assert(!app.running && app.fatal && fake_now == 103);
    puts("PASS: repeated systemd notification failure requests failure exit");

    reset_watchdog(&app);
    stop_on_sleep = 1;
    watchdog_task(&app);
    assert(!app.running && !app.fatal);
    puts("PASS: normal stop does not become a fatal failure");

    memset(g_modules, 0, sizeof(g_modules));
    g_modules[0].name = "first";
    g_modules[0].ops.dtor = record_first;
    g_modules[1].name = "second";
    g_modules[1].ops.dtor = record_second;
    g_app.running = 1;
    g_app.threads_running = 1;
    wait_calls = 0;
    drain_on_wait = 50; /* A worker finishing at the deadline must still drain. */
    shutdown_modules_safe();
    assert(!g_app.running && wait_calls == 50);
    assert(destructor_count == 2 && destructors[0] == 2 && destructors[1] == 1);
    puts("PASS: drained workers allow reverse-order destruction at deadline");

    fflush(NULL);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        g_app.running = 1;
        g_app.threads_running = 1;
        wait_calls = drain_on_wait = 0;
        g_modules[1].ops.dtor = forbidden_destructor;
        assert(atexit(forbidden_atexit) == 0);
        shutdown_modules_safe();
        _Exit(90); /* Returning from timed-out shutdown would be a regression. */
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
    puts("PASS: stalled workers bypass destructors and atexit, exit status 1");
    return 0;
}
