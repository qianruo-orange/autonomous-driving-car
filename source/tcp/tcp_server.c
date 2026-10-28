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
#include "telemetry/ano.h"
#include "telemetry/ano_protocol.h"

/* Shared TCP loop: configuration wakeups and ANO connections. */
#define TCP_WAKE_TAG 0x40000000u
#define TCP_ANO_LISTEN_TAG 0x20000000u
#define TCP_ANO_CLIENT_TAG(idx) (0x10000000u | (uint32_t)((idx) + 1))
#define TCP_ANO_PENDING_MS 100
#define ANO_REQUEST_INFO 1u
#define ANO_REQUEST_PARAMETERS 2u
#define ANO_REQUEST_COMMANDS 4u

static unsigned long long tcp_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static uint64_t ano_event_tag(uint32_t token, uint32_t kind)
{
    return ((uint64_t)token << 32) | kind;
}

static void ano_client_events(tcp_ctx_t *ctx, int index)
{
    tcp_ano_client_t *client = &ctx->ano_clients[index];
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLRDHUP | (client->length ? EPOLLOUT : 0),
        .data.u64 = ano_event_tag(client->token, TCP_ANO_CLIENT_TAG(index)),
    };
    (void)epoll_ctl(ctx->epfd, EPOLL_CTL_MOD, client->fd, &event);
}

static void ano_close_client(tcp_ctx_t *ctx, int index, int dropped)
{
    tcp_ano_client_t *client = &ctx->ano_clients[index];
    if (client->fd < 0)
        return;
    if (dropped && client->length)
        ctx->ano_dropped_frames++;
    (void)epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, client->fd, NULL);
    close(client->fd);
    *client = (tcp_ano_client_t){.fd = -1};
}

static void ano_close_clients(tcp_ctx_t *ctx)
{
    for (int i = 0; i < TCP_ANO_CLIENTS; i++)
        ano_close_client(ctx, i, 1);
}

/* All calls are nonblocking; at most one bounded batch is retained per client. */
static void ano_flush_client(tcp_ctx_t *ctx, int index, unsigned long long now)
{
    tcp_ano_client_t *client = &ctx->ano_clients[index];
    if (client->fd < 0 || !client->length)
        return;
    if (now - client->pending_since >= TCP_ANO_PENDING_MS) {
        ctx->ano_errors++;
        ano_close_client(ctx, index, 1);
        return;
    }
    ssize_t written = send(client->fd, client->frame + client->offset,
                           client->length - client->offset, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (written > 0) {
        client->offset += (size_t)written;
        if (client->offset == client->length) {
            client->length = client->offset = 0;
            ctx->ano_sent_frames++;
            if (client->requests) ctx->ano_refresh = 1;
        }
    } else if (!written || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        ctx->ano_errors++;
        ano_close_client(ctx, index, 1);
        return;
    }
    ano_client_events(ctx, index);
}

int tcp_ano_stage(tcp_ctx_t *ctx, int enabled, int port, tcp_ano_stage_t *stage)
{
    if (!ctx || !ctx->ano_initialized || !stage || (enabled != 0 && enabled != 1) ||
        port < 1024 || port > 65535) {
        errno = EINVAL;
        return -1;
    }
    *stage = (tcp_ano_stage_t){.fd = -1};
    pthread_mutex_lock(&ctx->ano_mutex);
    int reuse = enabled && ctx->ano_listener >= 0 && ctx->ano_port == port;
    pthread_mutex_unlock(&ctx->ano_mutex);
    if (reuse)
        return 0;
    stage->replace = 1;
    if (!enabled)
        return 0;
    stage->fd = tcp_listen(port);
    if (stage->fd < 0)
        return -1;
    pthread_mutex_lock(&ctx->ano_mutex);
    stage->token = ++ctx->ano_nonce;
    struct epoll_event event = {
        .events = EPOLLIN,
        .data.u64 = ano_event_tag(stage->token, TCP_ANO_LISTEN_TAG),
    };
    int result = epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, stage->fd, &event);
    pthread_mutex_unlock(&ctx->ano_mutex);
    if (result) {
        int saved = errno;
        close(stage->fd);
        stage->fd = -1;
        errno = saved;
    }
    return result;
}

void tcp_ano_discard(tcp_ctx_t *ctx, tcp_ano_stage_t *stage)
{
    if (!ctx || !stage || stage->fd < 0)
        return;
    pthread_mutex_lock(&ctx->ano_mutex);
    (void)epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, stage->fd, NULL);
    close(stage->fd);
    stage->fd = -1;
    pthread_mutex_unlock(&ctx->ano_mutex);
}

void tcp_ano_commit(tcp_ctx_t *ctx, int enabled, int port, tcp_ano_stage_t *stage)
{
    pthread_mutex_lock(&ctx->ano_mutex);
    if (stage->replace) {
        ano_close_clients(ctx);
        if (ctx->ano_listener >= 0) {
            (void)epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, ctx->ano_listener, NULL);
            close(ctx->ano_listener);
        }
        ctx->ano_listener = stage->fd;
        ctx->ano_listener_token = stage->token;
        stage->fd = -1;
    }
    ctx->ano_enabled = enabled;
    ctx->ano_port = port;
    ctx->ano_refresh = 1;
    ctx->ano_listener_error = 0;
    pthread_mutex_unlock(&ctx->ano_mutex);
    eventfd_signal(ctx->wake_fd);
}

void tcp_ano_stop(tcp_ctx_t *ctx)
{
    if (!ctx || !ctx->ano_initialized)
        return;
    tcp_ano_stage_t stage = {.fd = -1, .replace = 1};
    tcp_ano_commit(ctx, 0, ctx->ano_port, &stage);
}

void tcp_ano_get_status(tcp_ctx_t *ctx, tcp_ano_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!ctx || !ctx->ano_initialized)
        return;
    pthread_mutex_lock(&ctx->ano_mutex);
    out->running = atomic_load(&ctx->task_running);
    out->listening = ctx->ano_listener >= 0;
    for (int i = 0; i < TCP_ANO_CLIENTS; i++)
        out->clients += ctx->ano_clients[i].fd >= 0;
    out->sent_frames = ctx->ano_sent_frames;
    out->dropped_frames = ctx->ano_dropped_frames;
    out->errors = ctx->ano_errors;
    if (ctx->ano_listener_error)
        safe_strncpy(out->error, sizeof(out->error), "listen_failed");
    pthread_mutex_unlock(&ctx->ano_mutex);
}

int tcp_ano_take_refresh(tcp_ctx_t *ctx)
{
    if (!ctx || !ctx->ano_initialized)
        return 0;
    pthread_mutex_lock(&ctx->ano_mutex);
    int refresh = ctx->ano_refresh;
    ctx->ano_refresh = 0;
    pthread_mutex_unlock(&ctx->ano_mutex);
    return refresh;
}

int tcp_ano_publish(tcp_ctx_t *ctx, const unsigned char *frame, size_t length)
{
    if (!ctx || !ctx->ano_initialized || !frame || !length || length > TCP_ANO_FRAME_BYTES)
        return -1;
    pthread_mutex_lock(&ctx->ano_mutex);
    if (!ctx->ano_enabled || ctx->ano_listener < 0) {
        pthread_mutex_unlock(&ctx->ano_mutex);
        return -1;
    }
    unsigned long long now = tcp_now_ms();
    for (int i = 0; i < TCP_ANO_CLIENTS; i++) {
        tcp_ano_client_t *client = &ctx->ano_clients[i];
        if (client->fd < 0)
            continue;
        ano_flush_client(ctx, i, now);
        if (client->fd < 0)
            continue;
        if (client->length) {
            ctx->ano_dropped_frames++;
            continue;
        }
        size_t used = 0, bytes;
        /* Only descriptive queries are supported. Parameters and movement
         * commands remain controlled by the authenticated Web/CAN paths. */
        if (client->requests & ANO_REQUEST_INFO) {
            bytes = ano_v8_device_info(client->frame, sizeof(client->frame) - length);
            if (bytes) { used += bytes; client->requests &= ~ANO_REQUEST_INFO; }
        }
        if (client->requests & ANO_REQUEST_PARAMETERS) {
            const unsigned char empty[] = {1, 0, 0};
            bytes = ano_v8_frame(ANO_V8_FID_PARAMETER, empty, sizeof(empty),
                                 client->frame + used, sizeof(client->frame) - length - used);
            if (bytes) { used += bytes; client->requests &= ~ANO_REQUEST_PARAMETERS; }
        }
        if (client->requests & ANO_REQUEST_COMMANDS) {
            const unsigned char empty[] = {0, 0, 0};
            bytes = ano_v8_frame(ANO_V8_FID_COMMAND_QUERY, empty, sizeof(empty),
                                 client->frame + used, sizeof(client->frame) - length - used);
            if (bytes) { used += bytes; client->requests &= ~ANO_REQUEST_COMMANDS; }
        }
        memcpy(client->frame + used, frame, length);
        client->length = used + length;
        client->offset = 0;
        client->pending_since = now;
        ano_flush_client(ctx, i, now);
    }
    pthread_mutex_unlock(&ctx->ano_mutex);
    return 0;
}

static void ano_consume_input(tcp_ctx_t *ctx, tcp_ano_client_t *client,
                              const unsigned char *bytes, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (client->input_length == sizeof(client->input)) {
            memmove(client->input, client->input + 1, --client->input_length);
        }
        client->input[client->input_length++] = bytes[i];
        while (client->input_length) {
            size_t consumed = 1;
            if (client->input[0] == ANO_V8_HEAD) {
                if (client->input_length < 6) break;
                size_t payload = (size_t)client->input[4] | ((size_t)client->input[5] << 8);
                size_t frame_length = payload + ANO_V8_OVERHEAD;
                /* This read-only endpoint accepts the one-byte query command
                 * and the standard command + two reserved bytes only. Early
                 * filtering prevents a corrupt length from swallowing a later
                 * complete query while waiting for a nonexistent payload. */
                int descriptive = client->input[1] == ANO_V8_HOST_ID &&
                    (client->input[2] == ANO_V8_DEVICE_ID ||
                     client->input[2] == ANO_V8_BROADCAST_ID) &&
                    (client->input[3] == ANO_V8_FID_PARAMETER ||
                     client->input[3] == ANO_V8_FID_COMMAND_QUERY) &&
                    (payload == 1 || payload == 3);
                if (descriptive && frame_length <= sizeof(client->input)) {
                    if (client->input_length < frame_length) break;
                    if (ano_v8_valid(client->input, frame_length)) {
                        consumed = frame_length;
                        if (payload && client->input[1] == ANO_V8_HOST_ID &&
                            (client->input[2] == ANO_V8_DEVICE_ID ||
                             client->input[2] == ANO_V8_BROADCAST_ID)) {
                            unsigned request = 0;
                            if (client->input[3] == ANO_V8_FID_PARAMETER) {
                                if (client->input[6] == 0) request = ANO_REQUEST_INFO;
                                if (client->input[6] == 1) request = ANO_REQUEST_PARAMETERS;
                            } else if (client->input[3] == ANO_V8_FID_COMMAND_QUERY &&
                                       client->input[6] == 0) {
                                request = ANO_REQUEST_COMMANDS;
                            }
                            client->requests |= request;
                            if (request) ctx->ano_refresh = 1;
                        }
                    }
                }
            }
            client->input_length -= consumed;
            memmove(client->input, client->input + consumed, client->input_length);
        }
    }
}

static void tcp_ano_events(tcp_ctx_t *ctx, uint64_t tag, uint32_t events)
{
    uint32_t kind = (uint32_t)tag, token = (uint32_t)(tag >> 32);
    pthread_mutex_lock(&ctx->ano_mutex);
    if (kind == TCP_ANO_LISTEN_TAG) {
        if (ctx->ano_listener < 0 || token != ctx->ano_listener_token)
            goto done;
        if (events & (EPOLLERR | EPOLLHUP)) {
            (void)epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, ctx->ano_listener, NULL);
            close(ctx->ano_listener);
            ctx->ano_listener = -1;
            ctx->ano_listener_error = 1;
            ctx->ano_errors++;
            ano_close_clients(ctx);
            goto done;
        }
        for (int attempt = 0; attempt <= TCP_ANO_CLIENTS; attempt++) {
            int fd = accept4(ctx->ano_listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0)
                break;
            int slot = -1;
            for (int i = 0; i < TCP_ANO_CLIENTS; i++)
                if (ctx->ano_clients[i].fd < 0) {
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
            tcp_ano_client_t *client = &ctx->ano_clients[slot];
            *client = (tcp_ano_client_t){.fd = fd, .token = ++ctx->ano_nonce,
                                          .requests = ANO_REQUEST_INFO};
            struct epoll_event event = {
                .events = EPOLLIN | EPOLLRDHUP,
                .data.u64 = ano_event_tag(client->token, TCP_ANO_CLIENT_TAG(slot)),
            };
            if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, fd, &event)) {
                close(fd);
                client->fd = -1;
                ctx->ano_errors++;
                continue;
            }
            ctx->ano_refresh = 1;
        }
    } else {
        int index = (int)(kind & 0x0fffffffu) - 1;
        if (index < 0 || index >= TCP_ANO_CLIENTS)
            goto done;
        tcp_ano_client_t *client = &ctx->ano_clients[index];
        if (client->fd < 0 || client->token != token)
            goto done;
        if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
            ano_close_client(ctx, index, 1);
            goto done;
        }
        if (events & EPOLLIN) {
            unsigned char input[512];
            ssize_t count = recv(client->fd, input, sizeof(input), MSG_DONTWAIT);
            if (!count || (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
                ano_close_client(ctx, index, 1);
                goto done;
            }
            if (count > 0) ano_consume_input(ctx, client, input, (size_t)count);
        }
        ano_flush_client(ctx, index, tcp_now_ms());
    }
done:
    pthread_mutex_unlock(&ctx->ano_mutex);
}

static void tcp_ano_deadlines(tcp_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->ano_mutex);
    unsigned long long now = tcp_now_ms();
    for (int i = 0; i < TCP_ANO_CLIENTS; i++)
        ano_flush_client(ctx, i, now);
    pthread_mutex_unlock(&ctx->ano_mutex);
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
    LOG_INFO("ANO TCP listening on port %d", port);
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
        ano_tick(app);
        tcp_ano_deadlines(ctx);
        struct epoll_event events[16];
        int timeout = ano_wait_ms(app);
        if (timeout > 100) timeout = 100;
        int count = epoll_wait_feed(ctx->epfd, events, 16, timeout, "tcp");
        if (count < 0) break;
        for (int i = 0; i < count; i++) {
            uint32_t tag = events[i].data.u32;
            if (tag == TCP_WAKE_TAG)
                eventfd_consume(ctx->wake_fd);
            else if (tag == TCP_ANO_LISTEN_TAG || (tag & 0xf0000000u) == 0x10000000u)
                tcp_ano_events(ctx, events[i].data.u64, events[i].events);
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
    ctx->epfd = ctx->wake_fd = ctx->ano_listener = -1;
    pthread_mutex_init(&ctx->ano_mutex, NULL);
    ctx->ano_initialized = 1;
    for (int i = 0; i < TCP_ANO_CLIENTS; i++) ctx->ano_clients[i].fd = -1;
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
    if (!ctx || !ctx->ano_initialized) return;
    tcp_ano_stop(ctx);
    if (ctx->wake_fd >= 0) close(ctx->wake_fd);
    if (ctx->epfd >= 0) close(ctx->epfd);
    pthread_mutex_destroy(&ctx->ano_mutex);
    ctx->wake_fd = ctx->epfd = -1;
    ctx->ano_initialized = 0;
}
