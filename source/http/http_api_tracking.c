/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "http/http_internal.h"
#include "control/image_tracking.h"
#include <errno.h>
#include <math.h>

void http_tracking_get(app_ctx_t *app, int fd, const char *method, const char *uri,
                       const char *request)
{
    (void)app; (void)method; (void)uri; (void)request;
    image_tracking_snapshot_t s;
    image_tracking_snapshot(&s);
    char json[32768];
    int off = 0;
    const char *state = !s.fresh ? "stale" : !s.active ? "idle" :
                        s.selected >= 0 ? "tracking" : "lost";
    JSON_ADD(json, off, "{\"seq\":%llu,\"age_ms\":%llu,\"width\":%d,\"height\":%d,"
             "\"active\":%s,\"selected\":%d,\"track_id\":%llu,\"state\":\"%s\",\"targets\":[",
             s.frame.seq, s.age_ms, s.frame.w, s.frame.h, s.active ? "true" : "false",
             s.selected, s.track_id, state);
    int emitted = 0;
    if (s.fresh) for (int i = 0; i < s.frame.count; ++i) {
        const yolo_det_t *b = &s.frame.dets[i];
        if (!isfinite(b->conf) || !isfinite(b->x1) || !isfinite(b->y1) ||
            !isfinite(b->x2) || !isfinite(b->y2) || b->x1 < 0 || b->y1 < 0 ||
            b->x2 > s.frame.w || b->y2 > s.frame.h || b->x2 <= b->x1 || b->y2 <= b->y1)
            continue;
        char name[YOLO_CLASS_NAME_LEN * 6 + 1];
        http_json_escape(b->cls >= 0 && b->cls < s.classes.count ?
                         s.classes.names[b->cls] : "obj", name, sizeof(name));
        JSON_ADD(json, off, "%s{\"index\":%d,\"class_id\":%d,\"name\":\"%s\",\"confidence\":%.4f,"
                 "\"x1\":%.6f,\"y1\":%.6f,\"x2\":%.6f,\"y2\":%.6f}",
                 emitted++ ? "," : "", i, b->cls, name, b->conf,
                 b->x1 / s.frame.w, b->y1 / s.frame.h, b->x2 / s.frame.w, b->y2 / s.frame.h);
    }
    JSON_ADD(json, off, "]}");
    http_ok_json(fd, json, (size_t)off);
}

void http_tracking_post(app_ctx_t *app, int fd, const char *method, const char *uri,
                        const char *body)
{
    (void)app; (void)method; (void)uri;
    http_form_field_t fields[4];
    int n = body ? http_form_parse(body, fields, 4) : 0;
    const char *action = NULL, *seq = NULL, *index = NULL;
    if (n <= 0 || n >= 4) goto invalid;
    for (int i = 0; i < n; ++i) {
        if (!strcmp(fields[i].key, "action") && !action) action = fields[i].val;
        else if (!strcmp(fields[i].key, "seq") && !seq) seq = fields[i].val;
        else if (!strcmp(fields[i].key, "index") && !index) index = fields[i].val;
        else goto invalid;
    }
    if (action && !strcmp(action, "stop") && n == 1) {
        image_tracking_stop();
        http_ok_text(fd, "stopped");
        return;
    }
    if (!action || strcmp(action, "select") || !seq || !index || n != 3 ||
        seq[0] < '0' || seq[0] > '9' || index[0] < '0' || index[0] > '9') goto invalid;
    char *end;
    errno = 0;
    unsigned long long frame_seq = strtoull(seq, &end, 10);
    if (errno || *end) goto invalid;
    errno = 0;
    unsigned long target = strtoul(index, &end, 10);
    if (errno || *end || target >= YOLO_MAX_DETS) goto invalid;
    if (image_tracking_select(frame_seq, (int)target)) {
        http_err(fd, 409, "Conflict", "Target expired or lost; select it again");
        return;
    }
    http_ok_text(fd, "selected");
    return;
invalid:
    http_err(fd, 400, "Bad Request", "Invalid image tracking selection");
}
