/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "http/http_client_limit.h"
#include <pthread.h>
#include <stddef.h>
#include <time.h>

/* The HTTP reactor independently bounds all accepted sockets to 64. */
#define HTTP_TRACKED_SOCKETS 64

static pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint32_t address;
    unsigned sockets;
    unsigned long long idle_since;
    int used;
} clients[HTTP_MAX_CLIENTS];
static struct {
    int fd;
    unsigned slot; /* 1-based; zero marks a free entry. */
} sockets[HTTP_TRACKED_SOCKETS];

unsigned long long http_client_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int http_client_acquire(int fd, uint32_t address, unsigned long long now_ms)
{
    pthread_mutex_lock(&client_lock);
    int entry = -1, slot = -1, available = -1;
    for (int i = 0; i < HTTP_TRACKED_SOCKETS; i++) {
        if (sockets[i].slot && sockets[i].fd == fd) {
            pthread_mutex_unlock(&client_lock);
            return -1;
        }
        if (!sockets[i].slot && entry < 0) entry = i;
    }
    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        if (clients[i].used && clients[i].address == address) slot = i;
        if (available < 0 && (!clients[i].used || (!clients[i].sockets &&
            now_ms >= clients[i].idle_since &&
            now_ms - clients[i].idle_since >= HTTP_CLIENT_GRACE_MS))) available = i;
    }
    if (slot < 0) slot = available;
    if (fd < 0 || entry < 0 || slot < 0) {
        pthread_mutex_unlock(&client_lock);
        return -1;
    }
    clients[slot].used = 1;
    clients[slot].address = address;
    clients[slot].sockets++;
    sockets[entry].fd = fd;
    sockets[entry].slot = (unsigned)slot + 1;
    pthread_mutex_unlock(&client_lock);
    return 0;
}

void http_client_release(int fd, unsigned long long now_ms)
{
    pthread_mutex_lock(&client_lock);
    for (int i = 0; i < HTTP_TRACKED_SOCKETS; i++) {
        if (!sockets[i].slot || sockets[i].fd != fd) continue;
        unsigned slot = sockets[i].slot - 1;
        if (!--clients[slot].sockets) clients[slot].idle_since = now_ms;
        sockets[i].slot = 0;
        break;
    }
    pthread_mutex_unlock(&client_lock);
}
