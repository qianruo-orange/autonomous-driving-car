/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "core/common.h"
#define TCP_SERVICE_MAX 4
#define TCP_CLIENTS_PER_PORT 2
#define TCP_REPLY_CAPACITY 256
typedef struct tcp_ctx tcp_ctx_t;
/* Protocol callbacks run under the port lock: bounded/nonblocking, no TCP API
 * reentry except tcp_wake. Input is an arbitrary TCP chunk, not a message.
 * A reply length of 0 keeps any pending reply; -1 closes the connection.
 * ops/user must outlive the service. poll runs outside transport locks and returns the next wait in
 * ms. */
typedef struct {
    void (*connected)(app_ctx_t *, int port, int fd, void *user);
    void (*disconnected)(app_ctx_t *, int port, int fd, void *user);
    int (*receive)(app_ctx_t *, int port, int fd, const unsigned char *, size_t,
                   unsigned char *reply, size_t capacity, void *user);
    int (*prepare)(int fd, const unsigned char *, size_t, unsigned char *reply, size_t capacity,
                   void *user);
    int (*poll)(app_ctx_t *, void *user);
} tcp_service_ops_t;
typedef struct {
    int service, fd, port, enabled, replace;
    uint32_t token;
} tcp_service_stage_t;
typedef struct {
    int running, listening, clients;
    unsigned long long sent_frames, dropped_frames, errors;
    char error[40];
} tcp_service_status_t;
/* Registration binds/reserves the port. id is a stable slot, not a descriptor.
 * Use stage/commit/discard when configuration must be persisted transactionally.
 * A disabled service still reserves its port. commit cannot fail after stage. */
int tcp_service_register(tcp_ctx_t *, int port, const tcp_service_ops_t *, void *user, int *id);
int tcp_service_stage(tcp_ctx_t *, int current_id, int enabled, int port, const tcp_service_ops_t *,
                      void *user, tcp_service_stage_t *);
void tcp_service_commit(tcp_ctx_t *, tcp_service_stage_t *);
void tcp_service_discard(tcp_ctx_t *, tcp_service_stage_t *);
void tcp_service_disable(tcp_ctx_t *, int id);
int tcp_service_publish(tcp_ctx_t *, int id, const unsigned char *, size_t);
void tcp_service_get_status(tcp_ctx_t *, int id, tcp_service_status_t *);
int tcp_ready(tcp_ctx_t *);
void tcp_wake(tcp_ctx_t *);
int tcp_init(void *);
void tcp_cleanup(void *);
void *tcp_task(void *);
