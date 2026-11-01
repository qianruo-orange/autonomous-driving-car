/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HTTP_H
#define HTTP_H

#define HTTP_DEFAULT_PORT 80

struct app_config_t;
/* HTTP owns detached request/stream workers and waits for them on shutdown. */
int http_worker_running(void);
int http_worker_start(void *(*task)(void *), void *argument);
int  http_server_start(void *arg);
void http_server_stop(void *arg);
void *http_server_task(void *arg);

#endif
