/* Test actual configuration persistence and decoder request rejection. */
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "core/config.c"
#include "http/http_api_config.c"

static int response_code, restart_calls, cycle_calls, unsupported_logs, ip_calls;
static char response_body[8192];
static char video_actions[8];
static size_t video_action_count;

void _log_info(const char *file, int line, const char *format, ...)
{ (void)file; (void)line; (void)format; }
void _log_error(const char *file, int line, const char *format, ...)
{ (void)file; (void)line; if (strstr(format, "unsupported video decoder")) unsupported_logs++; }
int npu_core_count(void) { return 2; }
const char *npu_platform_name(void) { return "rk3576"; }
void rknn_yolo_default_model_path(char *out, size_t size)
{ snprintf(out, size, "config/yolo26.rknn"); }
int rknn_yolo_active_workers(void) { return 4; }
int rknn_yolo_reload(void) { assert(!"Unexpected model reload"); return -1; }
int can_enumerate_system(char names[][IFNAMSIZ], int maximum)
{ (void)names; (void)maximum; return 0; }
int can_feature_set_locked(app_ctx_t *app, int enabled)
{ (void)app; (void)enabled; assert(!"Unexpected CAN feature change"); return -1; }
int net_ip_get_current(const char *ifname, char *address, size_t address_size,
                       char *mask, size_t mask_size, char *gateway, size_t gateway_size)
{
    (void)ifname; (void)address_size; (void)mask_size; (void)gateway_size;
    *address = *mask = *gateway = '\0'; return 0;
}
void net_ip_apply_async(const char *ifname, const char *mode, const char *address,
                        const char *mask, const char *gateway)
{
    assert(!strcmp(ifname, "eth7") && !strcmp(mode, "static"));
    assert(!strcmp(address, "192.168.7.5") && !strcmp(mask, "255.255.255.0"));
    assert(!strcmp(gateway, "192.168.7.1"));
    ip_calls++;
}
int can_queue_init(can_queue_t *queue) { (void)queue; assert(!"Unexpected CAN change"); return -1; }
int can_socket_configure(const char *name, int bitrate, int dbitrate, int fd, int restart, int up)
{ (void)name; (void)bitrate; (void)dbitrate; (void)fd; (void)restart; (void)up; assert(!"Unexpected CAN change"); return -1; }
int can_socket_open(const char *name, int fd)
{ (void)name; (void)fd; assert(!"Unexpected CAN change"); return -1; }
void can_socket_close(int fd) { (void)fd; assert(!"Unexpected CAN change"); }
int can_socket_apply_filters(app_ctx_t *app, can_iface_t *iface, int fd)
{ (void)app; (void)iface; (void)fd; assert(!"Unexpected CAN change"); return -1; }
void can_motor_prepare_close_locked(app_ctx_t *app, const char *ifname)
{ (void)app; (void)ifname; assert(!"Unexpected CAN change"); }
int watchdog_feed_thread(pthread_t tid, const char *name) { (void)tid; (void)name; return 0; }
void video_stream_restart(void)
{
    restart_calls++;
    assert(video_action_count + 1 < sizeof(video_actions));
    video_actions[video_action_count++] = 'R'; video_actions[video_action_count] = '\0';
}
void video_rec_cycle_session(void)
{
    cycle_calls++;
    assert(video_action_count + 1 < sizeof(video_actions));
    video_actions[video_action_count++] = 'C'; video_actions[video_action_count] = '\0';
}
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

static void expect_video_actions(app_ctx_t *app, const char *form, const char *expected)
{
    video_action_count = 0; video_actions[0] = '\0';
    int prior_cycles = cycle_calls, prior_restarts = restart_calls;
    http_config_post(app, 0, "POST", "/api/config", form);
    assert(response_code == 200 && !strcmp(video_actions, expected));
    assert(cycle_calls - prior_cycles == (strchr(expected, 'C') != NULL));
    assert(restart_calls - prior_restarts == (strchr(expected, 'R') != NULL));
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
    assert(!cfg.args.can_enabled);
    write_config("tcp_port 7777\nmax_clients 5\ntcp_bind eth7\n");
    assert(!config_load(&cfg) && !strcmp(cfg.args.ip_ifname, "eth7"));
    write_config("can_ifname can0\ncan_up can0 on\n");
    assert(!config_load(&cfg) && cfg.args.can_enabled && cfg.args.can_count == 1);
    write_config("can_enabled off\ncan_ifname can0\ncan_up can0 on\n");
    assert(!config_load(&cfg) && !cfg.args.can_enabled && cfg.args.can_ifaces[0].up);
    write_config("can_enabled on\n");
    assert(!config_load(&cfg) && cfg.args.can_enabled && !cfg.args.can_count);
    assert(!strcmp(cfg.yolo_model, "config/yolo26.rknn"));
    write_config("yolo_model config/yolo26.rknn\n");
    assert(config_load(&cfg) == 0 && !strcmp(cfg.yolo_model, "config/yolo26.rknn"));
    write_config("yolo_model ./config/yolo26.rknn\n");
    assert(config_load(&cfg) == 0 && !strcmp(cfg.yolo_model, "config/yolo26.rknn"));
    write_config("yolo_model config/models/rk3576/yolo26.rknn\n");
    assert(config_load(&cfg) == 0 && !strcmp(cfg.yolo_model, "config/yolo26.rknn"));
    write_config("yolo_model ./config/models/rk3576/yolo26.rknn\n");
    assert(config_load(&cfg) == 0 && !strcmp(cfg.yolo_model, "config/yolo26.rknn"));
    write_config("yolo_model config/models/custom/special.rknn\n");
    assert(config_load(&cfg) == 0 && !strcmp(cfg.yolo_model, "config/models/custom/special.rknn"));
    write_config("video_device /dev/video1\nvideo_width 1280\nvideo_height 720\nvideo_codec software\n");
    assert(config_load(&cfg) == 0 && !strcmp(cfg.video_decoder, "software"));
    write_config("video_device /dev/video1\nvideo_width 1280\nvideo_height 720\nvideo_decoder hardware\n");
    assert(config_load(&cfg) == 0 && !strcmp(cfg.video_decoder, "software") && unsupported_logs == 1);

    can_ctx_t can = {.count = 0};
    tcp_ctx_t tcp = {.epfd = -1, .vofa_listener = -1};
    app_ctx_t app = {.cfg = &cfg, .can = &can, .tcp = &tcp};
    pthread_mutex_init(&app.can_mutex, NULL);
    config_save(&app);
    FILE *file = fopen(PATH_CONFIG, "r"); assert(file);
    char persisted[8192]; size_t size = fread(persisted, 1, sizeof(persisted) - 1, file);
    persisted[size] = '\0'; fclose(file);
    assert(strstr(persisted, "video_decoder software\n"));
    assert(strstr(persisted, "can_enabled off\n"));
    assert(strstr(persisted, "yolo_model config/yolo26.rknn\n"));
    http_config_get(&app, 0, "GET", "/api/config", "");
    assert(response_code == 200 && strstr(response_body, "\"video_decoder\":\"software\""));
    assert(strstr(response_body, "\"can_enabled\":false"));

    const char *invalid[] = {
        "target=video&video_width=640&video_decoder=hardware",
        "target=all&max_clients=1&video_width=640&video_decoder=unknown",
        "video_width=640&video_decoder=",
        "target=video&video_width=640&video_decoder=software&video_decoder=hardware"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        http_config_post(&app, 0, "POST", "/api/config", invalid[i]);
        assert(response_code == 400 && cfg.video_width == 1280 && cfg.video_height == 720);
        assert(!strcmp(cfg.video_decoder, "software") && !restart_calls && !cycle_calls);
    }
    expect_video_actions(&app, "target=video&video_decoder=software", "CR");
    assert(!strcmp(cfg.video_decoder, "software"));
    /* Capture-only changes must also replace the shared encoder, and do so
       before restarting capture. Exercise the all-target path so the explicit
       video-card restart cannot conceal a missing change bit. */
    expect_video_actions(&app, "target=all&video_width=1920&video_height=1080", "CR");
    assert(cfg.video_width == 1920 && cfg.video_height == 1080);
    expect_video_actions(&app, "target=all&video_fps=25", "CR");
    assert(cfg.video_fps == 25);
    expect_video_actions(&app, "target=all&video_device=%2Fdev%2Fvideo9", "CR");
    assert(!strcmp(cfg.video_device, "/dev/video9"));
    expect_video_actions(&app, "target=all&video_codec=hardware", "C");
    assert(!strcmp(cfg.video_codec, "hardware"));
    expect_video_actions(&app, "target=all&video_codec=software&video_width=1280&video_height=720&video_fps=30", "CR");
    assert(!strcmp(cfg.video_codec, "software") && cfg.video_width == 1280 && cfg.video_height == 720 && cfg.video_fps == 30);
    expect_video_actions(&app, "target=all", "");
    http_config_post(&app, 0, "POST", "/api/config", "target=net&tcp_port=7777&max_clients=12");
    assert(response_code == 400 && tcp.vofa_listener == -1);
    http_config_get(&app, 0, "GET", "/api/config", "");
    assert(response_code == 200 && strstr(response_body, "\"http_port\":80"));
    assert(!strstr(response_body, "tcp_port") && !strstr(response_body, "max_clients"));
    expect_video_actions(&app, "target=net&ip_ifname=eth7&ip_mode=static&ip_addr=192.168.7.5&ip_mask=255.255.255.0&ip_gw=192.168.7.1", "");
    assert(ip_calls == 1 && !strcmp(cfg.args.ip_ifname, "eth7"));
    assert(config_load(&cfg) == 0 && !strcmp(cfg.video_decoder, "software"));
    assert(!strcmp(cfg.args.ip_addr, "192.168.7.5"));
    FILE *saved = fopen(PATH_CONFIG, "r");
    assert(saved);
    char saved_text[8192] = {0};
    assert(fread(saved_text, 1, sizeof(saved_text) - 1, saved) > 0);
    fclose(saved);
    assert(!strstr(saved_text, "tcp_port") && !strstr(saved_text, "tcp_bind") && !strstr(saved_text, "max_clients"));
    pthread_mutex_destroy(&app.can_mutex);
    assert(unlink(PATH_CONFIG) == 0 && rmdir("config") == 0);
    assert(fchdir(original_directory) == 0); close(original_directory); assert(rmdir(directory) == 0);
    puts("PASS: decoder rejection/persistence and ordered shared-encoder rebuild for capture changes");
    return 0;
}
