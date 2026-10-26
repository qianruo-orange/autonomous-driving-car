/* Validate the production split command's concurrency/stale-file guards. */
#include "video/video_rec.c"
#include <assert.h>

int main(void)
{
    pthread_mutex_init(&g_rec.lock, NULL);
    const char *active = "20261026/rec_120000.mp4";
    assert(video_rec_split(active) == -1);
    g_rec.recording = 1;
    g_rec.frames = 10;
    safe_strncpy(g_rec.file, sizeof(g_rec.file), active);
    assert(video_rec_split("20261026/rec_110000.mp4") == -2);
    assert(g_rec.split_pending == 0);
    assert(video_rec_split(active) == 0);
    assert(g_rec.recording == 1);
    assert(video_rec_split(active) == -1);
    video_rec_status_t status;
    assert(video_rec_status(&status) == 0);
    assert(status.recording && status.split_pending);
    assert(strcmp(status.file, active) == 0);
    g_rec.split_pending = 0;
    g_rec.cmd = REC_CMD_STOP;
    assert(video_rec_split(active) == -1);
    g_rec.cmd = REC_CMD_NONE;
    g_rec.frames = 0;
    assert(video_rec_split(active) == -1);
    pthread_mutex_destroy(&g_rec.lock);
    puts("PASS: split preserves active recording, rejects stale/duplicate/stop/empty requests");
    return 0;
}
