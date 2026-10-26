/* Integration check: actual production ring + stream client -> socketpair.
 * cc -std=gnu11 -Wall -Wextra -Werror -pthread -Iinclude -Isource/video
 *    tests/h264_timeline_test.c -o /tmp/h264_timeline_test
 * Only synthetic NALs and emitted MP4 metadata are inspected; no video is saved. */
#include "h264_stream.c"
#include <assert.h>
#include <stdarg.h>
#include <sys/time.h>

int rknn_yolo_enabled(void) { return 1; }
void _log_info(const char *file, int line, const char *fmt, ...) {
    (void)file; (void)line; (void)fmt;
}
void _log_error(const char *file, int line, const char *fmt, ...) {
    (void)file; (void)line; (void)fmt;
}
static const unsigned char sps[] = {0x67,0x64,0,0x1f};
static const unsigned char pps[] = {0x68,0,0,1};
static const unsigned char vcl[] = {0,0,0,1,0x65,0x88,0x80};
static const unsigned char sei[] = {0,0,0,1,0x06,0x80};
typedef struct { int fd; pthread_t tid; char header[512]; } test_client_t;
static void closed(int fd) { close(fd); }
static void read_exact(int fd, unsigned char *data, size_t len) {
    while (len) {
        ssize_t n = read(fd, data, len);
        assert(n > 0);
        data += n;
        len -= (size_t)n;
    }
}
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static uint64_t u64(const unsigned char *p) {
    return (uint64_t)u32(p)<<32 | u32(p+4);
}
static void config(void) {
    pthread_mutex_lock(&g_h264s.lock);
    g_h264s.stop = 0;
    pthread_mutex_unlock(&g_h264s.lock);
    h264_stream_push_config(640,480,30,sps,sizeof(sps),pps,sizeof(pps));
}
static void push(uint64_t ts, int keyframe) {
    h264_stream_push_frame(vcl,sizeof(vcl),keyframe,ts);
}
static test_client_t start_client(void) {
    int sv[2];
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,sv) == 0);
    struct timeval tv = {2,0};
    setsockopt(sv[1],SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    h264s_client_t *client = calloc(1,sizeof(*client));
    assert(client);
    client->fd = sv[0];
    client->on_close = closed;
    test_client_t test = {.fd=sv[1]};
    assert(pthread_create(&test.tid,NULL,h264_stream_client_task,client) == 0);
    size_t hn = 0;
    do {
        assert(hn < sizeof(test.header)-1);
        read_exact(test.fd,(unsigned char *)test.header+hn,1);
        hn++;
    } while (hn < 4 || memcmp(test.header+hn-4,"\r\n\r\n",4));
    test.header[hn] = '\0';
    assert(!memcmp(test.header,"HTTP/1.1 200",12));
    assert(strstr(test.header,"X-Stream-Epoch: "));
    return test;
}
static uint32_t read_sample(test_client_t *test, unsigned expected_seq, uint64_t *next_dts) {
    while (1) {
        unsigned char bh[8];
        read_exact(test->fd,bh,sizeof(bh));
        uint32_t size = u32(bh);
        assert(size >= 8 && size < 4096);
        unsigned char payload[4096];
        read_exact(test->fd,payload,size-8);
        if (memcmp(bh+4,"moof",4)) continue;
        assert(size == 100);
        assert(!memcmp(payload+44,"tfdt",4));
        assert(!memcmp(payload+64,"trun",4));
        assert(u32(payload+12) == expected_seq);
        assert(u64(payload+52) == *next_dts);
        uint32_t duration = u32(payload+84);
        assert(duration > 0);
        *next_dts += duration;
        return duration;
    }
}
static void stop_client(test_client_t *test) {
    h264_stream_shutdown();
    assert(pthread_join(test->tid,NULL) == 0);
    close(test->fd);
    h264_stream_push_end();
}
static void test_continuous_dts_and_wrap(void) {
    uint64_t origin = UINT32_MAX - 80u;
    config();
    push(origin,1);
    push(origin+39,0);
    h264_stream_push_frame(sei,sizeof(sei),0,origin+60);
    push(origin+69,0);
    push(origin+69,0);
    push(origin+769,0);
    push(origin+802,0);
    test_client_t test = start_client();
    uint32_t durations[] = {3000,3510,2700,3000,3000,2970};
    uint64_t next_dts = 0;
    for (unsigned i=0; i<sizeof(durations)/sizeof(durations[0]); i++)
        assert(read_sample(&test,i+1,&next_dts) == durations[i]);
    stop_client(&test);
    puts("PASS: contiguous DTS across variable/zero/700ms gaps, SEI skip and uint32 wrap");
}
static void *spurious_wakes(void *arg) {
    (void)arg;
    for (unsigned i=0; i<40; i++) {
        usleep(5000);
        pthread_mutex_lock(&g_h264s.lock);
        pthread_cond_broadcast(&g_h264s.cond);
        pthread_mutex_unlock(&g_h264s.lock);
    }
    return NULL;
}
static void test_catchup_and_live_pacing(void) {
    config();
    uint64_t origin = h264s_now_ms()-1500;
    for (unsigned i=0; i<46; i++) push(origin+i*33,i==0);
    uint64_t start = h264s_now_ms();
    test_client_t test = start_client();
    char *age = strstr(test.header,"X-Stream-Age-Ms: ");
    assert(age);
    unsigned long age_ms = strtoul(age+strlen("X-Stream-Age-Ms: "),NULL,10);
    assert(age_ms >= 1500 && age_ms < 1600);
    uint64_t next_dts = 0;
    for (unsigned i=0; i<46; i++) {
        uint32_t duration = read_sample(&test,i+1,&next_dts);
        assert(duration == (i ? 2970u : 3000u));
    }
    uint64_t catchup_ms = h264s_now_ms()-start;
    assert(catchup_ms < 100);
    uint64_t latest = origin+45*33;
    push(latest+80,0);
    push(latest+160,0);
    pthread_t wakes;
    assert(pthread_create(&wakes,NULL,spurious_wakes,NULL) == 0);
    assert(read_sample(&test,47,&next_dts) == 7200);
    uint64_t first_ms = h264s_now_ms()-start;
    assert(first_ms >= 50 && first_ms < 250);
    assert(read_sample(&test,48,&next_dts) == 7200);
    uint64_t second_ms = h264s_now_ms()-start;
    assert(second_ms >= 130 && second_ms < 330);
    assert(pthread_join(wakes,NULL) == 0);
    push(latest+1160,0);
    usleep(20000);
    uint64_t stopping = h264s_now_ms();
    stop_client(&test);
    assert(h264s_now_ms()-stopping < 100);
    printf("PASS: 1.5s historical GOP delivered in %llums; live samples paced at %llu/%llums; stop interrupts pacing\n",
           (unsigned long long)catchup_ms,(unsigned long long)first_ms,(unsigned long long)second_ms);
}
static void test_epoch_interrupt(void) {
    config();
    uint64_t origin = h264s_now_ms();
    push(origin,1);
    test_client_t test = start_client();
    uint64_t next_dts = 0;
    read_sample(&test,1,&next_dts);
    push(origin+1000,0);
    usleep(20000);
    uint64_t stopping = h264s_now_ms();
    config();
    assert(pthread_join(test.tid,NULL) == 0);
    assert(h264s_now_ms()-stopping < 100);
    close(test.fd);
    h264_stream_push_end();
    puts("PASS: epoch rotation interrupts pacing immediately");
}
int main(void) {
    alarm(8);
    test_continuous_dts_and_wrap();
    test_catchup_and_live_pacing();
    test_epoch_interrupt();
    return 0;
}
