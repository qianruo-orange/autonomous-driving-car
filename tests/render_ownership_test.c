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
    slot->rgb_done = 1;
    frame_ring_buf_take_locked(&ring, FRAME_RING_POOL_RGB, 16 * 16 * 3, &slot->rgb);
    assert(slot->rgb.buf);
    memset(slot->rgb.buf, 0x5a, 16 * 16 * 3);
    unsigned char *original = slot->rgb.buf;
    frame_ring_produce_commit_locked(&ring);
    unsigned long long seq = slot->seq;
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

    /* Reproduce the old race: no raw claims remain, so quiesce clears slots
       immediately while rendering still uses the transferred RGB memory. */
    frame_ring_quiesce_begin(&ring);
    assert(frame_ring_quiesce_wait(&ring, 20) == 0);
    assert(slot->seq == 0 && !slot->rgb.buf);
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
    return 0;
}
