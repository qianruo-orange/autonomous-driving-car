/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "video/mpp_encoder.h"
#include "core/log.h"

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>

struct mpp_h264 {
    MppCtx context;
    MppApi *api;
    MppBufferGroup group;
    MppBuffer input;
    int width, height, stride, vstride;
    size_t buffer_size;
    RK_S64 pts;
};

int mpp_h264_available(void)
{
    return access("/dev/mpp_service", R_OK | W_OK) == 0;
}

void mpp_h264_destroy(mpp_h264_t *e)
{
    if (!e) return;
    /* Stop workers before releasing buffers which queued tasks can reference. */
    if (e->context) mpp_destroy(e->context);
    if (e->input) mpp_buffer_put(e->input);
    if (e->group) mpp_buffer_group_put(e->group);
    free(e);
}

mpp_h264_t *mpp_h264_create(int width, int height, int fps, int bitrate)
{
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
        (width & 1) || (height & 1) || fps <= 0 || fps > 240 || bitrate <= 0 ||
        bitrate > 100000000 || !mpp_h264_available()) return NULL;
    mpp_h264_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->width = width; e->height = height;
    e->stride = (width + 15) & ~15;
    e->vstride = (height + 15) & ~15;
    /* Match the MPP sample's allocation padding; visible plane offsets use
     * the explicitly configured strides, not the allocation's padding. */
    e->buffer_size = (size_t)((e->stride + 63) & ~63) *
                     ((e->vstride + 63) & ~63) * 3 / 2;
    MppEncCfg config = NULL;
    MppPollType input_timeout = MPP_POLL_BLOCK, output_timeout = 1000;
    MppEncHeaderMode headers = MPP_ENC_HEADER_MODE_EACH_IDR;
    if (mpp_create(&e->context, &e->api) ||
        e->api->control(e->context, MPP_SET_INPUT_TIMEOUT, &input_timeout) ||
        e->api->control(e->context, MPP_SET_OUTPUT_TIMEOUT, &output_timeout) ||
        mpp_init(e->context, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) ||
        mpp_enc_cfg_init(&config) ||
        e->api->control(e->context, MPP_ENC_GET_CFG, config)) goto fail;
    const struct { const char *key; int value; } settings[] = {
        {"codec:type", MPP_VIDEO_CodingAVC},
        {"prep:width", width}, {"prep:height", height},
        {"prep:hor_stride", e->stride}, {"prep:ver_stride", e->vstride},
        /* The annotated RGB -> NV12 conversion produces BT.601 limited range. */
        {"prep:format", MPP_FMT_YUV420SP}, {"prep:range", MPP_FRAME_RANGE_MPEG},
        {"rc:mode", MPP_ENC_RC_MODE_CBR},
        {"rc:fps_in_flex", 0}, {"rc:fps_in_num", fps}, {"rc:fps_in_denom", 1},
        {"rc:fps_out_flex", 0}, {"rc:fps_out_num", fps}, {"rc:fps_out_denom", 1},
        {"rc:gop", fps * 2}, {"rc:bps_target", bitrate},
        {"rc:bps_max", bitrate * 17 / 16}, {"rc:bps_min", bitrate * 15 / 16},
        {"rc:qp_init", -1}, {"rc:qp_min", 10}, {"rc:qp_max", 44},
        {"rc:qp_min_i", 10}, {"rc:qp_max_i", 40},
        {"rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED},
        {"split:mode", 0}, {"h264:profile", 100}, {"h264:level", 51},
        {"h264:cabac_en", 1}, {"h264:trans8x8", 1}
    };
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++)
        if (mpp_enc_cfg_set_s32(config, settings[i].key, settings[i].value)) goto fail;
    if (e->api->control(e->context, MPP_ENC_SET_CFG, config) ||
        e->api->control(e->context, MPP_ENC_SET_HEADER_MODE, &headers) ||
        mpp_buffer_group_get_internal(&e->group, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE) ||
        mpp_buffer_get(e->group, &e->input, e->buffer_size) ||
        !mpp_buffer_get_ptr(e->input)) goto fail;
    mpp_enc_cfg_deinit(config);
    return e;
fail:
    if (config) mpp_enc_cfg_deinit(config);
    LOG_ERROR("h264: MPP encoder initialization failed");
    mpp_h264_destroy(e);
    return NULL;
}

int mpp_h264_encode(mpp_h264_t *e, const unsigned char *nv12,
                    unsigned char **output, size_t *length)
{
    if (!e || !nv12 || !output || !length) return -1;
    *output = NULL; *length = 0;
    if (mpp_buffer_sync_begin(e->input)) return -1;
    unsigned char *data = mpp_buffer_get_ptr(e->input);
    memset(data, 0, e->buffer_size);
    for (int y = 0; y < e->height; y++)
        memcpy(data + (size_t)y * e->stride, nv12 + (size_t)y * e->width, e->width);
    for (int y = 0; y < e->height / 2; y++)
        memcpy(data + (size_t)e->stride * e->vstride + (size_t)y * e->stride,
               nv12 + (size_t)e->width * e->height + (size_t)y * e->width, e->width);
    if (mpp_buffer_sync_end(e->input)) return -1;
    MppFrame frame = NULL;
    MppPacket packet = NULL;
    int result = -1;
    if (mpp_frame_init(&frame)) goto done;
    mpp_frame_set_width(frame, e->width);
    mpp_frame_set_height(frame, e->height);
    mpp_frame_set_hor_stride(frame, e->stride);
    mpp_frame_set_ver_stride(frame, e->vstride);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_pts(frame, e->pts++);
    mpp_frame_set_buffer(frame, e->input);
    /* A synchronous put returns the input frame to its owner. Let MPP allocate
     * and manage output packets so an output timeout cannot lose a local one.
     * The driver task timeout and process watchdog supervise a blocked input. */
    if (e->api->encode_put_frame(e->context, frame)) goto done;
    mpp_frame_deinit(&frame);
    if (e->api->encode_get_packet(e->context, &packet) || !packet ||
        (mpp_packet_is_partition(packet) && !mpp_packet_is_eoi(packet))) goto done;
    size_t size = mpp_packet_get_length(packet);
    if (!size || size > e->buffer_size || !mpp_packet_get_pos(packet)) goto done;
    unsigned char *copy = malloc(size);
    if (!copy) goto done;
    memcpy(copy, mpp_packet_get_pos(packet), size);
    *output = copy; *length = size;
    result = 0;
done:
    /* On failure the recorder destroys the context, including queued output. */
    if (frame) mpp_frame_deinit(&frame);
    if (packet) mpp_packet_deinit(&packet);
    return result;
}
