/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IMAGE_TRACKING_H
#define IMAGE_TRACKING_H
#include "yolo/yolo_types.h"

typedef struct {
    yolo_result_t frame;
    yolo_classes_t classes;
    unsigned long long capture_ms, track_id, age_ms, reacquire_remaining_ms;
    int active, selected, fresh;
    int features_available, reference_ready, reidentifications;
    double appearance_similarity;
    unsigned char feature_ready[YOLO_MAX_DETS];
    unsigned long long object_ids[YOLO_MAX_DETS];
} image_tracking_snapshot_t;

/* Visual tracking only: these functions never change motor/follow settings.
 * Update once per rendered inference, before annotation, for frame-matched color.
 * RGB24 is tightly packed and must contain the unannotated matching frame.
 * NULL RGB retains motion-only behavior for callers without image access. */
int image_tracking_update(const yolo_result_t *, const yolo_classes_t *,
                          unsigned long long capture_ms,
                          const unsigned char *rgb, size_t rgb_bytes,
                          unsigned long long *object_ids);
void image_tracking_snapshot(image_tracking_snapshot_t *);
/* Select an exact detection from the bounded recent rendered-frame history.
 * With appearance, enrollment can await confirmation in subsequent frames.
 * Returns -1 for an expired frame, invalid crop/index, or an unmatchable
 * motion-only target. */
int image_tracking_select(unsigned long long seq, int index);
void image_tracking_stop(void);
#endif
