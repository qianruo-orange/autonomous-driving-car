/* SPDX-License-Identifier: GPL-3.0-or-later */
/* CAN 的注册、数据槽、发送和接口开关集中在本模块。
 * 注册表只保存绑定元数据：接口名 + CAN ID + 格式 + 业务槽，不解释电机数据。
 * 注册/注销/RX/TX 都由 can_mutex 串行化；数据槽只负责复制帧。
 * 电机编码、反馈解析及控制条件保留在 can_motor.c。 */
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/netlink.h>
#include <linux/can/raw.h>
#include <linux/if.h>
#include <linux/if_arp.h>
#include <netlink/netlink.h>
#include <netlink/route/link.h>
#include <netlink/route/link/can.h>
#include <netlink/socket.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "can/can_socket.h"
#include "can/dbc_parser.h"
#include "core/common.h"
#include "core/epoll_util.h"
#include "core/log.h"
#include "http/http_api_can.h"
#include "watchdog/watchdog.h"
#include <limits.h>
#include <math.h>
#include <time.h>

#ifndef CANFD_FDF
#define CANFD_FDF 0x04
#endif

static app_ctx_t *can_app;
static uint32_t binding_generation; /* 注销后不回退，旧业务句柄不会命中新注册项 */
static struct {
    int used;
    uint64_t handle;
    char port[IFNAMSIZ];
    can_binding_t spec;
} bindings[CAN_BIND_MAX];
static int can_write_frame_locked(app_ctx_t *, int, const struct canfd_frame *, int, int);
static int can_queue_frame_locked(app_ctx_t *app, int index, const struct canfd_frame *frame,
                                  int fd_mode)
{
    struct canfd_frame queued = *frame;
    if (fd_mode)
        queued.flags |= CANFD_FDF;
    if (data_slot_push(&app->can->ifaces[index].txq, &queued, 0) != 0) {
        errno = ENOBUFS;
        return -1;
    }
    eventfd_signal(app->can->tx_efd);
    return 0;
}
static int binding_index(uint64_t handle)
{
    unsigned index = handle & 0xffu;
    if (index >= CAN_BIND_MAX || !bindings[index].used || bindings[index].handle != handle) {
        errno = ESTALE;
        return -1;
    }
    return (int)index;
}
/* 先验证参数、配置通道和内核接口类型，再发布绑定；不能把普通网卡当 CAN。
 * 同一接口可注册多个不同 ID；标准 ID 和扩展 ID 由 CAN_EFF_FLAG 区分。 */
uint64_t can_locked_register(const can_binding_t *spec)
{
    if (!can_app || !spec || !ifname_valid(spec->port) ||
        !(spec->flags & (CAN_BIND_RX | CAN_BIND_TX)) ||
        (spec->flags & ~(CAN_BIND_RX | CAN_BIND_TX | CAN_BIND_RESERVED)) ||
        (spec->flags & CAN_BIND_RX && !spec->slot) || (spec->fd != 0 && spec->fd != 1) ||
        (spec->id & ~(CAN_EFF_FLAG | CAN_EFF_MASK)) ||
        (!(spec->id & CAN_EFF_FLAG) && spec->id > CAN_SFF_MASK)) {
        errno = EINVAL;
        return 0;
    }
    if ((spec->flags & CAN_BIND_RESERVED) && !(spec->flags & CAN_BIND_TX)) {
        errno = EINVAL;
        return 0;
    }
    if ((spec->flags & CAN_BIND_RX) && (spec->slot->queue.items != spec->slot->items ||
                                        spec->slot->queue.capacity != CAN_SLOT_CAPACITY ||
                                        spec->slot->queue.item_size != sizeof(can_data_t))) {
        errno = EINVAL;
        return 0;
    }
    int channel = can_iface_index(can_app->can->ifaces, can_app->can->count, spec->port);
    if (channel < 0) {
        errno = ENODEV;
        return 0;
    }
    int probe = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (probe < 0)
        return 0;
    struct ifreq request = {0};
    ifreq_set_name(&request, spec->port);
    int result = ioctl(probe, SIOCGIFHWADDR, &request);
    int saved = errno;
    close(probe);
    if (result < 0 || request.ifr_hwaddr.sa_family != ARPHRD_CAN) {
        errno = result < 0 ? saved : ENODEV;
        return 0;
    }
    if (spec->fd && !can_app->can->ifaces[channel].fd_mode) {
        errno = EPROTONOSUPPORT;
        return 0;
    }
    int free_index = -1;
    for (int i = 0; i < CAN_BIND_MAX; i++) {
        if (!bindings[i].used) {
            if (free_index < 0)
                free_index = i;
            continue;
        }
        if (!strcmp(bindings[i].port, spec->port) && bindings[i].spec.id == spec->id) {
            errno = EEXIST;
            return 0;
        }
    }
    if (free_index < 0) {
        errno = ENOSPC;
        return 0;
    }
    unsigned i = (unsigned)free_index;
    bindings[i].used = 1;
    bindings[i].spec = *spec;
    safe_strncpy(bindings[i].port, IFNAMSIZ, spec->port);
    bindings[i].spec.port = bindings[i].port;
    if (!++binding_generation)
        binding_generation++;
    bindings[i].handle = ((uint64_t)binding_generation << 8) | i;
    return bindings[i].handle;
}
int can_locked_unregister(uint64_t handle)
{
    int i = binding_index(handle);
    if (i < 0)
        return -1;
    bindings[i].used = 0;
    return 0;
}
/* 每次按接口名重新定位通道，避免 Web 修改通道顺序后把旧句柄发到另一总线。
 * CAN ID、经典/FD 格式均取自注册项；业务只提供载荷。 */
static int can_binding_send_locked(uint64_t handle, const void *bytes, size_t length, int immediate)
{
    int i = binding_index(handle);
    if (i < 0)
        return -1;
    can_binding_t *spec = &bindings[i].spec;
    int index = can_iface_index(can_app->can->ifaces, can_app->can->count, bindings[i].port);
    if (index < 0) {
        errno = ENODEV;
        return -1;
    }
    can_iface_t *iface = &can_app->can->ifaces[index];
    if (!(spec->flags & CAN_BIND_TX) || !bytes ||
        length > (spec->fd ? CANFD_MAX_DLEN : CAN_MAX_DLEN)) {
        errno = EINVAL;
        return -1;
    }
    if (!can_app->cfg->args.can_enabled || iface->sock_fd < 0 || !iface->up ||
        (spec->fd && !iface->fd_mode)) {
        errno = ENETDOWN;
        return -1;
    }
    struct canfd_frame frame = {.can_id = spec->id, .len = (unsigned char)length};
    if (spec->fd)
        frame.flags |= CANFD_FDF;
    memcpy(frame.data, bytes, length);
    if (immediate)
        return can_write_frame_locked(can_app, index, &frame, spec->fd, 1);
    return can_queue_frame_locked(can_app, index, &frame, spec->fd);
}
int can_send_locked(uint64_t handle, const void *bytes, size_t length)
{
    return can_app ? can_binding_send_locked(handle, bytes, length, 1) : -1;
}
int can_send(uint64_t handle, const void *bytes, size_t length)
{
    if (!can_app) {
        errno = ENODEV;
        return -1;
    }
    pthread_mutex_lock(&can_app->can_mutex);
    int i = binding_index(handle);
    int result = i < 0 ? -1
                       : can_binding_send_locked(handle, bytes, length,
                                                 !!(bindings[i].spec.flags & CAN_BIND_RESERVED));
    pthread_mutex_unlock(&can_app->can_mutex);
    return result;
}
/* 接收时间在 CAN 层记录，不能用业务出槽时间冒充反馈采集时间。
 * 有界槽满时保留最新 CAN 帧，丢弃数量由该槽自己统计。 */
static void can_slot_dispatch_locked(const char *port, const struct canfd_frame *frame)
{
    uint64_t stamp = monotonic_ms();
    canid_t id = frame->can_id & (CAN_EFF_FLAG | CAN_EFF_MASK);
    for (int i = 0; i < CAN_BIND_MAX; i++)
        if (bindings[i].used && !strcmp(bindings[i].port, port) && bindings[i].spec.id == id &&
            (bindings[i].spec.flags & CAN_BIND_RX)) {
            can_data_t item = {.handle = bindings[i].handle, .received_ms = stamp, .frame = *frame};
            data_slot_push(&bindings[i].spec.slot->queue, &item, 1);
        }
}
/* 关闭前先清空旧反馈；同一业务注册多个 ID 时，停止钩子每个接口只调用一次。
 * 停止钩子已持 can_mutex，只能使用已持锁的即时发送入口。 */
void can_service_prepare_close_locked(app_ctx_t *app, const char *port)
{
    for (int i = 0; i < CAN_BIND_MAX; i++)
        if (bindings[i].used && (!port || !strcmp(bindings[i].port, port))) {
            can_binding_t *spec = &bindings[i].spec;
            if (spec->slot)
                data_slot_clear(&spec->slot->queue);
            if (!spec->before_close)
                continue;
            int duplicate = 0;
            for (int j = 0; j < i; j++)
                if (bindings[j].used && bindings[j].spec.before_close == spec->before_close &&
                    !strcmp(bindings[j].port, bindings[i].port))
                    duplicate = 1;
            if (!duplicate)
                spec->before_close(app, bindings[i].port);
        }
}
static int can_service_reserved_locked(const char *port, canid_t id)
{
    for (int i = 0; i < CAN_BIND_MAX; i++)
        if (bindings[i].used && (bindings[i].spec.flags & CAN_BIND_RESERVED) &&
            !strcmp(bindings[i].port, port) &&
            bindings[i].spec.id == (id & (CAN_EFF_FLAG | CAN_EFF_MASK)))
            return 1;
    return 0;
}
static int can_service_filters_locked(const char *port, struct can_filter *out, size_t capacity)
{
    int n = 0;
    for (int i = 0; i < CAN_BIND_MAX; i++)
        if (bindings[i].used && (bindings[i].spec.flags & CAN_BIND_RX) &&
            !strcmp(bindings[i].port, port)) {
            if ((size_t)n == capacity)
                return -1;
            canid_t id = bindings[i].spec.id;
            out[n++] = (struct can_filter){.can_id = id,
                                           .can_mask =
                                               ((id & CAN_EFF_FLAG) ? CAN_EFF_MASK : CAN_SFF_MASK) |
                                               CAN_EFF_FLAG | CAN_RTR_FLAG};
        }
    return n;
}

/* Web/DBC observation is an adapter, separate from the socket transport. */
static void observe(app_ctx_t *app, int index, const struct canfd_frame *frame, int direction)
{
    const char *ifname = app->can->ifaces[index].ifname;
    if (direction == 0)
        http_can_record_rx(ifname, frame);
    char name[DBC_MAX_NAME_LEN], text[DBC_DECODE_TEXT_MAX];
    canid_t id = 0;
    if (!dbc_decode_frame(app, index, ifname, frame, &id, name, sizeof(name), text, sizeof(text)))
        http_dbc_record(direction == 0 ? DBC_DIR_RX : DBC_DIR_TX, ifname, id, name, text);
}
void can_observe_rx_locked(app_ctx_t *app, int index, const struct canfd_frame *frame)
{
    observe(app, index, frame, 0);
}
void can_observe_tx_locked(app_ctx_t *app, int index, const struct canfd_frame *frame)
{
    observe(app, index, frame, 1);
}

static struct can_filter ordinary_filter(can_filter_t source)
{
    if ((source.id & CAN_EFF_FLAG) || (source.id & CAN_EFF_MASK) > CAN_SFF_MASK ||
        (source.mask & CAN_EFF_MASK) > CAN_SFF_MASK) {
        source.id |= CAN_EFF_FLAG;
        source.mask |= CAN_EFF_FLAG;
    }
    return (struct can_filter){.can_id = source.id, .can_mask = source.mask};
}

static int ordinary_frame_allowed(const can_iface_t *iface, canid_t id)
{
    if (!iface->filter_count)
        return 1;
    for (int i = 0; i < iface->filter_count; i++) {
        struct can_filter filter = ordinary_filter(iface->filters[i]);
        int equal = (id & filter.can_mask) == (filter.can_id & filter.can_mask);
        if ((filter.can_id & CAN_INV_FILTER) ? !equal : equal)
            return 1;
    }
    return 0;
}

int can_socket_apply_filters(app_ctx_t *app, can_iface_t *iface, int fd)
{
    (void)app;
    if (!iface || fd < 0 || iface->filter_count < 0 || iface->filter_count > CAN_MAX_FILTERS) {
        errno = EINVAL;
        return -1;
    }
    struct can_filter filters[CAN_MAX_FILTERS + CAN_BIND_MAX];
    int count = 0;
    if (!iface->filter_count)
        filters[count++] = (struct can_filter){0, 0};
    else {
        for (int i = 0; i < iface->filter_count; i++)
            filters[count++] = ordinary_filter(iface->filters[i]);
        int extra = can_service_filters_locked(iface->ifname, filters + count, CAN_BIND_MAX);
        if (extra < 0 || extra > CAN_BIND_MAX) {
            errno = EINVAL;
            return -1;
        }
        count += extra;
    }
    return setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, filters, count * sizeof(filters[0]));
}

/* Common writer: wire size depends on the frame format, not sizeof(canfd_frame).
 * Motor calls defer only the DBC bookkeeping to the existing CAN send worker. */
int can_write_frame_locked(app_ctx_t *app, int index, const struct canfd_frame *frame, int fd_frame,
                           int defer_record)
{
    can_iface_t *iface = &app->can->ifaces[index];
    if (frame->len > (fd_frame ? CANFD_MAX_DLEN : CAN_MAX_DLEN) || (fd_frame && !iface->fd_mode)) {
        errno = EMSGSIZE;
        return -1;
    }
    size_t size = fd_frame ? CANFD_MTU : CAN_MTU;
    ssize_t written = send(iface->sock_fd, frame, size, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (written != (ssize_t)size) {
        if (written >= 0)
            errno = EIO;
        return -1;
    }
    if (defer_record) {
        unsigned next = (app->can->record_head + 1) % 32;
        if (next != app->can->record_tail) {
            app->can->records[app->can->record_head].index = index;
            app->can->records[app->can->record_head].frame = *frame;
            app->can->record_head = next;
            eventfd_signal(app->can->tx_efd);
        }
    } else {
        can_observe_tx_locked(app, index, frame);
    }
    return 0;
}

static void flush_deferred_records_locked(app_ctx_t *app)
{
    while (app->can->record_tail != app->can->record_head) {
        int index = app->can->records[app->can->record_tail].index;
        struct canfd_frame frame = app->can->records[app->can->record_tail].frame;
        app->can->record_tail = (app->can->record_tail + 1) % 32;
        if (index >= 0 && index < app->can->count)
            can_observe_tx_locked(app, index, &frame);
    }
}

/**
 * ifname_valid - 检查 CAN 接口名是否合法（公共实现见 core/common.h 内联函数）。
 * @name: 待校验的接口名。
 * @return: 合法返回 1，否则返回 0。
 */

/**
 * apply_dbitrate - 使用 netlink 设置 CAN FD 数据位时钟参数，确保接口工作在指定数据速率。
 * @sk: netlink socket。
 * @ifname: 目标 CAN 接口名。
 * @dbitrate: 数据位速率（bps）。
 */
static void apply_dbitrate(struct nl_sock *sk, const char *ifname, uint32_t dbitrate)
{
    struct rtnl_link *live = NULL;
    if (rtnl_link_get_kernel(sk, 0, ifname, &live) != 0 || !live)
        return;
    struct can_bittiming_const dbtc;
    if (rtnl_link_can_get_data_bittiming_const(live, &dbtc) == 0) {
        struct can_bittiming dbt = {.bitrate = dbitrate};
        dbt.brp = dbtc.brp_min;
        dbt.prop_seg = dbtc.tseg1_min > 1 ? dbtc.tseg1_min - 1 : 1;
        dbt.phase_seg1 = 1;
        dbt.phase_seg2 = dbtc.tseg2_min < 1 ? 1 : dbtc.tseg2_min;
        dbt.sjw = 1;
        dbt.sample_point = 800;
        rtnl_link_can_set_ctrlmode(live, CAN_CTRLMODE_FD);
        rtnl_link_can_set_data_bittiming(live, &dbt);
        rtnl_link_change(sk, live, live, 0);
        LOG_INFO("CAN %s: data bitrate set to %u bps", ifname, dbitrate);
    }
    rtnl_link_put(live);
}

/**
 * can_socket_configure - 配置指定 CAN 设备的比特率、FD 模式和上电状态。
 * @ifname: CAN 接口名。
 * @bitrate: 普通 CAN 比特率。
 * @dbitrate: CAN FD 数据比特率。
 * @fd_mode: 是否启用 CAN FD。
 * @restart_ms: 自动重启等待时间。
 * @bring_up: 是否启动接口。
 * @return: 成功返回 0，失败返回 -1。
 */
int can_socket_configure(const char *ifname, int bitrate, int dbitrate, int fd_mode, int restart_ms,
                         int bring_up)
{
    if (!ifname_valid(ifname)) {
        LOG_ERROR("invalid CAN interface name: '%s'", ifname ? ifname : "(null)");
        return -1;
    }
    if (bitrate <= 0)
        bitrate = 500000;

    struct nl_sock *sk = nl_socket_alloc();
    if (!sk) {
        LOG_ERROR("nl_socket_alloc");
        return -1;
    }
    if (nl_connect(sk, NETLINK_ROUTE) < 0) {
        LOG_ERROR("nl_connect");
        nl_socket_free(sk);
        return -1;
    }

    /* 用 netlink 探测接口是否已存在；避免 if_nametoindex() 返回 0 后仍走
       rtnl_link_add(NLM_F_CREATE) 而对已存在接口报 "Object busy" */
    struct rtnl_link *existing = NULL;
    int exists = (rtnl_link_get_kernel(sk, 0, ifname, &existing) == 0 && existing != NULL);
    if (existing)
        rtnl_link_put(existing);

    if (exists) {
        /* 接口已存在，用 ip 命令配置（MCP251xFD 等芯片需要原子操作） */
        char cmd[512];
        size_t off = 0;
        /* 安全追加：缓冲区写满后安全截断，指针/长度均不越界（公共宏见 common.h） */
        BUF_APPEND(cmd, off, "ip link set %s down 2>/dev/null; ip link set %s type can bitrate %d",
                   ifname, ifname, bitrate);
        if (fd_mode)
            BUF_APPEND(cmd, off, " fd on");
        if (fd_mode && dbitrate > 0)
            BUF_APPEND(cmd, off, " dbitrate %d", dbitrate);
        /* 用 ; 而非 &&：ip link set type can 对已有CAN接口可能返回 EEXIST */
        BUF_APPEND(cmd, off, " 2>/dev/null");
        if (bring_up)
            BUF_APPEND(cmd, off, "; ip link set %s up 2>/dev/null", ifname);
        int rc = system(cmd);
        if (rc != 0 && bring_up) {
            /* type can 部分失败但 up 可能成功——验证接口是否真的 up */
            unsigned int flags = 0;
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            ifreq_set_name(&ifr, ifname);
            int sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock >= 0) {
                ioctl(sock, SIOCGIFFLAGS, &ifr);
                flags = (unsigned int)ifr.ifr_flags;
                close(sock);
            }
            if (!(flags & IFF_UP)) {
                LOG_ERROR("CAN %s: config via ip command failed", ifname);
                nl_socket_free(sk);
                return -1;
            }
            LOG_INFO("CAN %s: type can returned EEXIST, interface is up anyway", ifname);
        }
        LOG_INFO("CAN %s configured (existing), %s", ifname, bring_up ? "up" : "down");
        nl_socket_free(sk);
        return 0;
    }

    /* 接口不存在：创建新 link */
    struct rtnl_link *link = rtnl_link_alloc();
    rtnl_link_set_name(link, ifname);
    rtnl_link_set_type(link, "can");

    rtnl_link_can_set_bitrate(link, (uint32_t)bitrate);
    if (fd_mode)
        rtnl_link_can_set_ctrlmode(link, CAN_CTRLMODE_FD);
    if (restart_ms > 0)
        rtnl_link_can_set_restart_ms(link, (uint32_t)restart_ms);

    int ret = rtnl_link_add(sk, link, NLM_F_CREATE);
    if (ret < 0) {
        LOG_ERROR("CAN %s: configure failed: %s", ifname, nl_geterror(ret));
        rtnl_link_put(link);
        nl_socket_free(sk);
        return -1;
    }

    if (fd_mode && dbitrate > 0)
        apply_dbitrate(sk, ifname, (uint32_t)dbitrate);

    if (bring_up) {
        rtnl_link_set_flags(link, IFF_UP);
        if (rtnl_link_change(sk, link, link, 0) < 0)
            LOG_ERROR("CAN %s: bring up failed", ifname);
    }

    LOG_INFO("CAN %s configured (bitrate=%d%s%s) %s", ifname, bitrate, fd_mode ? " FD" : "",
             (fd_mode && dbitrate > 0) ? "+dbitrate" : "", bring_up ? "up" : "down");
    rtnl_link_put(link);
    nl_socket_free(sk);
    return 0;
}

/**
 * can_socket_open - 打开一个原始 CAN socket，并绑定到指定接口。
 * @ifname: CAN 接口名。
 * @fd_mode: 是否启用 CAN FD 模式。
 * @return: 成功返回 socket fd，失败返回 -1。
 */
int can_socket_open(const char *ifname, int fd_mode)
{
    if (!ifname_valid(ifname)) {
        LOG_ERROR("invalid CAN interface name: '%s'", ifname ? ifname : "(null)");
        return -1;
    }
    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) {
        LOG_ERROR("socket(PF_CAN)");
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifreq_set_name(&ifr, ifname);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        LOG_ERROR("ioctl SIOCGIFINDEX for %s", ifname);
        close(s);
        return -1;
    }
    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind CAN %s", ifname);
        close(s);
        return -1;
    }
    if (fd_mode) {
        int enable = 1;
        if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof(enable)) < 0)
            LOG_INFO("CAN_RAW_FD_FRAMES not available on %s, classic CAN only", ifname);
        else
            LOG_INFO("CAN FD socket enabled on %s", ifname);
    }

    /* 非阻塞：配合 epoll 就绪通知，read/write 在无数据/不可写时返回 EAGAIN 而不是阻塞 */
    if (set_nonblock(s) < 0) {
        LOG_ERROR("CAN %s: fcntl O_NONBLOCK: %s", ifname, strerror(errno));
        close(s);
        return -1;
    }

    LOG_INFO("CAN socket opened on %s", ifname);
    return s;
}

/**
 * can_socket_close - 关闭已打开的 CAN socket fd。
 * @fd: socket 文件描述符。
 */
void can_socket_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

/* ---- TX 队列 + eventfd 跨线程唤醒 ----
   写/读 eventfd 的公共实现见 core/common.h（eventfd_signal / eventfd_consume） */

/**
 * can_epoll_update_tx - 按该接口发送队列是否为空，动态切换 epoll 是否监听 EPOLLOUT。
 * 队列非空才监听 EPOLLOUT，避免 socket 一直可写导致 epoll 忙轮询。
 * @ctx: CAN 上下文。
 * @idx: 接口索引。
 */
static void can_epoll_update_tx(can_ctx_t *ctx, int idx)
{
    int fd = ctx->ifaces[idx].sock_fd;
    if (fd < 0)
        return;
    int want_out = data_slot_count(&ctx->ifaces[idx].txq) > 0;
    if (want_out) {
        struct epoll_event ev;
        ev.events = EPOLLOUT;
        ev.data.u32 = (uint32_t)(idx + 1);
        if (epoll_ctl(ctx->send_epfd, EPOLL_CTL_MOD, fd, &ev) < 0 && errno == ENOENT)
            epoll_ctl(ctx->send_epfd, EPOLL_CTL_ADD, fd, &ev);
    } else {
        epoll_ctl(ctx->send_epfd, EPOLL_CTL_DEL, fd, NULL);
    }
}

/* 把接口 socket 以 EPOLLIN 注册到接收 epoll（can_recv_task 使用） */
static void can_epoll_add_recv(can_ctx_t *ctx, int idx)
{
    int fd = ctx->ifaces[idx].sock_fd;
    if (fd < 0)
        return;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = (uint32_t)(idx + 1);
    epoll_ctl(ctx->recv_epfd, EPOLL_CTL_ADD, fd, &ev);
}

/**
 * can_tx_frame - 异步发送一帧 CAN 数据。
 * 压入目标接口 TX 队列，由 can_send_task 排空写 socket 并做发送方向 DBC 解析。
 * @app: 全局应用上下文。
 * @ifname: 目标接口名。
 * @frame: 待发送 CAN(FD) 帧。
 * @return: 入队成功返回 0；失败返回 -1。
 */
int can_tx_frame(app_ctx_t *app, const char *ifname, const struct canfd_frame *frame)
{
    if (!app || !app->can || !ifname || !frame) {
        errno = EINVAL;
        return -1;
    }
    can_ctx_t *ctx = app->can;

    pthread_mutex_lock(&app->can_mutex);
    if (!app->cfg || !app->cfg->args.can_enabled) {
        pthread_mutex_unlock(&app->can_mutex);
        errno = EACCES;
        return -1;
    }
    int idx = can_iface_index(ctx->ifaces, ctx->count, ifname);
    if (idx < 0 || ctx->ifaces[idx].sock_fd < 0 || !ctx->ifaces[idx].up) {
        pthread_mutex_unlock(&app->can_mutex);
        errno = idx < 0 ? ENODEV : ENETDOWN;
        return -1;
    }

    if (can_service_reserved_locked(ifname, frame->can_id)) {
        pthread_mutex_unlock(&app->can_mutex);
        errno = EBUSY;
        return -1;
    }
    if (frame->len > (ctx->ifaces[idx].fd_mode ? CANFD_MAX_DLEN : CAN_MAX_DLEN)) {
        pthread_mutex_unlock(&app->can_mutex);
        errno = EMSGSIZE;
        return -1;
    }
    /* 发送统一走 TX 队列：由 can_send_task 弹出写 socket 并做发送方向 DBC 解析 */
    if (can_queue_frame_locked(app, idx, frame, ctx->ifaces[idx].fd_mode) != 0) {
        pthread_mutex_unlock(&app->can_mutex);
        return -1;
    }
    pthread_mutex_unlock(&app->can_mutex);
    return 0;
}

/**
 * handle_can_output - 排空各接口发送队列，把待发帧写入对应 socket。
 * @app: 全局应用上下文。
 */
static void handle_can_output(app_ctx_t *app)
{
    can_ctx_t *ctx = app->can;
    pthread_mutex_lock(&app->can_mutex);
    flush_deferred_records_locked(app);
    if (!app->cfg->args.can_enabled) {
        pthread_mutex_unlock(&app->can_mutex);
        return;
    }

    for (int idx = 0; idx < ctx->count; idx++) {
        int fd = ctx->ifaces[idx].sock_fd;
        if (fd < 0)
            continue;

        data_slot_t *q = &ctx->ifaces[idx].txq;
        /* 一轮最多处理当前队列长度，避免 EAGAIN 帧重排入队尾导致死循环 */
        int limit = data_slot_count(q);
        while (limit-- > 0) {
            struct canfd_frame f;
            if (!data_slot_pop(q, &f))
                break;

            if (can_service_reserved_locked(ctx->ifaces[idx].ifname, f.can_id))
                continue;
            int sent = can_write_frame_locked(app, idx, &f, !!(f.flags & CANFD_FDF), 0);
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (data_slot_push(q, &f, 0) != 0)
                    LOG_ERROR("can tx queue full on %s, drop frame", ctx->ifaces[idx].ifname);
                break;
            }
        }
    }

    /* 队列非空才监听 EPOLLOUT，排空则回到仅监听 TX eventfd */
    for (int i = 0; i < ctx->count; i++)
        can_epoll_update_tx(ctx, i);
    pthread_mutex_unlock(&app->can_mutex);
}

/**
 * handle_can_input - 处理单个 CAN 接口上的输入事件，读取帧并在失败时自动重建 socket。
 * @app: 全局应用上下文。
 * @can_idx: CAN 接口索引。
 */
static void handle_can_input(app_ctx_t *app, int can_idx)
{
    can_ctx_t *ctx = app->can;
    pthread_mutex_lock(&app->can_mutex);
    /* An epoll batch may outlive the switch-off that closed these sockets. */
    if (!app->cfg->args.can_enabled || ctx->ifaces[can_idx].sock_fd < 0) {
        pthread_mutex_unlock(&app->can_mutex);
        return;
    }
    int fd = ctx->ifaces[can_idx].sock_fd;
    const char *ifname = ctx->ifaces[can_idx].ifname;

    /* 每次 epoll 事件最多排空 64 帧就返回：避免 CAN 总线持续有数据时无限排水，
       导致 can_recv_task 无法回到下一次 epoll_wait 前的 watchdog_feed_self("can_recv")，
       从而被看门狗误判为卡死。 */
    for (int i = 0; i < 64; i++) {
        struct canfd_frame frame = {0};
        ssize_t n = read(fd, &frame, sizeof(frame));
        if (n == CAN_MTU || n == CANFD_MTU) {
            if (frame.len > (n == CAN_MTU ? CAN_MAX_DLEN : CANFD_MAX_DLEN))
                continue;
            if (n == CANFD_MTU)
                frame.flags |= CANFD_FDF;
            int ordinary = ordinary_frame_allowed(&ctx->ifaces[can_idx], frame.can_id);
            can_slot_dispatch_locked(ifname, &frame);
            if (!ordinary)
                continue;
            can_observe_rx_locked(app, can_idx, &frame);

        } else if (n > 0) {
            continue; /* Reject truncated/non-CAN datagrams without reading padding. */
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            LOG_ERROR("can read %s: %s", ifname, strerror(errno));
            can_service_prepare_close_locked(app, ifname);
            can_socket_close(fd);
            ctx->ifaces[can_idx].sock_fd = -1;
            int new_fd = can_socket_open(ifname, ctx->ifaces[can_idx].fd_mode);
            if (new_fd >= 0) {
                ctx->ifaces[can_idx].sock_fd = new_fd;
                if (can_socket_apply_filters(app, &ctx->ifaces[can_idx], new_fd)) {
                    can_socket_close(new_fd);
                    ctx->ifaces[can_idx].sock_fd = -1;
                    break;
                }
                /* 重新注册到接收 epoll；并同步发送 epoll 的 EPOLLOUT 状态 */
                can_epoll_add_recv(ctx, can_idx);
                can_epoll_update_tx(ctx, can_idx);
                LOG_INFO("CAN %s reconnected", ifname);
            }
            break;
        }
    }

    pthread_mutex_unlock(&app->can_mutex);
}

void *can_recv_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    (void)pthread_setname_np(pthread_self(), "can_recv");
    if (watchdog_register_thread(pthread_self(), "can_recv", 3, 3) < 0) {
        app->fatal = 1;
        app->running = 0;
        return NULL;
    }
    can_ctx_t *ctx = app->can;
    if (!ctx)
        goto out;

    LOG_INFO("can_recv_task started (%d iface(s))", ctx->count);
    while (app->running) {
        struct epoll_event events[64];
        int nfds = epoll_wait_feed(ctx->recv_epfd, events, 64, 500, "can_recv");
        if (nfds < 0)
            break;
        for (int i = 0; i < nfds; i++) {
            uint32_t tag = events[i].data.u32;
            if (tag < 1 || tag > (uint32_t)ctx->count)
                continue;
            int idx = (int)(tag - 1);
            uint32_t ev = events[i].events;
            if (ev & (EPOLLIN | EPOLLERR | EPOLLHUP))
                handle_can_input(app, idx);
        }
    }
    LOG_INFO("can_recv_task stopped");
out:
    if (app->running) app->fatal = 1;
    app->running = 0;
    watchdog_unregister_self("can_recv");
    return NULL;
}

void *can_send_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    (void)pthread_setname_np(pthread_self(), "can_send");
    if (watchdog_register_thread(pthread_self(), "can_send", 3, 3) < 0) {
        app->fatal = 1;
        app->running = 0;
        return NULL;
    }
    can_ctx_t *ctx = app->can;
    if (!ctx)
        goto out;

    for (int i = 0; i < ctx->count; i++)
        can_epoll_update_tx(ctx, i);
    LOG_INFO("can_send_task started (%d iface(s))", ctx->count);
    while (app->running) {
        struct epoll_event events[64];
        int nfds = epoll_wait_feed(ctx->send_epfd, events, 64, 500, "can_send");
        if (nfds < 0)
            break;
        for (int i = 0; i < nfds; i++) {
            uint32_t tag = events[i].data.u32;
            if (tag == 0) {
                /* TX eventfd：有帧压入 TX 队列，排空各接口发送队列 */
                eventfd_consume(ctx->tx_efd);
                handle_can_output(app);
                continue;
            }
            if (tag < 1 || tag > (uint32_t)ctx->count)
                continue;
            uint32_t ev = events[i].events;
            if (ev & EPOLLOUT)
                handle_can_output(app);
        }
    }
    LOG_INFO("can_send_task stopped");
out:
    if (app->running) app->fatal = 1;
    app->running = 0;
    watchdog_unregister_self("can_send");
    return NULL;
}

int can_enumerate_system(char names[][IFNAMSIZ], int max)
{
    int count = 0;
    struct nl_sock *sk = nl_socket_alloc();
    if (!sk)
        return 0;
    if (nl_connect(sk, NETLINK_ROUTE) < 0) {
        nl_socket_free(sk);
        return 0;
    }

    for (int idx = 1;; idx++) {
        struct rtnl_link *link = NULL;
        if (rtnl_link_get_kernel(sk, idx, NULL, &link) != 0)
            break;
        if (!link)
            break;

        const char *kind = rtnl_link_get_type(link);
        if (!kind || strcmp(kind, "can") != 0) {
            rtnl_link_put(link);
            continue;
        }

        const char *name = rtnl_link_get_name(link);
        if (name && ifname_valid(name) && count < max)
            safe_strncpy(names[count++], IFNAMSIZ, name);
        rtnl_link_put(link);
    }

    nl_socket_free(sk);
    return count;
}

/* 内核 uapi struct can_ioctl_data（内核 >= 3.5）：布局稳定，自行声明避免依赖过旧系统头文件 */
struct local_can_ioctl_data {
    uint32_t index;
    uint32_t flags;
    union {
        struct {
            uint32_t ctrlmode;
            uint32_t ctrlmode_supported;
        };
        struct {
            uint32_t bitrate;
            uint32_t dbitrate;
        };
    };
};

/* 查询指定 CAN 接口是否支持 CAN FD（读内核 can 控制模式支持位）。
 * 通过 ioctl(SIOCGIFINFO_EXT) 获取 ctrlmode_supported，供配置页展示。 */
int can_fd_supported(const char *ifname)
{
    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0)
        return 0;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifreq_set_name(&ifr, ifname);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        close(s);
        return 0;
    }

    struct local_can_ioctl_data data;
    memset(&data, 0, sizeof(data));
    data.index = ifr.ifr_ifindex;
    if (ioctl(s, _IOR('j', 0x0D, struct local_can_ioctl_data), &data) < 0) {
        close(s);
        return 0;
    }

    close(s);
    return (data.ctrlmode_supported & CAN_CTRLMODE_FD) ? 1 : 0;
}

int can_init(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    app_args_t *args = &app->cfg->args;
    static can_ctx_t storage;
    can_ctx_t *ctx = &storage;
    int rc = pthread_mutex_init(&app->can_mutex, NULL);
    if (rc != 0) {
        LOG_ERROR("can: mutex init failed: %s", strerror(rc));
        return -1;
    }
    app->can = ctx;
    memset(ctx, 0, sizeof(*ctx));
    ctx->recv_epfd = -1;
    ctx->send_epfd = -1;
    ctx->tx_efd = -1;
    ctx->ifaces = args->can_ifaces;
    ctx->count = 0;
    /* 每个接口各初始化一个发送队列和一个接收队列 */
    for (int i = 0; i < args->can_count; i++) {
        ctx->ifaces[i].sock_fd = -1;
        if (data_slot_init(&ctx->ifaces[i].txq, ctx->ifaces[i].tx_frames,
                           sizeof(struct canfd_frame), CAN_TX_SLOT_CAPACITY) != 0)
            goto fail;
        ctx->count++;
    }
    /* 接收 epoll（can_recv_task，监听各接口 EPOLLIN） */
    ctx->recv_epfd = epoll_create1(0);
    if (ctx->recv_epfd < 0) {
        LOG_ERROR("can: recv epoll_create1 failed");
        goto fail;
    }
    /* 发送 epoll（can_send_task，监听 TX eventfd + 各接口 EPOLLOUT） */
    ctx->send_epfd = epoll_create1(0);
    if (ctx->send_epfd < 0) {
        LOG_ERROR("can: send epoll_create1 failed");
        goto fail;
    }
    /* TX eventfd：跨线程唤醒 can_send_task，让其检测“有帧压入 TX 队列” */
    ctx->tx_efd = eventfd(0, EFD_NONBLOCK);
    if (ctx->tx_efd < 0) {
        LOG_ERROR("can: eventfd failed");
        goto fail;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = 0;
    if (epoll_ctl(ctx->send_epfd, EPOLL_CTL_ADD, ctx->tx_efd, &ev) < 0) {
        LOG_ERROR("can: epoll_ctl(tx_efd) failed");
        goto fail;
    }

    for (int i = 0; i < args->can_count; i++) {
        can_iface_t *iface = &args->can_ifaces[i];
        iface->sock_fd = -1;
        if (!args->can_enabled)
            continue;
        if (can_socket_configure(iface->ifname, iface->bitrate, iface->dbitrate, iface->fd_mode,
                                 iface->restart_ms, iface->up) < 0) {
            /* 无 CAN 硬件/驱动未加载时接口无法配置：跳过该接口，不阻塞整体启动，
               之后可通过 Web /api/can/toggle 或重启服务再试 */
            LOG_ERROR("CAN %s: configure failed, skip this interface", iface->ifname);
            continue;
        }
        int fd = can_socket_open(iface->ifname, iface->fd_mode);
        if (fd < 0) {
            LOG_ERROR("CAN %s: open failed, skip this interface", iface->ifname);
            continue;
        }
        iface->sock_fd = fd;
        if (can_socket_apply_filters(app, iface, fd)) {
            can_socket_close(fd);
            iface->sock_fd = -1;
            continue;
        }
        can_epoll_add_recv(ctx, i);
    }

    can_app = app;
    LOG_INFO("%d CAN interface(s) initialized", args->can_count);
    for (int i = 0; i < args->can_count; i++)
        LOG_INFO("  CAN[%d]=%s filters=%d", i, args->can_ifaces[i].ifname,
                 args->can_ifaces[i].filter_count);
    return 0;

fail:
    can_cleanup(app);
    return -1;
}

void can_cleanup(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    can_ctx_t *ctx = app->can;
    if (!ctx)
        return;

    can_service_prepare_close_locked(app, NULL);
    memset(bindings, 0, sizeof(bindings));
    for (int i = 0; i < ctx->count; i++) {
        can_socket_close(ctx->ifaces[i].sock_fd);
        ctx->ifaces[i].sock_fd = -1;
        data_slot_destroy(&ctx->ifaces[i].txq);
    }
    if (ctx->tx_efd >= 0)
        close(ctx->tx_efd);
    if (ctx->recv_epfd >= 0)
        close(ctx->recv_epfd);
    if (ctx->send_epfd >= 0)
        close(ctx->send_epfd);
    app->can = NULL;
    can_app = NULL;
    pthread_mutex_destroy(&app->can_mutex);
}

/* Keep workers and their watchdog heartbeats alive while CAN is off.
 * Only descriptors and pending frames belong to this feature's lifecycle. */
int can_feature_set_locked(app_ctx_t *app, int enabled)
{
    if (!app || !app->cfg || !app->can || (enabled != 0 && enabled != 1)) {
        errno = EINVAL;
        return -1;
    }
    can_ctx_t *ctx = app->can;
    if (!!app->cfg->args.can_enabled == enabled)
        return 0;
    if (!enabled) {
        can_service_prepare_close_locked(app, NULL);
        app->cfg->args.can_enabled = 0;
        for (int i = 0; i < ctx->count; i++) {
            can_iface_t *iface = &ctx->ifaces[i];
            if (iface->sock_fd >= 0) {
                if (ctx->recv_epfd >= 0)
                    epoll_ctl(ctx->recv_epfd, EPOLL_CTL_DEL, iface->sock_fd, NULL);
                if (ctx->send_epfd >= 0)
                    epoll_ctl(ctx->send_epfd, EPOLL_CTL_DEL, iface->sock_fd, NULL);
                can_socket_close(iface->sock_fd);
                iface->sock_fd = -1;
            }
            struct canfd_frame discarded;
            while (data_slot_pop(&iface->txq, &discarded)) {
            }
        }
        return 0;
    }

    int staged[CAN_MAX_IFACES];
    for (int i = 0; i < CAN_MAX_IFACES; i++)
        staged[i] = -1;
    for (int i = 0; i < ctx->count; i++) {
        can_iface_t *iface = &ctx->ifaces[i];
        watchdog_feed_self("http");
        if (can_socket_configure(iface->ifname, iface->bitrate, iface->dbitrate, iface->fd_mode,
                                 iface->restart_ms, iface->up) < 0)
            goto fail;
        staged[i] = can_socket_open(iface->ifname, iface->fd_mode);
        if (staged[i] < 0)
            goto fail;
        if (can_socket_apply_filters(app, iface, staged[i]) < 0)
            goto fail;
        struct epoll_event ev = {.events = EPOLLIN, .data.u32 = (uint32_t)(i + 1)};
        if (epoll_ctl(ctx->recv_epfd, EPOLL_CTL_ADD, staged[i], &ev) < 0)
            goto fail;
    }
    for (int i = 0; i < ctx->count; i++)
        ctx->ifaces[i].sock_fd = staged[i];
    app->cfg->args.can_enabled = 1;
    return 0;

fail:
    {
        int saved_errno = errno;
        for (int i = 0; i < ctx->count; i++) {
            if (staged[i] < 0)
                continue;
            epoll_ctl(ctx->recv_epfd, EPOLL_CTL_DEL, staged[i], NULL);
            can_socket_close(staged[i]);
        }
        errno = saved_errno;
        return -1;
    }
}
