/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HTTP_API_CAN_H
#define HTTP_API_CAN_H

#include <linux/can.h>

/* CAN 接收路径记录一帧原始报文到环形缓存（供 GET /api/can/frames 展示）。
 * 由 can_recv_task 在收到帧后调用；ifname 为来源接口名，frame 为原始 CAN(FD) 帧。 */
void http_can_record_rx(const char *ifname, const struct canfd_frame *frame);

/* DBC decode direction: received frames or transmitted frames. */
typedef enum { DBC_DIR_RX = 0, DBC_DIR_TX = 1 } dbc_dir_t;
void http_dbc_record(dbc_dir_t dir, const char *ifname, canid_t can_id,
                     const char *name, const char *text);

#endif /* HTTP_API_CAN_H */
