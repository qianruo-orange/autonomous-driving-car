/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef YOLO_EVENTS_H
#define YOLO_EVENTS_H

#include <stddef.h>
#include "yolo/yolo_types.h"

#define YOLO_EVENTS_DIR "recordings/detections"
#define YOLO_EVENT_NAME_LEN 96
#define YOLO_EVENT_QUEUE_SIZE 4
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
/* Reserve at most one snapshot per second. No I/O and no blocking lock. */
int yolo_events_due(int count, unsigned long long monotonic_ms);
/* Copies both inputs. Returns 0 when accepted, -1 when busy/invalid/disabled.
 * The renderer never waits for filesystem writes; a full queue drops the event. */
int yolo_events_submit(const unsigned char *jpeg, size_t jpeg_len,
                       const yolo_event_meta_t *meta);
/* Stop after producers have exited; drains accepted events before returning. */
void yolo_events_destroy(void);

/* Strict YYYYMMDD/det_HHMMSS_mmm_SEQ.jpg|json validation for downloads. */
int yolo_event_rel_valid(const char *relative);

#endif
