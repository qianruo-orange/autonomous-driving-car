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
/* Motor protocol adapter: a validated car_motor_can.conf binds the built-in
 * shared CAN service; custom drivers can bind through the same interface.
 * send receives bounded normalized actuator commands [-1,1], not raw speed.
 * Built-in mapping sends bounded batches through the shared CAN socket without
 * queueing stale commands. stop MUST encode the documented motor stop command.
 * Custom callbacks must provide the same guarantees and finish within 1 ms;
 * they must not reenter follow APIs. Bind once during startup, before enabling. */
typedef struct {
    int (*send)(app_ctx_t *app, double left, double right);
    int (*stop)(app_ctx_t *app);
} follow_motor_ops_t;
int follow_bind_motor(const follow_motor_ops_t *ops);
/* Motor CAN decoder publishes measured signed wheel speeds in m/s, using the
 * CLOCK_MONOTONIC reception timestamp. Never inject fabricated feedback. */
int follow_feedback(double left_mps, double right_mps, unsigned long long monotonic_ms);
/* Read-only, nonblocking snapshot for telemetry; available between init/destroy.
 * Steering is half the left/right speed difference in m/s, not a measured angle.
 * Missing or stale real wheel feedback is represented by NaN actual values. */
typedef struct {
    double speed_target, speed_actual, steering_target, steering_actual;
    unsigned long long control_seq, feedback_ms;
    unsigned long long feedback_valid_until_ms;
    int feedback_valid;
} follow_telemetry_t;
int follow_telemetry_snapshot(follow_telemetry_t *out);
/* Shared CAN close/reconfigure invalidates measured feedback without taking the control lock. */
void follow_invalidate_feedback(void);
int follow_init(void *app);
void follow_destroy(void *app);
void *follow_task(void *app);
/* Called once for each completed inference; ignores reordered/duplicate frames.
 * Copies the latest result; never waits for the control worker to consume it. */
void follow_submit(const yolo_event_meta_t *frame);

/* ---- External agent motion source ----------------------------------------
 * A command is a bounded (v, steer) pair carried by a short lease. The control
 * worker consumes it at its own rate, so the caller may be slow, but it MUST
 * refresh the lease before ttl_ms elapses or the vehicle stops. steer reuses
 * max_turn_delta's unit: half the left/right wheel-speed difference in m/s,
 * positive meaning the left wheel runs faster. Both values are re-clamped
 * against hard caps here, independent of any caller-side validation.
 * The source is only usable while the follow profile is enabled, and any
 * interlock or lease failure latches a fault that requires follow_agent_reset.
 */
enum { AGENT_MOTION_IDLE = 0, AGENT_MOTION_ACTIVE = 1, AGENT_MOTION_FAULT = 2 };

typedef struct {
    int state;                        /* AGENT_MOTION_* */
    double v, steer;                  /* last accepted command */
    double traveled_m;                /* integrated |v|*dt over the armed session */
    unsigned long long expires_in_ms; /* 0 when no command is outstanding */
    unsigned long long accepted;      /* accepted command count since arming */
    int enabled;                      /* follow profile (operator master switch) */
    int motor_bound;                  /* actuator adapter present */
    int can_enabled;                  /* CAN transport up */
    int actuation_ready;              /* every interlock the next tick will check */
    char fault[32];                   /* latched fault reason, empty when none */
    char reason[24];                  /* last control-loop reason string */
} agent_motion_t;

int follow_agent_cmd(double v, double steer, unsigned ttl_ms);
void follow_agent_cancel(void);
void follow_agent_reset(void);
int follow_agent_snapshot(agent_motion_t *out);
int follow_profile_enabled(void);
/* Operator-level stop: clears the enable flag and demands the documented stop
 * sequence. Used by the capability layer when a credential is revoked. */
int follow_force_disable(const char *reason);
void http_follow_get(app_ctx_t *, int, const char *, const char *, const char *);
void http_follow_post(app_ctx_t *, int, const char *, const char *, const char *);
#endif
