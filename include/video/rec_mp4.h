/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef REC_MP4_H
#define REC_MP4_H

#include <stddef.h>
#include <stdint.h>

/**
 * video/rec_mp4.h —— H.264 裸流到 MP4 的封装（单遍写，收尾回填 moov）。
 *
 * 输入是 Annex-B（MPP 编码器输出，带 00 00 00 01 起始码），内部转成 AVCC
 * （4 字节长度前缀）写入 mdat，并从首个访问单元里的 SPS/PPS 生成 avcC。
 * 不依赖任何外部库。
 *
 * 布局：ftyp | mdat(samples) | moov(mvhd, trak(tkhd, mdia(mdhd, hdlr,
 *       minf(vmhd, dinf(dref), stbl(stsd(avc1+avcC), stts, stsc, stsz, co64)))))
 *
 * 样本索引在内存里累积（每帧 4 字节）；偏移表用 co64，单文件不受 4 GiB 限制。
 */

typedef struct rec_mp4 rec_mp4_t;

/* 用指定文件名创建（调用方已决定名字，用于会话与文件名对齐）。 */
rec_mp4_t *rec_mp4_create_named(const char *dir, const char *name, int w, int h, int fps);

/* 写入一帧 Annex-B 访问单元。返回 0 成功，-1 失败。 */
int rec_mp4_write(rec_mp4_t *s, const unsigned char *annexb, size_t len);

/* 收尾：写 moov、回填 mdat 长度、落盘并关闭，内部释放对象。 */
int rec_mp4_close(rec_mp4_t *s);

#endif /* REC_MP4_H */
