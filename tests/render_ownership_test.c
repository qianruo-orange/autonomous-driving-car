/* Exercise the production render ownership handoff during camera quiesce. */
#include "yolo/rknn_yolo.c"
#include <assert.h>

int main(void)
{
    frame_ring_t ring;
    frame_ring_init(&ring);
    frame_ring_lock(&ring);
    frame_slot_t *slot = frame_ring_produce_slot_locked(&ring);
    assert(slot);
    slot->w = 16; slot->h = 16;
    slot->capture_ms = 2000;
    slot->focus_valid = 1;
    slot->focus_position = 220;
    slot->autofocus = 1;
    slot->focus_sample_ms = 2033;
    slot->rgb_done = 1;
    frame_ring_buf_take_locked(&ring, FRAME_RING_POOL_RGB, 16 * 16 * 3, &slot->rgb);
    assert(slot->rgb.buf);
    memset(slot->rgb.buf, 0x5a, 16 * 16 * 3);
    unsigned char *original = slot->rgb.buf;
    frame_ring_produce_commit_locked(&ring);
    unsigned long long seq = slot->seq;
    yolo_event_meta_t metadata;
    yolo_event_capture_meta_locked(slot, &metadata);
    ring_buf_t owned = {0};
    int w = 0, h = 0;
    assert(yolo_render_take_rgb_locked(&ring, seq + 1, &owned, &w, &h) < 0);
    assert(slot->rgb.buf == original);
    ring.quiescing = 1;
    assert(yolo_render_take_rgb_locked(&ring, seq, &owned, &w, &h) < 0);
    assert(slot->rgb.buf == original);
    ring.quiescing = 0;
    assert(yolo_render_take_rgb_locked(&ring, seq, &owned, &w, &h) == 0);
    assert(owned.buf == original && !slot->rgb.buf && !slot->rgb.cap);
    assert(w == 16 && h == 16);
    frame_ring_unlock(&ring);

    /* Snapshot metadata describes exactly the post-EMA boxes drawn by the
       renderer, with lens values copied from this captured sequence. */
    g_ai.classes.count = 1;
    strcpy(g_ai.classes.names[0], "person");
    yolo_result_t display = {.seq = seq, .w = 16, .h = 16, .count = 1};
    display.dets[0] = (yolo_det_t){.cls = 0, .conf = .9f, .x1 = 1, .y1 = 1, .x2 = 10, .y2 = 10};
    yolo_result_t previous = display;
    previous.dets[0].x1 = 2;
    previous.dets[0].x2 = 12;
    yolo_smooth(&previous, &display);
    assert(display.dets[0].x1 != 1);
    assert(yolo_event_describe_rendered(&metadata, &display, 3000, 1000000) == 0);
    assert(metadata.seq == seq && metadata.capture_monotonic_ms == 2000);
    assert(metadata.capture_unix_ms == 999000 && metadata.count == 1);
    assert(metadata.focus_valid && metadata.focus_position == 220 && metadata.autofocus == 1);
    assert(metadata.focus_sample_monotonic_ms == 2033);
    assert(!strcmp(metadata.objects[0].name, "person"));
    assert(metadata.objects[0].x1 == display.dets[0].x1);
    assert(metadata.objects[0].x2 == display.dets[0].x2);
    assert(metadata.objects[0].conf == display.dets[0].conf);
    yolo_result_t wrong_frame = display;
    wrong_frame.seq++;
    assert(yolo_event_describe_rendered(&metadata, &wrong_frame, 3000, 1000000) < 0);
    frame_slot_t unknown = {.seq = seq, .w = 16, .h = 16, .capture_ms = 2000,
                            .focus_valid = 0, .focus_position = 999, .autofocus = 9};
    yolo_event_meta_t unknown_meta;
    yolo_event_capture_meta_locked(&unknown, &unknown_meta);
    assert(!unknown_meta.focus_valid && !unknown_meta.focus_position && unknown_meta.autofocus == -1);
    assert(!unknown_meta.focus_sample_monotonic_ms);

    /* Reproduce the old race: no raw claims remain, so quiesce clears slots
       immediately while rendering still uses the transferred RGB memory. */
    frame_ring_quiesce_begin(&ring);
    assert(frame_ring_quiesce_wait(&ring, 20) == 0);
    assert(slot->seq == 0 && !slot->rgb.buf);
    assert(metadata.seq == seq && metadata.focus_position == 220 && metadata.capture_monotonic_ms == 2000);
    frame_ring_lock(&ring);
    for (pool_item_t *item = ring.pool_rgb; item; item = item->next)
        assert(item->buf != owned.buf);
    ring_buf_t next = {0};
    frame_ring_buf_take_locked(&ring, FRAME_RING_POOL_RGB, 16 * 16 * 3, &next);
    assert(next.buf && next.buf != owned.buf);
    memset(next.buf, 0xa5, 16 * 16 * 3);
    for (int i = 0; i < 16 * 16 * 3; ++i) assert(owned.buf[i] == 0x5a);
    frame_ring_buf_put_locked(&ring, FRAME_RING_POOL_RGB, next.buf, next.cap);
    frame_ring_buf_put_locked(&ring, FRAME_RING_POOL_RGB, owned.buf, owned.cap);
    frame_ring_unlock(&ring);
    frame_ring_destroy(&ring);
    puts("PASS: rendering owns RGB across camera quiesce; cleared slots cannot recycle it");
    puts("PASS: event metadata retains the captured lens sample and exactly matches rendered EMA detections");
    return 0;
}
