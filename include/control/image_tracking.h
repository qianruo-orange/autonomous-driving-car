/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IMAGE_TRACKING_H
#define IMAGE_TRACKING_H
#include "yolo/yolo_types.h"

typedef struct {
    yolo_result_t frame;
    yolo_classes_t classes;
    unsigned long long capture_ms, track_id, age_ms;
    int active, selected, fresh;
} image_tracking_snapshot_t;

/* Visual tracking only: these functions never change motor/follow settings.
 * Update once per rendered inference, before annotation, for frame-matched color. */
int image_tracking_update(const yolo_result_t *, const yolo_classes_t *,
                          unsigned long long capture_ms);
void image_tracking_snapshot(image_tracking_snapshot_t *);
/* Select an exact detection from the bounded recent rendered-frame history.
 * Returns -1 for an expired frame, invalid index, or an unmatchable target. */
int image_tracking_select(unsigned long long seq, int index);
void image_tracking_stop(void);
#endif
