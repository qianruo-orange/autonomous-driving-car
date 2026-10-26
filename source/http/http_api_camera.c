/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Live camera controls. Changing focus never reopens the capture stream. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_AVCODEC
#include <libavcodec/avcodec.h>
#endif
#include "http/http_internal.h"
#include "video/h264_encoder.h"

typedef struct {
    int supported, value, min, max, step, read_only, active;
} camera_control_t;

typedef struct {
    int has_auto, autofocus, has_focus, focus;
} camera_focus_request_t;

static pthread_mutex_t camera_lock = PTHREAD_MUTEX_INITIALIZER;

static int camera_device_valid(const char *device)
{
    if (!device || strncmp(device, "/dev/video", 10) || !device[10]) return 0;
    for (const char *p = device + 10; *p; ++p)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

static int camera_open(app_ctx_t *app)
{
    if (!app || !app->cfg || !camera_device_valid(app->cfg->video_device)) return -1;
    int fd = open(app->cfg->video_device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct v4l2_capability cap = {0};
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { close(fd); return -1; }
    unsigned int caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE))) {
        close(fd); return -1;
    }
    return fd;
}

static camera_control_t camera_read_control(int fd, unsigned int id)
{
    camera_control_t result = {0};
    struct v4l2_queryctrl query = {.id = id};
    struct v4l2_control value = {.id = id};
    if (ioctl(fd, VIDIOC_QUERYCTRL, &query) < 0 ||
        (query.flags & V4L2_CTRL_FLAG_DISABLED) || ioctl(fd, VIDIOC_G_CTRL, &value) < 0)
        return result;
    result.supported = 1;
    result.value = value.value;
    result.min = query.minimum;
    result.max = query.maximum;
    result.step = query.step > 0 ? query.step : 1;
    result.read_only = !!(query.flags & V4L2_CTRL_FLAG_READ_ONLY);
    result.active = !(query.flags & V4L2_CTRL_FLAG_INACTIVE);
    return result;
}

/* Strictly parse this small form before touching any device control. */
static int camera_parse_integer(const char *text, int *value)
{
    if (!text || !*text) return -1;
    const char *digits = text + (*text == '-');
    if (!*digits) return -1;
    for (const char *p = digits; *p; ++p) if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end;
    long parsed = strtol(text, &end, 10);
    if (errno || *end || parsed < INT_MIN || parsed > INT_MAX) return -1;
    *value = (int)parsed;
    return 0;
}

static int camera_parse_request(const char *req, camera_focus_request_t *out)
{
    const char *body = http_body_start(req);
    if (!body || !*body || strlen(body) >= 128) return -1;
    char copy[128];
    strcpy(copy, body);
    memset(out, 0, sizeof(*out));
    char *segment = copy;
    while (segment && *segment) {
        char *next = strchr(segment, '&');
        if (next) { *next++ = 0; if (!*next) return -1; }
        if (!strncmp(segment, "auto=", 5)) {
            if (out->has_auto || camera_parse_integer(segment + 5, &out->autofocus) ||
                (out->autofocus != 0 && out->autofocus != 1)) return -1;
            out->has_auto = 1;
        } else if (!strncmp(segment, "focus=", 6)) {
            if (out->has_focus || camera_parse_integer(segment + 6, &out->focus)) return -1;
            out->has_focus = 1;
        } else return -1;
        segment = next;
    }
    return out->has_auto || out->has_focus ? 0 : -1;
}

/* 400 = malformed/range, 409 = unavailable or conflicting control state. */
static int camera_validate_request(const camera_focus_request_t *req,
                                   const camera_control_t *automatic,
                                   const camera_control_t *focus)
{
    if (req->has_auto && (!automatic->supported || automatic->read_only || !automatic->active)) return 409;
    if (!req->has_focus) return 0;
    if (!focus->supported || focus->read_only) return 409;
    if (req->focus < focus->min || req->focus > focus->max || focus->step < 1 ||
        ((long long)req->focus - focus->min) % focus->step) return 400;
    int auto_value = req->has_auto ? req->autofocus : automatic->value;
    if (automatic->supported && auto_value) return 409;
    /* An auto-dependent manual control becomes active after auto=0. */
    if (!focus->active && !(automatic->supported && automatic->value && req->has_auto && !auto_value)) return 409;
    return 0;
}

static int camera_set_control(int fd, unsigned int id, int value)
{
    struct v4l2_control control = {.id = id, .value = value};
    return ioctl(fd, VIDIOC_S_CTRL, &control);
}

static int camera_apply_focus(int fd, const camera_focus_request_t *req,
                              const camera_control_t *automatic)
{
    int changed_auto = req->has_auto && req->autofocus != automatic->value;
    if (changed_auto && camera_set_control(fd, V4L2_CID_FOCUS_AUTO, req->autofocus) < 0) return -1;
    if (req->has_focus && camera_set_control(fd, V4L2_CID_FOCUS_ABSOLUTE, req->focus) < 0) {
        if (changed_auto) camera_set_control(fd, V4L2_CID_FOCUS_AUTO, automatic->value);
        return -1;
    }
    return 0;
}

/* Probing is read-only but scans nodes; cache it rather than scan on each focus poll. */
static int camera_hardware_encoder_available(void)
{
    static time_t checked;
    static int available;
    time_t now = time(NULL);
    if (!checked || now - checked >= 60) {
        char device[128];
        available = h264_encoder_probe(device, sizeof(device)) == 0;
#ifdef HAVE_AVCODEC
        if (!available && access("/dev/mpp_service", F_OK) == 0 &&
            avcodec_find_encoder_by_name("h264_rkmpp")) available = 1;
#endif
        checked = now;
    }
    return available;
}

static void camera_send_status(app_ctx_t *app, int client, int device)
{
    camera_control_t automatic = camera_read_control(device, V4L2_CID_FOCUS_AUTO);
    camera_control_t focus = camera_read_control(device, V4L2_CID_FOCUS_ABSOLUTE);
    char json[768];
    int n = snprintf(json, sizeof(json),
        "{\"device\":\"%s\",\"focus_auto\":{\"supported\":%s,\"value\":%d,\"min\":%d,\"max\":%d,\"step\":%d,\"read_only\":%s,\"active\":%s},"
        "\"focus_absolute\":{\"supported\":%s,\"value\":%d,\"min\":%d,\"max\":%d,\"step\":%d,\"read_only\":%s,\"active\":%s},\"hardware_encoder_available\":%s}",
        app->cfg->video_device,
        automatic.supported ? "true" : "false", automatic.value, automatic.min, automatic.max, automatic.step,
        automatic.read_only ? "true" : "false", automatic.active ? "true" : "false",
        focus.supported ? "true" : "false", focus.value, focus.min, focus.max, focus.step,
        focus.read_only ? "true" : "false", focus.active ? "true" : "false",
        camera_hardware_encoder_available() ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(json)) { http_err(client, 500, "Error", "Camera response failed"); return; }
    http_ok_json(client, json, (size_t)n);
}

void http_camera_get(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req)
{
    (void)method; (void)uri; (void)req;
    pthread_mutex_lock(&camera_lock);
    int device = camera_open(app);
    if (device < 0) http_err(fd, 503, "Unavailable", "Camera unavailable");
    else { camera_send_status(app, fd, device); close(device); }
    pthread_mutex_unlock(&camera_lock);
}

void http_camera_post(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req)
{
    (void)method; (void)uri;
    camera_focus_request_t request;
    if (camera_parse_request(req, &request)) { http_err(fd, 400, "Bad Request", "Invalid focus parameters"); return; }
    pthread_mutex_lock(&camera_lock);
    int device = camera_open(app);
    if (device < 0) http_err(fd, 503, "Unavailable", "Camera unavailable");
    else {
        camera_control_t automatic = camera_read_control(device, V4L2_CID_FOCUS_AUTO);
        camera_control_t focus = camera_read_control(device, V4L2_CID_FOCUS_ABSOLUTE);
        int error = camera_validate_request(&request, &automatic, &focus);
        if (error) http_err(fd, error, error == 400 ? "Bad Request" : "Conflict", "Unsupported or invalid focus setting");
        else if (camera_apply_focus(device, &request, &automatic) < 0)
            http_err(fd, 503, "Unavailable", "Camera rejected focus setting");
        else camera_send_status(app, fd, device);
        close(device);
    }
    pthread_mutex_unlock(&camera_lock);
}
