/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef YOLO_EVENTS_H
#define YOLO_EVENTS_H

#include <stddef.h>
#include "yolo/yolo_types.h"

#define YOLO_EVENTS_DIR "recordings/detections"
#define YOLO_EVENT_NAME_LEN 96
#define YOLO_EVENT_CAMERA_LEN 192
#define YOLO_EVENT_QUEUE_SIZE 512
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
    char camera[YOLO_EVENT_CAMERA_LEN]; /* Captured device identity, not current global configuration. */
    int count;
    yolo_event_object_t objects[YOLO_MAX_DETS];
} yolo_event_meta_t;

/* root is normally NULL (YOLO_EVENTS_DIR); tests can use a private directory.
 * Initialize once before rendering starts. A failed init disables event saving. */
int yolo_events_init(const char *root);
/* Every frame is eligible, including frames with zero detected targets. */
int yolo_events_due(int count, unsigned long long monotonic_ms);
/* Copies metadata into the bounded queue, waiting for space rather than dropping
 * frames. Returns -1 for invalid data or shutdown. Zero-target frames produce a
 * 'none' row. The writer flushes private daily journals every second or 64 KiB;
 * downloads export a standard Excel workbook from a stable journal snapshot. */
int yolo_events_submit_meta(const yolo_event_meta_t *meta);
/* Compatibility entry point. Image arguments are ignored; no photo is saved. */
int yolo_events_submit(const unsigned char *jpeg, size_t jpeg_len,
                       const yolo_event_meta_t *meta);
/* Call before waiting for inference producers to exit. Healthy writes still
 * accept their last results; persistent storage failure releases blocked
 * producers after bounded retries so shutdown cannot deadlock. */
void yolo_events_prepare_stop(void);
/* Stop after producers have exited; drains accepted events before returning. */
void yolo_events_destroy(void);

/* Strict YYYYMMDD/det_HHMMSS_mmm_SEQ.jpg|json validation for downloads. */
int yolo_event_rel_valid(const char *relative);
int yolo_report_rel_valid(const char *relative);
/* Delete the strict daily Excel report and journal. Active-day results can
 * recreate it; queued pre-delete batches are blocked by a generation barrier. */
int yolo_events_delete_report(const char *relative);
/* Daily Excel reports only. Archive descriptor is rewound and owned by caller. */
int yolo_events_pack_reports(unsigned *file_count);
int yolo_events_export_report(const char *relative);
int yolo_events_clear_reports(unsigned *deleted);

#endif
