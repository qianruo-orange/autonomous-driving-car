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
#include "tcp/tcp_queue.h"
#include "core/common.h"
#include "core/epoll_util.h"
#include "core/log.h"
#include "watchdog/watchdog.h"
#include "telemetry/vofa.h"

/* epoll 事件 tag：listen_fd 用 0，TX eventfd 用高位标识，客户端用 0x80000000|(idx+1) */
#define TCP_TXEFD_TAG 0x40000000u
#define TCP_CLIENT_TAG(idx) (0x80000000u | (uint32_t)((idx) + 1))
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
    stage->fd = tcp_listen(port, NULL);
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
    eventfd_signal(ctx->tx_efd);
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

int tcp_listen(int port, const char *bind_ifname)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { LOG_ERROR("tcp socket"); return -1; }
    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))) {
        close(fd);
        return -1;
    }

    /* 指定网卡时用 SO_BINDTODEVICE 绑定到该接口（否则 INADDR_ANY 绑定所有网卡） */
    if (bind_ifname && bind_ifname[0]) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        ifreq_set_name(&ifr, bind_ifname);
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0) {
            LOG_ERROR("tcp bind device %s: %s", bind_ifname, strerror(errno));
            close(fd); return -1;
        }
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
    if (bind_ifname && bind_ifname[0])
        LOG_INFO("TCP listening on %s:%d", bind_ifname, port);
    else
        LOG_INFO("TCP listening on port %d", port);
    return fd;
}

static int tcp_accept(int listen_fd)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
        /* 非阻塞监听 fd 下 EAGAIN/EWOULDBLOCK 属正常无连接，不刷错误日志 */
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            LOG_ERROR("tcp accept: %s", strerror(errno));
        return -1;
    }
    if (set_nonblock(client_fd) < 0) {
        LOG_ERROR("tcp client fcntl O_NONBLOCK: %s", strerror(errno));
        close(client_fd);
        return -1;
    }
    char ip_str[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str)) == NULL)
        safe_strncpy(ip_str, sizeof(ip_str), "?");
    LOG_INFO("TCP client connected: %s:%d  fd=%d", ip_str, ntohs(client_addr.sin_port), client_fd);
    return client_fd;
}

/* ---- eventfd 跨线程唤醒：TX 队列有数据压入时通知 tcp_task ----
   写/读 eventfd 的公共实现见 core/common.h（eventfd_signal / eventfd_consume） */

/*
 * tcp_tx_packet - 异步发送数据到客户端：压入 TX 队列，写 eventfd 唤醒 tcp_task。
 * client_idx < 0 表示广播所有已连接客户端。返回 0 成功，-1 失败。
 */
static int tcp_tx_packet(tcp_ctx_t *ctx, int client_idx, const void *buf, size_t len)
{
    if (!ctx || !buf || len == 0 || len > WBUF_SIZE) { errno = EINVAL; return -1; }

    int pushed = 0;

    if (client_idx < 0) {
        /* 广播：先快照已连接客户端索引，再统一压入 TX 队列 */
        int idxs[TCP_MAX_CLIENTS];
        int n = 0;
        pthread_mutex_lock(&ctx->client_mutex);
        for (int i = 0; i < ctx->client_count && n < TCP_MAX_CLIENTS; i++)
            if (ctx->clients[i].fd >= 0) idxs[n++] = i;
        pthread_mutex_unlock(&ctx->client_mutex);

        pthread_mutex_lock(&ctx->tx_mutex);
        for (int i = 0; i < n; i++)
            if (tcp_queue_push(&ctx->txq, idxs[i], buf, len) == 0) pushed++;
        pthread_mutex_unlock(&ctx->tx_mutex);
    } else {
        pthread_mutex_lock(&ctx->tx_mutex);
        if (tcp_queue_push(&ctx->txq, client_idx, buf, len) == 0) pushed++;
        pthread_mutex_unlock(&ctx->tx_mutex);
    }

    if (pushed > 0)
        eventfd_signal(ctx->tx_efd);

    return pushed > 0 ? 0 : -1;
}

/* 读取单个客户端数据并丢弃（不再转发到 CAN）；返回 1 表示客户端已断开 */
static int handle_tcp_input(tcp_ctx_t *ctx, int client_idx)
{
    char buf[WBUF_SIZE];

    pthread_mutex_lock(&ctx->client_mutex);
    if (client_idx < 0 || client_idx >= TCP_MAX_CLIENTS) {
        pthread_mutex_unlock(&ctx->client_mutex);
        return 1;
    }
    client_t *c = &ctx->clients[client_idx];
    if (c->fd < 0) { pthread_mutex_unlock(&ctx->client_mutex); return 1; }
    ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
    pthread_mutex_unlock(&ctx->client_mutex);

    if (n == 0) return 1;                                   /* 对端正常关闭 */
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return 1;
    return 0;
}

/* 弹出 TX 队列并写客户端 socket，处理部分写（用每客户端 wbuf 暂存未发完的字节） */
static void tcp_flush_tx(tcp_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->tx_mutex);
    pthread_mutex_lock(&ctx->client_mutex);

    /* 1) 先推进每个客户端尚未发完的 wbuf */
    for (int i = 0; i < ctx->client_count; i++) {
        client_t *c = &ctx->clients[i];
        if (c->fd < 0 || c->wlen <= 0) continue;
        ssize_t n = send(c->fd, c->wbuf, (size_t)c->wlen, MSG_NOSIGNAL);
        if (n > 0) {
            memmove(c->wbuf, c->wbuf + n, (size_t)(c->wlen - n));
            c->wlen -= (int)n;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            c->wlen = 0; /* 连接异常，丢弃残留 */
        }
    }

    /* 2) 再把 TX 队列里的包搬到客户端 wbuf 并发送。
          队头包的目标客户端若暂时写不出去（wbuf 未排空），不阻塞其他客户端：
          把该包转回队尾稍后再试，继续处理后续包；每个目标客户端的包保持 FIFO。 */
    int remaining = ctx->txq.count;   /* 本次尝试处理的包数上限（旋转可能把包放回队尾） */
    int examined = 0;
    while (examined < remaining) {
        tcp_pkt_t p;
        if (tcp_queue_peek(&ctx->txq, &p) != 0) break;   /* 队列已空 */
        examined++;
        int idx = p.client_idx;
        if (idx < 0 || idx >= TCP_MAX_CLIENTS || idx >= ctx->client_count) { tcp_queue_pop(&ctx->txq, &p); continue; }
        client_t *c = &ctx->clients[idx];
        if (c->fd < 0) { tcp_queue_pop(&ctx->txq, &p); continue; }   /* 客户端已断开，丢弃 */
        if (c->wlen > 0) {
            /* 慢客户端：队头包转回队尾，避免队头阻塞拖垮整个广播 */
            tcp_queue_pop(&ctx->txq, &p);
            tcp_queue_push(&ctx->txq, p.client_idx, p.data, p.len);
            continue;
        }
        tcp_queue_pop(&ctx->txq, &p);
        memcpy(c->wbuf, p.data, p.len);
        c->wlen = (int)p.len;
        ssize_t n = send(c->fd, c->wbuf, (size_t)c->wlen, MSG_NOSIGNAL);
        if (n > 0) {
            memmove(c->wbuf, c->wbuf + n, (size_t)(c->wlen - n));
            c->wlen -= (int)n;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            c->wlen = 0;
        }
    }

    /* 3) 按 wbuf 是否非空更新 EPOLLOUT 状态 */
    for (int i = 0; i < ctx->client_count; i++) {
        client_t *c = &ctx->clients[i];
        if (c->fd < 0) continue;
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLRDHUP | (c->wlen > 0 ? EPOLLOUT : 0);
        ev.data.u32 = TCP_CLIENT_TAG(i);
        epoll_ctl(ctx->epfd, EPOLL_CTL_MOD, c->fd, &ev);
    }

    pthread_mutex_unlock(&ctx->client_mutex);
    pthread_mutex_unlock(&ctx->tx_mutex);
}

/* tcp_client_add / tcp_client_del 定义在文件末尾，此处提前声明供 tcp_task 使用 */
static void tcp_client_add(tcp_ctx_t *ctx, int fd);
static void tcp_client_del(tcp_ctx_t *ctx, int idx);

void *tcp_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    tcp_ctx_t *ctx = app->tcp;
    if (!ctx) return NULL;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = 0;
    if (ctx->listen_fd >= 0)
        epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->listen_fd, &ev);

    ev.events = EPOLLIN;
    ev.data.u32 = TCP_TXEFD_TAG;
    if (ctx->tx_efd >= 0)
        epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->tx_efd, &ev);

    pthread_mutex_lock(&ctx->client_mutex);
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
        if (ctx->clients[i].fd < 0) continue;
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.u32 = TCP_CLIENT_TAG(i);
        epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->clients[i].fd, &ev);
    }
    pthread_mutex_unlock(&ctx->client_mutex);

    atomic_store(&ctx->task_running, 1);
    LOG_INFO("tcp_task started");
    while (app->running) {
        /* Sampling runs outside transport locks; it only reads controller state. */
        vofa_tick(app);
        tcp_vofa_deadlines(ctx);
        struct epoll_event events[64];
        int timeout = vofa_wait_ms(app);
        if (timeout > 100)
            timeout = 100;
        int nfds = epoll_wait_feed(ctx->epfd, events, 64, timeout, "tcp");
        if (nfds < 0) break;
        for (int i = 0; i < nfds; i++) {
            uint32_t tag = events[i].data.u32;
            if (tag == TCP_VOFA_LISTEN_TAG || (tag & 0xf0000000u) == 0x10000000u) {
                tcp_vofa_events(ctx, events[i].data.u64, events[i].events);
                continue;
            }
            if (tag == 0) {
                for (int accepted = 0; accepted <= TCP_MAX_CLIENTS; accepted++) {
                    /* HTTP swaps/closes the listener under the same mutex.
                     * Keep the descriptor stable through nonblocking accept,
                     * then release before tcp_client_add takes this lock. */
                    pthread_mutex_lock(&ctx->client_mutex);
                    int fd = ctx->listen_fd >= 0 ? tcp_accept(ctx->listen_fd) : -1;
                    pthread_mutex_unlock(&ctx->client_mutex);
                    if (fd < 0)
                        break;
                    tcp_client_add(ctx, fd);
                }
                continue;
            }
            if (tag == TCP_TXEFD_TAG) {
                eventfd_consume(ctx->tx_efd);
                tcp_flush_tx(ctx);
                continue;
            }
            int client_idx = (int)(tag & 0x7FFFFFFFu) - 1;
            if (client_idx < 0 || client_idx >= TCP_MAX_CLIENTS) continue;
            uint32_t e = events[i].events;
            int dead = 0;
            if (e & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))
                dead = handle_tcp_input(ctx, client_idx);
            if (!dead && (e & EPOLLOUT))
                tcp_flush_tx(ctx);
            if (dead) tcp_client_del(ctx, client_idx);
        }
    }
    atomic_store(&ctx->task_running, 0);
    LOG_INFO("tcp_task stopped");
    return NULL;
}

int tcp_init(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    tcp_ctx_t *ctx = app->tcp;
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->listen_fd = -1;
    ctx->epfd = -1;
    ctx->tx_efd = -1;
    ctx->vofa_listener = -1;
    pthread_mutex_init(&ctx->client_mutex, NULL);
    pthread_mutex_init(&ctx->tx_mutex, NULL);
    pthread_mutex_init(&ctx->vofa_mutex, NULL);
    ctx->vofa_initialized = 1;
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) ctx->clients[i].fd = -1;
    for (int i = 0; i < TCP_VOFA_CLIENTS; i++) ctx->vofa_clients[i].fd = -1;
    ctx->port        = app->cfg->args.tcp_port;
    ctx->max_clients = app->cfg->args.max_clients;
    safe_strncpy(ctx->bind_ifname, sizeof(ctx->bind_ifname), app->cfg->args.tcp_bind);
    ctx->listen_fd = tcp_listen(ctx->port, ctx->bind_ifname);
    if (ctx->listen_fd < 0) return -1;
    /* TCP 数据收发专用 epoll（listen + 客户端 + TX eventfd，与 CAN 分开管理） */
    ctx->epfd = epoll_create1(0);
    if (ctx->epfd < 0) { LOG_ERROR("tcp: epoll_create1 failed"); return -1; }
    ctx->tx_efd = eventfd(0, EFD_NONBLOCK);
    if (ctx->tx_efd < 0) { LOG_ERROR("tcp: eventfd failed"); return -1; }
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) ctx->clients[i].fd = -1;
    tcp_queue_reset(&ctx->txq);
    return 0;
}

void tcp_cleanup(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    tcp_ctx_t *ctx = app->tcp;
    if (!ctx || !ctx->vofa_initialized) return;

    tcp_vofa_stop(ctx);

    for (int i = 0; i < TCP_MAX_CLIENTS; i++)
        if (ctx->clients[i].fd >= 0) { close(ctx->clients[i].fd); ctx->clients[i].fd = -1; }
    if (ctx->listen_fd >= 0) close(ctx->listen_fd);
    if (ctx->tx_efd >= 0) close(ctx->tx_efd);
    if (ctx->epfd >= 0) close(ctx->epfd);
    pthread_mutex_destroy(&ctx->client_mutex);
    pthread_mutex_destroy(&ctx->tx_mutex);
    pthread_mutex_destroy(&ctx->vofa_mutex);
    ctx->vofa_initialized = 0;
}

static void tcp_client_add(tcp_ctx_t *ctx, int fd)
{
    pthread_mutex_lock(&ctx->client_mutex);
    int idx = -1;
    int limit = ctx->max_clients > 0 ? ctx->max_clients : TCP_MAX_CLIENTS;
    if (limit > TCP_MAX_CLIENTS) limit = TCP_MAX_CLIENTS;   /* clamp，防止数组越界 */
    for (int i = 0; i < limit; i++) { if (ctx->clients[i].fd == -1) { idx = i; break; } }
    if (idx < 0 && ctx->client_count < limit) idx = ctx->client_count;
    if (idx < 0) { LOG_ERROR("too many clients, rejecting fd=%d", fd); close(fd); pthread_mutex_unlock(&ctx->client_mutex); return; }
    client_t *c = &ctx->clients[idx]; memset(c, 0, sizeof(*c)); c->fd = fd;
    if (idx >= ctx->client_count) ctx->client_count = idx + 1;
    struct epoll_event ev; ev.events = EPOLLIN | EPOLLRDHUP; ev.data.u32 = TCP_CLIENT_TAG(idx);
    epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, fd, &ev);
    LOG_INFO("client added fd=%d idx=%d", fd, idx);
    pthread_mutex_unlock(&ctx->client_mutex);
}

static void tcp_client_del(tcp_ctx_t *ctx, int idx)
{
    pthread_mutex_lock(&ctx->client_mutex);
    if (idx < 0 || idx >= TCP_MAX_CLIENTS) { pthread_mutex_unlock(&ctx->client_mutex); return; }
    client_t *c = &ctx->clients[idx];
    if (c->fd < 0) { pthread_mutex_unlock(&ctx->client_mutex); return; }
    epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd); LOG_INFO("client removed fd=%d idx=%d", c->fd, idx);
    c->fd = -1;
    pthread_mutex_unlock(&ctx->client_mutex);
}
