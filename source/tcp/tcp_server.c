/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "tcp/tcp_server.h"
#include "core/epoll_util.h"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#define TCP_WAKE_TAG 0x40000000u
#define LISTEN_KIND 0x20000000u
#define CLIENT_KIND 0x10000000u
#define PENDING_MS 100

typedef struct {
    int fd;
    uint32_t token;
    unsigned char bytes[TCP_REPLY_CAPACITY];
    size_t length, offset;
    unsigned long long pending_ms;
} tcp_client_t;
typedef struct {
    pthread_mutex_t lock;
    int assigned, active, listener, port, enabled, staged, pending_port, staged_fd;
    uint32_t nonce, listener_token, stage_token;
    const tcp_service_ops_t *ops;
    void *user;
    tcp_client_t clients[TCP_CLIENTS_PER_PORT];
    unsigned long long sent, dropped, errors;
    int listener_error;
} tcp_port_t;
struct tcp_ctx {
    int epfd, wake_fd, initialized_locks;
    atomic_int ready, running;
    pthread_mutex_t registry_lock;
    app_ctx_t *app;
    tcp_port_t ports[TCP_SERVICE_MAX];
};
static unsigned long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static uint64_t event_tag(uint32_t generation, unsigned slot, uint32_t kind)
{
    return ((uint64_t)generation << 32) | ((uint32_t)slot << 16) | kind;
}
static int valid(tcp_ctx_t *ctx, int id)
{
    return ctx && atomic_load(&ctx->ready) && id >= 0 && id < TCP_SERVICE_MAX;
}
int tcp_ready(tcp_ctx_t *ctx) { return ctx && atomic_load(&ctx->ready); }
void tcp_wake(tcp_ctx_t *ctx)
{
    if (tcp_ready(ctx))
        eventfd_signal(ctx->wake_fd);
}
static void close_client(tcp_ctx_t *ctx, tcp_port_t *p, int i, int dropped)
{
    tcp_client_t *c = &p->clients[i];
    if (c->fd < 0)
        return;
    if (p->ops && p->ops->disconnected)
        p->ops->disconnected(ctx->app, p->port, c->fd, p->user);
    if (dropped && c->length)
        p->dropped++;
    epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    *c = (tcp_client_t){.fd = -1};
}
static void close_listener(tcp_ctx_t *ctx, tcp_port_t *p)
{
    for (int i = 0; i < TCP_CLIENTS_PER_PORT; i++)
        close_client(ctx, p, i, 1);
    if (p->listener >= 0) {
        epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, p->listener, NULL);
        close(p->listener);
    }
    p->listener = -1;
}
static void client_events(tcp_ctx_t *ctx, tcp_port_t *p, int slot, int i)
{
    tcp_client_t *c = &p->clients[i];
    struct epoll_event e = {.events = EPOLLRDHUP | (c->length ? EPOLLOUT : EPOLLIN),
                            .data.u64 = event_tag(c->token, slot, CLIENT_KIND | (i + 1))};
    epoll_ctl(ctx->epfd, EPOLL_CTL_MOD, c->fd, &e);
}
static void flush(tcp_ctx_t *ctx, tcp_port_t *p, int slot, int i)
{
    tcp_client_t *c = &p->clients[i];
    if (c->fd < 0 || !c->length)
        return;
    if (now_ms() - c->pending_ms >= PENDING_MS) {
        p->errors++;
        close_client(ctx, p, i, 1);
        return;
    }
    ssize_t n =
        send(c->fd, c->bytes + c->offset, c->length - c->offset, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n > 0) {
        c->offset += (size_t)n;
        if (c->offset == c->length) {
            c->length = c->offset = 0;
            p->sent++;
        }
    } else if (!n || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        p->errors++;
        close_client(ctx, p, i, 1);
        return;
    }
    client_events(ctx, p, slot, i);
}
static int listen_port(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(port)};
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
        bind(fd, (void *)&addr, sizeof(addr)) || listen(fd, SOMAXCONN)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}
int tcp_service_stage(tcp_ctx_t *ctx, int current, int enabled, int port,
                      const tcp_service_ops_t *ops, void *user, tcp_service_stage_t *stage)
{
    if (!tcp_ready(ctx) || !stage || !ops || !ops->receive || port < 1 || port > 65535 ||
        (enabled != 0 && enabled != 1) || current < -1 || current >= TCP_SERVICE_MAX) {
        errno = EINVAL;
        return -1;
    }
    *stage = (tcp_service_stage_t){.service = -1, .fd = -1};
    pthread_mutex_lock(&ctx->registry_lock);
    int id = current;
    for (int i = 0; i < TCP_SERVICE_MAX; i++) {
        tcp_port_t *other = &ctx->ports[i];
        if (i != current && other->assigned &&
            (other->port == port || (other->staged && other->pending_port == port))) {
            errno = EADDRINUSE;
            goto fail;
        }
    }
    if (id < 0)
        for (int i = 0; i < TCP_SERVICE_MAX; i++)
            if (!ctx->ports[i].assigned) {
                id = i;
                break;
            }
    if (id < 0) {
        errno = ENOSPC;
        goto fail;
    }
    tcp_port_t *p = &ctx->ports[id];
    pthread_mutex_lock(&p->lock);
    if (p->staged || (current >= 0 && (!p->assigned || p->ops != ops || p->user != user))) {
        errno = EBUSY;
        goto port_fail;
    }
    int replace = !p->active || p->enabled != enabled || p->port != port;
    int fd = -1;
    uint32_t token = ++p->nonce;
    if (enabled && replace) {
        fd = listen_port(port);
        if (fd < 0)
            goto port_fail;
        struct epoll_event e = {.events = EPOLLIN, .data.u64 = event_tag(token, id, LISTEN_KIND)};
        if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, fd, &e)) {
            int saved = errno;
            close(fd);
            errno = saved;
            goto port_fail;
        }
    }
    p->assigned = 1;
    p->staged = 1;
    p->pending_port = port;
    p->staged_fd = fd;
    p->stage_token = token;
    p->ops = ops;
    p->user = user;
    *stage = (tcp_service_stage_t){.service = id,
                                   .fd = fd,
                                   .port = port,
                                   .enabled = enabled,
                                   .replace = replace,
                                   .token = token};
    pthread_mutex_unlock(&p->lock);
    pthread_mutex_unlock(&ctx->registry_lock);
    return 0;
port_fail:
    pthread_mutex_unlock(&p->lock);
fail:
    pthread_mutex_unlock(&ctx->registry_lock);
    return -1;
}
void tcp_service_commit(tcp_ctx_t *ctx, tcp_service_stage_t *stage)
{
    if (!stage || !valid(ctx, stage->service))
        return;
    pthread_mutex_lock(&ctx->registry_lock);
    tcp_port_t *p = &ctx->ports[stage->service];
    pthread_mutex_lock(&p->lock);
    if (p->staged && p->stage_token == stage->token) {
        if (stage->replace) {
            close_listener(ctx, p);
            p->listener = stage->fd;
            p->listener_token = stage->token;
            p->listener_error = 0;
        }
        p->port = stage->port;
        p->enabled = stage->enabled;
        p->active = 1;
        p->staged = 0;
        p->staged_fd = -1;
        stage->fd = -1;
    }
    pthread_mutex_unlock(&p->lock);
    pthread_mutex_unlock(&ctx->registry_lock);
    tcp_wake(ctx);
}
void tcp_service_discard(tcp_ctx_t *ctx, tcp_service_stage_t *stage)
{
    if (!stage || !valid(ctx, stage->service))
        return;
    pthread_mutex_lock(&ctx->registry_lock);
    tcp_port_t *p = &ctx->ports[stage->service];
    pthread_mutex_lock(&p->lock);
    if (p->staged && p->stage_token == stage->token) {
        if (p->staged_fd >= 0) {
            epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, p->staged_fd, NULL);
            close(p->staged_fd);
        }
        p->staged = 0;
        p->staged_fd = -1;
        if (!p->active)
            p->assigned = 0;
        stage->fd = -1;
    }
    pthread_mutex_unlock(&p->lock);
    pthread_mutex_unlock(&ctx->registry_lock);
}
int tcp_service_register(tcp_ctx_t *ctx, int port, const tcp_service_ops_t *ops, void *user,
                         int *id)
{
    if (!id) {
        errno = EINVAL;
        return -1;
    }
    tcp_service_stage_t stage;
    if (tcp_service_stage(ctx, -1, 1, port, ops, user, &stage))
        return -1;
    tcp_service_commit(ctx, &stage);
    *id = stage.service;
    return 0;
}
void tcp_service_disable(tcp_ctx_t *ctx, int id)
{
    if (!valid(ctx, id))
        return;
    pthread_mutex_lock(&ctx->registry_lock);
    tcp_port_t *p = &ctx->ports[id];
    pthread_mutex_lock(&p->lock);
    close_listener(ctx, p);
    p->enabled = 0;
    pthread_mutex_unlock(&p->lock);
    pthread_mutex_unlock(&ctx->registry_lock);
}
int tcp_service_publish(tcp_ctx_t *ctx, int id, const unsigned char *data, size_t length)
{
    if (!valid(ctx, id) || !data || !length || length > TCP_REPLY_CAPACITY) {
        errno = EINVAL;
        return -1;
    }
    tcp_port_t *p = &ctx->ports[id];
    pthread_mutex_lock(&p->lock);
    if (!p->active || !p->enabled || p->listener < 0) {
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    for (int i = 0; i < TCP_CLIENTS_PER_PORT; i++) {
        tcp_client_t *c = &p->clients[i];
        flush(ctx, p, id, i);
        if (c->fd < 0)
            continue;
        if (c->length) {
            p->dropped++;
            continue;
        }
        int n;
        if (p->ops->prepare)
            n = p->ops->prepare(c->fd, data, length, c->bytes, sizeof(c->bytes), p->user);
        else {
            memcpy(c->bytes, data, length);
            n = (int)length;
        }
        if (n < 0 || (size_t)n > sizeof(c->bytes)) {
            p->errors++;
            close_client(ctx, p, i, 1);
            continue;
        }
        if (n) {
            c->length = (size_t)n;
            c->offset = 0;
            c->pending_ms = now_ms();
            flush(ctx, p, id, i);
        }
    }
    pthread_mutex_unlock(&p->lock);
    return 0;
}
void tcp_service_get_status(tcp_ctx_t *ctx, int id, tcp_service_status_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!valid(ctx, id))
        return;
    tcp_port_t *p = &ctx->ports[id];
    pthread_mutex_lock(&p->lock);
    out->running = atomic_load(&ctx->running);
    out->listening = p->listener >= 0;
    for (int i = 0; i < TCP_CLIENTS_PER_PORT; i++)
        if (p->clients[i].fd >= 0)
            out->clients++;
    out->sent_frames = p->sent;
    out->dropped_frames = p->dropped;
    out->errors = p->errors;
    if (p->listener_error)
        safe_strncpy(out->error, sizeof(out->error), "listener_failed");
    pthread_mutex_unlock(&p->lock);
}
static void port_event(tcp_ctx_t *ctx, int slot, uint64_t tag, uint32_t events)
{
    tcp_port_t *p = &ctx->ports[slot];
    uint32_t token = tag >> 32, kind = (uint32_t)tag;
    pthread_mutex_lock(&p->lock);
    if (!p->active)
        goto done;
    if ((kind & 0xf0000000u) == LISTEN_KIND) {
        if (p->listener < 0 || token != p->listener_token)
            goto done;
        if (events & (EPOLLERR | EPOLLHUP)) {
            close_listener(ctx, p);
            p->listener_error = 1;
            p->errors++;
            goto done;
        }
        for (int attempt = 0; attempt <= TCP_CLIENTS_PER_PORT; attempt++) {
            int fd = accept4(p->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0)
                break;
            int i;
            for (i = 0; i < TCP_CLIENTS_PER_PORT; i++)
                if (p->clients[i].fd < 0)
                    break;
            if (i == TCP_CLIENTS_PER_PORT) {
                close(fd);
                continue;
            }
            int one = 1, buffer = 1024;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
            tcp_client_t *c = &p->clients[i];
            *c = (tcp_client_t){.fd = fd, .token = ++p->nonce};
            struct epoll_event e = {.events = EPOLLIN | EPOLLRDHUP,
                                    .data.u64 = event_tag(c->token, slot, CLIENT_KIND | (i + 1))};
            if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, fd, &e)) {
                close(fd);
                c->fd = -1;
                p->errors++;
                continue;
            }
            if (p->ops->connected)
                p->ops->connected(ctx->app, p->port, fd, p->user);
        }
    } else {
        int i = (kind & 0xffffu) - 1;
        if (i < 0 || i >= TCP_CLIENTS_PER_PORT)
            goto done;
        tcp_client_t *c = &p->clients[i];
        if (c->fd < 0 || token != c->token)
            goto done;
        if (events & (EPOLLERR | EPOLLHUP)) {
            close_client(ctx, p, i, 1);
            goto done;
        }
        if ((events & EPOLLIN) && !c->length) {
            unsigned char input[512];
            ssize_t n = recv(c->fd, input, sizeof(input), MSG_DONTWAIT);
            if (!n || (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
                close_client(ctx, p, i, 1);
                goto done;
            }
            if (n > 0) {
                int reply = p->ops->receive(ctx->app, p->port, c->fd, input, (size_t)n, c->bytes,
                                            sizeof(c->bytes), p->user);
                if (reply < 0 || (size_t)reply > sizeof(c->bytes)) {
                    p->errors++;
                    close_client(ctx, p, i, 1);
                    goto done;
                }
                if (reply) {
                    c->length = (size_t)reply;
                    c->offset = 0;
                    c->pending_ms = now_ms();
                }
            }
        }
        flush(ctx, p, slot, i);
    }
done:
    pthread_mutex_unlock(&p->lock);
}
void *tcp_task(void *arg)
{
    app_ctx_t *app = arg;
    tcp_ctx_t *ctx = app->tcp;
    if (!tcp_ready(ctx))
        return NULL;
    atomic_store(&ctx->running, 1);
    while (app->running) {
        int timeout = 100;
        for (int i = 0; i < TCP_SERVICE_MAX; i++) {
            tcp_port_t *p = &ctx->ports[i];
            pthread_mutex_lock(&p->lock);
            const tcp_service_ops_t *ops = p->active ? p->ops : NULL;
            void *user = p->user;
            for (int c = 0; c < TCP_CLIENTS_PER_PORT; c++)
                flush(ctx, p, i, c);
            pthread_mutex_unlock(&p->lock);
            if (ops && ops->poll) {
                int delay = ops->poll(app, user);
                if (delay >= 0 && delay < timeout)
                    timeout = delay;
            }
        }
        struct epoll_event events[16];
        int n = epoll_wait_feed(ctx->epfd, events, 16, timeout, "tcp");
        if (n < 0)
            break;
        for (int i = 0; i < n; i++) {
            uint32_t tag = (uint32_t)events[i].data.u64;
            if (tag == TCP_WAKE_TAG) {
                eventfd_consume(ctx->wake_fd);
                continue;
            }
            unsigned slot = (tag >> 16) & 0xffu, kind = tag & 0xf0000000u;
            if (slot < TCP_SERVICE_MAX && (kind == LISTEN_KIND || kind == CLIENT_KIND))
                port_event(ctx, slot, events[i].data.u64, events[i].events);
        }
    }
    atomic_store(&ctx->running, 0);
    return NULL;
}
int tcp_init(void *arg)
{
    app_ctx_t *app = arg;
    static tcp_ctx_t storage;
    memset(&storage, 0, sizeof(storage));
    storage.epfd = storage.wake_fd = -1;
    storage.app = app;
    int rc = pthread_mutex_init(&storage.registry_lock, NULL);
    if (rc)
        return -1;
    app->tcp = &storage;
    for (int i = 0; i < TCP_SERVICE_MAX; i++) {
        tcp_port_t *p = &storage.ports[i];
        p->listener = p->staged_fd = -1;
        for (int c = 0; c < TCP_CLIENTS_PER_PORT; c++)
            p->clients[c].fd = -1;
        if (pthread_mutex_init(&p->lock, NULL))
            goto fail;
        storage.initialized_locks++;
    }
    storage.epfd = epoll_create1(EPOLL_CLOEXEC);
    if (storage.epfd < 0)
        goto fail;
    storage.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (storage.wake_fd < 0)
        goto fail;
    struct epoll_event e = {.events = EPOLLIN, .data.u64 = TCP_WAKE_TAG};
    if (epoll_ctl(storage.epfd, EPOLL_CTL_ADD, storage.wake_fd, &e))
        goto fail;
    atomic_store(&storage.ready, 1);
    return 0;
fail:
    tcp_cleanup(app);
    return -1;
}
void tcp_cleanup(void *arg)
{
    app_ctx_t *app = arg;
    tcp_ctx_t *ctx = app->tcp;
    if (!ctx)
        return;
    atomic_store(&ctx->ready, 0);
    for (int i = 0; i < ctx->initialized_locks; i++) {
        tcp_port_t *p = &ctx->ports[i];
        close_listener(ctx, p);
        if (p->staged_fd >= 0)
            close(p->staged_fd);
        pthread_mutex_destroy(&p->lock);
    }
    if (ctx->wake_fd >= 0)
        close(ctx->wake_fd);
    if (ctx->epfd >= 0)
        close(ctx->epfd);
    pthread_mutex_destroy(&ctx->registry_lock);
    app->tcp = NULL;
}
