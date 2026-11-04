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
#include <stdint.h>
#include <stdlib.h>
#include <sys/syscall.h>

#include "core/config.h"
#include "can/can_socket.h"
#include "can/dbc_parser.h"
#include "tcp/tcp_server.h"
#include "http/http.h"
#include "watchdog/watchdog.h"
#include "core/log.h"
#include "core/common.h"
#include "core/thread.h"
#include "core/cpu_affinity.h"
#include "video/video_stream.h"
#include "video/video_rec.h"
#include "yolo/rknn_yolo.h"
#include "control/car_sport_ctrl.h"
#include "telemetry/ano.h"

/* SCHED_DEADLINE 支持：glibc 的 pthread_attr_setschedpolicy() 不接受该策略，
 * 必须在线程创建后用 sched_setattr() 切换。准入控制失败（EBUSY）、参数非法或
 * 内核不支持时回退 FIFO，回退优先级取表中的 priority，保证线程总能起来。 */
#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#endif
#ifndef SYS_sched_setattr
#define SYS_sched_setattr 274
#endif

/* uapi 的 struct sched_attr（SCHED_ATTR_SIZE_VER0 = 48） */
struct app_sched_attr {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t  sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime;
    uint64_t sched_deadline;
    uint64_t sched_period;
};

typedef struct {
    void *(*task)(void *);
    void *arg;
    const char *name;
    int fallback_priority;
    uint64_t runtime_us, deadline_us, period_us;
} deadline_start_t;

typedef struct {
    const char *name;
    int (*init)(void *);
    void (*dtor)(void *);
    void *(*task)(void *);
    int policy, priority;
    cpu_group_t cpus;
    /* 仅当 dl_period_us > 0 时生效：runtime/deadline/period，单位微秒 */
    uint64_t dl_runtime_us, dl_deadline_us, dl_period_us;
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

/* 同表管理生命周期、调度策略、优先级和 CPU 分组。policy=-1 表示无线程；
 * 内部线程配置行由所属模块创建。各模块自行注册/注销看门狗。 */
static const module_t g_modules[] = {
    /* name             init                dtor                    task                policy       priority  cpus       dl_runtime dl_deadline dl_period (us) */
    {"context",         context_init,       NULL,                   NULL,               -1,           0,       CPU_INHERIT},
    {"log",             log_init,           log_close,              NULL,               -1,           0,       CPU_INHERIT},
    {"config",          config_init,        config_cleanup,         NULL,               -1,           0,       CPU_INHERIT},
    {"main",            scheduling_init,    NULL,                   NULL,               SCHED_FIFO,   5,       CPU_LITTLE},
    {"dbc",             dbc_init,           dbc_cleanup,            NULL,               -1,           0,       CPU_INHERIT},
    {"can",             can_init,           can_cleanup,            NULL,               -1,           0,       CPU_INHERIT},
    {"can_recv",        NULL,               NULL,                   can_recv_task,      SCHED_FIFO,  15,       CPU_LITTLE},
    {"can_send",        NULL,               NULL,                   can_send_task,      SCHED_FIFO,  15,       CPU_LITTLE},
    {"tcp",             tcp_init,           tcp_cleanup,            tcp_task,           SCHED_FIFO,   5,       CPU_LITTLE},
    {"http",            http_server_start,  http_server_stop,       http_server_task,   SCHED_FIFO,   5,       CPU_LITTLE},
    {"http_worker",     NULL,               NULL,                   NULL,               SCHED_FIFO,   1,       CPU_LITTLE},
    {"car_sport_ctrl",  follow_init,        follow_destroy,         follow_task,        SCHED_FIFO,  20,       CPU_LITTLE},
    {"ano",             ano_init,           ano_destroy,            NULL,               -1,           0,       CPU_INHERIT},
    /* SCHED_DEADLINE 要求亲和性覆盖整个 root domain，故用 CPU_ALL；绑核与
       deadline 在内核里互斥（__check_sched_dl_overflow 直接返回 EPERM）。 */
    {"video",           video_stream_init,  video_stream_shutdown,  video_stream_task,  SCHED_FIFO,  15,       CPU_ALL,    2000,      34000,       34000},
    /* YOLO 为必要模块：初始化失败即回滚启动。 */
    {"yolo",            rknn_yolo_init,     rknn_yolo_destroy,      rknn_yolo_task,     SCHED_FIFO,  10,       CPU_BIG},
    {"yolo_infer",      NULL,               NULL,                   NULL,               SCHED_FIFO,  12,       CPU_BIG},
    {"yolo_render",     NULL,               NULL,                   NULL,               SCHED_FIFO,  10,       CPU_BIG},
    {"yolo_report",     NULL,               NULL,                   NULL,               SCHED_FIFO,   1,       CPU_LITTLE},
    {"watchdog",        watchdog_init,      NULL,                   watchdog_task,      SCHED_FIFO,  30,       CPU_LITTLE},
    {"rec",             video_rec_init,     video_rec_destroy,      video_rec_task,     SCHED_FIFO,  10,       CPU_BIG},
    {"signal",          signal_setup,       NULL,                   NULL,               -1,           0,       CPU_INHERIT},
};
#define MOD_COUNT (sizeof(g_modules) / sizeof(g_modules[0]))

static int sched_set_deadline(pid_t tid, const deadline_start_t *d)
{
    struct app_sched_attr a;
    memset(&a, 0, sizeof(a));
    a.size = sizeof(a);
    a.sched_policy = SCHED_DEADLINE;
    a.sched_runtime = d->runtime_us * 1000ull;
    a.sched_deadline = d->deadline_us * 1000ull;
    a.sched_period = d->period_us * 1000ull;
    return (int)syscall(SYS_sched_setattr, tid, &a, 0);
}

static void *deadline_trampoline(void *p)
{
    deadline_start_t d = *(const deadline_start_t *)p;
    free(p);

    if (sched_set_deadline((pid_t)syscall(SYS_gettid), &d) == 0) {
        LOG_INFO("%s: SCHED_DEADLINE runtime=%llu deadline=%llu period=%llu us", d.name,
                 (unsigned long long)d.runtime_us, (unsigned long long)d.deadline_us,
                 (unsigned long long)d.period_us);
    } else {
        /* 准入控制拒绝或内核不支持：退回 FIFO，绝不让线程起不来 */
        LOG_ERROR("%s: SCHED_DEADLINE rejected (%s), falling back to FIFO %d", d.name,
                  strerror(errno), d.fallback_priority);
        struct sched_param sp = {.sched_priority = d.fallback_priority};
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp))
            LOG_ERROR("%s: FIFO fallback failed: %s", d.name, strerror(errno));
    }
    return d.task(d.arg);
}

static const module_t *thread_config(const char *name)
{
    for (size_t i = 0; i < MOD_COUNT; i++)
        if (!strcmp(name, g_modules[i].name)) return g_modules[i].policy >= 0 ? &g_modules[i] : NULL;
    return NULL;
}

int app_thread_create(pthread_t *thread, const char *name,
                      void *(*task)(void *), void *argument, int detached)
{
    if (!thread || !name || !task || (detached != 0 && detached != 1)) return EINVAL;
    const module_t *m = thread_config(name);
    if (!m) return EINVAL;
    int deadline = m->dl_period_us > 0 && m->dl_runtime_us > 0 && m->dl_deadline_us > 0;
    struct sched_param param = {.sched_priority = m->priority};
    pthread_attr_t attributes;
    int rc = pthread_attr_init(&attributes);
    if (rc) return rc;
    rc = pthread_attr_setschedpolicy(&attributes, m->policy);
    if (!rc) rc = pthread_attr_setschedparam(&attributes, &param);
    if (!rc) rc = pthread_attr_setinheritsched(&attributes, PTHREAD_EXPLICIT_SCHED);
    if (!rc && detached) rc = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (!rc && m->cpus != CPU_INHERIT) {
        cpu_set_t cpus;
        rc = cpu_affinity_get(m->cpus, &cpus) < 0 ? errno
             : pthread_attr_setaffinity_np(&attributes, sizeof(cpus), &cpus);
    }
    if (!rc) {
        if (!deadline) {
            rc = pthread_create(thread, &attributes, task, argument);
        } else {
            /* 先用表里的 FIFO 参数创建（可用的回退状态），进线程后切 deadline */
            deadline_start_t *d = malloc(sizeof(*d));
            if (!d) {
                rc = ENOMEM;
            } else {
                d->task = task;
                d->arg = argument;
                d->name = name;
                d->fallback_priority = m->priority;
                d->runtime_us = m->dl_runtime_us;
                d->deadline_us = m->dl_deadline_us;
                d->period_us = m->dl_period_us;
                rc = pthread_create(thread, &attributes, deadline_trampoline, d);
                if (rc) free(d);
            }
        }
    }
    pthread_attr_destroy(&attributes);
    if (rc) LOG_ERROR("create thread '%s' failed: %s", name, strerror(rc));
    return rc;
}

static int scheduling_init(void *arg)
{
    (void)arg;
    const module_t *m = thread_config("main");
    if (!m) { LOG_ERROR("main: missing scheduling configuration"); return -1; }
    int rc = 0;
    if (m->cpus != CPU_INHERIT) {
        cpu_set_t cpus;
        rc = cpu_affinity_get(m->cpus, &cpus) < 0 ? errno
             : pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    }
    struct sched_param param = {.sched_priority = m->priority};
    if (!rc) rc = pthread_setschedparam(pthread_self(), m->policy, &param);
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
