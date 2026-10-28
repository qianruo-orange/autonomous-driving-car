/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CAR_MOTOR_CAN_H
#define CAR_MOTOR_CAN_H

#include "core/common.h"

#define CAR_MOTOR_CAN_CONFIG "config/car_motor_can.conf"

typedef struct {
    int configured;
    int online;
    unsigned long long tx_frames;
    unsigned long long tx_errors;
    unsigned long long rx_frames;
    unsigned long long decode_errors;
    char interface_name[IFNAMSIZ];
    char error[160];
} car_motor_can_status_t;

/* Called after follow controller locks/ready are initialized, while stopped.
 * A missing, disabled or invalid profile is nonfatal and never binds a motor.
 * No activation or startup command is sent by this function. */
int car_motor_can_init(app_ctx_t *app);
/* Call after the control worker has joined, before its feedback mutex is destroyed. */
void car_motor_can_destroy(void);
/* Safe before initialization and after destruction. */
void car_motor_can_get_status(car_motor_can_status_t *out);

#endif
