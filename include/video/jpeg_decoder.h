/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef JPEG_DECODER_H
#define JPEG_DECODER_H

#include <stddef.h>

typedef struct jpeg_decoder jpeg_decoder_t;

/* One owner, one V4L2 context. No decoder threads or software fallback. */
jpeg_decoder_t *jpeg_decoder_open(void);
/* 选择解码通路：hardware=1 → V4L2 硬解(rockchip-jpegd)，=0 → libjpeg-turbo 软解。
   必须在 jpeg_decoder_open() 之前调用（yolo 模块初始化时按 config 的
   jpeg_decode 项设置）。硬解设备打不开时 open() 自动回落软解。 */
void jpeg_decoder_set_mode(int hardware);
void jpeg_decoder_close(jpeg_decoder_t *decoder);
/* 解码并留在 NV12 dma-buf 里，供 RGA 硬件 letterbox 直接读取。
   返回的 fd 属于解码器内部环形缓冲，调用方可安全使用到该槽被再次轮转
   （环深 8，远大于在途帧数）。失败返回 -1。 */
/* copy_to 非 NULL 时，解码完成立刻把 NV12 拷进这块内存（调用方保证
   width*height*3/2 字节）。推理路径传 NULL，不产生额外拷贝。 */
int jpeg_decoder_nv12(jpeg_decoder_t *decoder, const unsigned char *jpeg, size_t length,
                      int width, int height, int *nv12_fd, unsigned char *copy_to);
int jpeg_decoder_available(void);
void jpeg_decoder_stats(unsigned long long *frames, unsigned long long *errors);

#endif
