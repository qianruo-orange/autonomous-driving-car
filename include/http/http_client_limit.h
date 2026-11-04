/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HTTP_CLIENT_LIMIT_H
#define HTTP_CLIENT_LIMIT_H

#include <stdint.h>

/* Two slots were not enough: a browser page holds one identity while it
 * streams video, and any script or dashboard from another address then loses
 * the remaining slot, which turns every API call into 503. */
#define HTTP_MAX_CLIENTS 8
#define HTTP_CLIENT_GRACE_MS 30000ULL
/* An identity whose sockets never came back (aborted stream, lost close
 * callback) would otherwise hold its slot for the process lifetime. */
#define HTTP_CLIENT_STALE_MS 120000ULL

unsigned long long http_client_now_ms(void);
/* One client is one source IPv4 address, in network byte order. Parallel page,
 * API, video and download sockets from that address share a client slot. */
int http_client_acquire(int fd, uint32_t address, unsigned long long now_ms);
/* MUST run before close(fd), including on detached workers, so a reused file
 * descriptor cannot be mistaken for the connection that just finished. */
void http_client_release(int fd, unsigned long long now_ms);

#endif
