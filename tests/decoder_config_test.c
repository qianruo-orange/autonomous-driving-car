/* Test actual configuration persistence and decoder request rejection. */
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "core/config.c"
#include "http/http_api_config.c"

static int response_code, restart_calls, unsupported_logs;
static char response_body[8192];

void _log_info(const char *file, int line, const char *format, ...)
{ (void)file; (void)line; (void)format; }
void _log_error(const char *file, int line, const char *format, ...)
{ (void)file; (void)line; if (strstr(format, "unsupported video decoder")) unsupported_logs++; }
int npu_core_count(void) { return 2; }
int rknn_yolo_active_workers(void) { return 4; }
int rknn_yolo_reload(void) { assert(!"Unexpected model reload"); return -1; }
int can_enumerate_system(char names[][IFNAMSIZ], int maximum)
{ (void)names; (void)maximum; return 0; }
int net_ip_get_current(const char *ifname, char *address, size_t address_size,
                       char *mask, size_t mask_size, char *gateway, size_t gateway_size)
{
    (void)ifname; (void)address_size; (void)mask_size; (void)gateway_size;
    *address = *mask = *gateway = '\0'; return 0;
}
void net_ip_apply_async(const char *ifname, const char *mode, const char *address,
                        const char *mask, const char *gateway)
{ (void)ifname; (void)mode; (void)address; (void)mask; (void)gateway; assert(!"Unexpected IP change"); }
int can_queue_init(can_queue_t *queue) { (void)queue; assert(!"Unexpected CAN change"); return -1; }
int can_socket_configure(const char *name, int bitrate, int dbitrate, int fd, int restart, int up)
{ (void)name; (void)bitrate; (void)dbitrate; (void)fd; (void)restart; (void)up; assert(!"Unexpected CAN change"); return -1; }
int can_socket_open(const char *name, int fd)
{ (void)name; (void)fd; assert(!"Unexpected CAN change"); return -1; }
void can_socket_close(int fd) { (void)fd; assert(!"Unexpected CAN change"); }
int can_socket_set_filter(int fd, canid_t id, canid_t mask)
{ (void)fd; (void)id; (void)mask; assert(!"Unexpected CAN change"); return -1; }
int watchdog_feed_thread(pthread_t tid, const char *name) { (void)tid; (void)name; return 0; }
int tcp_listen(int port, const char *bind)
{ (void)port; (void)bind; assert(!"Unexpected TCP listener change"); return -1; }
void video_stream_restart(void) { restart_calls++; }
void video_rec_cycle_session(void) { assert(!"Unexpected encoder restart"); }
void http_send_response(int fd, int code, const char *status, const char *mime,
                        const void *body, size_t length)
{
    (void)fd; (void)status; (void)mime;
    response_code = code;
    assert(length < sizeof(response_body));
    if (length) memcpy(response_body, body, length);
    response_body[length] = '\0';
}

static void write_config(const char *text)
{
    FILE *file = fopen(PATH_CONFIG, "w"); assert(file);
    assert(fputs(text, file) >= 0); assert(!fclose(file));
}

int main(void)
{
    char directory[] = "/tmp/decoder-config-test-XXXXXX";
    assert(mkdtemp(directory));
    int original_directory = open(".", O_RDONLY | O_DIRECTORY);
    assert(original_directory >= 0 && chdir(directory) == 0);
    assert(mkdir("config", 0700) == 0);
    struct app_config_t cfg;
    assert(config_load(&cfg) == 0 && !strcmp(cfg.video_decoder, "software"));
    write_config("video_device /dev/video1\nvideo_width 1280\nvideo_height 720\nvideo_codec software\n");
    assert(config_load(&cfg) == 0 && !strcmp(cfg.video_decoder, "software"));
    write_config("video_device /dev/video1\nvideo_width 1280\nvideo_height 720\nvideo_decoder hardware\n");
    assert(config_load(&cfg) == 0 && !strcmp(cfg.video_decoder, "software") && unsupported_logs == 1);

    can_ctx_t can = {.count = 0};
    tcp_ctx_t tcp = {.port = 6666, .max_clients = 16, .listen_fd = -1, .epfd = -1};
    app_ctx_t app = {.cfg = &cfg, .can = &can, .tcp = &tcp};
    pthread_mutex_init(&app.can_mutex, NULL);
    config_save(&app);
    FILE *file = fopen(PATH_CONFIG, "r"); assert(file);
    char persisted[8192]; size_t size = fread(persisted, 1, sizeof(persisted) - 1, file);
    persisted[size] = '\0'; fclose(file);
    assert(strstr(persisted, "video_decoder software\n"));
    http_config_get(&app, 0, "GET", "/api/config", "");
    assert(response_code == 200 && strstr(response_body, "\"video_decoder\":\"software\""));

    const char *invalid[] = {
        "target=video&video_width=640&video_decoder=hardware",
        "target=all&max_clients=1&video_width=640&video_decoder=unknown",
        "video_width=640&video_decoder=",
        "target=video&video_width=640&video_decoder=software&video_decoder=hardware"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        http_config_post(&app, 0, "POST", "/api/config", invalid[i]);
        assert(response_code == 400 && cfg.video_width == 1280 && cfg.video_height == 720);
        assert(!strcmp(cfg.video_decoder, "software") && tcp.max_clients == 16 && !restart_calls);
    }
    http_config_post(&app, 0, "POST", "/api/config", "target=video&video_decoder=software");
    assert(response_code == 200 && restart_calls == 1 && !strcmp(cfg.video_decoder, "software"));
    assert(config_load(&cfg) == 0 && !strcmp(cfg.video_decoder, "software"));
    pthread_mutex_destroy(&app.can_mutex);
    assert(unlink(PATH_CONFIG) == 0 && rmdir("config") == 0);
    assert(fchdir(original_directory) == 0); close(original_directory); assert(rmdir(directory) == 0);
    puts("PASS: decoder defaults, legacy files, persistence, API reporting and mutation-free rejection");
    return 0;
}
