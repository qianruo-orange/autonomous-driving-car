/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * http_api_config.c — 运行时配置读写（通过 app_ctx_t 指针访问运行时状态）
 */

#include "http/http_internal.h"
#include "can/can_socket.h"
#include "tcp/tcp_server.h"
#include "video/video_stream.h"
#include "video/video_rec.h"
#include "watchdog/watchdog.h"
#include "tcp/net_ip.h"
#include "yolo/rknn_yolo.h"
#include <sys/epoll.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <spawn.h>
#include <sys/wait.h>

void http_config_get(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req)
{
    (void)method; (void)uri; (void)req;
    if (!app || !app->cfg || !app->can || !app->tcp) {
        http_err(fd, 500, "Error", NULL);
        return;
    }
    struct app_config_t *cfg = app->cfg;
    can_ctx_t *can = app->can;

    char json[4096]; int off = 0;

    pthread_mutex_lock(&app->can_mutex);
    int can_enabled = cfg->args.can_enabled;
    pthread_mutex_unlock(&app->can_mutex);
    JSON_ADD(json, off, "{\"can_enabled\":%s,\"cans\":{", can_enabled ? "true" : "false");
    for (int i = 0; i < can->count; i++) {
        can_iface_t *iface = &can->ifaces[i];
        JSON_ADD(json, off, "%s\"%s\":{\"bitrate\":%d,\"fd\":\"%s\",\"dbitrate\":%d,\"up\":\"%s\",\"filters\":[",
            i > 0 ? "," : "", iface->ifname,
            iface->bitrate, iface->fd_mode ? "on" : "off", iface->dbitrate,
            iface->up ? "on" : "off");
        for (int k = 0; k < iface->filter_count; k++)
            JSON_ADD(json, off, "%s{\"id\":\"%X\",\"mask\":\"%X\"}",
                k > 0 ? "," : "", iface->filters[k].id, iface->filters[k].mask);
        JSON_ADD(json, off, "]}");
    }
    JSON_ADD(json, off, "},");

    JSON_ADD(json, off, "\"http_port\":%d", HTTP_DEFAULT_PORT);

    /* Web management address belongs to the selected system interface. */
    const char *ip_if = cfg->args.ip_ifname[0] ? cfg->args.ip_ifname : "eth0";
    JSON_ADD(json, off, ",\"ip_ifname\":\"%s\",\"ip_mode\":\"%s\",\"ip_addr\":\"%s\",\"ip_mask\":\"%s\",\"ip_gw\":\"%s\"",
        cfg->args.ip_ifname, cfg->args.ip_mode, cfg->args.ip_addr,
        cfg->args.ip_mask, cfg->args.ip_gw);
    char cur_addr[32] = "", cur_mask[32] = "", cur_gw[32] = "";
    net_ip_get_current(ip_if, cur_addr, sizeof(cur_addr), cur_mask, sizeof(cur_mask), cur_gw, sizeof(cur_gw));
    JSON_ADD(json, off, ",\"ip_cur_if\":\"%s\",\"ip_cur_addr\":\"%s\",\"ip_cur_mask\":\"%s\",\"ip_cur_gw\":\"%s\"",
        ip_if, cur_addr, cur_mask, cur_gw);

    if (cfg->video_device[0]) JSON_ADD(json, off, ",\"video_device\":\"%s\"", cfg->video_device);
    JSON_ADD(json, off, ",\"video_width\":%d,\"video_height\":%d,\"video_fps\":%d",
             cfg->video_width, cfg->video_height, cfg->video_fps);
    JSON_ADD(json, off, ",\"video_codec\":\"%s\"", cfg->video_codec);
    JSON_ADD(json, off, ",\"video_decoder\":\"%s\"", cfg->video_decoder[0] ? cfg->video_decoder : "software");
    /* YOLO 检测配置（必要流程：推理不可关停，前端默认展示画框流） */
    JSON_ADD(json, off, ",\"yolo_model\":\"%s\",\"yolo_names\":\"%s\",\"yolo_input_size\":%d,"
        "\"yolo_conf\":%.2f,\"yolo_nms\":%.2f,\"yolo_interval_ms\":0,\"yolo_threads\":%d,\"yolo_threads_auto\":true",
        cfg->yolo_model, cfg->yolo_names, cfg->yolo_input_size,
        cfg->yolo_conf, cfg->yolo_nms, rknn_yolo_active_workers());
    JSON_ADD(json, off, "}");

    http_ok_json(fd, json, (size_t)off);
}

/* 表单字段容量：8 接口 × 5 基础项 + 8×16 过滤器 × 2 + 6 全局项 = 302，留余量 */
#define MAX_FORM_FIELDS 320

/* -- 各模块配置应用（按 target 独立触发，只处理本模块字段） -- */

/* CAN：解析并应用接口参数（含重新 configure/open），失败保持旧 socket 继续工作 */
static void apply_can(app_ctx_t *app, const http_form_field_t *fields, int count)
{
    can_ctx_t *can = app->can;

    /* Stop against the current descriptor/mode/up state before editing it. */
    if (app->cfg->args.can_enabled)
        can_motor_prepare_close_locked(app, NULL);

    for (int i = 0; i < CAN_MAX_IFACES; i++) {
        char kn[32];
        snprintf(kn, sizeof(kn), "ifname%d", i);
        const char *ifn = http_form_find(fields, count, kn);
        if (!ifn) continue;
        if (!ifname_valid(ifn)) { LOG_ERROR("config: invalid CAN ifname '%s'", ifn); continue; }
        int idx = can_iface_index(can->ifaces, can->count, ifn);
        if (idx < 0) {
            /* 表单新增了运行态中没有的 CAN 接口：动态加入，并在下方统一重配置 */
            if (can->count >= CAN_MAX_IFACES) {
                LOG_ERROR("config: cannot add CAN '%s', max %d ifaces", ifn, CAN_MAX_IFACES);
                continue;
            }
            idx = can->count++;
            can_iface_t *ni = &can->ifaces[idx];
            memset(ni, 0, sizeof(*ni));
            ni->sock_fd = -1;
            safe_strncpy(ni->ifname, sizeof(ni->ifname), ifn);
            ni->bitrate = 500000; ni->dbitrate = 2000000;
            ni->fd_mode = 1; ni->up = 1; ni->restart_ms = 0;
            /* 新增接口的收发队列必须初始化，否则 can_send/can_recv 任务访问未初始化队列 */
            can_queue_init(&ni->txq);
            can_queue_init(&ni->rxq);
            LOG_INFO("config: adding new CAN interface '%s' (runtime idx %d)", ifn, idx);
        }

        snprintf(kn, sizeof(kn), "bitrate%d", i);
        const char *v = http_form_find(fields, count, kn);
        if (v) can->ifaces[idx].bitrate = atoi(v);

        snprintf(kn, sizeof(kn), "fd%d", i);
        v = http_form_find(fields, count, kn);
        if (v) can->ifaces[idx].fd_mode = (strcmp(v, "on") == 0) ? 1 : 0;

        snprintf(kn, sizeof(kn), "dbitrate%d", i);
        v = http_form_find(fields, count, kn);
        if (v) can->ifaces[idx].dbitrate = atoi(v);

        snprintf(kn, sizeof(kn), "up%d", i);
        v = http_form_find(fields, count, kn);
        if (v) can->ifaces[idx].up = (strcmp(v, "on") == 0) ? 1 : 0;

        can->ifaces[idx].filter_count = 0;
        for (int f = 0; f < 16; f++) {
            char kn_id[32], kn_mask[32];
            snprintf(kn_id,   sizeof(kn_id),   "filter_id_%d_%d", i, f);
            snprintf(kn_mask, sizeof(kn_mask), "filter_mask_%d_%d", i, f);
            const char *fid  = http_form_find(fields, count, kn_id);
            const char *fmsk = http_form_find(fields, count, kn_mask);
            if (fid && fmsk) {
                unsigned int id, mask;
                if (sscanf(fid, "%x", &id) == 1 && sscanf(fmsk, "%x", &mask) == 1) {
                    can_filter_t *flt = &can->ifaces[idx].filters[can->ifaces[idx].filter_count++];
                    flt->id = (canid_t)id;
                    flt->mask = (canid_t)mask;
                }
            }
        }
    }

    /* Keep edited interface settings while the global feature is off. */
    if (!app->cfg->args.can_enabled) return;

    /* -- 应用 CAN 参数到系统接口 -- */
    for (int i = 0; i < can->count; i++) {
        can_iface_t *iface = &can->ifaces[i];
        int old_fd = iface->sock_fd;

        watchdog_feed_self("http");   /* 配置可能执行 ip 命令较慢，避免 HTTP 看门狗误杀 */

        /* 从 CAN 接收 epoll 移除旧 fd */
        if (old_fd >= 0 && can->recv_epfd >= 0)
            epoll_ctl(can->recv_epfd, EPOLL_CTL_DEL, old_fd, NULL);

        /* 重新配置并打开 */
        if (can_socket_configure(iface->ifname, iface->bitrate, iface->dbitrate,
                                  iface->fd_mode, iface->restart_ms, iface->up) < 0) {
            LOG_ERROR("config: CAN %s reconfigure failed", iface->ifname);
            if (old_fd >= 0 && can->recv_epfd >= 0)
                epoll_ctl(can->recv_epfd, EPOLL_CTL_ADD, old_fd, NULL);
            continue;
        }

        int new_fd = can_socket_open(iface->ifname, iface->fd_mode);
        if (new_fd < 0) {
            LOG_ERROR("config: CAN %s reopen failed", iface->ifname);
            /* 保持旧 socket 继续工作：重新注册到接收 epoll，
               否则该通道会静默失联（fd 有效但已不在 epoll 中） */
            if (old_fd >= 0 && can->recv_epfd >= 0) {
                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.u32 = (uint32_t)(i + 1);
                epoll_ctl(can->recv_epfd, EPOLL_CTL_ADD, old_fd, &ev);
            }
            continue;
        }

        if (can_socket_apply_filters(app, iface, new_fd)) {
            can_socket_close(new_fd);
            if (old_fd >= 0 && can->recv_epfd >= 0) {
                struct epoll_event ev = {.events=EPOLLIN, .data.u32=(uint32_t)(i+1)};
                epoll_ctl(can->recv_epfd, EPOLL_CTL_ADD, old_fd, &ev);
            }
            continue;
        }
        /* 关闭旧 socket，更新为新 fd */
        if (old_fd >= 0) can_socket_close(old_fd);
        iface->sock_fd = new_fd;

        /* 重新加入 CAN 接收 epoll */
        if (can->recv_epfd >= 0) {
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.u32 = (uint32_t)(i + 1);
            epoll_ctl(can->recv_epfd, EPOLL_CTL_ADD, new_fd, &ev);
        }
        LOG_INFO("config: CAN %s reconfigured (bitrate=%d, fd=%s, up=%s)",
                 iface->ifname, iface->bitrate,
                 iface->fd_mode ? "on" : "off",
                 iface->up ? "on" : "off");
    }
}

/* 视频：更新设备/分辨率/码率系数，返回 APPLY_VIDEO_* 位组合标识需要重启的部件 */
#define APPLY_VIDEO_CAPTURE  0x1   /* 采集参数变化：重启视频流 */
#define APPLY_VIDEO_ENCODER  0x2   /* 编码参数变化：滚动录制会话（编码器随会话创建） */

/* This build has no hardware capture decoder. Reject unsupported requests
   before applying any fields, including a mixed request with other changes. */
static int video_decoder_fields_valid(const http_form_field_t *fields, int count)
{
    for (int i = 0; i < count; i++)
        if (strcmp(fields[i].key, "video_decoder") == 0 &&
            strcmp(fields[i].val, "software") != 0) return 0;
    return 1;
}

static int apply_video(app_ctx_t *app, const http_form_field_t *fields, int count)
{
    struct app_config_t *cfg = app->cfg;
    int changed = 0;

    const char *decoder = http_form_find(fields, count, "video_decoder");
    if (decoder) safe_strncpy(cfg->video_decoder, sizeof(cfg->video_decoder), decoder);

    const char *v = http_form_find(fields, count, "video_device");
    if (v && strcmp(v, cfg->video_device) != 0) {
        changed |= APPLY_VIDEO_CAPTURE;
        safe_strncpy(cfg->video_device, sizeof(cfg->video_device), v);
    }
    v = http_form_find(fields, count, "video_width");
    if (v) {
        int nw = parse_int_clamped(v, 1, 4096, cfg->video_width > 0 ? cfg->video_width : 640);
        if (nw != cfg->video_width) { changed |= APPLY_VIDEO_CAPTURE; cfg->video_width = nw; }
    }
    v = http_form_find(fields, count, "video_height");
    if (v) {
        int nh = parse_int_clamped(v, 1, 4096, cfg->video_height > 0 ? cfg->video_height : 480);
        if (nh != cfg->video_height) { changed |= APPLY_VIDEO_CAPTURE; cfg->video_height = nh; }
    }
    v = http_form_find(fields, count, "video_fps");
    if (v) {
        int nf = parse_int_clamped(v, 0, 120, 0);   /* 0/空 = 驱动默认帧率 */
        if (nf != cfg->video_fps) { changed |= APPLY_VIDEO_CAPTURE; cfg->video_fps = nf; }
    }

    v = http_form_find(fields, count, "video_codec");
    if (v) {
        if (strcmp(v, "hardware") == 0 || strcmp(v, "software") == 0) {
            if (strcmp(v, cfg->video_codec) != 0) {
                changed |= APPLY_VIDEO_ENCODER;
                safe_strncpy(cfg->video_codec, sizeof(cfg->video_codec), v);
            }
        }
    }
    return changed;
}

/* Confidence/NMS changes reload the inference pool; concurrency is adaptive. */
static int apply_yolo(app_ctx_t *app, const http_form_field_t *fields, int count)
{
    struct app_config_t *cfg = app->cfg;
    int changed = 0;

    const char *v = http_form_find(fields, count, "yolo_conf");
    if (v) {
        float f = parse_int_clamped(v, 1, 100, 25) / 100.0f;
        if (f != cfg->yolo_conf) { cfg->yolo_conf = f; changed = 1; }
    }
    v = http_form_find(fields, count, "yolo_nms");
    if (v) {
        float f = parse_int_clamped(v, 1, 100, 45) / 100.0f;
        if (f != cfg->yolo_nms) { cfg->yolo_nms = f; changed = 1; }
    }
    return changed;
}

/* IP 设置：更新网卡静态/DHCP 地址。应用放后台线程延迟 ~300ms，
   先让 HTTP 响应发出，避免改 IP 切断当前连接导致浏览器收不到结果 */
static void apply_ip(app_ctx_t *app, const http_form_field_t *fields, int count)
{
    struct app_config_t *cfg = app->cfg;
    app_args_t *a = &cfg->args;

    const char *v = http_form_find(fields, count, "ip_ifname");
    char ifn[IFNAMSIZ] = "";
    if (v && v[0]) safe_strncpy(ifn, sizeof(ifn), v);
    else if (a->ip_ifname[0]) safe_strncpy(ifn, sizeof(ifn), a->ip_ifname);
    if (!ifn[0]) return;

    char mode[8] = "", addr[32] = "", mask[32] = "", gw[32] = "";
    v = http_form_find(fields, count, "ip_mode");
    if (v) safe_strncpy(mode, sizeof(mode), v);
    else   safe_strncpy(mode, sizeof(mode), a->ip_mode);
    v = http_form_find(fields, count, "ip_addr");
    if (v) safe_strncpy(addr, sizeof(addr), v);
    else   safe_strncpy(addr, sizeof(addr), a->ip_addr);
    v = http_form_find(fields, count, "ip_mask");
    if (v) safe_strncpy(mask, sizeof(mask), v);
    else   safe_strncpy(mask, sizeof(mask), a->ip_mask);
    v = http_form_find(fields, count, "ip_gw");
    if (v) safe_strncpy(gw, sizeof(gw), v);
    else   safe_strncpy(gw, sizeof(gw), a->ip_gw);

    if (strcmp(mode, "static") == 0 && (!addr[0] || !mask[0])) {
        LOG_ERROR("config: static IP requires address and netmask");
        return;
    }

    /* 持久化到运行时 + 配置文件 */
    safe_strncpy(a->ip_ifname, sizeof(a->ip_ifname), ifn);
    safe_strncpy(a->ip_mode,   sizeof(a->ip_mode),   mode);
    safe_strncpy(a->ip_addr,   sizeof(a->ip_addr),   addr);
    safe_strncpy(a->ip_mask,   sizeof(a->ip_mask),   mask);
    safe_strncpy(a->ip_gw,     sizeof(a->ip_gw),     gw);

    LOG_INFO("config: IP set %s mode=%s%s%s%s%s", ifn, mode[0] ? mode : "off",
             addr[0] ? " addr=" : "", addr,
             mask[0] ? " mask=" : "", mask,
             gw[0] ? " gw=" : "", gw);
    net_ip_apply_async(ifn, mode, addr, mask, gw);
}

void http_config_post(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body)
{
    (void)method; (void)uri;
    if (!body || !app || !app->cfg || !app->can || !app->tcp) {
        http_err(fd, 400, "Bad Request", NULL);
        return;
    }

    /* 整个配置应用过程加锁：串行化并发 POST，并与 CAN 重连互斥 */
    pthread_mutex_lock(&app->can_mutex);

    /* 解析 URL 编码表单（公共实现，自动跳过 HTTP 头） */
    http_form_field_t fields[MAX_FORM_FIELDS];
    int count = http_form_parse(body, fields, MAX_FORM_FIELDS);
    if (count == MAX_FORM_FIELDS)
        LOG_ERROR("config POST: fields exceed %d, tail ignored", MAX_FORM_FIELDS);

    LOG_INFO("config POST: parsed %d fields", count);

    /* target 指定只应用并重启哪个模块；缺省为全量（向后兼容） */
    const char *target = http_form_find(fields, count, "target");

    if (target && strcmp(target, "can_feature") == 0) {
        const char *value = http_form_find(fields, count, "can_enabled");
        if (!value || (strcmp(value, "on") && strcmp(value, "off"))) {
            pthread_mutex_unlock(&app->can_mutex);
            http_err(fd, 400, "Bad Request", "can_enabled must be on or off");
            return;
        }
        if (can_feature_set_locked(app, !strcmp(value, "on")) < 0) {
            pthread_mutex_unlock(&app->can_mutex);
            http_err(fd, 503, "Service Unavailable", "Unable to enable CAN interfaces");
            return;
        }
        config_save(app);
        pthread_mutex_unlock(&app->can_mutex);
        http_ok_text(fd, "saved");
        return;
    }

    if ((!target || strcmp(target, "all") == 0 || strcmp(target, "video") == 0) &&
        !video_decoder_fields_valid(fields, count)) {
        http_err(fd, 400, "Bad Request", "Only software video decoding is available");
        pthread_mutex_unlock(&app->can_mutex);
        return;
    }

    /* Retired transport settings must never reopen a second management port. */
    if (http_form_find(fields, count, "tcp_port") ||
        http_form_find(fields, count, "tcp_bind") ||
        http_form_find(fields, count, "max_clients")) {
        pthread_mutex_unlock(&app->can_mutex);
        http_err(fd, 400, "Bad Request", "Web management uses fixed HTTP port 80");
        return;
    }
    if (!target || strcmp(target, "all") == 0 || strcmp(target, "can") == 0)
        apply_can(app, fields, count);
    if (!target || strcmp(target, "all") == 0 || strcmp(target, "ip") == 0 || strcmp(target, "net") == 0)
        apply_ip(app, fields, count);

    int vid_changed = 0;
    if (!target || strcmp(target, "all") == 0 || strcmp(target, "video") == 0) {
        vid_changed = apply_video(app, fields, count);
        /* 单独保存视频模块：点击即期望立即生效，参数未变化也重启一次视频流 */
        if (target && strcmp(target, "video") == 0) vid_changed |= APPLY_VIDEO_CAPTURE;
    }

    int yolo_changed = 0;
    if (!target || strcmp(target, "all") == 0 || strcmp(target, "yolo") == 0)
        yolo_changed = apply_yolo(app, fields, count);

    LOG_INFO("config: applied to runtime, saving to file");
    config_save(app);
    http_ok_text(fd, "saved");

    /* The recording and live stream share a session-owned encoder. Rebuild it
       before capture changes, otherwise every frame at the new dimensions is
       rejected by the old encoder and reported as an inference stall. */
    if (vid_changed & (APPLY_VIDEO_CAPTURE | APPLY_VIDEO_ENCODER))
        video_rec_cycle_session();
    if (vid_changed & APPLY_VIDEO_CAPTURE) video_stream_restart();
    /* YOLO 参数变更后热重载推理池（停旧池 → 重读配置重建），无需重启服务 */
    if (yolo_changed) rknn_yolo_reload();

    pthread_mutex_unlock(&app->can_mutex);
}

/* ---- 配置目录打包下载（/api/config/export） ---- */

#define CONFIG_PACK_MAX (64UL * 1024 * 1024)  /* config 打包体积上限（含模型 ~7.5MB，留足余量） */

/* config 目录 = PATH_CONFIG 的 dirname（如 config/config.txt → config） */
static void config_dir_of(char *out, size_t out_size)
{
    safe_strncpy(out, out_size, PATH_CONFIG);
    char *slash = strrchr(out, '/');
    if (!slash) {                       /* 纯文件名：当前目录 */
        safe_strncpy(out, out_size, ".");
    } else if (slash == out) {          /* 根目录下 */
        safe_strncpy(out, out_size, "/");
    } else {
        *slash = '\0';
    }
}

/* Write only regular-file names to the NUL-delimited tar manifest. Empty legacy
 * directories have no entries, while retained models/weights remain exportable.
 * Return 1 for the size limit, -1 for an I/O error. Takes ownership of dir_fd. */
static int config_pack_files(int dir_fd, const char *prefix, FILE *files,
                             uint64_t *total, unsigned depth)
{
    DIR *directory = fdopendir(dir_fd);
    if (!directory) { close(dir_fd); return -1; }
    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(directory);
        if (!de) {
            if (errno) result = -1;
            break;
        }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") ||
            !strcmp(de->d_name, ".model-sync.lock") ||
            !strcmp(de->d_name, ".last-deployed.sha256")) continue;
        char relative[PATH_MAX];
        int length = snprintf(relative, sizeof(relative), "%s%s%s", prefix,
                              *prefix ? "/" : "", de->d_name);
        if (length < 0 || (size_t)length >= sizeof(relative)) { result = -1; break; }
        struct stat st;
        if (fstatat(dir_fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) { result = -1; break; }
        if (S_ISDIR(st.st_mode)) {
            if (depth >= 64) { result = -1; break; }
            int child = openat(dir_fd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { result = -1; break; }
            result = config_pack_files(child, relative, files, total, depth + 1);
            if (result) break;
        } else if (S_ISREG(st.st_mode)) {
            if ((uint64_t)st.st_size > CONFIG_PACK_MAX - *total) { result = 1; break; }
            *total += (uint64_t)st.st_size;
            if (fwrite(relative, (size_t)length + 1, 1, files) != 1) { result = -1; break; }
        }
    }
    closedir(directory);
    return result;
}

static int config_pack_tar(const char *config_dir, const char *archive, FILE *files)
{
    if (fflush(files) || fseek(files, 0, SEEK_SET)) return -1;
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) return -1;
    error = posix_spawn_file_actions_adddup2(&actions, fileno(files), STDIN_FILENO);
    if (!error)
        error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    char *args[] = {"tar", "--null", "--verbatim-files-from", "--no-recursion", "-czf",
                    (char *)archive, "-C", (char *)config_dir, "-T", "-", NULL};
    pid_t child;
    extern char **environ;
    if (!error) error = posix_spawnp(&child, "tar", &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error) return -1;
    int status, waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    return waited >= 0 && WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}

/* 打包下载整个 config 目录（tar.gz）：与日志打包相同模式——
   先打成临时文件，再复用 http_serve_stream 流式发送（发送后自动清理） */
static void serve_config_pack(int fd, const char *config_dir)
{
    FILE *files = tmpfile();
    int dir_fd = open(config_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!files || dir_fd < 0) {
        if (files) fclose(files);
        if (dir_fd >= 0) close(dir_fd);
        http_err(fd, 500, "Internal Error", NULL);
        return;
    }
    uint64_t total = 0;
    int result = config_pack_files(dir_fd, "", files, &total, 0);
    if (result) {
        fclose(files);
        if (result > 0)
            http_err(fd, 413, "Payload Too Large", "config too large");
        else
            http_err(fd, 500, "Internal Error", NULL);
        return;
    }
    char tmppath[] = "/tmp/rockchip_config_pack_XXXXXX";
    int archive_fd = mkstemp(tmppath);
    int failed = archive_fd < 0;
    if (archive_fd >= 0) {
        close(archive_fd);
        failed = config_pack_tar(config_dir, tmppath, files);
    }
    fclose(files);
    if (failed) {
        if (archive_fd >= 0) unlink(tmppath);
        http_err(fd, 500, "Internal Error", NULL);
        return;
    }

    FILE *fp = fopen(tmppath, "rb");
    if (!fp) {
        unlink(tmppath);
        http_err(fd, 500, "Internal Error", NULL);
        return;
    }
    size_t size = http_file_size(fp);

    http_serve_stream(fd, "application/gzip",
                      "Content-Disposition: attachment; filename=\"config_pack.tar.gz\"\r\n",
                      fp, size, tmppath);
}

/* /api/config/export：打包下载整个 config 目录
   （config.txt / DBC / 标签 / RKNN 模型一并导出） */
void http_config_export(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf)
{
    (void)app; (void)method; (void)uri; (void)req_buf;
    char config_dir[256];
    config_dir_of(config_dir, sizeof(config_dir));
    serve_config_pack(fd, config_dir);
}
