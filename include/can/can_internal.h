/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "can/can_socket.h"
/* Internal hooks: caller holds app.can_mutex; do not use as public send APIs. */
struct can_filter ordinary_filter(can_filter_t);
int ordinary_frame_allowed(const can_iface_t *, canid_t);
int can_write_frame_locked(app_ctx_t *, int, const struct canfd_frame *, int, int);
void can_services_clear_locked(void);
void can_service_dispatch_locked(app_ctx_t *, const char *, const struct canfd_frame *,
                                 can_frame_direction_t, int ordinary);
int can_service_reserved_locked(const char *, canid_t);
int can_service_filters_locked(const char *, struct can_filter *, size_t);
void can_observe_rx_locked(app_ctx_t *, int, const struct canfd_frame *);
void can_observe_tx_locked(app_ctx_t *, int, const struct canfd_frame *);

/* Same operation for module initialization already holding can_mutex. */
int can_service_register_locked(app_ctx_t *, const char *, const can_service_ops_t *, void *);
int can_service_unregister_locked(app_ctx_t *, const char *, const can_service_ops_t *, void *);
