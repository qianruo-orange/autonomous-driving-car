/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef YOLO_EVENTS_H
#define YOLO_EVENTS_H

#include <stddef.h>
#include "yolo/yolo_types.h"

#define YOLO_EVENTS_DIR "recordings/detections"
#define YOLO_EVENT_NAME_LEN 96
#define YOLO_EVENT_QUEUE_SIZE 128
#define YOLO_EVENT_JPEG_MAX (4u * 1024u * 1024u)

typedef struct {
    int cls;
    char name[YOLO_EVENT_NAME_LEN];
    float conf;
    float x1, y1, x2, y2;
} yolo_event_object_t;

typedef struct {
    unsigned long long seq;
    unsigned long long capture_monotonic_ms;
    unsigned long long capture_unix_ms;
    int width, height;
    int focus_valid, focus_position;
    int autofocus; /* -1 means unknown, otherwise 0 or 1. */
    unsigned long long focus_sample_monotonic_ms;
    int count;
    yolo_event_object_t objects[YOLO_MAX_DETS];
} yolo_event_meta_t;

/* root is normally NULL (YOLO_EVENTS_DIR); tests can use a private directory.
 * Initialize once before rendering starts. A failed init disables event saving. */
int yolo_events_init(const char *root);
/* Every detected frame is eligible. No image encoding, I/O, or blocking lock. */
int yolo_events_due(int count, unsigned long long monotonic_ms);
/* Copies metadata into the bounded queue; returns -1 on pressure/invalid input.
 * The background writer appends a daily TXT, flushing every second or 64 KiB. */
int yolo_events_submit_meta(const yolo_event_meta_t *meta);
/* Compatibility entry point. Image arguments are ignored; no photo is saved. */
int yolo_events_submit(const unsigned char *jpeg, size_t jpeg_len,
                       const yolo_event_meta_t *meta);
/* Stop after producers have exited; drains accepted events before returning. */
void yolo_events_destroy(void);

/* Strict YYYYMMDD/det_HHMMSS_mmm_SEQ.jpg|json validation for downloads. */
int yolo_event_rel_valid(const char *relative);
int yolo_report_rel_valid(const char *relative);

#endif
