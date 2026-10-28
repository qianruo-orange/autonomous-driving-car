/* Exercise the detached report transport with isolated files and socketpairs. */
#define DETECTION_DOWNLOAD_IDLE_MS 1000u
#define DETECTION_DOWNLOAD_TOTAL_MS 15000u
#include "http/http_detection_download.c"
#include <assert.h>

static pthread_mutex_t fixture_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fixture_changed = PTHREAD_COND_INITIALIZER;
static int fixture_block, fixture_entered, fixture_release, fixture_error, fixture_archive;
static int fixture_archive_only;
static size_t fixture_size;
static unsigned callbacks;

int yolo_report_rel_valid(const char *relative)
{
    return relative && !strcmp(relative, "20261027/result_20261027.txt");
}

static int fixture_export(int archive)
{
    pthread_mutex_lock(&fixture_lock);
    fixture_entered = 1; fixture_archive = archive;
    pthread_cond_broadcast(&fixture_changed);
    while (fixture_block && (!fixture_archive_only || archive) && !fixture_release)
        pthread_cond_wait(&fixture_changed, &fixture_lock);
    int failure = fixture_error;
    size_t length = fixture_size;
    pthread_mutex_unlock(&fixture_lock);
    if (failure) { errno = failure; return -1; }
    FILE *temporary = tmpfile(); assert(temporary);
    char block[4096];
    size_t total = 0;
    while (total < length) {
        size_t chunk = length - total > sizeof(block) ? sizeof(block) : length - total;
        for (size_t i = 0; i < chunk; i++) block[i] = (char)('A' + (total + i) % 26);
        assert(fwrite(block, 1, chunk, temporary) == chunk); total += chunk;
    }
    assert(!fflush(temporary));
    int descriptor = dup(fileno(temporary)); assert(descriptor >= 0);
    fclose(temporary); assert(lseek(descriptor, 0, SEEK_SET) == 0);
    return descriptor;
}
int yolo_events_export_report(const char *relative)
{ assert(yolo_report_rel_valid(relative)); return fixture_export(0); }
int yolo_events_pack_reports(unsigned *count)
{ if (count) *count = 1; return fixture_export(1); }

static void closed(int fd)
{
    /* Admission must be returned before close makes this fd reusable. */
    assert(fcntl(fd, F_GETFD) >= 0);
    pthread_mutex_lock(&fixture_lock);
    callbacks++; pthread_cond_broadcast(&fixture_changed);
    pthread_mutex_unlock(&fixture_lock);
}

static struct timespec deadline_ms(int milliseconds)
{
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += (long)milliseconds * 1000000L;
    until.tv_sec += until.tv_nsec / 1000000000L; until.tv_nsec %= 1000000000L;
    return until;
}

static void wait_entered(void)
{
    struct timespec until = deadline_ms(2000);
    pthread_mutex_lock(&fixture_lock);
    while (!fixture_entered) assert(pthread_cond_timedwait(&fixture_changed, &fixture_lock, &until) == 0);
    pthread_mutex_unlock(&fixture_lock);
}

static void wait_closed(unsigned expected)
{
    struct timespec until = deadline_ms(3000);
    pthread_mutex_lock(&fixture_lock);
    while (callbacks < expected) assert(pthread_cond_timedwait(&fixture_changed, &fixture_lock, &until) == 0);
    assert(callbacks == expected);
    pthread_mutex_unlock(&fixture_lock);
}

static void reset_fixture(size_t size, int error, int block)
{
    pthread_mutex_lock(&fixture_lock);
    fixture_size = size; fixture_error = error; fixture_block = block;
    fixture_entered = fixture_release = fixture_archive = 0;
    fixture_archive_only = 0;
    pthread_mutex_unlock(&fixture_lock);
}

static char *read_response(int fd, size_t *length)
{
    size_t capacity = 8192, used = 0;
    char *response = malloc(capacity); assert(response);
    for (;;) {
        struct pollfd ready = {.fd = fd, .events = POLLIN};
        assert(poll(&ready, 1, 3000) > 0);
        if (used + 4097 > capacity) { capacity *= 2; response = realloc(response, capacity); assert(response); }
        ssize_t n = read(fd, response + used, 4096);
        if (n < 0 && errno == EINTR) continue;
        assert(n >= 0);
        if (!n) break;
        used += (size_t)n;
    }
    response[used] = '\0'; *length = used;
    return response;
}

static void check_success(int archive, size_t body_size, int blocking_export)
{
    reset_fixture(body_size, 0, blocking_export);
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    int tiny = 1024; assert(!setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &tiny, sizeof(tiny)));
    unsigned expected = callbacks + 1;
    uint64_t before = download_now_ms();
    assert(!http_detection_download_start(pair[0], archive ? "/api/detections/reports/pack" :
        "/detectionfile/20261027/result_20261027.txt", closed));
    /* Export waits on a condition; returning here proves the caller is free. */
    assert(download_now_ms() - before < 500u);
    wait_entered();
    int queued[DETECTION_DOWNLOAD_MAX - 1u][2];
    if (blocking_export) {
        for (unsigned i=0; i<DETECTION_DOWNLOAD_MAX-1u; ++i) {
            assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, queued[i]));
            assert(!http_detection_download_start(queued[i][0], archive ? "/api/detections/reports/pack" :
                "/detectionfile/20261027/result_20261027.txt", closed));
            expected++;
        }
        int second[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, second));
        errno = 0;
        assert(http_detection_download_start(second[0], "/api/detections/reports/pack", closed) < 0 && errno == EBUSY);
        assert(fcntl(second[0], F_GETFD) >= 0); /* Failed start retains caller ownership. */
        close(second[0]); close(second[1]);
        pthread_mutex_lock(&fixture_lock); fixture_release = 1;
        pthread_cond_broadcast(&fixture_changed); pthread_mutex_unlock(&fixture_lock);
    }
    size_t length;
    char *response = read_response(pair[1], &length);
    assert(strstr(response, "HTTP/1.1 200 OK\r\n"));
    assert(strstr(response, archive ? "application/gzip" : "text/plain; charset=utf-8"));
    assert(strstr(response, archive ? "filename=\"detections_pack.tar.gz\"" : "filename=\"result_20261027.txt\""));
    char *body = strstr(response, "\r\n\r\n"); assert(body); body += 4;
    assert(length - (size_t)(body - response) == body_size);
    for (size_t i = 0; i < body_size; i++) assert(body[i] == 'A' + i % 26);
    free(response); close(pair[1]);
    if (blocking_export) for (unsigned i=0; i<DETECTION_DOWNLOAD_MAX-1u; ++i) {
        response=read_response(queued[i][1], &length);
        assert(strstr(response,"HTTP/1.1 200 OK\r\n"));
        free(response); close(queued[i][1]);
    }
    wait_closed(expected);
    assert(fixture_archive == archive);
}

static void check_txt_bypasses_archive(void)
{
    reset_fixture(64, 0, 1);
    fixture_archive_only = 1;
    int package[2], text[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, package));
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, text));
    unsigned expected = callbacks + 2;
    assert(!http_detection_download_start(package[0], "/api/detections/reports/pack", closed));
    wait_entered(); /* Compression remains blocked until after the TXT response. */
    assert(!http_detection_download_start(text[0],
        "/detectionfile/20261027/result_20261027.txt", closed));
    size_t length;
    char *response = read_response(text[1], &length);
    assert(length && strstr(response, "HTTP/1.1 200 OK\r\n"));
    free(response); close(text[1]);
    pthread_mutex_lock(&fixture_lock);
    fixture_release = 1; pthread_cond_broadcast(&fixture_changed);
    pthread_mutex_unlock(&fixture_lock);
    response = read_response(package[1], &length);
    assert(length && strstr(response, "HTTP/1.1 200 OK\r\n"));
    free(response); close(package[1]); wait_closed(expected);
}

int main(void)
{
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    assert(http_detection_download_start(pair[0], "/detectionfile/../../secret", closed) < 0 && errno == EINVAL);
    assert(http_detection_download_start(pair[0], "/detectionfile/20261027/detections_20261027.xlsx", closed) < 0 && errno == EINVAL);
    assert(fcntl(pair[0], F_GETFD) >= 0); close(pair[0]); close(pair[1]);
    check_success(0, 64, 1);
    check_success(1, 2u * 1024u * 1024u, 0); /* Repeated partial socket writes. */
    check_success(0, 0, 0);
    check_txt_bypasses_archive();

    const int failures[] = {ENOENT, EINVAL, EAGAIN, EFBIG, EIO};
    const char *statuses[] = {"404 Not Found", "400 Bad Request", "409 Conflict", "413 Payload Too Large", "500 Internal Server Error"};
    for (size_t i = 0; i < sizeof(failures)/sizeof(failures[0]); i++) {
        reset_fixture(0, failures[i], 0);
        assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
        unsigned expected = callbacks + 1;
        assert(!http_detection_download_start(pair[0], "/api/detections/reports/pack", closed));
        size_t length; char *response = read_response(pair[1], &length);
        assert(length && strstr(response, statuses[i])); free(response); close(pair[1]); wait_closed(expected);
    }
    /* A peer closing before export completes must not raise SIGPIPE or leak the slot. */
    reset_fixture(1024u * 1024u, 0, 1);
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    unsigned expected = callbacks + 1;
    assert(!http_detection_download_start(pair[0], "/api/detections/reports/pack", closed));
    wait_entered(); close(pair[1]);
    pthread_mutex_lock(&fixture_lock); fixture_release = 1;
    pthread_cond_broadcast(&fixture_changed); pthread_mutex_unlock(&fixture_lock);
    wait_closed(expected);

    /* A connected peer that never drains has a finite idle timeout. */
    reset_fixture(1024u * 1024u, 0, 0);
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    int tiny = 1024; assert(!setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &tiny, sizeof(tiny)));
    expected = callbacks + 1;
    assert(!http_detection_download_start(pair[0], "/api/detections/reports/pack", closed));
    wait_closed(expected); close(pair[1]);
    check_success(0, 128, 0); /* Timeout and disconnect both release concurrency. */
    puts("PASS: asynchronous TXT download, bounded send, partial writes, status mapping, disconnect and callback ownership");
    return 0;
}
