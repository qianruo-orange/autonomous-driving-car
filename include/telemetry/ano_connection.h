/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "tcp/tcp_server.h"
typedef tcp_service_stage_t ano_connection_stage_t;
typedef tcp_service_status_t ano_connection_status_t;
void ano_connection_init(void);
int ano_connection_stage(tcp_ctx_t *, int enabled, int port, ano_connection_stage_t *);
void ano_connection_commit(tcp_ctx_t *, int enabled, int port, ano_connection_stage_t *);
void ano_connection_discard(tcp_ctx_t *, ano_connection_stage_t *);
void ano_connection_stop(tcp_ctx_t *);
int ano_connection_take_refresh(tcp_ctx_t *);
int ano_connection_publish(tcp_ctx_t *, const unsigned char *, size_t);
void ano_connection_get_status(tcp_ctx_t *, ano_connection_status_t *);
