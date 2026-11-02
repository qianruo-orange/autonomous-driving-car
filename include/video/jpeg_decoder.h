/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef JPEG_DECODER_H
#define JPEG_DECODER_H

#include <stddef.h>

typedef struct jpeg_decoder jpeg_decoder_t;

/* One owner, one V4L2 context. No decoder threads or software fallback. */
jpeg_decoder_t *jpeg_decoder_open(void);
void jpeg_decoder_close(jpeg_decoder_t *decoder);
int jpeg_decoder_rgb(jpeg_decoder_t *decoder, const unsigned char *jpeg, size_t length,
                     unsigned char *rgb, int width, int height);
int jpeg_decoder_available(void);
void jpeg_decoder_stats(unsigned long long *frames, unsigned long long *errors);

#endif
