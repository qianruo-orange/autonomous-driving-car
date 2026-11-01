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
} module_t;

static void sig_handler(int sig);

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

/* 无 task 的模块只初始化/销毁；各模块在自己的 task 中管理看门狗。 */
static const module_t g_modules[] = {
    /* name             init                 dtor                   task */
    {"context",         context_init},
    {"log",             log_init,            log_close},
    {"config",          config_init,         config_cleanup},
    {"dbc",             dbc_init,            dbc_cleanup},
    {"can",             can_init,            can_cleanup},
    {"can_recv",        NULL,                NULL,                  can_recv_task},
    {"can_send",        NULL,                NULL,                  can_send_task},
    {"tcp",             tcp_init,            tcp_cleanup,           tcp_task},
    {"http",            http_server_start,   http_server_stop,      http_server_task},
    {"car_sport_ctrl",  follow_init,         follow_destroy,        follow_task},
    {"ano",             ano_init,            ano_destroy},
    {"video",           video_stream_init,   video_stream_shutdown, video_stream_task},
    /* YOLO 为必要模块：初始化失败即回滚启动。 */
    {"yolo",            rknn_yolo_init,      rknn_yolo_destroy,     rknn_yolo_task},
    {"watchdog",        watchdog_init,       NULL,                  watchdog_task},
    {"rec",             video_rec_init,      video_rec_destroy,     video_rec_task},
    {"signal",          signal_setup},
};
#define MOD_COUNT (sizeof(g_modules) / sizeof(g_modules[0]))

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

        int rc = g_app.running ? pthread_create(&threads[started], NULL, m->task, &g_app) : ECANCELED;
        if (rc) {
            LOG_ERROR("create thread '%s' failed: %s", m->name, strerror(rc));
            goto shutdown;
        }
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
