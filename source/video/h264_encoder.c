/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hardware-only H.264: synchronous MPP encoding and Annex-B parameter sets. */
#include <stdlib.h>
#include <string.h>
#include "video/h264_encoder.h"
#include "video/mpp_encoder.h"
#include "core/log.h"

struct h264_encoder_s {
    mpp_h264_t *mpp;
    unsigned char sps[64], pps[64];
    unsigned int sps_len, pps_len;
};

static long annexb_nal_start(const unsigned char *data,size_t length,size_t from,size_t *code)
{
    for(size_t i=from;i+3<=length;i++){
        if(data[i]==0&&data[i+1]==0&&data[i+2]==1){if(code)*code=i;return (long)(i+3);}
        if(data[i]==0&&data[i+1]==0&&data[i+2]==0&&i+3<length&&data[i+3]==1){
            if(code)*code=i;
            return (long)(i+4);
        }
    }
    return -1;
}

void h264_annexb_walk(const unsigned char *data,size_t length,
                      h264_nal_callback_t callback,void *context)
{
    if(!data||!callback)return;
    size_t from=0;
    while(from+3<=length){
        size_t code=0;long start=annexb_nal_start(data,length,from,&code);
        if(start<0)break;
        size_t next_code=0;long next=annexb_nal_start(data,length,(size_t)start,&next_code);
        size_t nal_length=next<0?length-(size_t)start:next_code-(size_t)start;
        callback(context,data+start,nal_length);
        if(next<0)break;
        from=(size_t)start;
    }
}

h264_encoder_t *h264_encoder_create(int w, int h, int fps, int bitrate_bps)
{
    h264_encoder_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->mpp = mpp_h264_create(w, h, fps, bitrate_bps);
    if (!e->mpp) { free(e); return NULL; }
    LOG_INFO("h264: MPP hardware encoder ready %dx%d fps=%d bitrate=%d",
             w, h, fps, bitrate_bps);
    return e;
}

void h264_encoder_destroy(h264_encoder_t *e)
{
    if (!e) return;
    mpp_h264_destroy(e->mpp);
    free(e);
}

/* Reuse the Annex-B walker so start-code bytes never enter avcC. */
struct h264_scan_context {
    h264_encoder_t *encoder;
    int *keyframe, *has_vcl;
};

static void h264_scan_nal(void *argument, const unsigned char *nal, size_t length)
{
    struct h264_scan_context *scan = argument;
    h264_encoder_t *e = scan->encoder;
    if (!length) return;
    unsigned type = nal[0] & 0x1f;
    if (type == 7 && length >= 4 && length <= sizeof(e->sps)) {
        memcpy(e->sps, nal, length); e->sps_len = (unsigned)length;
    } else if (type == 8 && length >= 1 && length <= sizeof(e->pps)) {
        memcpy(e->pps, nal, length); e->pps_len = (unsigned)length;
    }
    if (type == 5 && scan->keyframe) *scan->keyframe = 1;
    if ((type == 1 || type == 5) && scan->has_vcl) *scan->has_vcl = 1;
}

static void h264_scan(h264_encoder_t *e, const unsigned char *data, size_t length,
                      int *keyframe, int *has_vcl)
{
    if (has_vcl) *has_vcl = 0;
    struct h264_scan_context scan = { e, keyframe, has_vcl };
    h264_annexb_walk(data, length, h264_scan_nal, &scan);
}

int h264_encoder_encode(h264_encoder_t *e, const unsigned char *nv12,
                        unsigned char **out, size_t *out_len, int *keyframe)
{
    if (!e || !nv12 || !out || !out_len) return -1;
    *out = NULL;
    *out_len = 0;
    if (keyframe) *keyframe = 0;
    int result = mpp_h264_encode(e->mpp, nv12, out, out_len);
    int idr = 0, has_vcl = 0;
    if (!result) h264_scan(e, *out, *out_len, &idr, &has_vcl);
    if (result || !has_vcl) {
        free(*out); *out = NULL; *out_len = 0;
        return -1;
    }
    if (keyframe) *keyframe = idr;
    return 0;
}

const char *h264_encoder_name(const h264_encoder_t *e)
{
    return e ? "rkmpp" : "none";
}

int h264_encoder_sps_pps(h264_encoder_t *e,
                         const unsigned char **sps, unsigned int *sps_len,
                         const unsigned char **pps, unsigned int *pps_len)
{
    if (!e) return -1;
    if (sps)     *sps     = e->sps;
    if (sps_len) *sps_len = e->sps_len;
    if (pps)     *pps     = e->pps;
    if (pps_len) *pps_len = e->pps_len;
    return (e->sps_len > 0 && e->pps_len > 0) ? 0 : -1;
}
