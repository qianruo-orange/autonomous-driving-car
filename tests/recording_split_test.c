/* Validate the production split command's concurrency/stale-file guards. */
#include "video/video_rec.c"
#include <assert.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    int day_fd, started, done, result;
    const char *relative;
} delete_job_t;

static void *delete_created_file(void *argument)
{
    delete_job_t *job = argument;
    pthread_mutex_lock(&job->lock);
    job->started = 1;
    pthread_cond_broadcast(&job->condition);
    pthread_mutex_unlock(&job->lock);
    int result = video_rec_delete_file(job->day_fd, job->relative);
    pthread_mutex_lock(&job->lock);
    job->result = result;
    job->done = 1;
    pthread_cond_broadcast(&job->condition);
    pthread_mutex_unlock(&job->lock);
    return NULL;
}

static void check_recording_delete(void)
{
    char folder[] = "/tmp/car-recording-delete-XXXXXX";
    assert(mkdtemp(folder));
    int day = open(folder, O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    assert(day >= 0);
    const char *relative = "20261028/video_20261028_120000_000.mp4";
    const char *name = relative + 9;
    int fd = openat(day,name,O_CREAT|O_EXCL|O_WRONLY,0600);
    assert(fd >= 0); close(fd); /* An interrupted zero-byte MP4 is deletable. */
    g_rec.file[0] = 0;
    assert(video_rec_delete_file(day,relative) == 0);
    assert(video_rec_delete_file(day,relative) == 2);
    assert(video_rec_delete_file(day,"20261028/../config.txt") == -1 && errno == EINVAL);
    assert(!symlinkat("/etc/passwd",day,name));
    assert(video_rec_delete_file(day,relative) == -1 && errno == EINVAL);
    assert(!unlinkat(day,name,0));

    /* Reproduce a published directory entry whose active status is not yet
       visible. A deletion must wait for the writer's publication lock. */
    pthread_mutex_lock(&g_rec_files_lock);
    fd = openat(day,name,O_CREAT|O_EXCL|O_WRONLY,0600);
    assert(fd >= 0); close(fd);
    delete_job_t job = {.lock=PTHREAD_MUTEX_INITIALIZER,
        .condition=PTHREAD_COND_INITIALIZER,.day_fd=day,.relative=relative};
    pthread_t thread;
    assert(!pthread_create(&thread,NULL,delete_created_file,&job));
    pthread_mutex_lock(&job.lock);
    while(!job.started) pthread_cond_wait(&job.condition,&job.lock);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME,&deadline);
    deadline.tv_nsec += 100000000;
    if(deadline.tv_nsec >= 1000000000){deadline.tv_sec++;deadline.tv_nsec-=1000000000;}
    while(!job.done) {
        int rc = pthread_cond_timedwait(&job.condition,&job.lock,&deadline);
        if(rc == ETIMEDOUT) break;
        assert(!rc);
    }
    assert(!job.done);
    pthread_mutex_unlock(&job.lock);
    pthread_mutex_lock(&g_rec.lock);
    safe_strncpy(g_rec.file,sizeof(g_rec.file),relative);
    g_rec.recording = 1;
    pthread_mutex_unlock(&g_rec.lock);
    pthread_mutex_unlock(&g_rec_files_lock);
    assert(!pthread_join(thread,NULL) && job.result == 1);
    struct stat st;assert(!fstatat(day,name,&st,AT_SYMLINK_NOFOLLOW));

    /* A stop/cycle clears recording before finalize, but file still owns the
       path. Only publication of the closed state makes deletion possible. */
    video_rec_cycle_session();
    assert(!g_rec.recording && video_rec_delete_file(day,relative) == 1);
    pthread_mutex_lock(&g_rec_files_lock);
    pthread_mutex_lock(&g_rec.lock);
    g_rec.file[0] = 0;
    pthread_mutex_unlock(&g_rec.lock);
    pthread_mutex_unlock(&g_rec_files_lock);
    assert(video_rec_delete_file(day,relative) == 0);
    pthread_cond_destroy(&job.condition);
    pthread_mutex_destroy(&job.lock);
    close(day);
    assert(!rmdir(folder));
    puts("PASS: interrupted recordings deleted; creation and finalization paths protected");
}

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
    check_recording_delete();
    pthread_mutex_destroy(&g_rec.lock);
    puts("PASS: split preserves active recording, rejects stale/duplicate/stop/empty requests");
    return 0;
}
