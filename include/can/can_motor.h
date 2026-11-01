/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "can/can_socket.h"
#define CAN_MOTOR_CONFIG "config/car_motor_can.conf"
typedef struct {
    int configured, online;
    unsigned long long tx_frames, tx_errors, rx_frames, decode_errors;
    char interface_name[IFNAMSIZ];
    char error[160];
} can_motor_status_t;
/* Optional payload mapping on the existing CAN interfaces, no extra socket or
 * receiver. Initialize after the stopped controller is ready; clean up after
 * joining its worker, before destroying its feedback lock. */
int can_motor_init(app_ctx_t *app);
void can_motor_cleanup(void);
void can_motor_get_status(can_motor_status_t *out);
/* Caller holds app.can_mutex. Try the documented stop before interface down,
 * reconnect or global disable, then invalidate old feedback and queued IDs. */
void can_motor_prepare_close_locked(app_ctx_t *app, const char *ifname);
