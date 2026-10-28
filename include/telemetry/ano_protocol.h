/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ANO_PROTOCOL_H
#define ANO_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define ANO_V8_HEAD 0xAB
#define ANO_V8_DEVICE_ID 0x01
#define ANO_V8_HOST_ID 0xFE
#define ANO_V8_BROADCAST_ID 0xFF
#define ANO_V8_MAX_FRAME 256
#define ANO_V8_OVERHEAD 8
#define ANO_V8_MAX_PAYLOAD (ANO_V8_MAX_FRAME - ANO_V8_OVERHEAD)

#define ANO_V8_FID_SAMPLE 0xF1
#define ANO_V8_FID_PARAMETER 0xE0
#define ANO_V8_FID_COMMAND_QUERY 0xC1
#define ANO_V8_FID_DEVICE_INFO 0xE3

/* Frames originate at this device and target the PC. Return the complete frame
 * length, or zero for invalid arguments/insufficient capacity. Payload bytes are
 * opaque to ano_v8_frame; multi-byte values must already be little-endian. */
size_t ano_v8_frame(uint8_t id, const void *payload, size_t length,
                    unsigned char *out, size_t capacity);
/* Four IEEE-754 Float32 values, in the configured F1 channel order. Missing real
 * feedback may remain NaN; this encoder never substitutes measured values. */
size_t ano_v8_sample(const float channels[4], unsigned char *out, size_t capacity);
size_t ano_v8_device_info(unsigned char *out, size_t capacity);
/* Validates one complete frame's envelope/length/checksums. Address selection and
 * command handling belong to the transport; trailing bytes are not accepted. */
int ano_v8_valid(const unsigned char *frame, size_t length);

#endif
