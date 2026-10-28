/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ANO_H
#define ANO_H

#include "core/common.h"

/* Read-only ANO V8 sampling/configuration. All sockets and networking use
 * the existing TCP context and tcp_task; this module owns no TCP worker. */
int ano_init(void *app);
void ano_destroy(void *app);
void http_ano_get(app_ctx_t *, int, const char *, const char *, const char *);
void http_ano_post(app_ctx_t *, int, const char *, const char *, const char *);
/* Called only by the shared TCP event loop, outside its transport locks. */
void ano_tick(app_ctx_t *app);
int ano_wait_ms(app_ctx_t *app);

#endif
