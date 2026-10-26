#include <cassert>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
extern "C" {
#include "ai/yolo_draw.h"
#include "ai/yolo_image.h"
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
    return 0;
}
