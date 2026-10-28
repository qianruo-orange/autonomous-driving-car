/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <pthread.h>
#include "core/config.h"

#define TCP_VOFA_CLIENTS 2
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

/* TCP transport for VOFA; HTTP manages Web connections separately on :80. */
typedef struct tcp_ctx {
    int epfd;
    int wake_fd;
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

int tcp_listen(int port);

/* Initialize the shared event loop; VOFA opens its configured listener. */
int  tcp_init(void *arg);
void tcp_cleanup(void *ctx);

/* VOFA connections and configuration wakeups share one epoll worker. */
void *tcp_task(void *arg);

#endif /* TCP_SERVER_H */
