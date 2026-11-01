/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CAN_SOCKET_H
#define CAN_SOCKET_H

#include "core/config.h"
#include <linux/can.h>
#include <linux/if.h>
#include <pthread.h>

/* RX is routed to registered business slots. TX uses the same bounded slot
 * implementation, with per-interface frame storage owned by the CAN context. */

/* ---- CAN 子系统上下文 ---- */
typedef struct can_ctx {
    can_iface_t *ifaces;
    int count;
    int recv_epfd; /* 接收 epoll（can_recv_task） */
    int send_epfd; /* 发送 epoll（can_send_task，监听 tx_efd + 各接口 EPOLLOUT） */
    /* Already-sent records deferred from the real-time motor path. */
    struct {
        int index;
        struct canfd_frame frame;
    } records[32];
    unsigned record_head, record_tail;
    int tx_efd; /* TX eventfd：压入 TX 队列后 write，唤醒 can_send_task */
} can_ctx_t;

/* CAN 接收线程：按接口和 ID 投递业务槽，并记录 DBC 结果 */
void *can_recv_task(void *arg);

/* CAN 发送线程：排空 txq 写 socket 并做发送方向 DBC 解析 */
void *can_send_task(void *arg);

/* 异步发送一帧：入对应接口 TX 队列，由 can_send_task 排空写 socket 并做发送方向 DBC 解析。
 * ifname 为目标接口名。返回 0 表示已入队，-1 失败。 */
int can_tx_frame(app_ctx_t *app, const char *ifname, const struct canfd_frame *frame);

#include "core/data_slot.h"
#define CAN_BIND_MAX 32
#define CAN_SLOT_CAPACITY 16
#define CAN_BIND_RX 1u
#define CAN_BIND_TX 2u
#define CAN_BIND_RESERVED 4u

typedef struct {
    uint64_t handle, received_ms;
    struct canfd_frame frame;
} can_data_t;
typedef struct {
    data_slot_t queue;
    can_data_t items[CAN_SLOT_CAPACITY];
} can_slot_t;
#define can_slot_init(s)                                                                           \
    data_slot_init(&(s)->queue, (s)->items, sizeof(can_data_t), CAN_SLOT_CAPACITY)
typedef struct {
    const char *port;
    canid_t id;     /* CAN_EFF_FLAG distinguishes extended IDs */
    unsigned flags; /* RX/TX; reserved TX IDs reject raw Web/queue injection */
    int fd;
    can_slot_t *slot;
    void (*before_close)(app_ctx_t *, const char *);
} can_binding_t;
/* CAN validates the configured port and verifies it is a system CAN interface.
 * The returned generation handle binds port, ID, format and destination slot. */
uint64_t can_component_register(const can_binding_t *);
int can_component_unregister(uint64_t handle);
/* 仅填业务句柄和载荷；接口、CAN ID、帧格式由 CAN 模块封装。 */
int can_send(uint64_t handle, const void *data, size_t length);
/* Motor control already holds can_mutex: bounded immediate send, no stale queue. */
uint64_t can_locked_register(const can_binding_t *);
int can_locked_unregister(uint64_t handle);
int can_send_locked(uint64_t handle, const void *data, size_t length);
void can_service_prepare_close_locked(app_ctx_t *, const char *);

/* 打开/关闭单个 CAN 套接字（can_init 内部使用） */
int can_socket_open(const char *ifname, int fd_mode);
void can_socket_close(int fd);

/* 设置 CAN 硬件接收过滤器 */
int can_socket_set_filter(int fd, canid_t id, canid_t mask);

/* 通过 ip link 命令配置 CAN 接口 */
int can_socket_configure(const char *ifname, int bitrate, int dbitrate, int fd, int restart_ms,
                         int up);

/* 根据配置初始化所有 CAN 接口 */
int can_init(void *arg);
void can_cleanup(void *ctx);

/* Caller holds app->can_mutex. Disabling closes gateway sockets and preserves
 * interface settings; enabling reapplies them. Socket activation is transactional. */
int can_feature_set_locked(app_ctx_t *app, int enabled);

/* 枚举系统中实际存在的 CAN 接口（netlink 路由，kind=="can"），返回写入的数量 */
int can_enumerate_system(char names[][IFNAMSIZ], int max);

/* 查询指定 CAN 接口是否支持 CAN FD，支持返回 1，不支持或查询失败返回 0 */
int can_fd_supported(const char *ifname);

/* Apply the OR-union of ordinary filters and registered service IDs.
 * Caller holds app.can_mutex; also works for a staged replacement descriptor. */
int can_socket_apply_filters(app_ctx_t *app, can_iface_t *iface, int fd);

#endif /* CAN_SOCKET_H */
