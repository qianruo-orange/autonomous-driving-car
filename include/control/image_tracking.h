/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IMAGE_TRACKING_H
#define IMAGE_TRACKING_H
#include "yolo/yolo_types.h"

typedef struct {
    yolo_result_t frame;
    yolo_classes_t classes;
    unsigned long long capture_ms, track_id, age_ms;
    int active, selected, fresh;
    unsigned long long object_ids[YOLO_MAX_DETS];
} image_tracking_snapshot_t;

/* Visual tracking only: these functions never change motor/follow settings.
 * Match detections using constant-velocity prediction, overlap and size.
 * Associations expire after 800 ms without a reliable match. */
int image_tracking_update(const yolo_result_t *, const yolo_classes_t *,
                          unsigned long long capture_ms,
                          unsigned long long *object_ids);
void image_tracking_snapshot(image_tracking_snapshot_t *);
/* Select an exact detection from the bounded recent rendered-frame history.
 * Returns -1 for an expired frame, invalid box/index, or an unmatchable target. */
int image_tracking_select(unsigned long long seq, int index);
void image_tracking_stop(void);
/* Drop recent frames and IDs when the inference pipeline restarts. */
void image_tracking_reset(void);
#endif
