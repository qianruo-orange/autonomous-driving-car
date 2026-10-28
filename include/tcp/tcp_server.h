/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <pthread.h>
#include "core/config.h"

#define TCP_ANO_CLIENTS 2
#define TCP_ANO_FRAME_BYTES 256

typedef struct {
    int fd;
    uint32_t token;
    size_t offset, length;
    unsigned long long pending_since;
    unsigned char frame[TCP_ANO_FRAME_BYTES];
    unsigned char input[TCP_ANO_FRAME_BYTES];
    size_t input_length;
    unsigned requests;
} tcp_ano_client_t;

typedef struct {
    int fd, replace;
    uint32_t token;
} tcp_ano_stage_t;

typedef struct {
    int running, listening, clients;
    unsigned long long sent_frames, dropped_frames, errors;
    char error[40];
} tcp_ano_status_t;

/* TCP transport for ANO; HTTP manages Web connections separately on :80. */
typedef struct tcp_ctx {
    int epfd;
    int wake_fd;
    pthread_mutex_t ano_mutex;
    int ano_initialized, ano_listener, ano_enabled, ano_port, ano_refresh;
    uint32_t ano_nonce, ano_listener_token;
    tcp_ano_client_t ano_clients[TCP_ANO_CLIENTS];
    unsigned long long ano_sent_frames, ano_dropped_frames, ano_errors;
    int ano_listener_error;
    atomic_int task_running;
} tcp_ctx_t;

/* Stage validates/binds/registers a new ANO listener without replacing the
 * active endpoint. Commit cannot fail after configuration is persisted. */
int tcp_ano_stage(tcp_ctx_t *ctx, int enabled, int port, tcp_ano_stage_t *stage);
void tcp_ano_discard(tcp_ctx_t *ctx, tcp_ano_stage_t *stage);
void tcp_ano_commit(tcp_ctx_t *ctx, int enabled, int port, tcp_ano_stage_t *stage);
void tcp_ano_stop(tcp_ctx_t *ctx);
void tcp_ano_get_status(tcp_ctx_t *ctx, tcp_ano_status_t *out);
int tcp_ano_take_refresh(tcp_ctx_t *ctx);
int tcp_ano_publish(tcp_ctx_t *ctx, const unsigned char *frame, size_t length);

int tcp_listen(int port);

/* Initialize the shared event loop; ANO opens its configured listener. */
int  tcp_init(void *arg);
void tcp_cleanup(void *ctx);

/* ANO connections and configuration wakeups share one epoll worker. */
void *tcp_task(void *arg);

#endif /* TCP_SERVER_H */
