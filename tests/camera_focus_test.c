/* Exercise production validation and rollback without camera hardware. */
#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
static int camera_test_ioctl(int fd, unsigned long request, ...);
#define ioctl camera_test_ioctl
#include "http/http_api_camera.c"
#undef ioctl

static int writes, fail_focus, auto_value, focus_value;
static int camera_test_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    assert(request == VIDIOC_S_CTRL);
    va_list args;
    va_start(args, request);
    struct v4l2_control *ctrl = va_arg(args, struct v4l2_control *);
    va_end(args);
    writes++;
    if (ctrl->id == V4L2_CID_FOCUS_AUTO) auto_value = ctrl->value;
    else { assert(ctrl->id == V4L2_CID_FOCUS_ABSOLUTE); if (fail_focus) return -1; focus_value = ctrl->value; }
    return 0;
}

int main(void)
{
    camera_focus_request_t request;
    camera_control_t automatic = {.supported=1, .active=1, .value=1};
    camera_control_t focus = {.supported=1, .active=0, .value=220, .min=0, .max=1023, .step=1};
    const char *invalid[] = {"", "auto=2", "auto=-1", "auto=0&auto=1", "focus=1x", "focus=2147483648", "auto=0&", "auto=0&&focus=220", "bogus=0", "focus= 1", "focus=", "focus=1&focus=2"};
    for (size_t i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) assert(camera_parse_request(invalid[i], &request) < 0);
    assert(camera_device_valid("/dev/video1"));
    assert(!camera_device_valid("/dev/video"));
    assert(!camera_device_valid("/dev/video1/../mem"));
    assert(!camera_device_valid("/etc/passwd"));
    assert(camera_parse_request("auto=0&focus=220", &request) == 0);
    assert(camera_validate_request(&request, &automatic, &focus) == 0);
    auto_value = 1;
    assert(camera_apply_focus(9, &request, &automatic) == 0);
    assert(writes == 2 && auto_value == 0 && focus_value == 220);
    fail_focus = 1; writes = 0; auto_value = 1;
    assert(camera_apply_focus(9, &request, &automatic) < 0);
    assert(writes == 3 && auto_value == 1); /* Restores autofocus if focus fails. */
    request.focus = 1024;
    assert(camera_validate_request(&request, &automatic, &focus) == 400);
    request.focus = 221; focus.step = 2;
    assert(camera_validate_request(&request, &automatic, &focus) == 400);
    request.focus = 220; request.autofocus = 1;
    assert(camera_validate_request(&request, &automatic, &focus) == 409);
    request.autofocus = 0; focus.read_only = 1;
    assert(camera_validate_request(&request, &automatic, &focus) == 409);
    focus.read_only = 0; automatic.read_only = 1;
    assert(camera_validate_request(&request, &automatic, &focus) == 409);
    automatic.read_only = 0; request.has_auto = 0;
    assert(camera_validate_request(&request, &automatic, &focus) == 409);
    automatic.value = 0; focus.active = 1;
    assert(camera_validate_request(&request, &automatic, &focus) == 0);
    puts("PASS: focus validation, hardware bounds, manual/auto conflict, and write rollback");
    return 0;
}
