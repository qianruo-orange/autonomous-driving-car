/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "agent/agent_hw.h"

#include <grp.h>
#include <stdarg.h>
#include <math.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "control/car_sport_ctrl.h"
#include "agent/agent_view.h"
#include "core/log.h"
#include "http/http_internal.h"

/* Hard ceiling for anything a credential can ask for. The control worker
 * enforces the same numbers again; this copy exists so that an out-of-range
 * request is refused before it reaches the real-time thread. */
#define AGENT_V_HARD         0.30
#define AGENT_STEER_HARD     0.15
#define AGENT_BUDGET_HARD    100.0
#define AGENT_BUDGET_DEFAULT 20.0
#define AGENT_TTL_S_MAX      3600
#define AGENT_MIN_INTERVAL   40u    /* 25 Hz command ceiling per credential */
#define AGENT_AUDIT_WINDOW   393216 /* tail window for GET /api/agent/audit */

typedef struct {
    char token[AGENT_TOKEN_HEX + 1];
    unsigned scope;
    char label[24];
    double v_max, steer_max, budget_m;
    double used_m;
    unsigned long long issued_ms, expires_ms, last_cmd_ms;
    int live;
} cap_t;

static struct {
    pthread_mutex_t lock;
    cap_t caps[AGENT_TOKENS];
    int armed;
    char armed_token[AGENT_TOKEN_HEX + 1];
    unsigned long long armed_ms;
    unsigned long long cmds, rejected, stops, faults;
    char last_reject[40];
    unsigned long long last_reject_ms;
} s;

/* ---- small helpers -------------------------------------------------------
 * The module deliberately avoids printf-family token parsing and any use of
 * strcmp on secrets; the token comparison below is length-checked and then
 * accumulated, so a partial match cannot short-circuit. */
static unsigned long long now_ms(void) { return monotonic_ms(); }

static void json_escape(const char *in, char *out, size_t size)
{
    size_t o = 0;
    if (!out || !size) return;
    out[0] = 0;
    for (const char *p = in; p && *p && o + 6 < size; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20 || c > 0x7e) {
            out[o++] = '?';
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

static int token_eq(const char *a, const char *b)
{
    if (!a || !b || strlen(a) != AGENT_TOKEN_HEX || strlen(b) != AGENT_TOKEN_HEX) return 0;
    unsigned char diff = 0;
    for (int i = 0; i < AGENT_TOKEN_HEX; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

static int token_generate(char *out)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[AGENT_TOKEN_HEX / 2];
    int ok = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        ok = fread(raw, 1, sizeof(raw), f) == sizeof(raw);
        fclose(f);
    }
    if (!ok) {
        /* Refuse to mint a predictable credential: an unreadable entropy source
         * is a configuration fault, not something to paper over. */
        LOG_ERROR("agent: /dev/urandom unavailable, refusing to issue a credential");
        return -1;
    }
    for (size_t i = 0; i < sizeof(raw); i++) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 15];
    }
    out[AGENT_TOKEN_HEX] = 0;
    return 0;
}

/* Header lookup over the raw request buffer; returns 0 when absent. */
static int req_header(const char *req, const char *name, char *out, size_t size)
{
    if (!req || !name || !out || size < 2) return 0;
    size_t nl = strlen(name);
    const char *p = strstr(req, "\r\n");
    if (p) {
        p += 2; /* skip past the request line */
    } else {
        p = strchr(req, '\n');
        if (!p) return 0;
        p++;
    }
    while (*p) {
        if (*p == '\r' || *p == '\n') break;
        const char *eol = strstr(p, "\r\n");
        if (!eol) eol = strchr(p, '\n');
        if (!eol) break;
        if ((size_t)(eol - p) > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            size_t len = (size_t)(eol - v);
            if (len >= size) len = size - 1;
            memcpy(out, v, len);
            out[len] = 0;
            return len > 0;
        }
        p = eol + (eol[0] == '\r' ? 2 : 1);
    }
    return 0;
}

/* 拼接 JSON 片段：snprintf 在截断时返回"本来想写的长度"，把它当偏移加回去会让
 * 下一次 sizeof - off 下溢成巨大的 size_t。只在完整写入时前进偏移。 */
static int jadd(char *buf, size_t size, int off, const char *fmt, ...)
{
    if (off < 0 || (size_t)off >= size) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, size - (size_t)off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size - (size_t)off) return -1;
    return off + n;
}

/* 任何一个片段放不下就整体 500，绝不把半截 JSON 发出去。 */
#define CAT(fd, json, off, ...)                                                        \
    do {                                                                               \
        int _n = jadd(json, sizeof(json), off, __VA_ARGS__);                            \
        if (_n < 0) {                                                                  \
            http_err(fd, 500, "Internal Server Error", "observe response too large");   \
            return;                                                                    \
        }                                                                              \
        off = _n;                                                                      \
    } while (0)

static void audit_path(char *out, size_t size)
{
    time_t now = time(NULL);
    struct tm parts;
    if (!localtime_r(&now, &parts)) {
        snprintf(out, size, AGENT_DIR "/audit.jsonl");
        return;
    }
    strftime(out, size, AGENT_DIR "/audit-%Y%m%d.jsonl", &parts);
}

/* One JSON object per accepted or refused operation. This is the only artefact
 * that survives a restart, so it carries the full argument set and verdict.
 *
 * 文件句柄常开：这条路径在 8 Hz 的指令流上，逐条 fopen/fclose 会把一个实时
 * 控制循环的旁路变成每秒 8 次 open+write+close。每 AUDIT_FLUSH_LINES 行或
 * 超过 1 s 才 fflush，崩溃时最多丢这一段。 */
#define AUDIT_FLUSH_LINES 64
static struct {
    FILE   *fp;
    int     day;
    int     lines;
    time_t  last_flush;
} g_audit;
static pthread_mutex_t g_audit_lock = PTHREAD_MUTEX_INITIALIZER;

static int audit_day(struct tm *parts)
{
    return (parts->tm_year + 1900) * 10000 + (parts->tm_mon + 1) * 100 + parts->tm_mday;
}

static void audit(const char *tool, const char *actor, const char *args, const char *result,
                  const char *reason)
{
    time_t now = time(NULL);
    struct tm parts;
    if (!localtime_r(&now, &parts)) return;
    int day = audit_day(&parts);

    char a_args[320], a_reason[96], a_actor[48];
    json_escape(args ? args : "", a_args, sizeof(a_args));
    json_escape(reason ? reason : "", a_reason, sizeof(a_reason));
    json_escape(actor ? actor : "-", a_actor, sizeof(a_actor));
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &parts);

    pthread_mutex_lock(&g_audit_lock);
    if (g_audit.fp && g_audit.day != day) {
        fclose(g_audit.fp);
        g_audit.fp = NULL;
    }
    if (!g_audit.fp) {
        char path[96];
        audit_path(path, sizeof(path));
        g_audit.fp = fopen(path, "a");
        g_audit.day = day;
        g_audit.lines = 0;
        if (!g_audit.fp) { pthread_mutex_unlock(&g_audit_lock); return; }
    }
    fprintf(g_audit.fp,
            "{\"ts\":\"%s\",\"mono_ms\":%llu,\"tool\":\"%s\",\"actor\":\"%s\",\"args\":\"%s\","
            "\"result\":\"%s\",\"reason\":\"%s\"}\n",
            ts, now_ms(), tool, a_actor, a_args, result, a_reason);
    g_audit.lines++;
    if (g_audit.lines >= AUDIT_FLUSH_LINES || now - g_audit.last_flush >= 1) {
        fflush(g_audit.fp);
        g_audit.lines = 0;
        g_audit.last_flush = now;
    }
    pthread_mutex_unlock(&g_audit_lock);
}

static void actor_of(const cap_t *cap, char *out, size_t size)
{
    snprintf(out, size, "%s:%.6s", cap->label[0] ? cap->label : "cap", cap->token);
}

/* ---- credential table (caller holds s.lock) ---- */
static int cap_find(const char *token)
{
    for (int i = 0; i < AGENT_TOKENS; i++)
        if (s.caps[i].live && token_eq(s.caps[i].token, token)) return i;
    return -1;
}

static void cap_reclaim(void)
{
    unsigned long long now = now_ms();
    for (int i = 0; i < AGENT_TOKENS; i++) {
        if (!s.caps[i].live) continue;
        if (s.caps[i].expires_ms && now > s.caps[i].expires_ms) {
            memset(&s.caps[i], 0, sizeof(s.caps[i]));
        }
    }
}

static int cap_alloc(void)
{
    for (int i = 0; i < AGENT_TOKENS; i++)
        if (!s.caps[i].live) return i;
    /* Evict the credential that expires soonest. */
    int victim = -1;
    unsigned long long best = 0;
    for (int i = 0; i < AGENT_TOKENS; i++) {
        if (victim < 0 || s.caps[i].expires_ms < best) {
            victim = i;
            best = s.caps[i].expires_ms;
        }
    }
    if (victim >= 0) memset(&s.caps[victim], 0, sizeof(s.caps[victim]));
    return victim;
}

/* Resolve a credential from the request: X-Agent-Cap header, else bearer. */
static int cap_from_request(const char *req, unsigned need, cap_t *out)
{
    char token[AGENT_TOKEN_HEX + 1] = "";
    char raw[64];
    if (req_header(req, "X-Agent-Cap", raw, sizeof(raw))) {
        if (strlen(raw) == AGENT_TOKEN_HEX) memcpy(token, raw, AGENT_TOKEN_HEX + 1);
    } else {
        char auth[128];
        if (req_header(req, "Authorization", auth, sizeof(auth)) &&
            !strncasecmp(auth, "Bearer ", 7) && strlen(auth + 7) == AGENT_TOKEN_HEX)
            memcpy(token, auth + 7, AGENT_TOKEN_HEX + 1);
    }
    if (strlen(token) != AGENT_TOKEN_HEX) return -1;
    pthread_mutex_lock(&s.lock);
    int i = cap_find(token);
    if (i < 0 || (s.caps[i].scope & need) != need) {
        pthread_mutex_unlock(&s.lock);
        return -1;
    }
    if (s.caps[i].expires_ms && now_ms() > s.caps[i].expires_ms) {
        pthread_mutex_unlock(&s.lock);
        return -1;
    }
    *out = s.caps[i];
    pthread_mutex_unlock(&s.lock);
    return i;
}

/* 视觉通道与执行通道共用一个凭证表：读像素和打方向盘的授权边界是同一件事，
 * 差别只在 scope 位上。 */
int agent_cap_authorize(const char *req, unsigned need)
{
    cap_t cap;
    return cap_from_request(req, need, &cap) < 0 ? -1 : 0;
}

/* ---- body helpers ---- */
static int num_field(const http_form_field_t *f, int n, const char *key, double def, double *out)
{
    const char *v = http_form_find(f, n, key);
    if (!v) {
        *out = def;
        return 0;
    }
    char *end = NULL;
    errno = 0;
    double parsed = strtod(v, &end);
    if (errno || !end || *end || !isfinite(parsed)) return -1;
    *out = parsed;
    return 0;
}

static void send_state_json(int fd)
{
    char json[1024];
    int off = 0;
    CAT(fd, json, off, "{\"armed\":%s,", s.armed ? "true" : "false");
    CAT(fd, json, off,
                    "\"armed_token\":\"%.6s\",\"cmds\":%llu,\"rejected\":%llu,\"stops\":%llu,"
                    "\"faults\":%llu,\"last_reject\":\"%s\",\"caps\":[",
                    s.armed ? s.armed_token : "", s.cmds, s.rejected, s.stops, s.faults,
                    s.last_reject);
    int first = 1;
    for (int i = 0; i < AGENT_TOKENS && off < (int)sizeof(json) - 200; i++) {
        if (!s.caps[i].live) continue;
        CAT(fd, json, off,
                        "%s{\"label\":\"%s\",\"mask\":\"%.6s\",\"scope\":%u,\"v_max\":%.3f,"
                        "\"steer_max\":%.3f,\"budget_m\":%.1f,\"used_m\":%.2f,\"ttl_s\":%llu}",
                        first ? "" : ",", s.caps[i].label, s.caps[i].token, s.caps[i].scope,
                        s.caps[i].v_max, s.caps[i].steer_max, s.caps[i].budget_m,
                        s.caps[i].used_m,
                        (s.caps[i].expires_ms - now_ms()) / 1000);
        first = 0;
    }
    CAT(fd, json, off, "]}");
    http_ok_json(fd, json, (size_t)off);
}

/* ---- POST /api/agent/token (root) ---- */
void http_agent_token(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body)
{
    (void)app;
    (void)method;
    (void)uri;
    http_form_field_t f[16];
    int n = http_form_parse(body, f, 16);
    if (n <= 0) {
        http_err(fd, 400, "Bad Request", "expected form fields: scope, ttl_s, v_max, steer_max, budget_m, label");
        return;
    }
    const char *scope_s = http_form_find(f, n, "scope");
    if (!scope_s) scope_s = "observe";
    unsigned scope = 0;
    if (strstr(scope_s, "observe") || strstr(scope_s, "all")) scope |= AGENT_SCOPE_OBSERVE;
    if (strstr(scope_s, "motion") || strstr(scope_s, "all")) scope |= AGENT_SCOPE_MOTION | AGENT_SCOPE_OBSERVE;
    if (strstr(scope_s, "drive")) scope |= AGENT_SCOPE_MOTION | AGENT_SCOPE_OBSERVE;
    if (!scope) {
        http_err(fd, 400, "Bad Request", "unknown scope; use observe or motion");
        return;
    }

    double ttl_s, v_max, steer_max, budget_m;
    if (num_field(f, n, "ttl_s", 900, &ttl_s) < 0 || num_field(f, n, "v_max", 0.15, &v_max) < 0 ||
        num_field(f, n, "steer_max", 0.10, &steer_max) < 0 ||
        num_field(f, n, "budget_m", AGENT_BUDGET_DEFAULT, &budget_m) < 0) {
        http_err(fd, 400, "Bad Request", "numeric field is not a finite number");
        return;
    }
    if (ttl_s < 30 || ttl_s > AGENT_TTL_S_MAX) {
        http_err(fd, 400, "Bad Request", "ttl_s must be within 30..3600");
        return;
    }
    if (v_max <= 0 || v_max > AGENT_V_HARD) {
        http_err(fd, 400, "Bad Request", "v_max must be within 0..0.30 m/s (hard cap)");
        return;
    }
    if (steer_max < 0 || steer_max > AGENT_STEER_HARD) {
        http_err(fd, 400, "Bad Request", "steer_max must be within 0..0.15 (hard cap)");
        return;
    }
    if (budget_m <= 0 || budget_m > AGENT_BUDGET_HARD) {
        http_err(fd, 400, "Bad Request", "budget_m must be within 0..100 (hard cap)");
        return;
    }

    char label[24] = "cap";
    const char *label_s = http_form_find(f, n, "label");
    if (label_s) {
        size_t o = 0;
        for (const char *p = label_s; *p && o < sizeof(label) - 1; p++)
            if (isalnum((unsigned char)*p) || *p == '-' || *p == '_') label[o++] = *p;
        label[o] = 0;
        if (!o) snprintf(label, sizeof(label), "cap");
    }

    char token[AGENT_TOKEN_HEX + 1];
    if (token_generate(token) < 0) {
        http_err(fd, 503, "Service Unavailable", "entropy source unavailable");
        return;
    }

    pthread_mutex_lock(&s.lock);
    cap_reclaim();
    int slot = cap_alloc();
    if (slot < 0) {
        pthread_mutex_unlock(&s.lock);
        http_err(fd, 507, "Insufficient Storage", "credential table full");
        return;
    }
    cap_t *cap = &s.caps[slot];
    memset(cap, 0, sizeof(*cap));
    snprintf(cap->token, sizeof(cap->token), "%s", token);
    snprintf(cap->label, sizeof(cap->label), "%s", label);
    cap->scope = scope;
    cap->v_max = v_max;
    cap->steer_max = steer_max;
    cap->budget_m = budget_m;
    cap->issued_ms = now_ms();
    cap->expires_ms = cap->issued_ms + (unsigned long long)(ttl_s * 1000);
    cap->live = 1;
    unsigned long long expires_ms = cap->expires_ms;
    pthread_mutex_unlock(&s.lock);

    int persist = 0;
    const char *p = http_form_find(f, n, "persist");
    if (p && (!strcmp(p, "1") || !strcasecmp(p, "true"))) {
        /* Opt-in handoff to the on-board executor: adc-agent reads this file
         * when $AGENT_CAP is empty. It lives under recordings/ because that
         * directory is both writable at runtime and excluded from Git — the
         * previous location (config/) was one `git add -A` away from being
         * committed. Group-readable by the service account, never world. */
        char path[96];
        snprintf(path, sizeof(path), "%s/token", AGENT_DIR);
        FILE *out = fopen(path, "w");
        if (out) {
            fprintf(out, "%s\n", token);
            fclose(out);
            (void)chmod(path, 0640);
            struct group *gr = getgrnam(AGENT_SERVICE_GROUP);
            if (gr) (void)chown(path, 0, gr->gr_gid);
            persist = 1;
        }
    }

    audit("token.issue", label, scope_s, "ok", "");
    char json[640];
    int off = snprintf(json, sizeof(json),
                       "{\"token\":\"%s\",\"scope\":\"%s\",\"ttl_s\":%d,\"v_max\":%.3f,"
                       "\"steer_max\":%.3f,\"budget_m\":%.1f,\"expires_in_ms\":%llu,"
                       "\"persisted\":%s,\"arm_required\":true,"
                       "\"next\":\"POST /api/agent/arm (root Basic) with token=<token>\"}",
                       token, (scope & AGENT_SCOPE_MOTION) ? "motion+observe" : "observe",
                       (int)ttl_s, v_max, steer_max, budget_m, expires_ms - now_ms(),
                       persist ? "true" : "false");
    http_ok_json(fd, json, (size_t)off);
}

/* ---- POST /api/agent/revoke (root) ---- */
void http_agent_revoke(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body)
{
    (void)app;
    (void)method;
    (void)uri;
    http_form_field_t f[8];
    int n = http_form_parse(body, f, 8);
    const char *token = http_form_find(f, n, "token");
    int all = 0;
    const char *a = http_form_find(f, n, "all");
    if (a && (!strcmp(a, "1") || !strcasecmp(a, "true"))) all = 1;
    if (!all && (!token || strlen(token) != AGENT_TOKEN_HEX)) {
        http_err(fd, 400, "Bad Request", "provide token=<32 hex> or all=1");
        return;
    }
    int dropped = 0, was_armed = 0;
    pthread_mutex_lock(&s.lock);
    for (int i = 0; i < AGENT_TOKENS; i++) {
        if (!s.caps[i].live) continue;
        if (all || token_eq(s.caps[i].token, token)) {
            if (s.armed && token_eq(s.armed_token, s.caps[i].token)) was_armed = 1;
            memset(&s.caps[i], 0, sizeof(s.caps[i]));
            dropped++;
        }
    }
    if (was_armed) {
        s.armed = 0;
        memset(s.armed_token, 0, sizeof(s.armed_token));
    }
    pthread_mutex_unlock(&s.lock);

    if (was_armed) {
        follow_agent_cancel(1);
        follow_force_disable("agent_revoked");
        s.stops++;
    }
    audit("token.revoke", all ? "all" : token, all ? "all=1" : "token", dropped ? "ok" : "miss", "");
    char json[128];
    int off = snprintf(json, sizeof(json), "{\"revoked\":%d,\"stopped\":%s}", dropped,
                       was_armed ? "true" : "false");
    http_ok_json(fd, json, (size_t)off);
}

/* ---- POST /api/agent/arm (root) ---- */
void http_agent_arm(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body)
{
    (void)app;
    (void)method;
    (void)uri;
    http_form_field_t f[8];
    int n = http_form_parse(body, f, 8);
    const char *token = http_form_find(f, n, "token");
    if (!token || strlen(token) != AGENT_TOKEN_HEX) {
        http_err(fd, 400, "Bad Request", "token=<32 hex> is required");
        return;
    }
    if (!follow_profile_enabled()) {
        /* Two independent human switches: the follow profile must be enabled
         * from the console before a credential can be armed. */
        http_err(fd, 409, "Conflict", "enable the follow profile first (POST /api/follow enabled=1)");
        return;
    }
    pthread_mutex_lock(&s.lock);
    int i = cap_find(token);
    if (i < 0 || !(s.caps[i].scope & AGENT_SCOPE_MOTION)) {
        pthread_mutex_unlock(&s.lock);
        http_err(fd, 403, "Forbidden", "unknown credential or missing motion scope");
        return;
    }
    s.armed = 1;
    snprintf(s.armed_token, sizeof(s.armed_token), "%s", token);
    s.armed_ms = now_ms();
    char actor[48];
    actor_of(&s.caps[i], actor, sizeof(actor));
    pthread_mutex_unlock(&s.lock);

    /* A fault latched by a previous session is cleared only here. */
    follow_agent_reset();
    audit("agent.arm", actor, "", "ok", "");
    send_state_json(fd);
}

/* ---- POST /api/agent/disarm ---- */
void http_agent_disarm(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body)
{
    (void)app;
    (void)method;
    (void)uri;
    cap_t cap;
    if (cap_from_request(body, AGENT_SCOPE_MOTION, &cap) < 0) {
        http_err(fd, 401, "Unauthorized", "missing or invalid X-Agent-Cap");
        return;
    }
    pthread_mutex_lock(&s.lock);
    int was_armed = s.armed;
    s.armed = 0;
    memset(s.armed_token, 0, sizeof(s.armed_token));
    pthread_mutex_unlock(&s.lock);
    follow_agent_cancel(1);
    s.stops++;
    char actor[48];
    actor_of(&cap, actor, sizeof(actor));
    audit("agent.disarm", actor, was_armed ? "armed" : "idle", "ok", "");
    send_state_json(fd);
}

/* ---- POST /api/agent/motion ---- */
void http_agent_motion(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body)
{
    (void)app;
    (void)method;
    (void)uri;
    cap_t cap;
    if (cap_from_request(body, AGENT_SCOPE_MOTION, &cap) < 0) {
        http_err(fd, 401, "Unauthorized", "missing or invalid X-Agent-Cap");
        return;
    }
    http_form_field_t f[8];
    int n = http_form_parse(body, f, 8);
    double v, steer, ttl;
    if (n <= 0 || num_field(f, n, "v", 0, &v) < 0 || num_field(f, n, "steer", 0, &steer) < 0 ||
        num_field(f, n, "ttl_ms", 300, &ttl) < 0) {
        s.rejected++;
        snprintf(s.last_reject, sizeof(s.last_reject), "malformed command");
        audit("motion", "cap", "", "reject", "malformed command");
        http_err(fd, 400, "Bad Request", "expected v, steer, ttl_ms");
        return;
    }
    if (v < 0 || v > cap.v_max || fabs(steer) > cap.steer_max) {
        s.rejected++;
        snprintf(s.last_reject, sizeof(s.last_reject), "outside credential envelope");
        audit("motion", "cap", "", "reject", "outside envelope");
        http_err(fd, 422, "Unprocessable Entity",
                 "v or steer outside this credential's envelope (see /api/agent/state)");
        return;
    }
    if (ttl < 100 || ttl > 500) {
        s.rejected++;
        audit("motion", "cap", "", "reject", "bad ttl");
        http_err(fd, 400, "Bad Request", "ttl_ms must be within 100..500");
        return;
    }
    pthread_mutex_lock(&s.lock);
    int armed = s.armed && token_eq(s.armed_token, cap.token);
    unsigned long long since = now_ms() - cap.last_cmd_ms;
    if (armed) {
        for (int i = 0; i < AGENT_TOKENS; i++)
            if (s.caps[i].live && token_eq(s.caps[i].token, cap.token)) s.caps[i].last_cmd_ms = now_ms();
    }
    pthread_mutex_unlock(&s.lock);
    if (!armed) {
        s.rejected++;
        snprintf(s.last_reject, sizeof(s.last_reject), "credential not armed");
        audit("motion", "cap", "", "reject", "not armed");
        http_err(fd, 403, "Forbidden", "credential is not armed; an operator must POST /api/agent/arm");
        return;
    }
    if (since < AGENT_MIN_INTERVAL) {
        s.rejected++;
        audit("motion", "cap", "", "reject", "rate limited");
        http_err(fd, 429, "Too Many Requests", "command rate above 25 Hz");
        return;
    }

    /* 快照用的是 trylock，控制线程在 1 ms tick 里时必然失败；它的临界区只有
     * 几十纳秒，重试几次就能拿到。直接把一条合法指令当"控制器启动中"丢弃，
     * 会让执行器把整条计划判成传输失败。 */
    agent_motion_t st;
    int snap = -1;
    for (int i = 0; i < 3 && snap < 0; i++) {
        snap = follow_agent_snapshot(&st);
        if (snap < 0) {
            struct timespec pause = {.tv_nsec = 1000000};
            nanosleep(&pause, NULL);
        }
    }
    if (snap < 0) {
        http_err(fd, 503, "Service Unavailable", "controller busy; retry");
        return;
    }
    if (st.traveled_m >= cap.budget_m) {
        /* The leash is spent: stop and retire the credential rather than
         * letting the agent decide how much further it may go. */
        pthread_mutex_lock(&s.lock);
        s.armed = 0;
        memset(s.armed_token, 0, sizeof(s.armed_token));
        s.stops++;
        pthread_mutex_unlock(&s.lock);
        follow_agent_cancel(1);
        follow_force_disable("agent_budget_exhausted");
        audit("motion", "cap", "", "reject", "budget exhausted");
        http_err(fd, 409, "Conflict", "distance budget exhausted; credential disarmed");
        return;
    }

    int rc = follow_agent_cmd(v, steer, (unsigned)ttl);
    /* 应答里的 state/reason 是"接受那一刻"的快照：真正的执行结果由 1 ms 后的
     * 控制 tick 决定，故障会在下一次 observe 里以 fault 形式出现。 */
    char args[96], actor[48];
    actor_of(&cap, actor, sizeof(actor));
    snprintf(args, sizeof(args), "v=%.3f steer=%.3f ttl=%ums", v, steer, (unsigned)ttl);
    if (rc != 0) {
        s.rejected++;
        if (rc == -3 && st.state == AGENT_MOTION_FAULT) s.faults++;
        const char *why = rc == -2 ? "outside control-layer envelope"
                          : rc == -3 ? "profile disabled or fault latched"
                                    : "controller unavailable";
        snprintf(s.last_reject, sizeof(s.last_reject), "%s", why);
        audit("motion", actor, args, "reject", why);
        http_err(fd, rc == -3 ? 409 : 422, "Rejected", why);
        return;
    }
    s.cmds++;
    audit("motion", actor, args, "ok", "");

    char json[400];
    int off = snprintf(json, sizeof(json),
                       "{\"accepted\":true,\"state\":\"%s\",\"v\":%.3f,\"steer\":%.3f,"
                       "\"expires_in_ms\":%llu,\"traveled_m\":%.2f,\"budget_m\":%.1f,"
                       "\"reason\":\"%s\",\"actuation\":\"%s\"}",
                       st.state == AGENT_MOTION_FAULT ? "fault" : "active", v, steer,
                       (unsigned long long)ttl, st.traveled_m, cap.budget_m, st.reason,
                       st.actuation_ready ? "live" : "blocked");
    http_ok_json(fd, json, (size_t)off);
}

/* ---- POST /api/agent/stop ---- */
void http_agent_stop(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body)
{
    (void)app;
    (void)method;
    (void)uri;
    cap_t cap;
    /* 停车会关掉整个 profile，是执行动作，不是观测动作：只读凭证不该有
     * 把车踢出自动驾驶模式的能力。紧急停车另有一个 estop 端点，它只要求
     * observe，因为让车停下永远是安全方向。 */
    if (cap_from_request(body, AGENT_SCOPE_MOTION, &cap) < 0) {
        http_err(fd, 401, "Unauthorized", "missing or invalid X-Agent-Cap");
        return;
    }
    pthread_mutex_lock(&s.lock);
    /* Stopping is always allowed for a valid credential, but it also retires
     * the arming so that a stuck caller cannot keep pulsing afterwards. */
    int armed = s.armed && token_eq(s.armed_token, cap.token);
    if (armed) {
        s.armed = 0;
        memset(s.armed_token, 0, sizeof(s.armed_token));
    }
    pthread_mutex_unlock(&s.lock);
    follow_agent_cancel(1);
    s.stops++;
    char actor[48];
    actor_of(&cap, actor, sizeof(actor));
    audit("agent.stop", actor, armed ? "armed" : "idle", "ok", "");
    send_state_json(fd);
}

/* ---- POST /api/agent/release ----
 * 与 stop 的区别只有一件事：不关 profile。一条计划跑完后 release，操作员的
 * follow 开关保持原样，下一段计划可以直接续上；stop 是"我不要它继续动了"。 */
void http_agent_release(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body)
{
    (void)app;
    (void)method;
    (void)uri;
    cap_t cap;
    if (cap_from_request(body, AGENT_SCOPE_MOTION, &cap) < 0) {
        http_err(fd, 401, "Unauthorized", "missing or invalid X-Agent-Cap");
        return;
    }
    follow_agent_cancel(0);
    char actor[48];
    actor_of(&cap, actor, sizeof(actor));
    audit("agent.release", actor, "", "ok", "lease cleared, profile kept");
    send_state_json(fd);
}

/* ---- POST /api/agent/estop ---- */
void http_agent_estop(app_ctx_t *app, int fd, const char *method, const char *uri, const char *body)
{
    (void)app;
    (void)method;
    (void)uri;
    cap_t cap;
    if (cap_from_request(body, AGENT_SCOPE_OBSERVE, &cap) < 0) {
        http_err(fd, 401, "Unauthorized", "missing or invalid X-Agent-Cap");
        return;
    }
    pthread_mutex_lock(&s.lock);
    s.armed = 0;
    memset(s.armed_token, 0, sizeof(s.armed_token));
    pthread_mutex_unlock(&s.lock);
    follow_agent_cancel(1);
    /* Emergency stop also drops the operator's enable flag: nothing moves
     * again until a human re-enables the profile. */
    follow_force_disable("agent_estop");
    s.stops++;
    char actor[48];
    actor_of(&cap, actor, sizeof(actor));
    audit("agent.estop", actor, (cap.scope & AGENT_SCOPE_MOTION) ? "scope=motion" : "scope=observe",
          "ok", "profile disabled");
    send_state_json(fd);
}

/* ---- GET /api/agent/observe ---- */
void http_agent_observe(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req)
{
    (void)app;
    (void)method;
    (void)uri;
    cap_t cap;
    if (cap_from_request(req, AGENT_SCOPE_OBSERVE, &cap) < 0) {
        http_err(fd, 401, "Unauthorized", "missing or invalid X-Agent-Cap");
        return;
    }
    follow_telemetry_t tel;
    int have_tel = follow_telemetry_snapshot(&tel) == 0;
    agent_motion_t st;
    int have_agent = follow_agent_snapshot(&st) == 0;
    char json[9216];
    int off = 0;
    CAT(fd, json, off, "{\"ts_ms\":%llu,", now_ms());
    CAT(fd, json, off,
                    "\"profile\":{\"enabled\":%s,\"reason\":\"%s\"},",
                    have_agent && st.enabled ? "true" : "false", have_agent ? st.reason : "");
    if (have_tel) {
        char spd[24], act[24], str[24], sact[24];
        if (isnan(tel.speed_actual))
            snprintf(act, sizeof(act), "null");
        else
            snprintf(act, sizeof(act), "%.3f", tel.speed_actual);
        if (isnan(tel.steering_actual))
            snprintf(sact, sizeof(sact), "null");
        else
            snprintf(sact, sizeof(sact), "%.3f", tel.steering_actual);
        snprintf(spd, sizeof(spd), "%.3f", tel.speed_target);
        snprintf(str, sizeof(str), "%.3f", tel.steering_target);
        CAT(fd, json, off,
                        "\"motion\":{\"speed_target\":%s,\"steer_target\":%s,\"speed_actual\":%s,"
                        "\"steer_actual\":%s,\"wheel_feedback\":%s},",
                        spd, str, act, sact, tel.feedback_valid ? "true" : "false");
    } else {
        CAT(fd, json, off, "\"motion\":null,");
    }
    CAT(fd, json, off,
                    "\"agent\":{\"armed\":%s,\"state\":\"%s\",\"expires_in_ms\":%llu,"
                    "\"traveled_m\":%.2f,\"budget_m\":%.1f,\"fault\":\"%s\"},",
                    s.armed ? "true" : "false",
                    have_agent ? (st.state == AGENT_MOTION_FAULT ? "fault"
                                 : st.state == AGENT_MOTION_ACTIVE ? "active" : "idle")
                               : "idle",
                    have_agent ? st.expires_in_ms : 0ULL, have_agent ? st.traveled_m : 0.0,
                    cap.budget_m, have_agent ? st.fault : "");
    CAT(fd, json, off,
                    "\"actuation\":{\"motor_bound\":%s,\"can_enabled\":%s,\"ready\":%s},",
                    have_agent && st.motor_bound ? "true" : "false",
                    have_agent && st.can_enabled ? "true" : "false",
                    have_agent && st.actuation_ready ? "true" : "false");
    /* The caller's own envelope, so a local executor can refuse an oversized
     * plan before it ever reaches the control loop. */
    CAT(fd, json, off,
                    "\"limits\":{\"v_max\":%.3f,\"steer_max\":%.3f,\"budget_m\":%.1f,"
                    "\"expires_in_ms\":%llu},",
                    cap.v_max, cap.steer_max, cap.budget_m,
                    cap.expires_ms > now_ms() ? cap.expires_ms - now_ms() : 0ULL);
    /* 一次调用同时给"能不能动"和"看见了什么"：只读状态下也能跑完整感知回路。 */
    CAT(fd, json, off, "\"perception\":");
    int p = agent_perception_json(json, sizeof(json), off);
    if (p < 0) {
        http_err(fd, 500, "Internal Server Error", "perception snapshot too large");
        return;
    }
    off = p;
    CAT(fd, json, off, "}");
    http_ok_json(fd, json, (size_t)off);
}

/* ---- GET /api/agent/state (root) ---- */
void http_agent_state(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req)
{
    (void)app;
    (void)method;
    (void)uri;
    (void)req;
    pthread_mutex_lock(&s.lock);
    cap_reclaim();
    send_state_json(fd);
    pthread_mutex_unlock(&s.lock);
}

/* ---- GET /api/agent/audit (root) ---- */
void http_agent_audit(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req)
{
    (void)app;
    (void)method;
    (void)uri;
    char path[96];
    audit_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) {
        http_err(fd, 404, "Not Found", "no audit log for today");
        return;
    }
    long size = (long)http_file_size(f);
    long start = size > AGENT_AUDIT_WINDOW ? size - AGENT_AUDIT_WINDOW : 0;
    if (fseek(f, start, SEEK_SET) != 0) {
        fclose(f);
        http_err(fd, 500, "Internal Server Error", "seek failed");
        return;
    }
    size_t cap = (size_t)(size - start);
    char *buf = malloc(cap + 1);
    if (!buf) {
        fclose(f);
        http_err(fd, 500, "Internal Server Error", "out of memory");
        return;
    }
    size_t got = fread(buf, 1, cap, f);
    fclose(f);
    buf[got] = 0;
    if (start > 0) {
        /* Drop the partial first line. */
        char *nl = strchr(buf, '\n');
        if (nl) {
            got -= (size_t)(nl + 1 - buf);
            memmove(buf, nl + 1, got + 1);
        }
    }
    http_send_response(fd, 200, "OK", "application/x-ndjson", buf, got);
    free(buf);
}

/* ---- lifecycle ---- */
int agent_hw_init(void *app)
{
    (void)app;
    memset(&s, 0, sizeof(s));
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&s.lock, &ma);
    pthread_mutexattr_destroy(&ma);
    (void)mkdir(PATH_RECORDINGS, 0755);
    (void)mkdir(AGENT_DIR, 0755);
    LOG_INFO("agent: capability layer ready (no credential issued, nothing armed)");
    return 0;
}

void agent_hw_revoke_all(const char *reason)
{
    pthread_mutex_lock(&s.lock);
    memset(s.caps, 0, sizeof(s.caps));
    s.armed = 0;
    memset(s.armed_token, 0, sizeof(s.armed_token));
    pthread_mutex_unlock(&s.lock);
    audit("token.revoke", "system", "all=1", "ok", reason ? reason : "shutdown");
}

void agent_hw_destroy(void *app)
{
    (void)app;
    agent_hw_revoke_all("shutdown");
    pthread_mutex_destroy(&s.lock);
}
