/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ANO V8 wire format, independently implemented from the official definitions:
 * https://gitee.com/anotc/AnoPTv8/wikis/21-协议帧定义大全
 * https://gitee.com/anotc/AnoPTv8/wikis/05-协议帧格式详解
 * F1 types/names are configured in the PC's flexible-data editor, not transmitted.
 */
#include "telemetry/ano_protocol.h"
#include <float.h>
#include <string.h>

_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
               FLT_MAX_EXP == 128, "ANO Float32 requires IEEE-754 binary32");

static void checksum(const unsigned char *bytes, size_t length,
                      unsigned char *sum, unsigned char *add)
{
    uint8_t sc = 0, ac = 0;
    for (size_t i = 0; i < length; i++) {
        sc = (uint8_t)(sc + bytes[i]);
        ac = (uint8_t)(ac + sc);
    }
    *sum = sc;
    *add = ac;
}

size_t ano_v8_frame(uint8_t id, const void *payload, size_t length,
                    unsigned char *out, size_t capacity)
{
    if (!out || length > ANO_V8_MAX_PAYLOAD || (!payload && length) ||
        capacity < length + ANO_V8_OVERHEAD)
        return 0;
    /* Move payload before setting the header so in-place framing is supported. */
    if (length)
        memmove(out + 6, payload, length);
    out[0] = ANO_V8_HEAD;
    out[1] = ANO_V8_DEVICE_ID;
    out[2] = ANO_V8_HOST_ID;
    out[3] = id;
    out[4] = (unsigned char)length;
    out[5] = (unsigned char)(length >> 8);
    checksum(out, length + 6, &out[length + 6], &out[length + 7]);
    return length + ANO_V8_OVERHEAD;
}

size_t ano_v8_sample(const float channels[4], unsigned char *out, size_t capacity)
{
    if (!channels)
        return 0;
    unsigned char payload[16];
    for (size_t i = 0; i < 4; i++) {
        uint32_t bits;
        memcpy(&bits, &channels[i], sizeof(bits));
        for (size_t byte = 0; byte < 4; byte++)
            payload[i * 4 + byte] = (unsigned char)(bits >> (8 * byte));
    }
    return ano_v8_frame(ANO_V8_FID_SAMPLE, payload, sizeof(payload), out, capacity);
}

size_t ano_v8_device_info(unsigned char *out, size_t capacity)
{
    static const char name[] = "AutonomousCar";
    /* DEV_ID, HW=unspecified, SW=project 1.1.0, BL=none, protocol=8.00.
     * All version fields are 16-bit little-endian; name has no trailing NUL. */
    unsigned char payload[9 + sizeof(name) - 1] = {
        ANO_V8_DEVICE_ID, 0, 0, 110, 0, 0, 0, 0x20, 0x03
    };
    memcpy(payload + 9, name, sizeof(name) - 1);
    return ano_v8_frame(ANO_V8_FID_DEVICE_INFO, payload, sizeof(payload), out, capacity);
}

int ano_v8_valid(const unsigned char *frame, size_t length)
{
    if (!frame || length < ANO_V8_OVERHEAD || length > ANO_V8_MAX_FRAME ||
        frame[0] != ANO_V8_HEAD)
        return 0;
    size_t payload = (size_t)frame[4] | ((size_t)frame[5] << 8);
    if (payload > ANO_V8_MAX_PAYLOAD || length != payload + ANO_V8_OVERHEAD)
        return 0;
    unsigned char sum, add;
    checksum(frame, length - 2, &sum, &add);
    return sum == frame[length - 2] && add == frame[length - 1];
}
