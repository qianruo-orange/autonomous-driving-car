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
    double appearance_similarity; /* Detector-feature cosine, -1 when unavailable. */
    unsigned char feature_ready[YOLO_MAX_DETS];
    unsigned long long object_ids[YOLO_MAX_DETS];
} image_tracking_snapshot_t;

/* Visual tracking only: these functions never change motor/follow settings.
 * Match same-frame detector descriptors with constant-velocity prediction.
 * Missing descriptors permit nearby motion association only, not distant ReID.
 * Detector-neck features are not a separately trained identity/ReID model. */
int image_tracking_update(const yolo_result_t *, const yolo_classes_t *,
                          unsigned long long capture_ms,
                          unsigned long long *object_ids);
void image_tracking_snapshot(image_tracking_snapshot_t *);
/* Select an exact detection from the bounded recent rendered-frame history.
 * With model features, enrollment can await confirmation in subsequent frames.
 * Returns -1 for an expired frame, invalid box/index, or an unmatchable
 * motion-only target. */
int image_tracking_select(unsigned long long seq, int index);
void image_tracking_stop(void);
/* Drop references and IDs before changing the model descriptor basis. */
void image_tracking_reset(void);
#endif
