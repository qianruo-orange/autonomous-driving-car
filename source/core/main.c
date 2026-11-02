/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE

/* 模块表按依赖顺序初始化，退出时按相反顺序销毁。 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "core/config.h"
#include "can/can_socket.h"
#include "can/dbc_parser.h"
#include "tcp/tcp_server.h"
#include "http/http.h"
#include "watchdog/watchdog.h"
#include "core/log.h"
#include "core/common.h"
#include "core/thread.h"
#include "video/video_stream.h"
#include "video/video_rec.h"
#include "yolo/rknn_yolo.h"
#include "control/car_sport_ctrl.h"
#include "telemetry/ano.h"

typedef struct {
    const char *name;
    int (*init)(void *);
    void (*dtor)(void *);
    void *(*task)(void *);
    int priority;
} module_t;

static void sig_handler(int sig);
static int scheduling_init(void *arg);

static int signal_setup(void *arg)
{
    (void)arg;
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    /* 写已断开 socket 会触发 SIGPIPE，默认会终止进程；统一忽略，
       由 write() 的返回值处理错误（见 http.c） */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

/* 文件级应用上下文（仅本文件直接访问；其他模块通过指针参数获得） */
static app_ctx_t g_app;

/* Application threads start with explicit attributes; codec/runtime helpers
 * inherit their owner's scheduler. All policy and priority choices live here. */
static const int g_thread_policy = SCHED_FIFO;

/* 上下文仅保存运行标志和模块借出的指针，资源由各模块持有。 */
static int context_init(void *arg)
{
    app_ctx_t *app = arg;
    memset(app, 0, sizeof(*app));
    atomic_init(&app->running, 1);
    atomic_init(&app->fatal, 0);
    (void)pthread_setname_np(pthread_self(), "main");
    return 0;
}

/* 同表管理生命周期和调度。只有优先级的行由所属模块创建内部线程；
 * 无 task 的行不由 main 创建线程。各模块自行注册/注销看门狗。 */
static const module_t g_modules[] = {
    /* name             init                 dtor                   task                 priority */
    {"context",         context_init},
    {"log",             log_init,            log_close},
    {"config",          config_init,         config_cleanup},
    {"main",            scheduling_init,     .priority = 5},
    {"dbc",             dbc_init,            dbc_cleanup},
    {"can",             can_init,            can_cleanup},
    {"can_recv",        NULL,                NULL,                  can_recv_task,       15},
    {"can_send",        NULL,                NULL,                  can_send_task,       15},
    {"tcp",             tcp_init,            tcp_cleanup,           tcp_task,             5},
    {"http",            http_server_start,   http_server_stop,      http_server_task,     5},
    {"http_worker",     .priority = 1},
    {"car_sport_ctrl",  follow_init,         follow_destroy,        follow_task,         20},
    {"ano",             ano_init,            ano_destroy},
    {"video",           video_stream_init,   video_stream_shutdown, video_stream_task,   15},
    /* YOLO 为必要模块：初始化失败即回滚启动。 */
    {"yolo",            rknn_yolo_init,      rknn_yolo_destroy,     rknn_yolo_task,       10},
    {"yolo_infer",      .priority = 12},
    {"yolo_render",     .priority = 10},
    {"yolo_report",     .priority = 1},
    {"watchdog",        watchdog_init,       NULL,                  watchdog_task,       30},
    {"rec",             video_rec_init,      video_rec_destroy,     video_rec_task,      10},
    {"signal",          signal_setup},
};
#define MOD_COUNT (sizeof(g_modules) / sizeof(g_modules[0]))

static int thread_priority(const char *name)
{
    for (size_t i = 0; i < MOD_COUNT; i++)
        if (!strcmp(name, g_modules[i].name)) return g_modules[i].priority > 0 ? g_modules[i].priority : -1;
    return -1;
}

int app_thread_create(pthread_t *thread, const char *name,
                      void *(*task)(void *), void *argument, int detached)
{
    if (!thread || !name || !task || (detached != 0 && detached != 1)) return EINVAL;
    struct sched_param param = {.sched_priority = thread_priority(name)};
    if (param.sched_priority < 0) return EINVAL;
    pthread_attr_t attributes;
    int rc = pthread_attr_init(&attributes);
    if (rc) return rc;
    rc = pthread_attr_setschedpolicy(&attributes, g_thread_policy);
    if (!rc) rc = pthread_attr_setschedparam(&attributes, &param);
    if (!rc) rc = pthread_attr_setinheritsched(&attributes, PTHREAD_EXPLICIT_SCHED);
    if (!rc && detached) rc = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (!rc) rc = pthread_create(thread, &attributes, task, argument);
    pthread_attr_destroy(&attributes);
    if (rc) LOG_ERROR("create thread '%s' failed: %s", name, strerror(rc));
    return rc;
}

static int scheduling_init(void *arg)
{
    (void)arg;
    struct sched_param param = {.sched_priority = thread_priority("main")};
    int rc = pthread_setschedparam(pthread_self(), g_thread_policy, &param);
    if (rc) LOG_ERROR("main: scheduling setup failed: %s", strerror(rc));
    return rc ? -1 : 0;
}

/**
 * sig_handler - 处理退出信号，通知主循环停止服务。
 * @sig: 接收到的信号编号。
 */
static void sig_handler(int sig) { (void)sig; g_app.running = 0; }

int main(void)
{
    int status = EXIT_FAILURE;
    size_t initialized = 0;
    pthread_t threads[MOD_COUNT];
    size_t started = 0;

    while (initialized < MOD_COUNT) {
        const module_t *m = &g_modules[initialized];
        if (m->init && m->init(&g_app) < 0) goto shutdown;
        initialized++;
        if (!m->task) continue;

        int rc = g_app.running ? app_thread_create(&threads[started], m->name, m->task, &g_app, 0) : ECANCELED;
        if (rc) goto shutdown;
        started++;
    }

    if (watchdog_register_thread(pthread_self(), "main", 15, 1) < 0) goto shutdown;
    while (g_app.running) {
        watchdog_feed_self("main");
        sleep(1);
    }
    status = g_app.fatal ? EXIT_FAILURE : EXIT_SUCCESS;

shutdown:
    watchdog_unregister_self("main");
    g_app.running = 0;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 5;
    while (started) {
        if (pthread_clockjoin_np(threads[--started], NULL, CLOCK_MONOTONIC, &deadline)) {
            /* 线程仍持有资源时不析构，失败退出让 systemd 重启。 */
            static const char message[] = "shutdown: could not join workers within 5s; exiting with failure\n";
            (void)write(STDERR_FILENO, message, sizeof(message) - 1);
            _Exit(EXIT_FAILURE);
        }
    }
    while (initialized) {
        const module_t *m = &g_modules[--initialized];
        if (m->dtor) {
            LOG_INFO("Shutting down module %s...", m->name);
            m->dtor(&g_app);
        }
    }
    return status;
}
