/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef REC_MJPEG_H
#define REC_MJPEG_H

#include <stddef.h>
#include <stdint.h>

/**
 * video/rec_mjpeg.h — 无损录像封装器：相机原生 JPEG 帧直接写入 AVI(MJPG)。
 *
 * 不经过任何解码/重编码：mdat 里的每个 '00dc' 块就是 V4L2 采集到的那一帧
 * JPEG 字节，录下来的画质与传感器输出完全一致。VLC / ffplay / Windows
 * 播放器按 MJPG 解码；浏览器不支持 AVI 容器（与之前的 MP4/MJPEG 一致，
 * 只能下载后本地播放）。
 *
 * 文件布局（RIFF，idx1 末尾写入，可就地回填）：
 *   RIFF AVI   LIST hdrl(avih + LIST strl(strh,strf))   LIST movi(00dc…)   idx1
 *
 * 纯封装，无线程；调用方保证并发安全。
 */

typedef struct rec_mjpeg_s rec_mjpeg_t;

/* 创建会话：目录建文件，文件名 <prefix>_YYYYMMDD_HHMMSS_mmm.avi（写入 name_out）。
   @fps 用于 AVI 头部（dwRate），须与采集帧率一致。失败返回 NULL。 */
rec_mjpeg_t *rec_mjpeg_create(const char *dir, const char *prefix, int w, int h,
                              int fps, char *name_out, size_t name_size);

/* 追加一帧相机原生 JPEG（完整 SOI…EOI，原样写入，不做任何改动）。
   达上限（近 2GB，AVI 32 位偏移的安全线）或写失败返回 -1，调用方收尾分段。 */
int rec_mjpeg_write_frame(rec_mjpeg_t *s, const unsigned char *jpeg, size_t len,
                          uint64_t ts_ms);

/* 结束会话：回填 RIFF/movi 长度、avih/strh 帧数、写 idx1 索引并关文件。
   @return 0 成功；-1 失败（无帧时删除残缺文件） */
int rec_mjpeg_finalize(rec_mjpeg_t *s);

/* 会话信息（录制状态展示用） */
const char *rec_mjpeg_name(const rec_mjpeg_t *s);
uint32_t    rec_mjpeg_frames(const rec_mjpeg_t *s);
uint32_t    rec_mjpeg_bytes(const rec_mjpeg_t *s);
uint64_t    rec_mjpeg_start_ms(const rec_mjpeg_t *s);

#endif /* REC_MJPEG_H */
