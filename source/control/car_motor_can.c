/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "control/car_motor_can.h"
#include "control/car_sport_ctrl.h"
#include <ctype.h>
#include <limits.h>
#include <linux/can/raw.h>
#include <math.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
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

static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    motor_profile_t profile;
    int fd, wake_fd, online, thread_started;
    pthread_t thread;
    atomic_int running, configured;
    atomic_ullong tx_frames, tx_errors, rx_frames, decode_errors;
    double speed[2];
    unsigned long long stamp[2], published_stamp;
    char error[160];
} transport = {.fd = -1, .wake_fd = -1};

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

static int transmit_frame(int fd, const motor_field_t *field, const unsigned char *bytes)
{
    struct canfd_frame frame = {0};
    frame.can_id = field->id | (field->extended ? CAN_EFF_FLAG : 0);
    frame.len = (unsigned char)field->length;
    frame.flags = field->fd ? CANFD_FDF : 0;
    memcpy(frame.data, bytes, field->length);
    size_t size = field->fd ? CANFD_MTU : CAN_MTU;
    ssize_t written = send(fd, &frame, size, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (written != (ssize_t)size) {
        atomic_fetch_add(&transport.tx_errors, 1);
        if (written >= 0)
            errno = EIO;
        return -1;
    }
    atomic_fetch_add(&transport.tx_frames, 1);
    return 0;
}

static int send_motor_pair(double left, double right, int stop)
{
    if (!atomic_load(&transport.configured)) {
        errno = ENODEV;
        return -1;
    }
    if (!stop && (!isfinite(left) || !isfinite(right) || fabs(left) > 1 || fabs(right) > 1)) {
        errno = EINVAL;
        return -1;
    }
    if (pthread_mutex_trylock(&io_lock)) {
        atomic_fetch_add(&transport.tx_errors, 1);
        errno = EAGAIN;
        return -1;
    }
    if (transport.fd < 0 || !transport.online) {
        pthread_mutex_unlock(&io_lock);
        atomic_fetch_add(&transport.tx_errors, 1);
        errno = ENETDOWN;
        return -1;
    }
    const motor_field_t *a = &transport.profile.wheels[0].command;
    const motor_field_t *b = &transport.profile.wheels[1].command;
    unsigned char bytes[CANFD_MAX_DLEN];
    memcpy(bytes, stop ? a->stop : a->bytes, a->length);
    if (!stop)
        encode_command(a, left, bytes);
    int combined = same_id(a, b);
    if (combined && !stop)
        encode_command(b, right, bytes);
    int result = transmit_frame(transport.fd, a, bytes);
    if (!combined && !result) {
        memcpy(bytes, stop ? b->stop : b->bytes, b->length);
        if (!stop)
            encode_command(b, right, bytes);
        result = transmit_frame(transport.fd, b, bytes);
    }
    if (result)
        safe_strncpy(transport.error, sizeof(transport.error), "motor_send_failed");
    else
        transport.error[0] = 0;
    pthread_mutex_unlock(&io_lock);
    return result;
}

static int motor_send(app_ctx_t *app, double left, double right)
{
    (void)app;
    return send_motor_pair(left, right, 0);
}
static int motor_stop(app_ctx_t *app)
{
    (void)app;
    return send_motor_pair(0, 0, 1);
}

static int open_bus(const motor_profile_t *profile)
{
    int fd = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
    if (fd < 0)
        return -1;
    struct ifreq request = {0};
    safe_strncpy(request.ifr_name, sizeof(request.ifr_name), profile->interface_name);
    if (ioctl(fd, SIOCGIFINDEX, &request))
        goto fail;
    int index = request.ifr_ifindex;
    if (ioctl(fd, SIOCGIFFLAGS, &request))
        goto fail;
    if (!(request.ifr_flags & IFF_UP)) {
        errno = ENETDOWN;
        goto fail;
    }
    int needs_fd = 0;
    for (int i = 0; i < 2; i++)
        needs_fd |= profile->wheels[i].command.fd | profile->wheels[i].feedback.fd;
    if (needs_fd) {
        if (ioctl(fd, SIOCGIFMTU, &request))
            goto fail;
        if (request.ifr_mtu != CANFD_MTU) {
            errno = EOPNOTSUPP;
            goto fail;
        }
        int enable = 1;
        if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof(enable)))
            goto fail;
    }
    struct can_filter filters[2];
    int filter_count = 0;
    for (int i = 0; i < 2; i++) {
        const motor_field_t *field = &profile->wheels[i].feedback;
        if (i && same_id(field, &profile->wheels[0].feedback))
            continue;
        filters[filter_count++] =
            (struct can_filter){.can_id = field->id | (field->extended ? CAN_EFF_FLAG : 0),
                                .can_mask = (field->extended ? CAN_EFF_MASK : CAN_SFF_MASK) |
                                            CAN_EFF_FLAG | CAN_RTR_FLAG};
    }
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, filters, filter_count * sizeof(filters[0])))
        goto fail;
    int receive_own = 0;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &receive_own, sizeof(receive_own)))
        goto fail;
    /* Keep kernel buffering small; no application command queue is maintained. */
    int send_buffer = 2 * CANFD_MTU;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)))
        goto fail;
    struct sockaddr_can address = {.can_family = AF_CAN, .can_ifindex = index};
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)))
        goto fail;
    return fd;
fail:
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
}

/* RX owns feedback pairing; it calls the controller only after releasing io_lock. */
static void receive_frame(const struct canfd_frame *frame, ssize_t bytes, unsigned long long now)
{
    if (bytes != CAN_MTU && bytes != CANFD_MTU) {
        atomic_fetch_add(&transport.decode_errors, 1);
        return;
    }
    if (frame->can_id & (CAN_RTR_FLAG | CAN_ERR_FLAG))
        return;
    atomic_fetch_add(&transport.rx_frames, 1);
    int updated = 0;
    pthread_mutex_lock(&io_lock);
    for (int i = 0; i < 2; i++) {
        const motor_field_t *field = &transport.profile.wheels[i].feedback;
        canid_t expected = field->id | (field->extended ? CAN_EFF_FLAG : 0);
        if (frame->can_id != expected || (bytes == CANFD_MTU) != field->fd ||
            frame->len != field->length)
            continue;
        double speed = decode_feedback(field, frame->data);
        if (!isfinite(speed) || fabs(speed) > 20) {
            atomic_fetch_add(&transport.decode_errors, 1);
            continue;
        }
        transport.speed[i] = speed;
        transport.stamp[i] = now;
        updated = 1;
    }
    if (!updated)
        atomic_fetch_add(&transport.decode_errors, 1);
    unsigned long long stamp =
        transport.stamp[0] < transport.stamp[1] ? transport.stamp[0] : transport.stamp[1];
    double left = transport.speed[0], right = transport.speed[1];
    int publish = updated && stamp && now >= stamp &&
                  now - stamp <= (unsigned)transport.profile.pair_timeout_ms &&
                  stamp > transport.published_stamp;
    if (publish)
        transport.published_stamp = stamp;
    pthread_mutex_unlock(&io_lock);
    if (publish && follow_feedback(left, right, stamp))
        atomic_fetch_add(&transport.decode_errors, 1);
}

static void disconnect_bus(const char *reason)
{
    pthread_mutex_lock(&io_lock);
    if (transport.fd >= 0)
        close(transport.fd);
    transport.fd = -1;
    transport.online = 0;
    transport.stamp[0] = transport.stamp[1] = 0;
    safe_strncpy(transport.error, sizeof(transport.error), reason);
    pthread_mutex_unlock(&io_lock);
}

static void *receive_worker(void *unused)
{
    (void)unused;
    pthread_setname_np(pthread_self(), "motor_can_rx");
    unsigned long long reopen_after = 0;
    while (atomic_load(&transport.running)) {
        pthread_mutex_lock(&io_lock);
        int fd = transport.fd;
        pthread_mutex_unlock(&io_lock);
        unsigned long long now = motor_now_ms();
        if (fd < 0 && now >= reopen_after) {
            fd = open_bus(&transport.profile);
            reopen_after = motor_now_ms() + 500;
            pthread_mutex_lock(&io_lock);
            transport.fd = fd;
            transport.online = fd >= 0;
            safe_strncpy(transport.error, sizeof(transport.error),
                         fd >= 0 ? "" : "CAN_interface_unavailable");
            pthread_mutex_unlock(&io_lock);
        }
        struct pollfd waiters[2] = {{.fd = transport.wake_fd, .events = POLLIN},
                                    {.fd = fd, .events = POLLIN}};
        now = motor_now_ms();
        int retry_wait = reopen_after > now ? (int)(reopen_after - now) : 1;
        int ready = poll(waiters, 2, fd < 0 ? retry_wait : 100);
        if (!atomic_load(&transport.running))
            break;
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            disconnect_bus("motor_poll_failed");
            reopen_after = motor_now_ms() + 500;
            continue;
        }
        if (waiters[0].revents & POLLIN) {
            uint64_t value;
            (void)read(transport.wake_fd, &value, sizeof(value));
        }
        if (fd < 0)
            continue;
        if (waiters[1].revents & POLLIN) {
            for (int i = 0; i < 32; i++) {
                struct canfd_frame frame = {0};
                ssize_t count = recv(fd, &frame, sizeof(frame), MSG_DONTWAIT);
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    break;
                if (count <= 0) {
                    disconnect_bus("motor_receive_failed");
                    reopen_after = motor_now_ms() + 500;
                    break;
                }
                receive_frame(&frame, count, motor_now_ms());
            }
        }
        if (waiters[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            disconnect_bus("motor_CAN_link_error");
            reopen_after = motor_now_ms() + 500;
        }
    }
    disconnect_bus("stopped");
    return NULL;
}

int car_motor_can_init(app_ctx_t *app)
{
    if (atomic_load(&transport.running))
        return 0;
    motor_profile_t profile;
    char error[160] = {0};
    int configured = load_profile(CAR_MOTOR_CAN_CONFIG, &profile, error, sizeof(error));
    pthread_mutex_lock(&io_lock);
    transport.profile = profile;
    transport.fd = transport.wake_fd = -1;
    transport.online = transport.thread_started = 0;
    transport.stamp[0] = transport.stamp[1] = transport.published_stamp = 0;
    atomic_store(&transport.configured, 0);
    atomic_store(&transport.tx_frames, 0);
    atomic_store(&transport.tx_errors, 0);
    atomic_store(&transport.rx_frames, 0);
    atomic_store(&transport.decode_errors, 0);
    safe_strncpy(transport.error, sizeof(transport.error), error);
    pthread_mutex_unlock(&io_lock);
    if (configured <= 0)
        return 0;
    transport.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (transport.wake_fd < 0) {
        disconnect_bus("motor_eventfd_failed");
        return 0;
    }
    atomic_store(&transport.running, 1);
    atomic_store(&transport.configured, 1);
    int rc = pthread_create(&transport.thread, NULL, receive_worker, NULL);
    if (rc) {
        atomic_store(&transport.running, 0);
        atomic_store(&transport.configured, 0);
        close(transport.wake_fd);
        transport.wake_fd = -1;
        disconnect_bus("motor_thread_failed");
        return 0;
    }
    transport.thread_started = 1;
    follow_motor_ops_t operations = {.send = motor_send, .stop = motor_stop};
    if (follow_bind_motor(&operations)) {
        car_motor_can_destroy();
        pthread_mutex_lock(&io_lock);
        safe_strncpy(transport.error, sizeof(transport.error), "motor_binding_failed");
        pthread_mutex_unlock(&io_lock);
    }
    (void)app;
    return 0;
}

void car_motor_can_destroy(void)
{
    atomic_store(&transport.running, 0);
    if (transport.thread_started) {
        uint64_t value = 1;
        (void)write(transport.wake_fd, &value, sizeof(value));
        pthread_join(transport.thread, NULL);
        transport.thread_started = 0;
    }
    pthread_mutex_lock(&io_lock);
    if (transport.fd >= 0)
        close(transport.fd);
    if (transport.wake_fd >= 0)
        close(transport.wake_fd);
    transport.fd = transport.wake_fd = -1;
    transport.online = 0;
    atomic_store(&transport.configured, 0);
    safe_strncpy(transport.error, sizeof(transport.error), "stopped");
    pthread_mutex_unlock(&io_lock);
}

void car_motor_can_get_status(car_motor_can_status_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&io_lock);
    out->configured = atomic_load(&transport.configured);
    out->online = transport.online;
    out->tx_frames = atomic_load(&transport.tx_frames);
    out->tx_errors = atomic_load(&transport.tx_errors);
    out->rx_frames = atomic_load(&transport.rx_frames);
    out->decode_errors = atomic_load(&transport.decode_errors);
    safe_strncpy(out->interface_name, sizeof(out->interface_name),
                 transport.profile.interface_name);
    safe_strncpy(out->error, sizeof(out->error),
                 transport.error[0] ? transport.error
                 : !out->configured ? "not_configured"
                                    : "");
    pthread_mutex_unlock(&io_lock);
}
