#include <cassert>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
extern "C" {
#include "yolo/yolo_draw.h"
#include "yolo/yolo_image.h"
#include "yolo/yolo_postprocess.h"
}

static void check_original_coordinates(int sw, int sh, bool nchw, int anchors = 16)
{
    const int dw = 640, dh = 640, channels = 6;
    std::vector<unsigned char> input((size_t)sw * sh * 3, 17), canvas(dw * dh * 3);
    float scale_x = 0, scale_y = 0;
    int px = -1, py = -1;
    yolo_rgb_letterbox(input.data(), sw, sh, canvas.data(), dw, dh, &scale_x, &scale_y, &px, &py);
    assert(scale_x > 0 && scale_y > 0 && px >= 0 && py >= 0);
    int nw = (int)std::lround(scale_x * sw), nh = (int)std::lround(scale_y * sh);
    assert(canvas[((size_t)py * dw + px) * 3] == 17);
    assert(canvas[((size_t)(py + nh - 1) * dw + px + nw - 1) * 3] == 17);
    if (py) assert(canvas[((size_t)(py - 1) * dw + px) * 3] == 114);
    if (px) assert(canvas[((size_t)py * dw + px - 1) * 3] == 114);
    // A nonintegral resized dimension has its own actual ratio, rather than
    // borrowing the other axis's ratio and shifting original-image positions.
    if (sw == 1280 && sh == 721) assert(std::fabs(scale_x - scale_y) > .0001f);

    std::vector<float> output((size_t)anchors * channels, 0);
    auto set = [&](int anchor, int channel, float value) {
        output[nchw ? (size_t)channel * anchors + anchor : (size_t)anchor * channels + channel] = value;
    };
    auto box = [&](int anchor, float x1, float y1, float x2, float y2, int cls, float score) {
        float mx1 = x1 * scale_x + px, mx2 = x2 * scale_x + px;
        float my1 = y1 * scale_y + py, my2 = y2 * scale_y + py;
        set(anchor, 0, (mx1 + mx2) / 2); set(anchor, 1, (my1 + my2) / 2);
        set(anchor, 2, mx2 - mx1); set(anchor, 3, my2 - my1); set(anchor, 4 + cls, score);
    };
    box(0, sw * .25f, sh * .2f, sw * .6f, sh * .6f, 0, .95f);
    box(1, -25, -15, sw * .1f, sh * .15f, 1, .9f);
    box(2, -40, -50, -1, -2, 1, .85f); // Entirely outside the original image.
    box(3, sw - 120.f, sh - 150.f, sw + 20.f, sh + 25.f, 1, .8f);
    box(4, 50, 50, 100, 100, 0, .75f);
    set(4, 0, std::numeric_limits<float>::quiet_NaN());
    rknn_tensor_attr attr = {};
    attr.n_dims = 3; attr.dims[0] = 1;
    attr.dims[1] = nchw ? channels : anchors; attr.dims[2] = nchw ? anchors : channels;
    attr.n_elems = channels * anchors;
    const float *buffers[] = {output.data()};
    yolo_det_t detections[YOLO_MAX_DETS] = {};
    int classes = 0;
    int count = yolo_postprocess(buffers, &attr, 1, scale_x, scale_y, px, py,
                                sw, sh, detections, YOLO_MAX_DETS, .45f, .45f, &classes);
    assert(count == 3 && classes == 2);
    auto close = [](float actual, float expected) { assert(std::fabs(actual - expected) < .001f); };
    close(detections[0].x1, sw * .25f); close(detections[0].x2, sw * .6f);
    close(detections[0].y1, sh * .2f); close(detections[0].y2, sh * .6f);
    close(detections[1].x1, 0); close(detections[1].y1, 0);
    close(detections[1].x2, sw * .1f); close(detections[1].y2, sh * .15f);
    close(detections[2].x1, sw - 120.f); close(detections[2].y1, sh - 150.f);
    close(detections[2].x2, sw - 1.f); close(detections[2].y2, sh - 1.f);
    for (int i = 0; i < count; ++i) {
        assert(detections[i].x1 >= 0 && detections[i].y1 >= 0);
        assert(detections[i].x2 <= sw - 1 && detections[i].y2 <= sh - 1);
        assert(detections[i].x1 < detections[i].x2 && detections[i].y1 < detections[i].y2);
    }
    assert(yolo_postprocess(buffers, &attr, 1, 0, scale_y, px, py,
                           sw, sh, detections, YOLO_MAX_DETS, .45f, .45f, nullptr) == 0);
    assert(yolo_postprocess(buffers, &attr, 1, scale_x, std::numeric_limits<float>::quiet_NaN(), px, py,
                           sw, sh, detections, YOLO_MAX_DETS, .45f, .45f, nullptr) == 0);
}

int main()
{
    const int sizes[][2] = {{16,16},{1280,720},{32,24},{1280,720}};
    for (const auto &size : sizes) {
        const int w = size[0], h = size[1];
        std::vector<unsigned char> rgb(w*h*3), actual(w*h*3/2), expected(w*h*3/2);
        for (size_t i=0; i<rgb.size(); i++) rgb[i] = (unsigned char)((i*37+i/11)%256);
        cv::Mat input(h,w,CV_8UC3,rgb.data()), i420;
        cv::cvtColor(input,i420,cv::COLOR_RGB2YUV_I420);
        std::copy(i420.data,i420.data+w*h,expected.begin());
        const unsigned char *u=i420.data+w*h, *v=u+w*h/4;
        for (int i=0; i<w*h/4; i++) {
            expected[w*h+2*i]=u[i]; expected[w*h+2*i+1]=v[i];
        }
        for (int repeat=0; repeat<3; repeat++) {
            yolo_rgb_to_nv12_fast(rgb.data(),w,h,actual.data());
            assert(actual==expected);
        }
    }
    puts("PASS: reused I420 scratch preserves exact NV12 colors across frames and resolution changes");
    for (const auto &size : {std::pair<int,int>{1280,720}, {1280,721}, {721,1280}, {1920,1080}}) {
        check_original_coordinates(size.first, size.second, true);
        check_original_coordinates(size.first, size.second, false);
    }
    check_original_coordinates(1280, 720, true, 16385); // NCHW large-output fallback.
    puts("PASS: actual letterbox geometry restores original coordinates, clips image edges and rejects padding-only boxes");
    return 0;
}
