/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VOFA_H
#define VOFA_H

#include "core/common.h"

/* Read-only JustFloat sampling/configuration. All sockets and networking use
 * the existing TCP context and tcp_task; this module owns no TCP worker. */
int vofa_init(void *app);
void vofa_destroy(void *app);
void http_vofa_get(app_ctx_t *, int, const char *, const char *, const char *);
void http_vofa_post(app_ctx_t *, int, const char *, const char *, const char *);
/* Called only by the shared TCP event loop, outside its transport locks. */
void vofa_tick(app_ctx_t *app);
int vofa_wait_ms(app_ctx_t *app);

#endif
