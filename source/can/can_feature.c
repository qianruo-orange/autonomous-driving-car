/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "can/can_socket.h"
#include "watchdog/watchdog.h"
#include <sys/epoll.h>

/* Keep worker threads and their watchdog heartbeats alive while CAN is off.
 * Only descriptors and pending frames belong to this feature's lifecycle. */
int can_feature_set_locked(app_ctx_t *app, int enabled)
{
    if (!app || !app->cfg || !app->can || (enabled != 0 && enabled != 1)) {
        errno = EINVAL;
        return -1;
    }
    can_ctx_t *ctx = app->can;
    if (!!app->cfg->args.can_enabled == enabled) return 0;
    if (!enabled) {
        can_motor_prepare_close_locked(app, NULL);
        app->cfg->args.can_enabled = 0;
        for (int i = 0; i < ctx->count; i++) {
            can_iface_t *iface = &ctx->ifaces[i];
            if (iface->sock_fd >= 0) {
                if (ctx->recv_epfd >= 0) epoll_ctl(ctx->recv_epfd, EPOLL_CTL_DEL, iface->sock_fd, NULL);
                if (ctx->send_epfd >= 0) epoll_ctl(ctx->send_epfd, EPOLL_CTL_DEL, iface->sock_fd, NULL);
                can_socket_close(iface->sock_fd);
                iface->sock_fd = -1;
            }
            struct canfd_frame discarded;
            while (can_queue_pop(&iface->txq, &discarded) == CAN_QUEUE_ERR_OK) {}
            while (can_queue_pop(&iface->rxq, &discarded) == CAN_QUEUE_ERR_OK) {}
        }
        return 0;
    }

    int staged[CAN_MAX_IFACES];
    for (int i = 0; i < CAN_MAX_IFACES; i++) staged[i] = -1;
    for (int i = 0; i < ctx->count; i++) {
        can_iface_t *iface = &ctx->ifaces[i];
        watchdog_feed_self("http");
        if (can_socket_configure(iface->ifname, iface->bitrate, iface->dbitrate,
                                 iface->fd_mode, iface->restart_ms, iface->up) < 0) goto fail;
        staged[i] = can_socket_open(iface->ifname, iface->fd_mode);
        if (staged[i] < 0) goto fail;
        if (can_socket_apply_filters(app, iface, staged[i]) < 0) goto fail;
        struct epoll_event ev = {.events = EPOLLIN, .data.u32 = (uint32_t)(i + 1)};
        if (epoll_ctl(ctx->recv_epfd, EPOLL_CTL_ADD, staged[i], &ev) < 0) goto fail;
    }
    for (int i = 0; i < ctx->count; i++) ctx->ifaces[i].sock_fd = staged[i];
    app->cfg->args.can_enabled = 1;
    return 0;

fail:
    {
        int saved_errno = errno;
        for (int i = 0; i < ctx->count; i++) {
            if (staged[i] < 0) continue;
            epoll_ctl(ctx->recv_epfd, EPOLL_CTL_DEL, staged[i], NULL);
            can_socket_close(staged[i]);
        }
        errno = saved_errno;
        return -1;
    }
}
