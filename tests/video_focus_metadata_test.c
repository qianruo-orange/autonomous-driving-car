/* Test production autofocus enforcement and per-frame sampling without a camera. */
#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
static int video_test_ioctl(int fd, unsigned long request, ...);
#define ioctl video_test_ioctl
#include "video/video_stream.c"
#undef ioctl

static int automatic_value, writes, gets, unsupported, fail_position, timing_writes;
static int video_test_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if (request == VIDIOC_QUERYCTRL) {
        if (unsupported) return -1;
        ((struct v4l2_queryctrl *)arg)->flags = 0;
        return 0;
    }
    struct v4l2_control *control = arg;
    if (request == VIDIOC_G_CTRL) {
        gets++;
        if (control->id == V4L2_CID_FOCUS_AUTO) control->value = automatic_value;
        else {
            assert(control->id == V4L2_CID_FOCUS_ABSOLUTE);
            if (fail_position) return -1;
            control->value = 525;
        }
        return 0;
    }
    assert(request == VIDIOC_S_CTRL);
    if (control->id == V4L2_CID_EXPOSURE_AUTO_PRIORITY) {
        assert(control->value == 0);
        timing_writes++;
        return 0;
    }
    assert(control->id == V4L2_CID_FOCUS_AUTO);
    assert(control->value == 1); /* The capture path must never request manual focus. */
    writes++;
    automatic_value = control->value;
    return 0;
}

/* Only the error branch references logging; satisfy it in the standalone test. */
void _log_error(const char *file, int line, const char *format, ...)
{ (void)file; (void)line; (void)format; }

int main(void)
{
    video_ctx_t context = {.fd = 42};
    video_focus_init(&context);
    assert(writes == 1 && automatic_value == 1);
    assert(context.focus_auto_supported && context.focus_absolute_supported);
    video_capture_timing_init(&context);
    assert(timing_writes == 1 && automatic_value == 1);
    video_focus_sample_t sample;
    video_focus_sample(&context, &sample);
    assert(sample.valid && sample.position == 525 && sample.autofocus == 1);
    assert(sample.sample_ms > 0 && writes == 1);
    automatic_value = 0;
    video_focus_sample(&context, &sample);
    assert(sample.autofocus == 0 && writes == 1); /* Recovery is throttled to one second. */
    context.focus_auto_check_ms = video_monotonic_ms() - 1001;
    video_focus_sample(&context, &sample);
    assert(sample.autofocus == 1 && writes == 2);
    fail_position = 1;
    video_focus_sample(&context, &sample);
    assert(!sample.valid && sample.position == 0 && sample.autofocus == 1);
    unsupported = 1;
    video_focus_init(&context);
    video_capture_timing_init(&context);
    assert(timing_writes == 1);
    int previous_gets = gets;
    video_focus_sample(&context, &sample);
    assert(!sample.valid && sample.autofocus == -1 && gets == previous_gets);
    puts("PASS: continuous autofocus is enabled/restored; lens samples retain validity and timestamps");
    return 0;
}
