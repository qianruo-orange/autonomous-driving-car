/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HTTP_CLIENT_LIMIT_H
#define HTTP_CLIENT_LIMIT_H

#include <stdint.h>

#define HTTP_MAX_CLIENTS 2
#define HTTP_CLIENT_GRACE_MS 30000ULL

unsigned long long http_client_now_ms(void);
/* One client is one source IPv4 address, in network byte order. Parallel page,
 * API, video and download sockets from that address share a client slot. */
int http_client_acquire(int fd, uint32_t address, unsigned long long now_ms);
/* MUST run before close(fd), including on detached workers, so a reused file
 * descriptor cannot be mistaken for the connection that just finished. */
void http_client_release(int fd, unsigned long long now_ms);

#endif
