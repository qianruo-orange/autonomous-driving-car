/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FMP4_H
#define FMP4_H

#include <stddef.h>
#include <stdint.h>

/**
 * video/fmp4.h —— H.264 分片 MP4（fMP4）封装，供浏览器 MSE 播放。
 *
 * 与 rec_mp4 的区别：rec_mp4 写的是收尾才有 moov 的普通 MP4（文件播放）；
 * 这里把 moov 拆成 init segment（ftyp + moov，含 mvex/trex 声明分片），
 * 之后每帧产出一个 moof + mdat 分片，浏览器 appendBuffer 即可连续播放，
 * 适合直播。
 *
 * 用法：
 *   fmp4_t *w = fmp4_create(w, h, fps);
 *   fmp4_write(w, annexb, len);            // 首次调用产出 init + 首片
 *   while (fmp4_take(w, &buf, &len)) ...   // 取出待发送的字节
 */

typedef struct fmp4 fmp4_t;

/* 每片包含多少帧（1 = 每帧一片，延迟最低；8 = 开销更小，延迟约 +8 帧） */
#define FMP4_FRAMES_PER_FRAGMENT 1

fmp4_t *fmp4_create(int w, int h, int fps);
void fmp4_destroy(fmp4_t *w);

/* 送入一帧 Annex-B 访问单元。成功返回 0；首帧缺 SPS/PPS 返回 -1。 */
int fmp4_write(fmp4_t *w, const unsigned char *annexb, size_t len);

/* 取出下一段待发送数据（init segment 或 moof+mdat）。无数据返回 0。 */
int fmp4_take(fmp4_t *w, const unsigned char **buf, size_t *len);

uint32_t fmp4_frames(const fmp4_t *w);

#endif /* FMP4_H */
