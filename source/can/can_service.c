/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "can/can_internal.h"
#define CAN_SERVICE_MAX 8
static struct {
    char ifname[IFNAMSIZ];
    const can_service_ops_t *ops;
    void *user;
} services[CAN_SERVICE_MAX];
static int service_count;
int can_service_register_locked(app_ctx_t *app, const char *ifname, const can_service_ops_t *ops,
                                void *user)
{
    if (!app || !app->can || !ifname_valid(ifname) || !ops || !ops->frame) {
        errno = EINVAL;
        return -1;
    }
    if (can_iface_index(app->can->ifaces, app->can->count, ifname) < 0) {
        errno = ENODEV;
        return -1;
    }
    for (int i = 0; i < service_count; i++)
        if (!strcmp(services[i].ifname, ifname)) {
            errno = EEXIST;
            return -1;
        }
    if (service_count == CAN_SERVICE_MAX) {
        errno = ENOSPC;
        return -1;
    }
    safe_strncpy(services[service_count].ifname, IFNAMSIZ, ifname);
    services[service_count].ops = ops;
    services[service_count++].user = user;
    return 0;
}
int can_service_register(app_ctx_t *app, const char *ifname, const can_service_ops_t *ops,
                         void *user)
{
    if (!app || !app->can) {
        errno = ENODEV;
        return -1;
    }
    pthread_mutex_lock(&app->can_mutex);
    int result = can_service_register_locked(app, ifname, ops, user);
    /* Apply added feedback filters immediately for an already-open interface. */
    if (!result) {
        int index = can_iface_index(app->can->ifaces, app->can->count, ifname);
        can_iface_t *iface = &app->can->ifaces[index];
        if (iface->sock_fd >= 0 && can_socket_apply_filters(app, iface, iface->sock_fd)) {
            can_service_unregister_locked(app, ifname, ops, user);
            result = -1;
        }
    }
    pthread_mutex_unlock(&app->can_mutex);
    return result;
}
int can_service_unregister_locked(app_ctx_t *app, const char *ifname, const can_service_ops_t *ops,
                                  void *user)
{
    (void)app;
    if (!ifname) {
        errno = EINVAL;
        return -1;
    }
    for (int i = 0; i < service_count; i++)
        if (!strcmp(services[i].ifname, ifname) && services[i].ops == ops &&
            services[i].user == user) {
            memmove(&services[i], &services[i + 1], (service_count - i - 1) * sizeof(services[0]));
            service_count--;
            return 0;
        }
    errno = ENOENT;
    return -1;
}
int can_service_unregister(app_ctx_t *app, const char *ifname, const can_service_ops_t *ops,
                           void *user)
{
    if (!app || !app->can) {
        errno = ENODEV;
        return -1;
    }
    pthread_mutex_lock(&app->can_mutex);
    int result = can_service_unregister_locked(app, ifname, ops, user);
    if (!result) {
        int index = can_iface_index(app->can->ifaces, app->can->count, ifname);
        if (index >= 0 && app->can->ifaces[index].sock_fd >= 0)
            result = can_socket_apply_filters(app, &app->can->ifaces[index],
                                              app->can->ifaces[index].sock_fd);
    }
    pthread_mutex_unlock(&app->can_mutex);
    return result;
}
void can_services_clear_locked(void) { service_count = 0; }
void can_service_dispatch_locked(app_ctx_t *app, const char *ifname,
                                 const struct canfd_frame *frame, can_frame_direction_t direction,
                                 int ordinary)
{
    for (int i = 0; i < service_count; i++)
        if (!strcmp(services[i].ifname, ifname))
            if (ordinary || services[i].ops->receive_filtered)
                services[i].ops->frame(app, ifname, frame, direction, services[i].user);
}
void can_service_prepare_close_locked(app_ctx_t *app, const char *ifname)
{
    for (int i = 0; i < service_count; i++)
        if ((!ifname || !strcmp(services[i].ifname, ifname)) && services[i].ops->before_close)
            services[i].ops->before_close(app, services[i].ifname, services[i].user);
}
int can_service_reserved_locked(const char *ifname, canid_t id)
{
    for (int i = 0; i < service_count; i++)
        if (!strcmp(services[i].ifname, ifname) && services[i].ops->reserved_id)
            return services[i].ops->reserved_id(ifname, id, services[i].user);
    return 0;
}
int can_service_filters_locked(const char *ifname, struct can_filter *out, size_t capacity)
{
    for (int i = 0; i < service_count; i++)
        if (!strcmp(services[i].ifname, ifname) && services[i].ops->extra_filters)
            return services[i].ops->extra_filters(ifname, out, capacity, services[i].user);
    return 0;
}
