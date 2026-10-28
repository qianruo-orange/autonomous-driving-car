/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <linux/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/can/netlink.h>
#include <netlink/netlink.h>
#include <netlink/socket.h>
#include <netlink/route/link.h>
#include <netlink/route/link/can.h>

#include "can/can_socket.h"
#include "control/car_sport_ctrl.h"
#include <limits.h>
#include <math.h>
#include <time.h>
#include "can/dbc_parser.h"
#include "http/http_api_dbc.h"
#include "http/http_api_can.h"
#include "core/common.h"
#include "core/epoll_util.h"
#include "core/log.h"
#include "watchdog/watchdog.h"

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
    /* These are already-sent frame records, never commands to transmit later. */
    struct { int index; struct canfd_frame frame; } records[32];
    unsigned record_head, record_tail;
} transport;

static int can_write_frame_locked(app_ctx_t *app, int index,
                                  const struct canfd_frame *frame, int fd_frame, int defer_record);
static unsigned long long motor_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000u + t.tv_nsec / 1000000u;
}

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


static struct can_filter ordinary_filter(can_filter_t source)
{
    if ((source.id & CAN_EFF_FLAG) || (source.id & CAN_EFF_MASK) > CAN_SFF_MASK ||
        (source.mask & CAN_EFF_MASK) > CAN_SFF_MASK) {
        source.id |= CAN_EFF_FLAG;
        source.mask |= CAN_EFF_FLAG;
    }
    return (struct can_filter){.can_id=source.id, .can_mask=source.mask};
}

static int ordinary_frame_allowed(const can_iface_t *iface, canid_t id)
{
    if (!iface->filter_count)
        return 1;
    for (int i = 0; i < iface->filter_count; i++) {
        struct can_filter filter = ordinary_filter(iface->filters[i]);
        int equal = (id & filter.can_mask) == (filter.can_id & filter.can_mask);
        if ((filter.can_id & CAN_INV_FILTER) ? !equal : equal)
            return 1;
    }
    return 0;
}

/* Called with app.can_mutex held, including staged replacement sockets. The
 * kernel union keeps feedback visible; ordinary RX still applies its own
 * configured filters in software before publishing/decoding/enqueueing. */
int can_socket_apply_filters(app_ctx_t *app, can_iface_t *iface, int fd)
{
    (void)app;
    if (!iface || fd < 0 || iface->filter_count < 0 || iface->filter_count > CAN_MAX_FILTERS) {
        errno = EINVAL;
        return -1;
    }
    struct can_filter filters[CAN_MAX_FILTERS + 2];
    int count = 0;
    if (!iface->filter_count) {
        filters[count++] = (struct can_filter){0,0};
    } else {
        for (int i = 0; i < iface->filter_count; i++)
            filters[count++] = ordinary_filter(iface->filters[i]);
        pthread_mutex_lock(&motor_lock);
        if (atomic_load(&transport.configured) &&
            !strcmp(iface->ifname, transport.profile.interface_name)) {
            for (int i = 0; i < 2; i++) {
                const motor_field_t *field = &transport.profile.wheels[i].feedback;
                if (i && same_id(field, &transport.profile.wheels[0].feedback))
                    continue;
                filters[count++] = (struct can_filter){
                    .can_id=field->id | (field->extended ? CAN_EFF_FLAG : 0),
                    .can_mask=(field->extended ? CAN_EFF_MASK : CAN_SFF_MASK) |
                               CAN_EFF_FLAG | CAN_RTR_FLAG};
            }
        }
        pthread_mutex_unlock(&motor_lock);
    }
    return setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, filters, count * sizeof(filters[0]));
}

static int motor_index_locked(app_ctx_t *app)
{
    if (!app || !app->can || !app->cfg || !atomic_load(&transport.configured))
        return -1;
    int index = can_iface_index(app->can->ifaces, app->can->count,
                                transport.profile.interface_name);
    if (index < 0 || !app->cfg->args.can_enabled)
        return -1;
    can_iface_t *iface = &app->can->ifaces[index];
    if (iface->sock_fd < 0 || !iface->up)
        return -1;
    for (int i = 0; i < 2; i++)
        if ((transport.profile.wheels[i].command.fd ||
             transport.profile.wheels[i].feedback.fd) && !iface->fd_mode)
            return -1;
    return index;
}

static int motor_command_reserved(const char *ifname, canid_t id)
{
    if (!atomic_load(&transport.configured) ||
        strcmp(ifname, transport.profile.interface_name))
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
    struct canfd_frame keep[CAN_QUEUE_MAX_SLOTS];
    int count = 0, limit = can_queue_count(&iface->txq);
    struct canfd_frame frame;
    while (limit-- > 0 && can_queue_pop(&iface->txq, &frame) == CAN_QUEUE_ERR_OK)
        if (!motor_command_reserved(iface->ifname, frame.can_id))
            keep[count++] = frame;
    for (int i = 0; i < count; i++)
        (void)can_queue_push(&iface->txq, &keep[i]);
}

static int transmit_motor_frame_locked(app_ctx_t *app, int index,
                                       const motor_field_t *field, const unsigned char *bytes)
{
    struct canfd_frame frame = {0};
    frame.can_id = field->id | (field->extended ? CAN_EFF_FLAG : 0);
    frame.len = (unsigned char)field->length;
    frame.flags = field->fd ? CANFD_FDF : 0;
    memcpy(frame.data, bytes, field->length);
    int result = can_write_frame_locked(app, index, &frame, field->fd, 1);
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

static int motor_stop(app_ctx_t *app)
{
    return send_motor_pair(app, 0, 0, 1);
}

/* Existing CAN receiver invokes this while holding app.can_mutex. Feedback
 * publication only takes the controller's feedback lock, never its main lock. */
static void can_motor_receive_locked(app_ctx_t *app, const char *ifname,
                                      const struct canfd_frame *frame, ssize_t bytes)
{
    if (atomic_load(&transport.app) != app || !atomic_load(&transport.configured) ||
        strcmp(ifname, transport.profile.interface_name) ||
        frame->can_id & (CAN_RTR_FLAG | CAN_ERR_FLAG))
        return;
    unsigned long long now = motor_now_ms();
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
        transport.stamp[i] = now;
        updated = 1;
    }
    if (matched)
        atomic_fetch_add(&transport.rx_frames, 1);
    unsigned long long stamp = transport.stamp[0] < transport.stamp[1]
                                  ? transport.stamp[0] : transport.stamp[1];
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

int can_motor_init(app_ctx_t *app)
{
    if (!app || !app->can || !app->cfg)
        return 0;
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
    transport.record_head = transport.record_tail = 0;
    if (configured > 0 && can_iface_index(app->can->ifaces, app->can->count,
                                         profile.interface_name) < 0) {
        configured = -1;
        safe_strncpy(error, sizeof(error), "shared_CAN_interface_not_configured");
    }
    atomic_store(&transport.configured, configured > 0);
    safe_strncpy(transport.error, sizeof(transport.error), error);
    pthread_mutex_unlock(&motor_lock);
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
        follow_motor_ops_t operations = {.send=motor_send, .stop=motor_stop};
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
    transport.record_head = transport.record_tail = 0;
    safe_strncpy(transport.error, sizeof(transport.error), "stopped");
    pthread_mutex_unlock(&motor_lock);
    if (app) {
        for (int i = 0; app->can && i < app->can->count; i++)
            if (app->can->ifaces[i].sock_fd >= 0)
                (void)can_socket_apply_filters(app, &app->can->ifaces[i],
                                               app->can->ifaces[i].sock_fd);
        pthread_mutex_unlock(&app->can_mutex);
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
    safe_strncpy(out->interface_name, sizeof(out->interface_name),transport.profile.interface_name);
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

/* Common writer: wire size depends on the frame format, not sizeof(canfd_frame).
 * Motor calls defer only the DBC bookkeeping to the existing CAN send worker. */
static void record_tx_frame(app_ctx_t *app, int index, const struct canfd_frame *frame)
{
    can_iface_t *iface = &app->can->ifaces[index];
    char name[DBC_MAX_NAME_LEN], text[DBC_DECODE_TEXT_MAX];
    canid_t id = 0;
    if (!dbc_decode_frame(app, index, iface->ifname, frame, &id, name, sizeof(name),
                          text, sizeof(text)))
        http_dbc_record(DBC_DIR_TX, iface->ifname, id, name, text);
}

static int can_write_frame_locked(app_ctx_t *app, int index,
                                  const struct canfd_frame *frame, int fd_frame, int defer_record)
{
    can_iface_t *iface = &app->can->ifaces[index];
    if (frame->len > (fd_frame ? CANFD_MAX_DLEN : CAN_MAX_DLEN) ||
        (fd_frame && !iface->fd_mode)) {
        errno = EMSGSIZE;
        return -1;
    }
    size_t size = fd_frame ? CANFD_MTU : CAN_MTU;
    ssize_t written = send(iface->sock_fd, frame, size, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (written != (ssize_t)size) {
        if (written >= 0)
            errno = EIO;
        return -1;
    }
    if (defer_record) {
        unsigned next = (transport.record_head + 1) % 32;
        if (next != transport.record_tail) {
            transport.records[transport.record_head].index = index;
            transport.records[transport.record_head].frame = *frame;
            transport.record_head = next;
            eventfd_signal(app->can->tx_efd);
        }
    } else {
        record_tx_frame(app, index, frame);
    }
    return 0;
}

static void flush_motor_records_locked(app_ctx_t *app)
{
    while (transport.record_tail != transport.record_head) {
        int index = transport.records[transport.record_tail].index;
        struct canfd_frame frame = transport.records[transport.record_tail].frame;
        transport.record_tail = (transport.record_tail + 1) % 32;
        if (index >= 0 && index < app->can->count)
            record_tx_frame(app, index, &frame);
    }
}

/**
 * ifname_valid - 检查 CAN 接口名是否合法（公共实现见 core/common.h 内联函数）。
 * @name: 待校验的接口名。
 * @return: 合法返回 1，否则返回 0。
 */

/**
 * apply_dbitrate - 使用 netlink 设置 CAN FD 数据位时钟参数，确保接口工作在指定数据速率。
 * @sk: netlink socket。
 * @ifname: 目标 CAN 接口名。
 * @dbitrate: 数据位速率（bps）。
 */
static void apply_dbitrate(struct nl_sock *sk, const char *ifname, uint32_t dbitrate)
{
    struct rtnl_link *live = NULL;
    if (rtnl_link_get_kernel(sk, 0, ifname, &live) != 0 || !live) return;
    struct can_bittiming_const dbtc;
    if (rtnl_link_can_get_data_bittiming_const(live, &dbtc) == 0) {
        struct can_bittiming dbt = { .bitrate = dbitrate };
        dbt.brp = dbtc.brp_min;
        dbt.prop_seg = dbtc.tseg1_min > 1 ? dbtc.tseg1_min - 1 : 1;
        dbt.phase_seg1 = 1;
        dbt.phase_seg2 = dbtc.tseg2_min < 1 ? 1 : dbtc.tseg2_min;
        dbt.sjw = 1;
        dbt.sample_point = 800;
        rtnl_link_can_set_ctrlmode(live, CAN_CTRLMODE_FD);
        rtnl_link_can_set_data_bittiming(live, &dbt);
        rtnl_link_change(sk, live, live, 0);
        LOG_INFO("CAN %s: data bitrate set to %u bps", ifname, dbitrate);
    }
    rtnl_link_put(live);
}

/**
 * can_socket_configure - 配置指定 CAN 设备的比特率、FD 模式和上电状态。
 * @ifname: CAN 接口名。
 * @bitrate: 普通 CAN 比特率。
 * @dbitrate: CAN FD 数据比特率。
 * @fd_mode: 是否启用 CAN FD。
 * @restart_ms: 自动重启等待时间。
 * @bring_up: 是否启动接口。
 * @return: 成功返回 0，失败返回 -1。
 */
int can_socket_configure(const char *ifname, int bitrate, int dbitrate,
                         int fd_mode, int restart_ms, int bring_up)
{
    if (!ifname_valid(ifname)) { LOG_ERROR("invalid CAN interface name: '%s'", ifname?ifname:"(null)"); return -1; }
    if (bitrate <= 0) bitrate = 500000;

    struct nl_sock *sk = nl_socket_alloc();
    if (!sk) { LOG_ERROR("nl_socket_alloc"); return -1; }
    if (nl_connect(sk, NETLINK_ROUTE) < 0) { LOG_ERROR("nl_connect"); nl_socket_free(sk); return -1; }

    /* 用 netlink 探测接口是否已存在；避免 if_nametoindex() 返回 0 后仍走
       rtnl_link_add(NLM_F_CREATE) 而对已存在接口报 "Object busy" */
    struct rtnl_link *existing = NULL;
    int exists = (rtnl_link_get_kernel(sk, 0, ifname, &existing) == 0 && existing != NULL);
    if (existing) rtnl_link_put(existing);

    if (exists) {
        /* 接口已存在，用 ip 命令配置（MCP251xFD 等芯片需要原子操作） */
        char cmd[512];
        size_t off = 0;
        /* 安全追加：缓冲区写满后安全截断，指针/长度均不越界（公共宏见 common.h） */
        BUF_APPEND(cmd, off, "ip link set %s down 2>/dev/null; ip link set %s type can bitrate %d",
               ifname, ifname, bitrate);
        if (fd_mode) BUF_APPEND(cmd, off, " fd on");
        if (fd_mode && dbitrate > 0) BUF_APPEND(cmd, off, " dbitrate %d", dbitrate);
        /* 用 ; 而非 &&：ip link set type can 对已有CAN接口可能返回 EEXIST */
        BUF_APPEND(cmd, off, " 2>/dev/null");
        if (bring_up) BUF_APPEND(cmd, off, "; ip link set %s up 2>/dev/null", ifname);
        int rc = system(cmd);
        if (rc != 0 && bring_up) {
            /* type can 部分失败但 up 可能成功——验证接口是否真的 up */
            unsigned int flags = 0;
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            ifreq_set_name(&ifr, ifname);
            int sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock >= 0) {
                ioctl(sock, SIOCGIFFLAGS, &ifr);
                flags = (unsigned int)ifr.ifr_flags;
                close(sock);
            }
            if (!(flags & IFF_UP)) {
                LOG_ERROR("CAN %s: config via ip command failed", ifname);
                nl_socket_free(sk); return -1;
            }
            LOG_INFO("CAN %s: type can returned EEXIST, interface is up anyway", ifname);
        }
        LOG_INFO("CAN %s configured (existing), %s", ifname, bring_up ? "up" : "down");
        nl_socket_free(sk);
        return 0;
    }

    /* 接口不存在：创建新 link */
    struct rtnl_link *link = rtnl_link_alloc();
    rtnl_link_set_name(link, ifname);
    rtnl_link_set_type(link, "can");

    rtnl_link_can_set_bitrate(link, (uint32_t)bitrate);
    if (fd_mode) rtnl_link_can_set_ctrlmode(link, CAN_CTRLMODE_FD);
    if (restart_ms > 0) rtnl_link_can_set_restart_ms(link, (uint32_t)restart_ms);

    int ret = rtnl_link_add(sk, link, NLM_F_CREATE);
    if (ret < 0) {
        LOG_ERROR("CAN %s: configure failed: %s", ifname, nl_geterror(ret));
        rtnl_link_put(link); nl_socket_free(sk); return -1;
    }

    if (fd_mode && dbitrate > 0)
        apply_dbitrate(sk, ifname, (uint32_t)dbitrate);

    if (bring_up) {
        rtnl_link_set_flags(link, IFF_UP);
        if (rtnl_link_change(sk, link, link, 0) < 0)
            LOG_ERROR("CAN %s: bring up failed", ifname);
    }

    LOG_INFO("CAN %s configured (bitrate=%d%s%s) %s", ifname, bitrate,
             fd_mode ? " FD" : "",
             (fd_mode && dbitrate > 0) ? "+dbitrate" : "",
             bring_up ? "up" : "down");
    rtnl_link_put(link);
    nl_socket_free(sk);
    return 0;
}

/**
 * can_socket_open - 打开一个原始 CAN socket，并绑定到指定接口。
 * @ifname: CAN 接口名。
 * @fd_mode: 是否启用 CAN FD 模式。
 * @return: 成功返回 socket fd，失败返回 -1。
 */
int can_socket_open(const char *ifname, int fd_mode)
{
    if (!ifname_valid(ifname)) { LOG_ERROR("invalid CAN interface name: '%s'", ifname?ifname:"(null)"); return -1; }
    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) { LOG_ERROR("socket(PF_CAN)"); return -1; }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifreq_set_name(&ifr, ifname);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) { LOG_ERROR("ioctl SIOCGIFINDEX for %s", ifname); close(s); return -1; }
    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) { LOG_ERROR("bind CAN %s", ifname); close(s); return -1; }
    if (fd_mode) {
        int enable = 1;
        if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof(enable)) < 0)
            LOG_INFO("CAN_RAW_FD_FRAMES not available on %s, classic CAN only", ifname);
        else LOG_INFO("CAN FD socket enabled on %s", ifname);
    }

    /* 非阻塞：配合 epoll 就绪通知，read/write 在无数据/不可写时返回 EAGAIN 而不是阻塞 */
    if (set_nonblock(s) < 0) {
        LOG_ERROR("CAN %s: fcntl O_NONBLOCK: %s", ifname, strerror(errno));
        close(s);
        return -1;
    }

    LOG_INFO("CAN socket opened on %s", ifname);
    return s;
}

/**
 * can_socket_close - 关闭已打开的 CAN socket fd。
 * @fd: socket 文件描述符。
 */
void can_socket_close(int fd) { if (fd >= 0) close(fd); }

/* ---- TX 队列 + eventfd 跨线程唤醒 ----
   写/读 eventfd 的公共实现见 core/common.h（eventfd_signal / eventfd_consume） */

/**
 * can_epoll_update_tx - 按该接口发送队列是否为空，动态切换 epoll 是否监听 EPOLLOUT。
 * 队列非空才监听 EPOLLOUT，避免 socket 一直可写导致 epoll 忙轮询。
 * @ctx: CAN 上下文。
 * @idx: 接口索引。
 */
static void can_epoll_update_tx(can_ctx_t *ctx, int idx)
{
    int fd = ctx->ifaces[idx].sock_fd;
    if (fd < 0) return;
    int want_out = can_queue_count(&ctx->ifaces[idx].txq) > 0;
    if (want_out) {
        struct epoll_event ev;
        ev.events = EPOLLOUT;
        ev.data.u32 = (uint32_t)(idx + 1);
        if (epoll_ctl(ctx->send_epfd, EPOLL_CTL_MOD, fd, &ev) < 0 && errno == ENOENT)
            epoll_ctl(ctx->send_epfd, EPOLL_CTL_ADD, fd, &ev);
    } else {
        epoll_ctl(ctx->send_epfd, EPOLL_CTL_DEL, fd, NULL);
    }
}

/* 把接口 socket 以 EPOLLIN 注册到接收 epoll（can_recv_task 使用） */
static void can_epoll_add_recv(can_ctx_t *ctx, int idx)
{
    int fd = ctx->ifaces[idx].sock_fd;
    if (fd < 0) return;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = (uint32_t)(idx + 1);
    epoll_ctl(ctx->recv_epfd, EPOLL_CTL_ADD, fd, &ev);
}

/**
 * can_tx_frame - 异步发送一帧 CAN 数据。
 * 压入目标接口 TX 队列，由 can_send_task 排空写 socket 并做发送方向 DBC 解析。
 * @app: 全局应用上下文。
 * @ifname: 目标接口名。
 * @frame: 待发送 CAN(FD) 帧。
 * @return: 入队成功返回 0；失败返回 -1。
 */
int can_tx_frame(app_ctx_t *app, const char *ifname, const struct canfd_frame *frame)
{
    if (!app || !app->can || !ifname || !frame) { errno = EINVAL; return -1; }
    can_ctx_t *ctx = app->can;

    pthread_mutex_lock(&app->can_mutex);
    if (!app->cfg || !app->cfg->args.can_enabled) {
        pthread_mutex_unlock(&app->can_mutex);
        errno = EACCES;
        return -1;
    }
    int idx = can_iface_index(ctx->ifaces, ctx->count, ifname);
    if (idx < 0 || ctx->ifaces[idx].sock_fd < 0 || !ctx->ifaces[idx].up) {
        pthread_mutex_unlock(&app->can_mutex);
        errno = idx < 0 ? ENODEV : ENETDOWN;
        return -1;
    }

    if (motor_command_reserved(ifname, frame->can_id)) {
        pthread_mutex_unlock(&app->can_mutex);
        errno = EBUSY;
        return -1;
    }
    if (frame->len > (ctx->ifaces[idx].fd_mode ? CANFD_MAX_DLEN : CAN_MAX_DLEN)) {
        pthread_mutex_unlock(&app->can_mutex);
        errno = EMSGSIZE;
        return -1;
    }
    /* 发送统一走 TX 队列：由 can_send_task 弹出写 socket 并做发送方向 DBC 解析 */
    can_queue_t *txq = &ctx->ifaces[idx].txq;
    if (can_queue_push(txq, frame) != CAN_QUEUE_ERR_OK) {
        pthread_mutex_unlock(&app->can_mutex);
        errno = ENOBUFS;
        return -1;
    }
    eventfd_signal(ctx->tx_efd);
    pthread_mutex_unlock(&app->can_mutex);
    return 0;
}

/**
 * handle_can_output - 排空各接口发送队列，把待发帧写入对应 socket。
 * @app: 全局应用上下文。
 */
static void handle_can_output(app_ctx_t *app)
{
    can_ctx_t *ctx = app->can;
    pthread_mutex_lock(&app->can_mutex);
    flush_motor_records_locked(app);
    if (!app->cfg->args.can_enabled) {
        pthread_mutex_unlock(&app->can_mutex);
        return;
    }

    for (int idx = 0; idx < ctx->count; idx++) {
        int fd = ctx->ifaces[idx].sock_fd;
        if (fd < 0) continue;

        can_queue_t *q = &ctx->ifaces[idx].txq;
        /* 一轮最多处理当前队列长度，避免 EAGAIN 帧重排入队尾导致死循环 */
        int limit = can_queue_count(q);
        while (limit-- > 0 && can_queue_count(q) > 0) {
            struct canfd_frame f;
            if (can_queue_pop(q, &f) != CAN_QUEUE_ERR_OK) break;

            if (motor_command_reserved(ctx->ifaces[idx].ifname, f.can_id))
                continue;
            int sent = can_write_frame_locked(app, idx, &f, ctx->ifaces[idx].fd_mode, 0);
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (can_queue_push(q, &f) != CAN_QUEUE_ERR_OK)
                    LOG_ERROR("can tx queue full on %s, drop frame",ctx->ifaces[idx].ifname);
                break;
            }
        }
    }

    /* 队列非空才监听 EPOLLOUT，排空则回到仅监听 TX eventfd */
    for (int i = 0; i < ctx->count; i++)
        can_epoll_update_tx(ctx, i);
    pthread_mutex_unlock(&app->can_mutex);
}

/**
 * handle_can_input - 处理单个 CAN 接口上的输入事件，读取帧并在失败时自动重建 socket。
 * @app: 全局应用上下文。
 * @can_idx: CAN 接口索引。
 */
static void handle_can_input(app_ctx_t *app, int can_idx)
{
    can_ctx_t *ctx = app->can;
    pthread_mutex_lock(&app->can_mutex);
    /* An epoll batch may outlive the switch-off that closed these sockets. */
    if (!app->cfg->args.can_enabled || ctx->ifaces[can_idx].sock_fd < 0) {
        pthread_mutex_unlock(&app->can_mutex);
        return;
    }
    int fd = ctx->ifaces[can_idx].sock_fd;
    can_queue_t *rxq = &ctx->ifaces[can_idx].rxq;
    const char *ifname = ctx->ifaces[can_idx].ifname;

    /* 每次 epoll 事件最多排空 64 帧就返回：避免 CAN 总线持续有数据时无限排水，
       导致 can_recv_task 无法回到下一次 epoll_wait 前的 watchdog_feed_self("can_recv")，
       从而被看门狗误判为卡死。 */
    for (int i = 0; i < 64; i++) {
        struct canfd_frame frame = {0};
        ssize_t n = read(fd, &frame, sizeof(frame));
        if (n == CAN_MTU || n == CANFD_MTU) {
            if (frame.len > (n == CAN_MTU ? CAN_MAX_DLEN : CANFD_MAX_DLEN))
                continue;
            if (n == CANFD_MTU)
                frame.flags |= CANFD_FDF;
            can_motor_receive_locked(app, ifname, &frame, n);
            if (!ordinary_frame_allowed(&ctx->ifaces[can_idx], frame.can_id))
                continue;
            /* 记录原始帧到 HTTP 报文缓存（与 DBC 解码队列相互独立，队列满也不影响展示） */
            http_can_record_rx(ifname, &frame);

            /* 入队前完成接收方向 DBC 解析并记录 */
            char name[DBC_MAX_NAME_LEN];
            char text[DBC_DECODE_TEXT_MAX];
            canid_t id = 0;
            if (dbc_decode_frame(app, can_idx, ifname, &frame,
                                 &id, name, sizeof(name), text, sizeof(text)) == 0)
                http_dbc_record(DBC_DIR_RX, ifname, id, name, text);

            /* 解析后入 rxq；接收线程只入队、不出队（由其他线程 pop） */
            if (can_queue_push(rxq, &frame) != CAN_QUEUE_ERR_OK)
                LOG_ERROR("can rx queue full on %s, drop frame", ifname);
        } else if (n > 0) {
            continue; /* Reject truncated/non-CAN datagrams without reading padding. */
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            LOG_ERROR("can read %s: %s", ifname, strerror(errno));
            can_motor_prepare_close_locked(app, ifname);
            can_socket_close(fd);
            ctx->ifaces[can_idx].sock_fd = -1;
            int new_fd = can_socket_open(ifname, ctx->ifaces[can_idx].fd_mode);
            if (new_fd >= 0) {
                ctx->ifaces[can_idx].sock_fd = new_fd;
                if (can_socket_apply_filters(app, &ctx->ifaces[can_idx], new_fd)) {
                    can_socket_close(new_fd);
                    ctx->ifaces[can_idx].sock_fd = -1;
                    break;
                }
                /* 重新注册到接收 epoll；并同步发送 epoll 的 EPOLLOUT 状态 */
                can_epoll_add_recv(ctx, can_idx);
                can_epoll_update_tx(ctx, can_idx);
                LOG_INFO("CAN %s reconnected", ifname);
            }
            break;
        }
    }

    pthread_mutex_unlock(&app->can_mutex);
}

void *can_recv_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    can_ctx_t *ctx = app->can;
    if (!ctx) return NULL;

    LOG_INFO("can_recv_task started (%d iface(s))", ctx->count);
    while (app->running) {
        struct epoll_event events[64];
        int nfds = epoll_wait_feed(ctx->recv_epfd, events, 64, 500, "can_recv");
        if (nfds < 0) break;
        for (int i = 0; i < nfds; i++) {
            uint32_t tag = events[i].data.u32;
            if (tag < 1 || tag > (uint32_t)ctx->count) continue;
            int idx = (int)(tag - 1);
            uint32_t ev = events[i].events;
            if (ev & (EPOLLIN | EPOLLERR | EPOLLHUP))
                handle_can_input(app, idx);
        }
    }
    LOG_INFO("can_recv_task stopped");
    return NULL;
}

void *can_send_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    can_ctx_t *ctx = app->can;
    if (!ctx) return NULL;

    for (int i = 0; i < ctx->count; i++)
        can_epoll_update_tx(ctx, i);
    LOG_INFO("can_send_task started (%d iface(s))", ctx->count);
    while (app->running) {
        struct epoll_event events[64];
        int nfds = epoll_wait_feed(ctx->send_epfd, events, 64, 500, "can_send");
        if (nfds < 0) break;
        for (int i = 0; i < nfds; i++) {
            uint32_t tag = events[i].data.u32;
            if (tag == 0) {
                /* TX eventfd：有帧压入 TX 队列，排空各接口发送队列 */
                eventfd_consume(ctx->tx_efd);
                handle_can_output(app);
                continue;
            }
            if (tag < 1 || tag > (uint32_t)ctx->count) continue;
            uint32_t ev = events[i].events;
            if (ev & EPOLLOUT)
                handle_can_output(app);
        }
    }
    LOG_INFO("can_send_task stopped");
    return NULL;
}

int can_socket_set_filter(int fd, canid_t id, canid_t mask)
{
    struct can_filter filter = ordinary_filter((can_filter_t){.id=id, .mask=mask});
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)) < 0) {
        LOG_ERROR("setsockopt CAN_RAW_FILTER id=%08X mask=%08X", id, mask); return -1;
    }
    LOG_INFO("CAN filter set: id=%08X mask=%08X", id, mask);
    return 0;
}

int can_enumerate_system(char names[][IFNAMSIZ], int max)
{
    int count = 0;
    struct nl_sock *sk = nl_socket_alloc();
    if (!sk) return 0;
    if (nl_connect(sk, NETLINK_ROUTE) < 0) { nl_socket_free(sk); return 0; }

    for (int idx = 1; ; idx++) {
        struct rtnl_link *link = NULL;
        if (rtnl_link_get_kernel(sk, idx, NULL, &link) != 0) break;
        if (!link) break;

        const char *kind = rtnl_link_get_type(link);
        if (!kind || strcmp(kind, "can") != 0) { rtnl_link_put(link); continue; }

        const char *name = rtnl_link_get_name(link);
        if (name && ifname_valid(name) && count < max)
            safe_strncpy(names[count++], IFNAMSIZ, name);
        rtnl_link_put(link);
    }

    nl_socket_free(sk);
    return count;
}

/* 内核 uapi struct can_ioctl_data（内核 >= 3.5）：布局稳定，自行声明避免依赖过旧系统头文件 */
struct local_can_ioctl_data {
    uint32_t index;
    uint32_t flags;
    union {
        struct { uint32_t ctrlmode; uint32_t ctrlmode_supported; };
        struct { uint32_t bitrate; uint32_t dbitrate; };
    };
};

/* 查询指定 CAN 接口是否支持 CAN FD（读内核 can 控制模式支持位）。
 * 通过 ioctl(SIOCGIFINFO_EXT) 获取 ctrlmode_supported，供配置页展示。 */
int can_fd_supported(const char *ifname)
{
    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) return 0;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifreq_set_name(&ifr, ifname);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) { close(s); return 0; }

    struct local_can_ioctl_data data;
    memset(&data, 0, sizeof(data));
    data.index = ifr.ifr_ifindex;
    if (ioctl(s, _IOR('j', 0x0D, struct local_can_ioctl_data), &data) < 0) { close(s); return 0; }

    close(s);
    return (data.ctrlmode_supported & CAN_CTRLMODE_FD) ? 1 : 0;
}

int can_init(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    app_args_t *args = &app->cfg->args;
    can_ctx_t *ctx = app->can;
    memset(ctx, 0, sizeof(*ctx));
    ctx->recv_epfd = -1;
    ctx->send_epfd = -1;
    ctx->tx_efd    = -1;
    ctx->ifaces = args->can_ifaces;
    ctx->count  = args->can_count;
    /* 每个接口各初始化一个发送队列和一个接收队列 */
    for (int i = 0; i < ctx->count; i++) {
        can_queue_init(&ctx->ifaces[i].txq);
        can_queue_init(&ctx->ifaces[i].rxq);
    }
    /* 接收 epoll（can_recv_task，监听各接口 EPOLLIN） */
    ctx->recv_epfd = epoll_create1(0);
    if (ctx->recv_epfd < 0) { LOG_ERROR("can: recv epoll_create1 failed"); return -1; }
    /* 发送 epoll（can_send_task，监听 TX eventfd + 各接口 EPOLLOUT） */
    ctx->send_epfd = epoll_create1(0);
    if (ctx->send_epfd < 0) { LOG_ERROR("can: send epoll_create1 failed"); return -1; }
    /* TX eventfd：跨线程唤醒 can_send_task，让其检测“有帧压入 TX 队列” */
    ctx->tx_efd = eventfd(0, EFD_NONBLOCK);
    if (ctx->tx_efd < 0) { LOG_ERROR("can: eventfd failed"); return -1; }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = 0;
    if (epoll_ctl(ctx->send_epfd, EPOLL_CTL_ADD, ctx->tx_efd, &ev) < 0) {
        LOG_ERROR("can: epoll_ctl(tx_efd) failed"); return -1;
    }

    for (int i = 0; i < args->can_count; i++) {
        can_iface_t *iface = &args->can_ifaces[i];
        iface->sock_fd = -1;
        if (!args->can_enabled) continue;
        if (can_socket_configure(iface->ifname, iface->bitrate, iface->dbitrate,
                                 iface->fd_mode, iface->restart_ms, iface->up) < 0) {
            /* 无 CAN 硬件/驱动未加载时接口无法配置：跳过该接口，不阻塞整体启动，
               之后可通过 Web /api/can/toggle 或重启服务再试 */
            LOG_ERROR("CAN %s: configure failed, skip this interface", iface->ifname);
            continue;
        }
        int fd = can_socket_open(iface->ifname, iface->fd_mode);
        if (fd < 0) {
            LOG_ERROR("CAN %s: open failed, skip this interface", iface->ifname);
            continue;
        }
        iface->sock_fd = fd;
        if (can_socket_apply_filters(app, iface, fd)) {
            can_socket_close(fd);
            iface->sock_fd = -1;
            continue;
        }
        can_epoll_add_recv(ctx, i);
    }

    LOG_INFO("%d CAN interface(s) initialized", args->can_count);
    for (int i = 0; i < args->can_count; i++)
        LOG_INFO("  CAN[%d]=%s filters=%d", i, args->can_ifaces[i].ifname, args->can_ifaces[i].filter_count);
    return 0;
}

void can_cleanup(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    can_ctx_t *ctx = app->can;
    if (!ctx) return;

    can_motor_cleanup();
    for (int i = 0; i < ctx->count; i++) {
        can_socket_close(ctx->ifaces[i].sock_fd);
        ctx->ifaces[i].sock_fd = -1;
        can_queue_destroy(&ctx->ifaces[i].txq);
        can_queue_destroy(&ctx->ifaces[i].rxq);
    }
    if (ctx->tx_efd >= 0) close(ctx->tx_efd);
    if (ctx->recv_epfd >= 0) close(ctx->recv_epfd);
    if (ctx->send_epfd >= 0) close(ctx->send_epfd);
}
