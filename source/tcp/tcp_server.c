/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE   /* 暴露 MSG_NOSIGNAL / eventfd 等 Linux 扩展 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <time.h>
#include "tcp/tcp_server.h"
#include "core/common.h"
#include "core/epoll_util.h"
#include "core/log.h"
#include "watchdog/watchdog.h"
#include "telemetry/vofa.h"

/* Shared TCP loop: configuration wakeups and VOFA connections. */
#define TCP_WAKE_TAG 0x40000000u
#define TCP_VOFA_LISTEN_TAG 0x20000000u
#define TCP_VOFA_CLIENT_TAG(idx) (0x10000000u | (uint32_t)((idx) + 1))
#define TCP_VOFA_PENDING_MS 100

static unsigned long long tcp_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static uint64_t vofa_event_tag(uint32_t token, uint32_t kind)
{
    return ((uint64_t)token << 32) | kind;
}

static void vofa_client_events(tcp_ctx_t *ctx, int index)
{
    tcp_vofa_client_t *client = &ctx->vofa_clients[index];
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLRDHUP | (client->length ? EPOLLOUT : 0),
        .data.u64 = vofa_event_tag(client->token, TCP_VOFA_CLIENT_TAG(index)),
    };
    (void)epoll_ctl(ctx->epfd, EPOLL_CTL_MOD, client->fd, &event);
}

static void vofa_close_client(tcp_ctx_t *ctx, int index, int dropped)
{
    tcp_vofa_client_t *client = &ctx->vofa_clients[index];
    if (client->fd < 0)
        return;
    if (dropped && client->length)
        ctx->vofa_dropped_frames++;
    (void)epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, client->fd, NULL);
    close(client->fd);
    *client = (tcp_vofa_client_t){.fd = -1};
}

static void vofa_close_clients(tcp_ctx_t *ctx)
{
    for (int i = 0; i < TCP_VOFA_CLIENTS; i++)
        vofa_close_client(ctx, i, 1);
}

/* All calls are nonblocking; at most one 20-byte frame is retained per client. */
static void vofa_flush_client(tcp_ctx_t *ctx, int index, unsigned long long now)
{
    tcp_vofa_client_t *client = &ctx->vofa_clients[index];
    if (client->fd < 0 || !client->length)
        return;
    if (now - client->pending_since >= TCP_VOFA_PENDING_MS) {
        ctx->vofa_errors++;
        vofa_close_client(ctx, index, 1);
        return;
    }
    ssize_t written = send(client->fd, client->frame + client->offset,
                           client->length - client->offset, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (written > 0) {
        client->offset += (size_t)written;
        if (client->offset == client->length) {
            client->length = client->offset = 0;
            ctx->vofa_sent_frames++;
        }
    } else if (!written || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        ctx->vofa_errors++;
        vofa_close_client(ctx, index, 1);
        return;
    }
    vofa_client_events(ctx, index);
}

int tcp_vofa_stage(tcp_ctx_t *ctx, int enabled, int port, tcp_vofa_stage_t *stage)
{
    if (!ctx || !ctx->vofa_initialized || !stage || (enabled != 0 && enabled != 1) ||
        port < 1024 || port > 65535) {
        errno = EINVAL;
        return -1;
    }
    *stage = (tcp_vofa_stage_t){.fd = -1};
    pthread_mutex_lock(&ctx->vofa_mutex);
    int reuse = enabled && ctx->vofa_listener >= 0 && ctx->vofa_port == port;
    pthread_mutex_unlock(&ctx->vofa_mutex);
    if (reuse)
        return 0;
    stage->replace = 1;
    if (!enabled)
        return 0;
    stage->fd = tcp_listen(port);
    if (stage->fd < 0)
        return -1;
    pthread_mutex_lock(&ctx->vofa_mutex);
    stage->token = ++ctx->vofa_nonce;
    struct epoll_event event = {
        .events = EPOLLIN,
        .data.u64 = vofa_event_tag(stage->token, TCP_VOFA_LISTEN_TAG),
    };
    int result = epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, stage->fd, &event);
    pthread_mutex_unlock(&ctx->vofa_mutex);
    if (result) {
        int saved = errno;
        close(stage->fd);
        stage->fd = -1;
        errno = saved;
    }
    return result;
}

void tcp_vofa_discard(tcp_ctx_t *ctx, tcp_vofa_stage_t *stage)
{
    if (!ctx || !stage || stage->fd < 0)
        return;
    pthread_mutex_lock(&ctx->vofa_mutex);
    (void)epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, stage->fd, NULL);
    close(stage->fd);
    stage->fd = -1;
    pthread_mutex_unlock(&ctx->vofa_mutex);
}

void tcp_vofa_commit(tcp_ctx_t *ctx, int enabled, int port, tcp_vofa_stage_t *stage)
{
    pthread_mutex_lock(&ctx->vofa_mutex);
    if (stage->replace) {
        vofa_close_clients(ctx);
        if (ctx->vofa_listener >= 0) {
            (void)epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, ctx->vofa_listener, NULL);
            close(ctx->vofa_listener);
        }
        ctx->vofa_listener = stage->fd;
        ctx->vofa_listener_token = stage->token;
        stage->fd = -1;
    }
    ctx->vofa_enabled = enabled;
    ctx->vofa_port = port;
    ctx->vofa_refresh = 1;
    ctx->vofa_listener_error = 0;
    pthread_mutex_unlock(&ctx->vofa_mutex);
    eventfd_signal(ctx->wake_fd);
}

void tcp_vofa_stop(tcp_ctx_t *ctx)
{
    if (!ctx || !ctx->vofa_initialized)
        return;
    tcp_vofa_stage_t stage = {.fd = -1, .replace = 1};
    tcp_vofa_commit(ctx, 0, ctx->vofa_port, &stage);
}

void tcp_vofa_get_status(tcp_ctx_t *ctx, tcp_vofa_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!ctx || !ctx->vofa_initialized)
        return;
    pthread_mutex_lock(&ctx->vofa_mutex);
    out->running = atomic_load(&ctx->task_running);
    out->listening = ctx->vofa_listener >= 0;
    for (int i = 0; i < TCP_VOFA_CLIENTS; i++)
        out->clients += ctx->vofa_clients[i].fd >= 0;
    out->sent_frames = ctx->vofa_sent_frames;
    out->dropped_frames = ctx->vofa_dropped_frames;
    out->errors = ctx->vofa_errors;
    if (ctx->vofa_listener_error)
        safe_strncpy(out->error, sizeof(out->error), "listen_failed");
    pthread_mutex_unlock(&ctx->vofa_mutex);
}

int tcp_vofa_take_refresh(tcp_ctx_t *ctx)
{
    if (!ctx || !ctx->vofa_initialized)
        return 0;
    pthread_mutex_lock(&ctx->vofa_mutex);
    int refresh = ctx->vofa_refresh;
    ctx->vofa_refresh = 0;
    pthread_mutex_unlock(&ctx->vofa_mutex);
    return refresh;
}

int tcp_vofa_publish(tcp_ctx_t *ctx, const unsigned char frame[TCP_VOFA_FRAME_BYTES])
{
    if (!ctx || !ctx->vofa_initialized)
        return -1;
    pthread_mutex_lock(&ctx->vofa_mutex);
    if (!ctx->vofa_enabled || ctx->vofa_listener < 0) {
        pthread_mutex_unlock(&ctx->vofa_mutex);
        return -1;
    }
    unsigned long long now = tcp_now_ms();
    for (int i = 0; i < TCP_VOFA_CLIENTS; i++) {
        tcp_vofa_client_t *client = &ctx->vofa_clients[i];
        if (client->fd < 0)
            continue;
        vofa_flush_client(ctx, i, now);
        if (client->fd < 0)
            continue;
        if (client->length) {
            ctx->vofa_dropped_frames++;
            continue;
        }
        memcpy(client->frame, frame, TCP_VOFA_FRAME_BYTES);
        client->length = TCP_VOFA_FRAME_BYTES;
        client->offset = 0;
        client->pending_since = now;
        vofa_flush_client(ctx, i, now);
    }
    pthread_mutex_unlock(&ctx->vofa_mutex);
    return 0;
}

static void tcp_vofa_events(tcp_ctx_t *ctx, uint64_t tag, uint32_t events)
{
    uint32_t kind = (uint32_t)tag, token = (uint32_t)(tag >> 32);
    pthread_mutex_lock(&ctx->vofa_mutex);
    if (kind == TCP_VOFA_LISTEN_TAG) {
        if (ctx->vofa_listener < 0 || token != ctx->vofa_listener_token)
            goto done;
        if (events & (EPOLLERR | EPOLLHUP)) {
            (void)epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, ctx->vofa_listener, NULL);
            close(ctx->vofa_listener);
            ctx->vofa_listener = -1;
            ctx->vofa_listener_error = 1;
            ctx->vofa_errors++;
            vofa_close_clients(ctx);
            goto done;
        }
        for (int attempt = 0; attempt <= TCP_VOFA_CLIENTS; attempt++) {
            int fd = accept4(ctx->vofa_listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0)
                break;
            int slot = -1;
            for (int i = 0; i < TCP_VOFA_CLIENTS; i++)
                if (ctx->vofa_clients[i].fd < 0) {
                    slot = i;
                    break;
                }
            if (slot < 0) {
                close(fd);
                continue;
            }
            int one = 1, buffer = 1024;
            (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
            (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
            tcp_vofa_client_t *client = &ctx->vofa_clients[slot];
            *client = (tcp_vofa_client_t){.fd = fd, .token = ++ctx->vofa_nonce};
            struct epoll_event event = {
                .events = EPOLLIN | EPOLLRDHUP,
                .data.u64 = vofa_event_tag(client->token, TCP_VOFA_CLIENT_TAG(slot)),
            };
            if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, fd, &event)) {
                close(fd);
                client->fd = -1;
                ctx->vofa_errors++;
                continue;
            }
            ctx->vofa_refresh = 1;
        }
    } else {
        int index = (int)(kind & 0x0fffffffu) - 1;
        if (index < 0 || index >= TCP_VOFA_CLIENTS)
            goto done;
        tcp_vofa_client_t *client = &ctx->vofa_clients[index];
        if (client->fd < 0 || client->token != token)
            goto done;
        if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
            vofa_close_client(ctx, index, 1);
            goto done;
        }
        if (events & EPOLLIN) {
            char input[128];
            ssize_t count = recv(client->fd, input, sizeof(input), MSG_DONTWAIT);
            if (!count || (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
                vofa_close_client(ctx, index, 1);
                goto done;
            }
        }
        vofa_flush_client(ctx, index, tcp_now_ms());
    }
done:
    pthread_mutex_unlock(&ctx->vofa_mutex);
}

static void tcp_vofa_deadlines(tcp_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->vofa_mutex);
    unsigned long long now = tcp_now_ms();
    for (int i = 0; i < TCP_VOFA_CLIENTS; i++)
        vofa_flush_client(ctx, i, now);
    pthread_mutex_unlock(&ctx->vofa_mutex);
}

int tcp_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { LOG_ERROR("tcp socket"); return -1; }
    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("tcp bind :%d: %s", port, strerror(errno));
        close(fd); return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        LOG_ERROR("tcp listen :%d: %s", port, strerror(errno));
        close(fd); return -1;
    }
    if (set_nonblock(fd) < 0) {
        LOG_ERROR("tcp fcntl O_NONBLOCK: %s", strerror(errno));
        close(fd); return -1;
    }
    LOG_INFO("VOFA TCP listening on port %d", port);
    return fd;
}

/* Networking stays in one worker; telemetry sampling owns no socket thread. */
void *tcp_task(void *arg)
{
    app_ctx_t *app = arg;
    tcp_ctx_t *ctx = app->tcp;
    if (!ctx) return NULL;
    atomic_store(&ctx->task_running, 1);
    LOG_INFO("tcp_task started");
    while (app->running) {
        vofa_tick(app);
        tcp_vofa_deadlines(ctx);
        struct epoll_event events[16];
        int timeout = vofa_wait_ms(app);
        if (timeout > 100) timeout = 100;
        int count = epoll_wait_feed(ctx->epfd, events, 16, timeout, "tcp");
        if (count < 0) break;
        for (int i = 0; i < count; i++) {
            uint32_t tag = events[i].data.u32;
            if (tag == TCP_WAKE_TAG)
                eventfd_consume(ctx->wake_fd);
            else if (tag == TCP_VOFA_LISTEN_TAG || (tag & 0xf0000000u) == 0x10000000u)
                tcp_vofa_events(ctx, events[i].data.u64, events[i].events);
        }
    }
    atomic_store(&ctx->task_running, 0);
    LOG_INFO("tcp_task stopped");
    return NULL;
}

int tcp_init(void *arg)
{
    app_ctx_t *app = arg;
    tcp_ctx_t *ctx = app->tcp;
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->epfd = ctx->wake_fd = ctx->vofa_listener = -1;
    pthread_mutex_init(&ctx->vofa_mutex, NULL);
    ctx->vofa_initialized = 1;
    for (int i = 0; i < TCP_VOFA_CLIENTS; i++) ctx->vofa_clients[i].fd = -1;
    ctx->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epfd < 0) return -1;
    ctx->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ctx->wake_fd < 0) return -1;
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = TCP_WAKE_TAG};
    return epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->wake_fd, &event);
}

void tcp_cleanup(void *arg)
{
    app_ctx_t *app = arg;
    tcp_ctx_t *ctx = app->tcp;
    if (!ctx || !ctx->vofa_initialized) return;
    tcp_vofa_stop(ctx);
    if (ctx->wake_fd >= 0) close(ctx->wake_fd);
    if (ctx->epfd >= 0) close(ctx->epfd);
    pthread_mutex_destroy(&ctx->vofa_mutex);
    ctx->wake_fd = ctx->epfd = -1;
    ctx->vofa_initialized = 0;
}
