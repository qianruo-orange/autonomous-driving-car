/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "can/can_internal.h"
#include "can/dbc_parser.h"
#include "http/http_api_can.h"
#include "http/http_api_dbc.h"

/* Web/DBC observation is an adapter, separate from the socket transport. */
static void observe(app_ctx_t *app, int index, const struct canfd_frame *frame,
                    can_frame_direction_t direction)
{
    const char *ifname = app->can->ifaces[index].ifname;
    if (direction == CAN_FRAME_RX)
        http_can_record_rx(ifname, frame);
    char name[DBC_MAX_NAME_LEN], text[DBC_DECODE_TEXT_MAX];
    canid_t id = 0;
    if (!dbc_decode_frame(app, index, ifname, frame, &id, name, sizeof(name), text, sizeof(text)))
        http_dbc_record(direction == CAN_FRAME_RX ? DBC_DIR_RX : DBC_DIR_TX, ifname, id, name,
                        text);
    if (direction == CAN_FRAME_TX)
        can_service_dispatch_locked(app, ifname, frame, direction, 1);
}
void can_observe_rx_locked(app_ctx_t *app, int index, const struct canfd_frame *frame)
{
    observe(app, index, frame, CAN_FRAME_RX);
}
void can_observe_tx_locked(app_ctx_t *app, int index, const struct canfd_frame *frame)
{
    observe(app, index, frame, CAN_FRAME_TX);
}
