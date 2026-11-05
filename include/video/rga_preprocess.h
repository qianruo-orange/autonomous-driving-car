#ifndef RGA_PREPROCESS_H
#define RGA_PREPROCESS_H

#include <stddef.h>

/* RGA 硬件预处理：把解码器输出的 NV12 直接缩放进模型输入的 letterbox 画布。
   等价于旧的 yolo_nv12_to_rgb_full() + yolo_rgb_letterbox()，但由 RGA 一趟完成。

   模型输入画布是 dma-buf（CMA 连续内存），RGA 需要物理连续；它的虚拟地址可以
   直接交给 rknn_inputs_set()。边框在初始化时填成 114 一次，之后每帧只写中心区域。 */

typedef struct rga_preprocess rga_preprocess_t;

/* 可用性：librga 能打开 /dev/rga 且分配画布成功 */
int rga_preprocess_available(void);

/* 打开并分配 dst_w×dst_h 的 RGB888 画布（CMA dma-buf），边框填 114 */
rga_preprocess_t *rga_preprocess_open(int dst_w, int dst_h);
void rga_preprocess_close(rga_preprocess_t *p);

/* RGA 核心选择：RGA2E 双核，推理线程按 worker 序号各绑一个，与 NPU 双核对应。
   AUTO 交给驱动调度（默认会全部落到 core0）。 */
enum { RGA_CORE_AUTO = 0, RGA_CORE_0 = 1, RGA_CORE_1 = 2 };

/* 一帧：NV12 dma-buf 源 → letterbox 到画布。
   src_fd 由 jpeg_decoder_nv12() 提供；core 见上面的枚举；几何输出用于后处理逆映射。
   成功返回画布虚拟地址（可直接喂 rknn），失败返回 NULL。 */
unsigned char *rga_preprocess_run(rga_preprocess_t *p, int src_fd, int src_w, int src_h, int core,
                                  float *scale_x, float *scale_y, int *pad_x, int *pad_y);

/* 负载统计：按核统计 RGA 作业占用率（百分比，1 秒滑动窗口）。
   pct[] 顺序固定为 core0/core1；RGA 未启用时返回 -1。
   jobs 为累计作业数（用于前端显示吞吐）。 */
void rga_preprocess_load(int pct[2], unsigned long long *jobs);
int rga_preprocess_active(void);

/* 画布虚拟地址与尺寸 */
unsigned char *rga_preprocess_canvas(const rga_preprocess_t *p);
int rga_preprocess_width(const rga_preprocess_t *p);
int rga_preprocess_height(const rga_preprocess_t *p);

#endif /* RGA_PREPROCESS_H */
