/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE

/**
 * main.c — 模块总表框架：
 *   每个模块一个 module_t：
 *     name 模块名，tid 运行时线程句柄
 *     ops  模块生命周期函数（init/dtor/task）
 *     wd   看门狗参数（timeout/max_miss）
 *     thr  线程参数（stack_size/priority/cpu）
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>

#include "core/config.h"
#include "can/can_socket.h"
#include "can/dbc_parser.h"
#include "tcp/tcp_server.h"
#include "http/http.h"
#include "watchdog/watchdog.h"
#include "core/log.h"
#include "core/common.h"
#include "core/cpu_affinity.h"
#include "video/video_stream.h"
#include "video/video_rec.h"
#include "yolo/rknn_yolo.h"
#include "control/car_sport_ctrl.h"
#include "telemetry/ano.h"

/* 模块生命周期函数 */
typedef struct {
    int  (*init)(void *arg);    // 初始化函数，返回 0 成功
    void (*dtor)(void *arg);    // 清理函数（各模块实现均返回 void）
    void *(*task)(void *arg);   // 任务函数，返回 NULL 退出
} module_ops_t;

/* 看门狗参数 */
typedef struct {
    int timeout;
    int max_miss;
} module_wd_t;

/* 线程参数（创建属性） */
typedef struct {
    size_t stack_size;   /* 线程栈大小（字节），0 = 系统默认 */
    int    priority;     /* 调度优先级，0 = 默认调度，>0 = SCHED_FIFO 实时优先级(1-99) */
    int    cpu;          /* 绑定核心，-1 = 不绑定，>=0 = 绑定到指定 CPU */
} module_thread_cfg_t;

/* 模块总表条目：一个模块的完整描述 */
typedef struct {
    pthread_t           tid;
    const char         *name;
    module_ops_t        ops;
    module_wd_t         wd;
    module_thread_cfg_t thr;
    int                 initialized; /* 仅清理已成功初始化的模块 */
} module_t;

static void sig_handler(int sig);

static void set_current_thread_name(const char *name)
{
    if (!name || !*name) return;
    char short_name[16];
    safe_strncpy(short_name, sizeof(short_name), name);
    (void)pthread_setname_np(pthread_self(), short_name);
}

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
    atomic_init(&app->threads_running, 0);
    set_current_thread_name("main");
    return 0;
}

static module_t g_modules[] = {
    {
        .name = "context",
        .ops = { .init = context_init },
    },
    /* 无独立线程：正序初始化、逆序清理，日志最后关闭。 */
    {
        .name = "log",
        .ops = { .init = log_init, .dtor = log_close },
    },
    {
        .name = "config",
        .ops = { .init = config_init, .dtor = config_cleanup },
    },
    {
        .name = "dbc",
        .ops = { .init = dbc_init, .dtor = dbc_cleanup },
    },
    /* can（只负责初始化 / 清理，不占独立线程） */
    {
        .name = "can",
        .ops  = { .init = can_init, .dtor = can_cleanup },
    },
    /* can_recv —— 接收线程：按端口和 CAN ID 投递业务数据槽，喂狗 */
    {
        .name = "can_recv",
        .ops  = { .task = can_recv_task },
        .wd   = { .timeout = 3, .max_miss = 3 },
        .thr  = { .cpu = -1 },
    },
    /* can_send —— 发送线程：排空 txq 写 socket 做发送 DBC 解析，喂狗 */
    {
        .name = "can_send",
        .ops  = { .task = can_send_task },
        .wd   = { .timeout = 3, .max_miss = 3 },
        .thr  = { .cpu = -1 },
    },
    /* Anonymous Assistant network transport. */
    {
        .name = "tcp",
        .ops = { .init = tcp_init, .dtor = tcp_cleanup, .task = tcp_task },
        .wd = { .timeout = 5, .max_miss = 3 },
        .thr = { .cpu = -1 },
    },
    /* http */
    {
        .name = "http",
        .ops  = { .init = http_server_start, .dtor = http_server_stop, .task = http_server_task },
        .wd   = { .timeout = 5, .max_miss = 3 },
        .thr  = { .cpu = -1 },
    },
    /* Event-driven differential vehicle controller owns a joinable FIFO worker. */
    {
        .name = "car_sport_ctrl",
        .ops = { .init = follow_init, .dtor = follow_destroy, .task = NULL },
    },
    /* Read-only Anonymous Assistant telemetry observes the controller. */
    {
        .name = "ano",
        .ops = { .init = ano_init, .dtor = ano_destroy, .task = NULL },
    },
    /* video */
    {
        .name = "video",
        .ops  = { .init = video_stream_init, .dtor = video_stream_shutdown, .task = video_stream_task },
        .wd   = { .timeout = 5, .max_miss = 3 },
        .thr  = { .cpu = -1 },
    },
    /* yolo —— RKNN YOLO26 检测（必要流程）：无模型/NPU 不可用时 init 返回 -1，
       服务启动直接失败（无 YOLO 直接报错），不进降级空转 */
    {
        .name = "yolo",
        .ops  = { .init = rknn_yolo_init, .dtor = rknn_yolo_destroy, .task = rknn_yolo_task },
        .wd   = { .timeout = 5, .max_miss = 3 },
        .thr  = { .cpu = -1 },
    },
    /* watchdog —— 监控者，不能监督自己，故 timeout=0（不注册自身） */
    {
        .name = "watchdog",
        .ops  = { .init = watchdog_init, .task = watchdog_task },
        .thr  = { .cpu = -1 },
    },
    /* rec —— 网络录像（HTTP 控制）：空闲空转喂狗，录制中每帧喂狗 */
    {
        .name = "rec",
        .ops  = { .init = video_rec_init, .dtor = video_rec_destroy, .task = video_rec_task },
        .wd   = { .timeout = 5, .max_miss = 3 },
        .thr  = { .cpu = -1 },
    },
    /* signal */
    {
        .name = "signal",
        .ops  = { .init = signal_setup },
    },
};
#define MOD_COUNT (int)(sizeof(g_modules)/sizeof(g_modules[0]))

/**
 * sig_handler - 处理退出信号，通知主循环停止服务。
 * @sig: 接收到的信号编号。
 */
static void sig_handler(int sig) { (void)sig; g_app.running = 0; }

/**
 * thread_wrapper - 线程入口函数，执行模块线程并在退出时递减活跃线程计数。
 * @arg: 模块下标（intptr_t 转 void*）。
 */
static void *thread_wrapper(void *arg)
{
    int idx = (int)(intptr_t)arg;
    const char *name = g_modules[idx].name;
    set_current_thread_name(name);
    if (!strcmp(name, "video") || !strcmp(name, "yolo") || !strcmp(name, "rec"))
        (void)cpu_bind_big();
    else
        (void)cpu_bind_little();
    if (g_modules[idx].ops.task) g_modules[idx].ops.task(&g_app);
    watchdog_unregister_self(name);
    __atomic_fetch_sub(&g_app.threads_running, 1, __ATOMIC_RELEASE);
    return NULL;
}

/**
 * wait_threads_exit - 在有限时间内等待所有工作线程退出，避免析构与线程并发访问资源。
 */
static int wait_threads_exit(void)
{
    for (int i = 0; i < 50; i++) {
        if (__atomic_load_n(&g_app.threads_running, __ATOMIC_ACQUIRE) <= 0) return 0;
        usleep(100000);   /* 每次 100ms，最多 5s */
    }
    return __atomic_load_n(&g_app.threads_running, __ATOMIC_ACQUIRE) > 0 ? -1 : 0;
}

/**
 * shutdown_modules_safe - 安全关闭各模块并等待线程退出，避免在关闭时出现竞争或卡死。
 * 顺序：先置停止标志（让工作线程自行退出）→ 等待线程全部退出 → 再按创建逆序
 * 执行析构（依赖方先清理，且此时已无工作线程访问资源，不会与析构竞争）。
 */
static void shutdown_modules_safe(void)
{
    LOG_INFO("Shutting down modules...");

    /* 1. 先通知所有工作线程退出。 */
    g_app.running = 0;

    /* 2. 等待线程退出后再析构，避免析构释放资源时线程仍在访问 */
    if (wait_threads_exit() < 0) {
        /* A live worker may hold module/log locks or still reference resources.
         * Do not run destructors, stdio/atexit cleanup, or return through main's
         * module-owned contexts. Process exit lets the kernel reclaim resources
         * and the failure status triggers systemd Restart=on-failure. */
        static const char message[] =
            "shutdown: workers did not exit within 5s; skipping destructors and exiting with failure\n";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        _Exit(EXIT_FAILURE);
    }

    /* 3. 按创建逆序执行析构，且不重复调用（http_server_stop / video_stream_shutdown
          分别是 http / video 模块的 dtor，这里统一调用一次） */
    for (int i = MOD_COUNT - 1; i >= 0; i--) {
        if (g_modules[i].initialized && g_modules[i].ops.dtor) {
            LOG_INFO("Shutting down module %s...", g_modules[i].name);
            g_modules[i].ops.dtor(&g_app);
            g_modules[i].initialized = 0;
        }
    }
}

/**
 * main - 初始化所有模块、创建线程、进入主循环，并在退出时统一清理资源。
 */
int main(void)
{
    for (int i = 0; i < MOD_COUNT; i++) {
        module_t *m = &g_modules[i];

        if (m->ops.init && m->ops.init(&g_app) < 0) goto fail;
        m->initialized = 1;
        if (!m->ops.task) continue;

        /* 按模块线程属性创建 detached 线程 */
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (m->thr.stack_size > 0)
            pthread_attr_setstacksize(&attr, m->thr.stack_size);

        if (m->thr.priority > 0) {
            struct sched_param sp;
            memset(&sp, 0, sizeof(sp));
            sp.sched_priority = m->thr.priority;
            pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
            pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
            pthread_attr_setschedparam(&attr, &sp);
        }

        __atomic_fetch_add(&g_app.threads_running, 1, __ATOMIC_RELAXED);
        int rc = pthread_create(&m->tid, &attr, thread_wrapper, (void *)(intptr_t)i);
        pthread_attr_destroy(&attr);

        if (rc != 0) {
            __atomic_fetch_sub(&g_app.threads_running, 1, __ATOMIC_RELAXED);
            LOG_ERROR("create thread '%s' failed: %s", m->name, strerror(rc));
            goto fail;
        }

        /* 绑核（可选） */
        if (m->thr.cpu >= 0) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(m->thr.cpu, &set);
            if (pthread_setaffinity_np(m->tid, sizeof(set), &set) != 0)
                LOG_ERROR("bind thread '%s' to cpu %d failed: %s",
                          m->name, m->thr.cpu, strerror(errno));
        }

        /* 注册看门狗（timeout>0 才纳入监督） */
        if (m->wd.timeout > 0)
            watchdog_register_thread(m->tid, m->name, m->wd.timeout, m->wd.max_miss);
    }

    watchdog_register_thread(pthread_self(), "main", 15, 1);
    while (g_app.running) { 
        watchdog_feed_self("main"); 
        sleep(1); 
    }
    watchdog_unregister_self("main");

    /* 通知各模块停止，让工作线程尽快退出。
       运行期致命错误（如 YOLO 停摆）以退出码 1 结束：systemd Restart=on-failure 自动拉起 */
    shutdown_modules_safe();
    return g_app.fatal ? 1 : 0;

fail:
    watchdog_unregister_self("main");
    shutdown_modules_safe();
    return 1;
}
