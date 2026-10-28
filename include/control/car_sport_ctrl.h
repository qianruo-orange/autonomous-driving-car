/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CAR_SPORT_CTRL_H
#define CAR_SPORT_CTRL_H
#include "core/common.h"
#include "yolo/yolo_events.h"
typedef struct {
    double kp, ki, kd;
} follow_gains_t;
typedef struct {
    double integral, previous, derivative;
    int valid;
} follow_pid_t;
double follow_pid_step(follow_pid_t *pid, follow_gains_t gains, double error, double dt,
                       double limit);
/* Bound the absolute I-term contribution independently from the total output.
 * An integral limit of zero clears/disables accumulation; wheel loops use 0..1. */
double follow_pid_step_limited(follow_pid_t *pid, follow_gains_t gains, double error, double dt,
                               double output_limit, double integral_limit);
/* Differential correction and both wheel targets are in m/s; no wheel track is needed. */
void follow_mix(double speed, double correction, double limit, double *left, double *right);
/* Motor protocol adapter, intentionally absent until the motor protocol is known.
 * send receives bounded normalized actuator commands [-1,1], not raw speed.
 * Implement CAN packing using can_tx_frame(). stop MUST encode the documented
 * motor stop command. Callbacks must be nonblocking and finish within 1 ms;
 * they must not reenter follow APIs. Bind once during startup, before enabling. */
typedef struct {
    int (*send)(app_ctx_t *app, double left, double right);
    int (*stop)(app_ctx_t *app);
} follow_motor_ops_t;
int follow_bind_motor(const follow_motor_ops_t *ops);
/* Motor CAN decoder publishes measured signed wheel speeds in m/s, using the
 * CLOCK_MONOTONIC reception timestamp. Never inject fabricated feedback. */
int follow_feedback(double left_mps, double right_mps, unsigned long long monotonic_ms);
int follow_init(void *app);
void follow_destroy(void *app);
/* Called once for each completed inference; ignores reordered/duplicate frames.
 * Copies the latest result; never waits for the control worker to consume it. */
void follow_submit(const yolo_event_meta_t *frame);
void http_follow_get(app_ctx_t *, int, const char *, const char *, const char *);
void http_follow_post(app_ctx_t *, int, const char *, const char *, const char *);
#endif
