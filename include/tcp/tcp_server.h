/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <pthread.h>
#include "core/config.h"
#include "tcp/tcp_queue.h"

#define TCP_MAX_CLIENTS   32
#define TCP_VOFA_CLIENTS 4
#define TCP_VOFA_FRAME_BYTES 20

typedef struct {
    int fd;
    uint32_t token;
    size_t offset, length;
    unsigned long long pending_since;
    unsigned char frame[TCP_VOFA_FRAME_BYTES];
} tcp_vofa_client_t;

typedef struct {
    int fd, replace;
    uint32_t token;
} tcp_vofa_stage_t;

typedef struct {
    int running, listening, clients;
    unsigned long long sent_frames, dropped_frames, errors;
    char error[40];
} tcp_vofa_status_t;

/* ---- TCP 客户端状态 ---- */
typedef struct {
    int  fd;
    char wbuf[WBUF_SIZE];   /* 单客户端正在发送的部分写缓冲 */
    int  wlen;
} client_t;

/* ---- TCP 子系统上下文 ----
 * TX 队列：业务线程压入待发送包，tcp_task 弹出并写客户端 socket。
 * 用 eventfd 唤醒，epoll 检测"有数据压入"。 */
typedef struct tcp_ctx {
    int           listen_fd;
    int           port;
    int           max_clients;
    char          bind_ifname[IFNAMSIZ]; /* TCP 监听绑定网卡名（空 = 绑定所有网卡） */
    client_t      clients[TCP_MAX_CLIENTS];
    int           client_count;
    pthread_mutex_t client_mutex;
    int           epfd;      /* TCP 数据收发 epoll（tcp_task 线程） */

    pthread_mutex_t tx_mutex;
    tcp_queue_t     txq;
    int             tx_efd;
    /* Separate protocol endpoints share this context, epoll and tcp_task. */
    pthread_mutex_t vofa_mutex;
    int vofa_initialized, vofa_listener, vofa_enabled, vofa_port, vofa_refresh;
    uint32_t vofa_nonce, vofa_listener_token;
    tcp_vofa_client_t vofa_clients[TCP_VOFA_CLIENTS];
    unsigned long long vofa_sent_frames, vofa_dropped_frames, vofa_errors;
    int vofa_listener_error;
    atomic_int task_running;
} tcp_ctx_t;

/* Stage validates/binds/registers a new VOFA listener without replacing the
 * active endpoint. Commit cannot fail after configuration is persisted. */
int tcp_vofa_stage(tcp_ctx_t *ctx, int enabled, int port, tcp_vofa_stage_t *stage);
void tcp_vofa_discard(tcp_ctx_t *ctx, tcp_vofa_stage_t *stage);
void tcp_vofa_commit(tcp_ctx_t *ctx, int enabled, int port, tcp_vofa_stage_t *stage);
void tcp_vofa_stop(tcp_ctx_t *ctx);
void tcp_vofa_get_status(tcp_ctx_t *ctx, tcp_vofa_status_t *out);
int tcp_vofa_take_refresh(tcp_ctx_t *ctx);
int tcp_vofa_publish(tcp_ctx_t *ctx, const unsigned char frame[TCP_VOFA_FRAME_BYTES]);

int tcp_listen(int port, const char *bind_ifname);

/* 创建 TCP 监听套接字。必须在 can_init() 之后调用。 */
int  tcp_init(void *arg);
void tcp_cleanup(void *ctx);

/* TCP 数据收发线程（独立 epoll 管理 listen + 客户端读写 + TX eventfd） */
void *tcp_task(void *arg);

#endif /* TCP_SERVER_H */
