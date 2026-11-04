/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MPP_ENCODER_H
#define MPP_ENCODER_H

#include <stddef.h>

/* Synchronous NV12 input; caller owns the returned Annex-B allocation.
 * The recorder owns this context and the MPP library's internal workers. */
typedef struct mpp_h264 mpp_h264_t;
int mpp_h264_available(void);
mpp_h264_t *mpp_h264_create(int width, int height, int fps, int bitrate);
int mpp_h264_encode(mpp_h264_t *encoder, const unsigned char *nv12,
                    unsigned char **output, size_t *length);
void mpp_h264_destroy(mpp_h264_t *encoder);

#endif
