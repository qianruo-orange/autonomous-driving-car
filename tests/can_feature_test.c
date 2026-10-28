/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise the vehicle CAN feature lifecycle without requiring physical CAN. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include "can/can_socket.h"

static int configure_calls, open_calls, filter_calls, close_calls;
static int fail_configure, fail_open, fail_filter;
static int configured_up[CAN_MAX_IFACES], opened[CAN_MAX_IFACES];

int watchdog_feed_thread(pthread_t thread, const char *name)
{ (void)thread; assert(!strcmp(name, "http")); return 0; }

void _log_info(const char *file, int line, const char *fmt, ...)
{ (void)file; (void)line; (void)fmt; }
void _log_error(const char *file, int line, const char *fmt, ...)
{ (void)file; (void)line; (void)fmt; }

int can_socket_configure(const char *name, int bitrate, int dbitrate,
                         int fd_mode, int restart_ms, int up)
{
    (void)name; (void)fd_mode; (void)restart_ms;
    assert(bitrate == 500000 && dbitrate == 2000000);
    configured_up[configure_calls++] = up;
    if (configure_calls == fail_configure) { errno = EIO; return -1; }
    return 0;
}

int can_socket_open(const char *name, int fd_mode)
{
    (void)name; (void)fd_mode;
    ++open_calls;
    if (open_calls == fail_open) { errno = ENODEV; return -1; }
    int fd = eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(fd >= 0);
    opened[open_calls - 1] = fd;
    return fd;
}

void can_socket_close(int fd)
{
    assert(fd >= 0);
    assert(!close(fd));
    ++close_calls;
}

int can_socket_apply_filters(app_ctx_t *app, can_iface_t *iface, int fd)
{
    assert(app && iface && iface->filter_count == 1);
    assert(fcntl(fd, F_GETFD) >= 0);
    assert(iface->filters[0].id == 0x123 && iface->filters[0].mask == 0x7ff);
    if (++filter_calls == fail_filter) { errno = EINVAL; return -1; }
    return 0;
}

void can_motor_prepare_close_locked(app_ctx_t *app, const char *ifname)
{ assert(app); if (ifname) assert(ifname_valid(ifname)); }

static void reset_stubs(void)
{
    configure_calls = open_calls = filter_calls = close_calls = 0;
    fail_configure = fail_open = fail_filter = 0;
    for (int i = 0; i < CAN_MAX_IFACES; i++) opened[i] = -1;
}

static void init_app(app_ctx_t *app, struct app_config_t *cfg, can_ctx_t *ctx, int count)
{
    memset(app, 0, sizeof(*app));
    memset(cfg, 0, sizeof(*cfg));
    memset(ctx, 0, sizeof(*ctx));
    assert(!pthread_mutex_init(&app->can_mutex, NULL));
    app->cfg = cfg;
    app->can = ctx;
    ctx->ifaces = cfg->args.can_ifaces;
    ctx->count = cfg->args.can_count = count;
    ctx->recv_epfd = epoll_create1(EPOLL_CLOEXEC);
    ctx->send_epfd = epoll_create1(EPOLL_CLOEXEC);
    ctx->tx_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(ctx->recv_epfd >= 0 && ctx->send_epfd >= 0 && ctx->tx_efd >= 0);
    for (int i = 0; i < count; i++) {
        can_iface_t *iface = &ctx->ifaces[i];
        snprintf(iface->ifname, sizeof(iface->ifname), "can%d", i);
        iface->sock_fd = -1;
        iface->bitrate = 500000;
        iface->dbitrate = 2000000;
        iface->fd_mode = 1;
        iface->up = i == 0;
        iface->restart_ms = 100;
        iface->filter_count = 1;
        iface->filters[0] = (can_filter_t){.id = 0x123, .mask = 0x7ff};
        snprintf(iface->dbc_path, sizeof(iface->dbc_path), "config/can%d.dbc", i);
        assert(can_queue_init(&iface->txq) == CAN_QUEUE_ERR_OK);
        assert(can_queue_init(&iface->rxq) == CAN_QUEUE_ERR_OK);
    }
    reset_stubs();
}

static int set_feature(app_ctx_t *app, int enabled)
{
    assert(!pthread_mutex_lock(&app->can_mutex));
    int result = can_feature_set_locked(app, enabled);
    assert(!pthread_mutex_unlock(&app->can_mutex));
    return result;
}

static void destroy_app(app_ctx_t *app)
{
    assert(!set_feature(app, 0));
    can_ctx_t *ctx = app->can;
    for (int i = 0; i < ctx->count; i++) {
        can_queue_destroy(&ctx->ifaces[i].txq);
        can_queue_destroy(&ctx->ifaces[i].rxq);
    }
    if (ctx->recv_epfd >= 0) assert(!close(ctx->recv_epfd));
    assert(!close(ctx->send_epfd));
    assert(!close(ctx->tx_efd));
    assert(!pthread_mutex_destroy(&app->can_mutex));
}

static void assert_metadata(can_ctx_t *ctx)
{
    for (int i = 0; i < ctx->count; i++) {
        can_iface_t *iface = &ctx->ifaces[i];
        char name[16], dbc_path[32];
        snprintf(name, sizeof(name), "can%d", i);
        snprintf(dbc_path, sizeof(dbc_path), "config/can%d.dbc", i);
        assert(!strcmp(iface->ifname, name));
        assert(!strcmp(iface->dbc_path, dbc_path));
        assert(iface->bitrate == 500000 && iface->dbitrate == 2000000);
        assert(iface->fd_mode == 1 && iface->restart_ms == 100);
        assert(iface->up == (i == 0));
        assert(iface->filter_count == 1 && iface->filters[0].id == 0x123);
    }
}

static void test_disable_enable(void)
{
    app_ctx_t app;
    struct app_config_t cfg;
    can_ctx_t ctx;
    init_app(&app, &cfg, &ctx, 2);
    assert(!set_feature(&app, 1));
    assert(cfg.args.can_enabled == 1);
    assert(configure_calls == 2 && open_calls == 2 && filter_calls == 2);
    assert(configured_up[0] == 1 && configured_up[1] == 0);
    struct epoll_event events[2];
    assert(epoll_wait(ctx.recv_epfd, events, 2, 0) == 2);
    assert((events[0].data.u32 == 1 && events[1].data.u32 == 2) ||
           (events[0].data.u32 == 2 && events[1].data.u32 == 1));
    struct canfd_frame frame = {.can_id = 0x123, .len = 1, .data = {42}};
    int old_fd[2];
    for (int i = 0; i < 2; i++) {
        old_fd[i] = ctx.ifaces[i].sock_fd;
        struct epoll_event ev = {.events = EPOLLIN, .data.u32 = (uint32_t)(i + 1)};
        assert(!epoll_ctl(ctx.send_epfd, EPOLL_CTL_ADD, old_fd[i], &ev));
        for (int k = 0; k < CAN_QUEUE_MAX_SLOTS; k++) {
            assert(can_queue_push(&ctx.ifaces[i].txq, &frame) == CAN_QUEUE_ERR_OK);
            assert(can_queue_push(&ctx.ifaces[i].rxq, &frame) == CAN_QUEUE_ERR_OK);
        }
    }
    assert(!set_feature(&app, 0));
    assert(cfg.args.can_enabled == 0 && close_calls == 2);
    for (int i = 0; i < 2; i++) {
        assert(ctx.ifaces[i].sock_fd == -1);
        assert(fcntl(old_fd[i], F_GETFD) == -1 && errno == EBADF);
        assert(can_queue_count(&ctx.ifaces[i].txq) == 0);
        assert(can_queue_count(&ctx.ifaces[i].rxq) == 0);
    }
    assert(epoll_wait(ctx.recv_epfd, events, 2, 0) == 0);
    assert(epoll_wait(ctx.send_epfd, events, 2, 0) == 0);
    assert_metadata(&ctx);
    assert(!set_feature(&app, 0));
    assert(close_calls == 2 && configure_calls == 2);
    assert(!set_feature(&app, 1));
    assert(configure_calls == 4 && open_calls == 4 && filter_calls == 4);
    assert(!set_feature(&app, 1));
    assert(open_calls == 4); /* idempotent; no descriptor leaks */
    assert_metadata(&ctx);
    for (int i = 0; i < 2; i++) {
        assert(can_queue_count(&ctx.ifaces[i].txq) == 0);
        assert(can_queue_push(&ctx.ifaces[i].txq, &frame) == CAN_QUEUE_ERR_OK);
        assert(can_queue_pop(&ctx.ifaces[i].txq, &frame) == CAN_QUEUE_ERR_OK);
    }
    destroy_app(&app);
}

static void test_enable_failure(int failure)
{
    app_ctx_t app;
    struct app_config_t cfg;
    can_ctx_t ctx;
    init_app(&app, &cfg, &ctx, 2);
    if (failure == 0) fail_configure = 2;
    else if (failure == 1) fail_open = 2;
    else if (failure == 2) fail_filter = 2;
    else { assert(!close(ctx.recv_epfd)); ctx.recv_epfd = -1; }
    assert(set_feature(&app, 1) == -1);
    int expected_errno = failure == 0 ? EIO : failure == 1 ? ENODEV : failure == 2 ? EINVAL : EBADF;
    assert(errno == expected_errno);
    assert(cfg.args.can_enabled == 0);
    assert(ctx.ifaces[0].sock_fd == -1 && ctx.ifaces[1].sock_fd == -1);
    int created = open_calls - (failure == 1);
    assert(close_calls == created);
    for (int i = 0; i < created; i++)
        assert(fcntl(opened[i], F_GETFD) == -1 && errno == EBADF);
    assert_metadata(&ctx);
    if (ctx.recv_epfd >= 0) {
        struct epoll_event events[2];
        assert(epoll_wait(ctx.recv_epfd, events, 2, 0) == 0);
    } else {
        ctx.recv_epfd = epoll_create1(EPOLL_CLOEXEC);
        assert(ctx.recv_epfd >= 0);
    }
    reset_stubs();
    assert(!set_feature(&app, 1)); /* a failed attempt must not prevent retry */
    assert(cfg.args.can_enabled == 1 && open_calls == 2);
    destroy_app(&app);
}

static void test_empty_and_invalid(void)
{
    app_ctx_t app;
    struct app_config_t cfg;
    can_ctx_t ctx;
    init_app(&app, &cfg, &ctx, 0);
    assert(!set_feature(&app, 1) && cfg.args.can_enabled == 1);
    assert(configure_calls == 0 && open_calls == 0);
    assert(set_feature(&app, 2) == -1 && errno == EINVAL);
    assert(can_feature_set_locked(NULL, 0) == -1 && errno == EINVAL);
    app_ctx_t invalid = {0};
    assert(can_feature_set_locked(&invalid, 1) == -1 && errno == EINVAL);
    destroy_app(&app);
}

int main(void)
{
    test_disable_enable();
    for (int i = 0; i < 4; i++) test_enable_failure(i);
    test_empty_and_invalid();
    puts("CAN feature lifecycle tests passed");
    return 0;
}
