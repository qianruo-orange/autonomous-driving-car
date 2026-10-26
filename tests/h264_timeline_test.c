/* Standalone integration check: actual production ring + stream client -> socketpair.
 * Build with the gateway include directory and production h264_stream.c beside this file.
 * It verifies emitted MP4 metadata, without decoding or writing any video content. */
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
int main(void) {
    alarm(5);
    unsigned char sps[] = {0x67,0x64,0,0x1f};
    unsigned char pps[] = {0x68,0,0,1};
    unsigned char vcl[] = {0,0,0,1,0x65,0x88,0x80};
    unsigned char sei[] = {0,0,0,1,0x06,0x80};
    uint64_t origin = UINT32_MAX - 80u;
    h264_stream_push_config(640,480,30,sps,sizeof(sps),pps,sizeof(pps));
    h264_stream_push_frame(vcl,sizeof(vcl),1,origin);
    h264_stream_push_frame(vcl,sizeof(vcl),0,origin+39);
    h264_stream_push_frame(sei,sizeof(sei),0,origin+60);
    h264_stream_push_frame(vcl,sizeof(vcl),0,origin+69);
    h264_stream_push_frame(vcl,sizeof(vcl),0,origin+69);
    h264_stream_push_frame(vcl,sizeof(vcl),0,origin+769);
    h264_stream_push_frame(vcl,sizeof(vcl),0,origin+802);
    int sv[2];
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,sv) == 0);
    struct timeval tv = {2,0};
    setsockopt(sv[1],SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    h264s_client_t *client = calloc(1,sizeof(*client));
    assert(client);
    client->fd = sv[0];
    client->on_close = closed;
    pthread_t tid;
    assert(pthread_create(&tid,NULL,h264_stream_client_task,client) == 0);
    unsigned char hdr[512];
    size_t hn = 0;
    do {
        assert(hn < sizeof(hdr));
        read_exact(sv[1],hdr+hn,1);
        hn++;
    } while (hn < 4 || memcmp(hdr+hn-4,"\r\n\r\n",4));
    assert(!memcmp(hdr,"HTTP/1.1 200",12));
    uint32_t durations[] = {3000,3510,2700,3000,3000,2970};
    uint64_t next_dts = 0;
    unsigned samples = 0;
    while (samples < sizeof(durations)/sizeof(durations[0])) {
        unsigned char bh[8];
        read_exact(sv[1],bh,sizeof(bh));
        uint32_t size = u32(bh);
        assert(size >= 8 && size < 4096);
        unsigned char payload[4096];
        read_exact(sv[1],payload,size-8);
        if (memcmp(bh+4,"moof",4)) continue;
        assert(size == 100);
        assert(!memcmp(payload+44,"tfdt",4));
        assert(!memcmp(payload+64,"trun",4));
        uint32_t seq = u32(payload+12);
        uint64_t dts = u64(payload+52);
        uint32_t duration = u32(payload+84);
        assert(seq == samples+1);
        assert(dts == next_dts);
        assert(duration == durations[samples]);
        assert(duration > 0);
        next_dts += duration;
        samples++;
    }
    h264_stream_shutdown();
    pthread_join(tid,NULL);
    close(sv[1]);
    h264_stream_push_end();
    printf("PASS: %u emitted samples have contiguous DTS; variable gaps, SEI skip, zero gap, 700ms gap, and uint32 source-clock wrap covered.\n",samples);
    return 0;
}
