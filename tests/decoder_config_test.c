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
static unsigned config_pack_calls;

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

void http_serve_stream(int fd, const char *mime, const char *extra, FILE *source,
                       size_t size, const char *unlink_after)
{
    (void)fd;
    assert(!strcmp(mime, "application/gzip") && strstr(extra, "config_pack.tar.gz"));
    FILE *archive = fopen("export.tar.gz", "wb"); assert(archive);
    size_t copied = 0, bytes;
    char buffer[4096];
    while ((bytes = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        assert(fwrite(buffer, 1, bytes, archive) == bytes);
        copied += bytes;
    }
    assert(!ferror(source) && copied == size && copied > 0);
    assert(!fclose(archive) && !fclose(source) && !unlink(unlink_after));
    config_pack_calls++;
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

static void pack_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "w"); assert(file);
    assert(fputs(text, file) >= 0 && !fclose(file));
}

static void pack_listing(char *listing, size_t size)
{
    FILE *tar = popen("tar --quoting-style=literal -tzf export.tar.gz", "r"); assert(tar);
    size_t bytes = fread(listing, 1, size - 1, tar);
    assert(bytes < size - 1 && !ferror(tar));
    listing[bytes] = 0;
    assert(!pclose(tar));
}

static void test_config_pack(app_ctx_t *app)
{
    assert(!mkdir("config/model", 0700) && !mkdir("config/models", 0700));
    pack_file("config/models/.model-sync.lock", "");
    pack_file("config/models/.last-deployed.sha256", "internal bookkeeping");
    pack_file("config/yolo26.rknn", "RKNN current model");
    pack_file("config/coco.names", "person\n");
    pack_file("config/.extra.conf", ""); /* Real zero-byte/hidden config is retained. */
    pack_file("config/--keep.conf", "option-like file name");
    assert(!symlink("/etc/passwd", "config/outside"));
    char listing[4096];
    http_config_export(app, 0, "GET", "/api/config/export", "");
    assert(config_pack_calls == 1);
    pack_listing(listing, sizeof(listing));
    assert(strstr(listing, "config.txt\n") && strstr(listing, "yolo26.rknn\n"));
    assert(strstr(listing, "coco.names\n") && strstr(listing, ".extra.conf\n"));
    assert(strstr(listing, "--keep.conf\n"));
    assert(!strstr(listing, "model") && !strstr(listing, "outside"));
    assert(!access("config/model", F_OK) && !access("config/models", F_OK));

    /* A nonempty legacy folder can still contain the user's only old model
     * or conversion input. Export files without including empty directories. */
    assert(!mkdir("config/models/rk3576", 0700) && !mkdir("config/models/weights", 0700));
    assert(!mkdir("config/models/empty", 0700));
    pack_file("config/models/rk3576/old.rknn", "RKNN retained model");
    pack_file("config/models/weights/custom model.pt", "user conversion input");
    http_config_export(app, 0, "GET", "/api/config/export", "");
    assert(config_pack_calls == 2);
    pack_listing(listing, sizeof(listing));
    assert(strstr(listing, "models/rk3576/old.rknn\n"));
    assert(strstr(listing, "models/weights/custom model.pt\n"));
    assert(!strstr(listing, "empty") && !strstr(listing, "model/\n"));
    assert(!strstr(listing, "models/\n") && !strstr(listing, ".model-sync.lock"));
    assert(!strstr(listing, ".last-deployed.sha256"));

    /* The same recursive selection must enforce the size limit on weights. */
    int large = open("config/models/weights/too-large.pt", O_WRONLY | O_CREAT, 0600);
    assert(large >= 0 && !ftruncate(large, CONFIG_PACK_MAX + 1)); close(large);
    http_config_export(app, 0, "GET", "/api/config/export", "");
    assert(response_code == 413 && config_pack_calls == 2);
    assert(!access("config/models/rk3576/old.rknn", F_OK));
    assert(!access("config/models/weights/custom model.pt", F_OK));
    assert(!unlink("config/models/weights/too-large.pt"));
    assert(!unlink("config/models/rk3576/old.rknn"));
    assert(!unlink("config/models/weights/custom model.pt"));
    assert(!rmdir("config/models/rk3576") && !rmdir("config/models/weights"));
    assert(!rmdir("config/models/empty"));
    assert(!unlink("config/models/.model-sync.lock"));
    assert(!unlink("config/models/.last-deployed.sha256"));
    assert(!rmdir("config/models") && !rmdir("config/model"));
    assert(!unlink("config/yolo26.rknn") && !unlink("config/coco.names"));
    assert(!unlink("config/.extra.conf") && !unlink("config/--keep.conf"));
    assert(!unlink("config/outside") && !unlink("export.tar.gz"));
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
    app_ctx_t app = {.cfg = &cfg, .can = &can};
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
    const char *retired_transport[] = {
        "target=net&tcp_port=7777", "target=net&tcp_bind=eth7", "target=net&max_clients=12"
    };
    for (size_t i = 0; i < sizeof(retired_transport) / sizeof(retired_transport[0]); i++) {
        http_config_post(&app, 0, "POST", "/api/config", retired_transport[i]);
        assert(response_code == 400 && ip_calls == 0);
    }
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
    test_config_pack(&app);
    pthread_mutex_destroy(&app.can_mutex);
    assert(unlink(PATH_CONFIG) == 0 && rmdir("config") == 0);
    assert(fchdir(original_directory) == 0); close(original_directory); assert(rmdir(directory) == 0);
    puts("PASS: configuration persistence, ordered encoder rebuild and file-only configuration export");
    return 0;
}
