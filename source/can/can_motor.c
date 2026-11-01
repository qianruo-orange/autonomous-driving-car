/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "can/can_motor.h"
#include "can/can_socket.h"
#include "control/car_sport_ctrl.h"
#include "core/log.h"
#include <ctype.h>
#include <limits.h>
#include <linux/can/raw.h>
#include <math.h>
#include <sys/socket.h>
#include <time.h>
#ifndef CANFD_FDF
#define CANFD_FDF 0x04
#endif

enum {
    CMD_ID,
    CMD_EXT,
    CMD_FD,
    CMD_LENGTH,
    CMD_TEMPLATE,
    CMD_OFFSET,
    CMD_WIDTH,
    CMD_ENDIAN,
    CMD_SCALE,
    CMD_BIAS,
    STOP_TEMPLATE,
    FB_ID,
    FB_EXT,
    FB_FD,
    FB_LENGTH,
    FB_OFFSET,
    FB_WIDTH,
    FB_ENDIAN,
    FB_SCALE,
    FIELD_COUNT
};

static const char *field_names[FIELD_COUNT] = {
    "cmd_id",        "cmd_extended", "cmd_fd",         "cmd_length", "cmd_template",
    "cmd_offset",    "cmd_width",    "cmd_big_endian", "cmd_scale",  "cmd_bias",
    "stop_template", "fb_id",        "fb_extended",    "fb_fd",      "fb_length",
    "fb_offset",     "fb_width",     "fb_big_endian",  "fb_scale"};

typedef struct {
    canid_t id;
    int extended, fd, length, offset, width, big_endian;
    double scale, bias;
    unsigned char bytes[CANFD_MAX_DLEN], stop[CANFD_MAX_DLEN];
    int bytes_count, stop_count;
} motor_field_t;

typedef struct {
    motor_field_t command, feedback;
    unsigned int seen;
} motor_wheel_t;

typedef struct {
    int enabled, pair_timeout_ms;
    char interface_name[IFNAMSIZ];
    motor_wheel_t wheels[2];
} motor_profile_t;

/* Motor payload mapping is part of the common CAN service. It owns neither a
 * socket nor a receiver thread. Lock order is app.can_mutex -> motor_lock.
 * Control callbacks try both locks and never wait behind ordinary CAN work. */
static pthread_mutex_t motor_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    motor_profile_t profile;
    _Atomic(app_ctx_t *) app;
    atomic_int configured;
    atomic_ullong tx_frames, tx_errors, rx_frames, decode_errors;
    double speed[2];
    unsigned long long stamp[2], published_stamp;
    char error[160];
} transport;

/* 电机业务独占此槽；左右轮反馈 ID 可以绑定同一实例，传输层只投递帧。
 * 控制线程在 PID 更新前消费它，保留 CAN 层采集时间用于新鲜度检查。 */
static can_slot_t motor_rx;
static int motor_slot_ready;
static struct {
    canid_t id;
    uint64_t handle;
} motor_bindings[4];
static int motor_binding_count;

static int profile_error(char *error, size_t capacity, const char *message)
{
    safe_strncpy(error, capacity, message);
    return -1;
}

static int parse_integer(const char *text, unsigned long long maximum, unsigned long long *value)
{
    if (!text || !*text || *text == '-')
        return -1;
    char *end;
    errno = 0;
    unsigned long long n = strtoull(text, &end, 0);
    if (errno || end == text || *end || n > maximum)
        return -1;
    *value = n;
    return 0;
}

static int parse_real(const char *text, double *value)
{
    char *end;
    errno = 0;
    double n = strtod(text, &end);
    if (errno || end == text || *end || !isfinite(n))
        return -1;
    *value = n;
    return 0;
}

static int parse_hex(const char *text, unsigned char *bytes)
{
    int count = 0, high = -1;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (isspace(*p))
            continue;
        int nibble = *p >= '0' && *p <= '9'   ? *p - '0'
                     : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10
                     : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10
                                              : -1;
        if (nibble < 0)
            return -1;
        if (high < 0)
            high = nibble;
        else {
            if (count >= CANFD_MAX_DLEN)
                return -1;
            bytes[count++] = (unsigned char)((high << 4) | nibble);
            high = -1;
        }
    }
    return high >= 0 || !count ? -1 : count;
}

static int legal_length(int length, int fd)
{
    if (length >= 1 && length <= CAN_MAX_DLEN)
        return 1;
    return fd && (length == 12 || length == 16 || length == 20 || length == 24 || length == 32 ||
                  length == 48 || length == 64);
}

static int field_valid(const motor_field_t *field, int command)
{
    if (field->id > (field->extended ? CAN_EFF_MASK : CAN_SFF_MASK) ||
        !legal_length(field->length, field->fd) || (field->width != 16 && field->width != 32) ||
        field->offset < 0 || field->offset + field->width / 8 > field->length ||
        !isfinite(field->scale) || field->scale == 0 || !isfinite(field->bias))
        return 0;
    if (!command)
        return fabs(field->scale) <= 100;
    double low = field->width == 16 ? INT16_MIN : INT32_MIN;
    double high = field->width == 16 ? INT16_MAX : INT32_MAX;
    return field->bytes_count == field->length && field->stop_count == field->length &&
           field->bias - fabs(field->scale) >= low && field->bias + fabs(field->scale) <= high;
}

static int same_id(const motor_field_t *a, const motor_field_t *b)
{
    return a->id == b->id && a->extended == b->extended;
}

static int fields_overlap(const motor_field_t *a, const motor_field_t *b)
{
    return a->offset < b->offset + b->width / 8 && b->offset < a->offset + a->width / 8;
}

static int compatible_pair(const motor_field_t *a, const motor_field_t *b, int command)
{
    if (!same_id(a, b))
        return 1;
    if (a->fd != b->fd || a->length != b->length || fields_overlap(a, b))
        return 0;
    return !command ||
           (!memcmp(a->bytes, b->bytes, a->length) && !memcmp(a->stop, b->stop, a->length));
}

static int assign_wheel_field(motor_wheel_t *wheel, int key, const char *value)
{
    motor_field_t *field = key <= STOP_TEMPLATE ? &wheel->command : &wheel->feedback;
    unsigned long long integer;
    switch (key) {
    case CMD_TEMPLATE:
        field->bytes_count = parse_hex(value, field->bytes);
        return field->bytes_count < 0 ? -1 : 0;
    case STOP_TEMPLATE:
        field->stop_count = parse_hex(value, field->stop);
        return field->stop_count < 0 ? -1 : 0;
    case CMD_SCALE:
    case FB_SCALE:
        return parse_real(value, &field->scale);
    case CMD_BIAS:
        return parse_real(value, &field->bias);
    default:
        break;
    }
    unsigned long long maximum = key == CMD_ID || key == FB_ID ? CAN_EFF_MASK
                                 : key == CMD_EXT || key == CMD_FD || key == CMD_ENDIAN ||
                                         key == FB_EXT || key == FB_FD || key == FB_ENDIAN
                                     ? 1
                                     : CANFD_MAX_DLEN;
    if (parse_integer(value, maximum, &integer))
        return -1;
    switch (key) {
    case CMD_ID:
    case FB_ID:
        field->id = (canid_t)integer;
        break;
    case CMD_EXT:
    case FB_EXT:
        field->extended = (int)integer;
        break;
    case CMD_FD:
    case FB_FD:
        field->fd = (int)integer;
        break;
    case CMD_LENGTH:
    case FB_LENGTH:
        field->length = (int)integer;
        break;
    case CMD_OFFSET:
    case FB_OFFSET:
        field->offset = (int)integer;
        break;
    case CMD_WIDTH:
    case FB_WIDTH:
        field->width = (int)integer;
        break;
    case CMD_ENDIAN:
    case FB_ENDIAN:
        field->big_endian = (int)integer;
        break;
    default:
        return -1;
    }
    return 0;
}

static int load_profile(const char *path, motor_profile_t *profile, char *error, size_t capacity)
{
    memset(profile, 0, sizeof(*profile));
    profile->pair_timeout_ms = 100;
    FILE *file = fopen(path, "r");
    if (!file) {
        if (errno == ENOENT) {
            safe_strncpy(error, capacity, "not_configured");
            return 0;
        }
        return profile_error(error, capacity, "cannot_read_motor_profile");
    }
    char line[512];
    unsigned globals = 0;
    int failed = 0;
    while (!failed && fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n') && !feof(file)) {
            failed = 1;
            break;
        }
        char *comment = strchr(line, '#');
        if (comment)
            *comment = 0;
        char *start = line;
        while (isspace((unsigned char)*start))
            start++;
        char *end = start + strlen(start);
        while (end > start && isspace((unsigned char)end[-1]))
            *--end = 0;
        if (!*start)
            continue;
        char *separator = start;
        while (*separator && *separator != '=' && !isspace((unsigned char)*separator))
            separator++;
        if (!*separator) {
            failed = 1;
            break;
        }
        *separator++ = 0;
        while (*separator == '=' || isspace((unsigned char)*separator))
            separator++;
        if (!*separator) {
            failed = 1;
            break;
        }
        unsigned long long integer;
        unsigned global = !strcmp(start, "enabled")                    ? 1
                          : !strcmp(start, "interface")                ? 2
                          : !strcmp(start, "feedback_pair_timeout_ms") ? 4
                                                                       : 0;
        if (global) {
            if (globals & global) {
                failed = 1;
                break;
            }
            globals |= global;
            if (global == 1) {
                if (parse_integer(separator, 1, &integer))
                    failed = 1;
                else
                    profile->enabled = (int)integer;
            } else if (global == 2) {
                if (!ifname_valid(separator))
                    failed = 1;
                else
                    safe_strncpy(profile->interface_name, sizeof(profile->interface_name),
                                 separator);
            } else {
                if (parse_integer(separator, 1000, &integer) || integer < 10)
                    failed = 1;
                else
                    profile->pair_timeout_ms = (int)integer;
            }
            continue;
        }
        int side = !strncmp(start, "left.", 5) ? 0 : !strncmp(start, "right.", 6) ? 1 : -1;
        if (side < 0) {
            failed = 1;
            break;
        }
        const char *name = start + (side ? 6 : 5);
        int key = -1;
        for (int i = 0; i < FIELD_COUNT; i++)
            if (!strcmp(name, field_names[i])) {
                key = i;
                break;
            }
        if (key < 0 || (profile->wheels[side].seen & (1u << key)) ||
            assign_wheel_field(&profile->wheels[side], key, separator)) {
            failed = 1;
            break;
        }
        profile->wheels[side].seen |= 1u << key;
    }
    if (ferror(file))
        failed = 1;
    fclose(file);
    if (failed || !(globals & 1))
        return profile_error(error, capacity, "invalid_or_duplicate_motor_profile_key");
    if (!profile->enabled) {
        safe_strncpy(error, capacity, "disabled");
        return 0;
    }
    unsigned required = ((1u << FIELD_COUNT) - 1) & ~(1u << CMD_BIAS);
    for (int side = 0; side < 2; side++) {
        motor_wheel_t *wheel = &profile->wheels[side];
        if ((wheel->seen & required) != required || !field_valid(&wheel->command, 1) ||
            !field_valid(&wheel->feedback, 0))
            return profile_error(error, capacity, "incomplete_or_invalid_motor_field");
    }
    if (!(globals & 2) ||
        !compatible_pair(&profile->wheels[0].command, &profile->wheels[1].command, 1) ||
        !compatible_pair(&profile->wheels[0].feedback, &profile->wheels[1].feedback, 0))
        return profile_error(error, capacity, "incompatible_or_overlapping_motor_fields");
    error[0] = 0;
    return 1;
}

static void encode_command(const motor_field_t *field, double command, unsigned char *bytes)
{
    int64_t signed_value = llround(command * field->scale + field->bias);
    uint32_t value = (uint32_t)signed_value;
    int count = field->width / 8;
    for (int i = 0; i < count; i++) {
        int shift = (field->big_endian ? count - 1 - i : i) * 8;
        bytes[field->offset + i] = (unsigned char)(value >> shift);
    }
}

static double decode_feedback(const motor_field_t *field, const unsigned char *bytes)
{
    uint64_t value = 0;
    int count = field->width / 8;
    for (int i = 0; i < count; i++) {
        int shift = (field->big_endian ? count - 1 - i : i) * 8;
        value |= (uint64_t)bytes[field->offset + i] << shift;
    }
    int64_t signed_value = value & (1ull << (field->width - 1))
                               ? (int64_t)value - (int64_t)(1ull << field->width)
                               : (int64_t)value;
    return signed_value * field->scale;
}

static int motor_index_locked(app_ctx_t *app)
{
    if (!app || !app->can || !app->cfg || !atomic_load(&transport.configured))
        return -1;
    int index =
        can_iface_index(app->can->ifaces, app->can->count, transport.profile.interface_name);
    if (index < 0 || !app->cfg->args.can_enabled)
        return -1;
    can_iface_t *iface = &app->can->ifaces[index];
    if (iface->sock_fd < 0 || !iface->up)
        return -1;
    for (int i = 0; i < 2; i++)
        if ((transport.profile.wheels[i].command.fd || transport.profile.wheels[i].feedback.fd) &&
            !iface->fd_mode)
            return -1;
    return index;
}

static int motor_command_reserved(const char *ifname, canid_t id)
{
    if (!atomic_load(&transport.configured) || strcmp(ifname, transport.profile.interface_name))
        return 0;
    for (int i = 0; i < 2; i++) {
        const motor_field_t *field = &transport.profile.wheels[i].command;
        if ((id & (CAN_EFF_MASK | CAN_EFF_FLAG)) ==
            (field->id | (field->extended ? CAN_EFF_FLAG : 0)))
            return 1;
    }
    return 0;
}

static void discard_queued_motor_commands_locked(can_iface_t *iface)
{
    /* Preserve unrelated manual CAN messages in their original order. */
    struct canfd_frame keep[CAN_TX_SLOT_CAPACITY];
    int count = 0, limit = data_slot_count(&iface->txq);
    struct canfd_frame frame;
    while (limit-- > 0 && data_slot_pop(&iface->txq, &frame))
        if (!motor_command_reserved(iface->ifname, frame.can_id))
            keep[count++] = frame;
    for (int i = 0; i < count; i++)
        (void)data_slot_push(&iface->txq, &keep[i], 0);
}

static int transmit_motor_frame_locked(app_ctx_t *app, int index, const motor_field_t *field,
                                       const unsigned char *bytes)
{
    (void)app;
    (void)index;
    canid_t id = field->id | (field->extended ? CAN_EFF_FLAG : 0);
    int result = -1;
    for (int i = 0; i < motor_binding_count; i++)
        if (motor_bindings[i].id == id) {
            result = can_send_locked(motor_bindings[i].handle, bytes, field->length);
            break;
        }
    atomic_fetch_add(result ? &transport.tx_errors : &transport.tx_frames, 1);
    return result;
}

static int send_motor_pair_locked(app_ctx_t *app, double left, double right, int stop)
{
    int index = motor_index_locked(app);
    if (index < 0) {
        atomic_fetch_add(&transport.tx_errors, 1);
        safe_strncpy(transport.error, sizeof(transport.error), "shared_CAN_interface_unavailable");
        errno = ENETDOWN;
        return -1;
    }
    if (stop)
        discard_queued_motor_commands_locked(&app->can->ifaces[index]);
    const motor_field_t *a = &transport.profile.wheels[0].command;
    const motor_field_t *b = &transport.profile.wheels[1].command;
    unsigned char bytes[CANFD_MAX_DLEN];
    memcpy(bytes, stop ? a->stop : a->bytes, a->length);
    if (!stop)
        encode_command(a, left, bytes);
    int combined = same_id(a, b);
    if (combined && !stop)
        encode_command(b, right, bytes);
    int result = transmit_motor_frame_locked(app, index, a, bytes);
    if (!combined && !result) {
        memcpy(bytes, stop ? b->stop : b->bytes, b->length);
        if (!stop)
            encode_command(b, right, bytes);
        result = transmit_motor_frame_locked(app, index, b, bytes);
    }
    safe_strncpy(transport.error, sizeof(transport.error), result ? "motor_send_failed" : "");
    return result;
}

static int send_motor_pair(app_ctx_t *app, double left, double right, int stop)
{
    if (!app || atomic_load(&transport.app) != app || !atomic_load(&transport.configured)) {
        errno = ENODEV;
        return -1;
    }
    if (!stop && (!isfinite(left) || !isfinite(right) || fabs(left) > 1 || fabs(right) > 1)) {
        errno = EINVAL;
        return -1;
    }
    if (pthread_mutex_trylock(&app->can_mutex)) {
        atomic_fetch_add(&transport.tx_errors, 1);
        errno = EAGAIN;
        return -1;
    }
    if (pthread_mutex_trylock(&motor_lock)) {
        pthread_mutex_unlock(&app->can_mutex);
        atomic_fetch_add(&transport.tx_errors, 1);
        errno = EAGAIN;
        return -1;
    }
    int result = send_motor_pair_locked(app, left, right, stop);
    pthread_mutex_unlock(&motor_lock);
    pthread_mutex_unlock(&app->can_mutex);
    return result;
}

static int motor_send(app_ctx_t *app, double left, double right)
{
    return send_motor_pair(app, left, right, 0);
}

static int motor_stop(app_ctx_t *app) { return send_motor_pair(app, 0, 0, 1); }

/* Existing CAN receiver invokes this while holding app.can_mutex. Feedback
 * publication only takes the controller's feedback lock, never its main lock. */
static void can_motor_receive_locked(app_ctx_t *app, const char *ifname,
                                     const struct canfd_frame *frame, ssize_t bytes,
                                     uint64_t received_ms)
{
    if (atomic_load(&transport.app) != app || !atomic_load(&transport.configured) ||
        strcmp(ifname, transport.profile.interface_name) ||
        frame->can_id & (CAN_RTR_FLAG | CAN_ERR_FLAG))
        return;
    unsigned long long now = monotonic_ms();
    if (!received_ms || received_ms > now)
        return;
    int matched = 0, updated = 0;
    pthread_mutex_lock(&motor_lock);
    for (int i = 0; i < 2; i++) {
        const motor_field_t *field = &transport.profile.wheels[i].feedback;
        canid_t expected = field->id | (field->extended ? CAN_EFF_FLAG : 0);
        if (frame->can_id != expected)
            continue;
        matched = 1;
        if ((bytes == CANFD_MTU) != field->fd || frame->len != field->length) {
            atomic_fetch_add(&transport.decode_errors, 1);
            continue;
        }
        double speed = decode_feedback(field, frame->data);
        if (!isfinite(speed) || fabs(speed) > 20) {
            atomic_fetch_add(&transport.decode_errors, 1);
            continue;
        }
        transport.speed[i] = speed;
        transport.stamp[i] = received_ms;
        updated = 1;
    }
    if (matched)
        atomic_fetch_add(&transport.rx_frames, 1);
    unsigned long long stamp =
        transport.stamp[0] < transport.stamp[1] ? transport.stamp[0] : transport.stamp[1];
    double left = transport.speed[0], right = transport.speed[1];
    int publish = updated && stamp && now >= stamp &&
                  now - stamp <= (unsigned)transport.profile.pair_timeout_ms &&
                  stamp > transport.published_stamp;
    if (publish)
        transport.published_stamp = stamp;
    pthread_mutex_unlock(&motor_lock);
    if (publish && follow_feedback(left, right, stamp))
        atomic_fetch_add(&transport.decode_errors, 1);
}

/* Before closing/downing/reconfiguring a shared interface, try its explicit
 * stop once and invalidate all old measurements. No command is queued. */
void can_motor_prepare_close_locked(app_ctx_t *app, const char *ifname)
{
    int affected = 0;
    pthread_mutex_lock(&motor_lock);
    if (atomic_load(&transport.app) == app && atomic_load(&transport.configured) &&
        (!ifname || !strcmp(ifname, transport.profile.interface_name))) {
        (void)send_motor_pair_locked(app, 0, 0, 1);
        transport.stamp[0] = transport.stamp[1] = transport.published_stamp = 0;
        affected = 1;
    }
    pthread_mutex_unlock(&motor_lock);
    if (affected)
        follow_invalidate_feedback();
}

/* The controller consumes measured feedback slots before each control update. */
void can_motor_poll(app_ctx_t *app)
{
    if (!motor_slot_ready)
        return;
    can_data_t item;
    pthread_mutex_lock(&app->can_mutex);
    for (int n = 0; n < CAN_SLOT_CAPACITY && data_slot_pop(&motor_rx.queue, &item); n++)
        can_motor_receive_locked(app, transport.profile.interface_name, &item.frame,
                                 (item.frame.flags & CANFD_FDF) ? CANFD_MTU : CAN_MTU,
                                 item.received_ms);
    pthread_mutex_unlock(&app->can_mutex);
}
/* 把命令/反馈 ID 转为句柄；同一帧包含双轮字段时合并为一次注册。
 * 任一注册失败立即回收已注册项，不能让半套协议进入运动输出。 */
static int motor_locked_register(app_ctx_t *app, const motor_profile_t *profile)
{
    can_binding_t specs[4] = {0};
    int count = 0;
    for (int kind = 0; kind < 2; kind++)
        for (int side = 0; side < 2; side++) {
            const motor_field_t *field =
                kind ? &profile->wheels[side].feedback : &profile->wheels[side].command;
            int port_index =
                can_iface_index(app->can->ifaces, app->can->count, profile->interface_name);
            if (port_index < 0 || (field->fd && !app->can->ifaces[port_index].fd_mode))
                return -1;
            canid_t id = field->id | (field->extended ? CAN_EFF_FLAG : 0);
            int i;
            for (i = 0; i < count; i++)
                if (specs[i].id == id)
                    break;
            if (i == count) {
                specs[i].port = profile->interface_name;
                specs[i].id = id;
                specs[i].fd = field->fd;
                count++;
            }
            specs[i].flags |= kind ? CAN_BIND_RX : (CAN_BIND_TX | CAN_BIND_RESERVED);
            if (kind)
                specs[i].slot = &motor_rx;
            specs[i].before_close = can_motor_prepare_close_locked;
        }
    for (int i = 0; i < count; i++) {
        uint64_t handle = can_locked_register(&specs[i]);
        if (!handle) {
            while (motor_binding_count)
                can_locked_unregister(motor_bindings[--motor_binding_count].handle);
            return -1;
        }
        motor_bindings[motor_binding_count].id = specs[i].id;
        motor_bindings[motor_binding_count++].handle = handle;
    }
    return 0;
}

int can_motor_init(app_ctx_t *app)
{
    if (!app || !app->can || !app->cfg)
        return 0;
    if (can_slot_init(&motor_rx))
        return -1;
    motor_slot_ready = 1;
    motor_binding_count = 0;
    motor_profile_t profile;
    char error[160] = {0};
    int configured = load_profile(CAN_MOTOR_CONFIG, &profile, error, sizeof(error));
    pthread_mutex_lock(&app->can_mutex);
    pthread_mutex_lock(&motor_lock);
    transport.profile = profile;
    atomic_store(&transport.app, app);
    atomic_store(&transport.configured, 0);
    atomic_store(&transport.tx_frames, 0);
    atomic_store(&transport.tx_errors, 0);
    atomic_store(&transport.rx_frames, 0);
    atomic_store(&transport.decode_errors, 0);
    transport.stamp[0] = transport.stamp[1] = transport.published_stamp = 0;
    if (app && app->can)
        app->can->record_head = app->can->record_tail = 0;
    if (configured > 0 &&
        can_iface_index(app->can->ifaces, app->can->count, profile.interface_name) < 0) {
        configured = -1;
        safe_strncpy(error, sizeof(error), "shared_CAN_interface_not_configured");
    }
    atomic_store(&transport.configured, configured > 0);
    safe_strncpy(transport.error, sizeof(transport.error), error);
    pthread_mutex_unlock(&motor_lock);
    if (configured > 0 && motor_locked_register(app, &profile)) {
        pthread_mutex_lock(&motor_lock);
        atomic_store(&transport.configured, 0);
        safe_strncpy(transport.error, sizeof(transport.error), "CAN_interface_service_conflict");
        pthread_mutex_unlock(&motor_lock);
        configured = -1;
    }
    if (configured > 0) {
        int index = can_iface_index(app->can->ifaces, app->can->count, profile.interface_name);
        can_iface_t *iface = &app->can->ifaces[index];
        discard_queued_motor_commands_locked(iface);
        if (iface->sock_fd >= 0 && can_socket_apply_filters(app, iface, iface->sock_fd)) {
            pthread_mutex_lock(&motor_lock);
            atomic_store(&transport.configured, 0);
            safe_strncpy(transport.error, sizeof(transport.error), "motor_feedback_filter_failed");
            pthread_mutex_unlock(&motor_lock);
            configured = -1;
        }
    }
    pthread_mutex_unlock(&app->can_mutex);
    if (configured > 0) {
        follow_motor_ops_t operations = {.send = motor_send, .stop = motor_stop};
        if (follow_bind_motor(&operations)) {
            can_motor_cleanup();
            pthread_mutex_lock(&motor_lock);
            safe_strncpy(transport.error, sizeof(transport.error), "motor_binding_failed");
            pthread_mutex_unlock(&motor_lock);
        }
    }
    return 0;
}

void can_motor_cleanup(void)
{
    app_ctx_t *app = atomic_load(&transport.app);
    if (app)
        pthread_mutex_lock(&app->can_mutex);
    pthread_mutex_lock(&motor_lock);
    atomic_store(&transport.configured, 0);
    atomic_store(&transport.app, NULL);
    transport.stamp[0] = transport.stamp[1] = transport.published_stamp = 0;
    if (app && app->can)
        app->can->record_head = app->can->record_tail = 0;
    safe_strncpy(transport.error, sizeof(transport.error), "stopped");
    pthread_mutex_unlock(&motor_lock);
    if (app) {
        while (motor_binding_count)
            can_locked_unregister(motor_bindings[--motor_binding_count].handle);
        for (int i = 0; app->can && i < app->can->count; i++)
            if (app->can->ifaces[i].sock_fd >= 0)
                (void)can_socket_apply_filters(app, &app->can->ifaces[i],
                                               app->can->ifaces[i].sock_fd);
        pthread_mutex_unlock(&app->can_mutex);
    }
    if (motor_slot_ready) {
        data_slot_destroy(&motor_rx.queue);
        motor_slot_ready = 0;
    }
}

void can_motor_get_status(can_motor_status_t *out)
{
    if (!out)
        return;
    app_ctx_t *app = atomic_load(&transport.app);
    if (app)
        pthread_mutex_lock(&app->can_mutex);
    pthread_mutex_lock(&motor_lock);
    memset(out, 0, sizeof(*out));
    out->configured = atomic_load(&transport.configured);
    out->online = app && motor_index_locked(app) >= 0;
    out->tx_frames = atomic_load(&transport.tx_frames);
    out->tx_errors = atomic_load(&transport.tx_errors);
    out->rx_frames = atomic_load(&transport.rx_frames);
    out->decode_errors = atomic_load(&transport.decode_errors);
    safe_strncpy(out->interface_name, sizeof(out->interface_name),
                 transport.profile.interface_name);
    const char *error = transport.error;
    if (!*error && out->configured && !out->online)
        error = "shared_CAN_interface_unavailable";
    if (!*error && !out->configured)
        error = "not_configured";
    safe_strncpy(out->error, sizeof(out->error), error);
    pthread_mutex_unlock(&motor_lock);
    if (app)
        pthread_mutex_unlock(&app->can_mutex);
}
