/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * codex-bridge.c — C11 port of scripts/codex_bridge.py.
 *
 * One-shot helper for the C HTTP service: every invocation writes a JSON
 * result to --out and exits.  Long-running work (one chat turn, one
 * device-code login) is detached into a background child, so the HTTP worker
 * never blocks on a model.
 *
 * User supplied text is always passed through --input files, never argv.
 *
 * The process normally runs as root (systemd service); every CLI spawn is
 * dropped to the unprivileged account first.  Only libc + POSIX are used;
 * network access goes through popen("curl ...").
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CLI_USER        "qianruo"
#define CLI_HOME        "/home/qianruo"
#define DEFAULT_ROOT    "/opt/autonomous-driving-car"
#define FORCED_SANDBOX  "danger-full-access"
#define CLI_PATH_DEF    "/home/qianruo/.local/bin/codex"
#define CLI_PATH_ENV    "/usr/local/bin:/usr/bin:/bin:/home/qianruo/.local/bin"
#define UPDATE_COMMAND  "codex update"
#define UPDATE_TOTAL_DEFAULT 187844332LL
#define APP_SERVER_TIMEOUT_MS 20000
#define UPDATE_TIMEOUT_S 600
#define MODELS_TTL 3600
#define QUOTA_TTL 60
#define PROXY_CONF "/etc/proxy-client.conf"

/* ------------------------------------------------------------------ */
/* small string buffer                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *p;
    size_t n;
    size_t cap;
} sb_t;

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("codex-bridge: out of memory\n", stderr);
        _exit(70);
    }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("codex-bridge: out of memory\n", stderr);
        _exit(70);
    }
    return q;
}

static void sb_init(sb_t *s)
{
    s->p = NULL;
    s->n = 0;
    s->cap = 0;
}

static void sb_reserve(sb_t *s, size_t extra)
{
    if (s->n + extra + 1 <= s->cap)
        return;
    size_t cap = s->cap ? s->cap : 256;
    while (cap < s->n + extra + 1)
        cap *= 2;
    s->p = xrealloc(s->p, cap);
    s->cap = cap;
}

static void sb_addn(sb_t *s, const void *data, size_t n)
{
    /* a single append is never larger than the biggest pipe buffer */
    if (!n || n > ((size_t)1 << 32))
        return;
    sb_reserve(s, n);
    memcpy(s->p + s->n, data, n);
    s->n += n;
    s->p[s->n] = 0;
}

static void sb_adds(sb_t *s, const char *str)
{
    sb_addn(s, str, strlen(str));
}

static void sb_addc(sb_t *s, char c)
{
    sb_addn(s, &c, 1);
}

static void sb_addf(sb_t *s, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof(tmp)) {
        sb_addn(s, tmp, (size_t)n);
        return;
    }
    sb_reserve(s, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(s->p + s->n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    s->n += (size_t)n;
}

/* Deep copy of a byte range into a fresh NUL terminated string. */
static char *xstrndup(const char *s, size_t n)
{
    char *r = xmalloc(n + 1);
    if (n)
        memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

static const char *sb_cstr(sb_t *s)
{
    if (!s->p)
        sb_reserve(s, 0);
    s->p[s->n] = 0;
    return s->p;
}

static char *sb_take(sb_t *s)
{
    sb_cstr(s);
    char *p = s->p;
    s->p = NULL;
    s->n = s->cap = 0;
    return p;
}

static void sb_free(sb_t *s)
{
    free(s->p);
    s->p = NULL;
    s->n = s->cap = 0;
}

static void sb_reset(sb_t *s)
{
    s->n = 0;
    if (s->p)
        s->p[0] = 0;
}

/* ------------------------------------------------------------------ */
/* error handling: Python raises, main() catches and writes {"ok":false} */
/* ------------------------------------------------------------------ */

static jmp_buf g_raise_jmp;
static char    g_raise_msg[1200];

static void raise_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_raise_msg, sizeof(g_raise_msg), fmt, ap);
    va_end(ap);
    longjmp(g_raise_jmp, 1);
}

/* "[Errno 2] No such file or directory: '/x'" — same text Python uses. */
static void raise_oserror(int err, const char *path)
{
    if (path)
        raise_msg("OSError: [Errno %d] %s: '%s'", err, strerror(err), path);
    raise_msg("OSError: [Errno %d] %s", err, strerror(err));
}

/* ------------------------------------------------------------------ */
/* paths                                                               */
/* ------------------------------------------------------------------ */

static char P_ROOT[PATH_MAX];
static char P_CX[PATH_MAX];
static char P_REC[PATH_MAX];
static char P_SETTINGS[PATH_MAX];
static char P_STATE[PATH_MAX];
static char P_LOGIN[PATH_MAX];
static char P_AUTH[PATH_MAX];
static char P_TOML[PATH_MAX];
static char P_OUT[PATH_MAX];
static char P_INBOX[PATH_MAX];

static void pj(char *dst, size_t size, const char *a, const char *b)
{
    size_t al = strlen(a), bl = strlen(b);
    if (!size)
        return;
    if (al > size - 1)
        al = size - 1;
    memcpy(dst, a, al);
    size_t n = al;
    if (n + 1 < size)
        dst[n++] = '/';
    if (bl > size - n - 1)
        bl = size - n - 1;
    memcpy(dst + n, b, bl);
    dst[n + bl] = 0;
}

static void init_paths(void)
{
    const char *root = getenv("CODEX_WEB_ROOT");
    if (!root || !*root)
        root = DEFAULT_ROOT;
    snprintf(P_ROOT, sizeof(P_ROOT), "%s", root);
    pj(P_CX, sizeof(P_CX), P_ROOT, ".codex");
    pj(P_REC, sizeof(P_REC), P_ROOT, "recordings/codex");
    pj(P_SETTINGS, sizeof(P_SETTINGS), P_CX, "settings.json");
    pj(P_STATE, sizeof(P_STATE), P_CX, "state.json");
    pj(P_LOGIN, sizeof(P_LOGIN), P_CX, "login_state.json");
    pj(P_AUTH, sizeof(P_AUTH), P_CX, "auth.json");
    pj(P_TOML, sizeof(P_TOML), P_CX, "config.toml");
    pj(P_OUT, sizeof(P_OUT), P_CX, "out");
    pj(P_INBOX, sizeof(P_INBOX), P_CX, "inbox");
}

/* ------------------------------------------------------------------ */
/* time                                                                */
/* ------------------------------------------------------------------ */

static long long now_epoch(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec;
}

static long long now_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void fmt_localtime(long long t, const char *fmt, char *out, size_t size)
{
    time_t tt = (time_t)t;
    struct tm tm;
    if (!localtime_r(&tt, &tm)) {
        if (size)
            out[0] = 0;
        return;
    }
    strftime(out, size, fmt, &tm);
}

/* datetime.now().astimezone().isoformat(timespec="seconds") */
static void now_iso(char *out, size_t size)
{
    time_t tt = time(NULL);
    struct tm tm;
    if (!localtime_r(&tt, &tm)) {
        if (size)
            out[0] = 0;
        return;
    }
    char base[32];
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);
    long off = tm.tm_gmtoff;
    char sign = off < 0 ? '-' : '+';
    if (off < 0)
        off = -off;
    snprintf(out, size, "%s%c%02ld:%02ld", base, sign, off / 3600,
             (off % 3600) / 60);
}

/* "23:18:38 你：..." stamp from an ISO timestamp, else the current clock. */
static void rec_stamp(const char *ts, char *out, size_t size)
{
    /* datetime.fromisoformat() accepts YYYY-MM-DD[T ]HH:MM[:SS[.ffffff]]. */
    const char *p = ts ? ts : "";
    size_t len = strlen(p);
    if (len >= 16 && isdigit((unsigned char)p[0]) &&
        isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2]) &&
        isdigit((unsigned char)p[3]) && p[4] == '-' && p[7] == '-' &&
        (p[10] == 'T' || p[10] == ' ') && p[13] == ':') {
        if (len >= 19 && p[16] == ':' && isdigit((unsigned char)p[17])) {
            snprintf(out, size, "%.8s", p + 11);
            return;
        }
        snprintf(out, size, "%.5s:00", p + 11);
        return;
    }
    fmt_localtime(now_epoch(), "%H:%M:%S", out, size);
}

/* ------------------------------------------------------------------ */
/* filesystem                                                          */
/* ------------------------------------------------------------------ */

static int file_exists(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0;
}

static long long file_size(const char *path, int *ok)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        if (ok)
            *ok = 0;
        return 0;
    }
    if (ok)
        *ok = 1;
    return (long long)st.st_size;
}

static int read_all(const char *path, sb_t *out)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    char buf[8192];
    ssize_t r;
    while ((r = read(fd, buf, sizeof(buf))) > 0)
        sb_addn(out, buf, (size_t)r);
    int err = (r < 0) ? errno : 0;
    close(fd);
    if (err) {
        errno = err;
        return -1;
    }
    return 0;
}

/* Write to <path>.tmp, fsync and rename — same order the Python version uses. */
static int write_atomic(const char *path, const char *data, size_t len, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            int err = errno;
            close(fd);
            unlink(tmp);
            errno = err;
            return -1;
        }
        off += (size_t)w;
    }
    fsync(fd);
    close(fd);
    if (rename(tmp, path) != 0) {
        int err = errno;
        unlink(tmp);
        errno = err;
        return -1;
    }
    chmod(path, mode);
    return 0;
}

/* mkdir -p; returns 1 when the leaf was created, 0 when it already existed. */
static int mkdir_p(const char *path, mode_t mode)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : -1;

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = 0;

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            *p = '/';
            return -1;
        }
        *p = '/';
    }
    if (mkdir(tmp, mode) != 0) {
        if (errno == EEXIST)
            return 0;
        return -1;
    }
    chmod(tmp, mode);
    return 1;
}

static void chown_cli_user(const char *path)
{
    if (geteuid() != 0)
        return;
    struct passwd *pw = getpwnam(CLI_USER);
    if (!pw)
        return;
    if (chown(path, pw->pw_uid, pw->pw_gid) != 0) {
        /* best effort, exactly like the Python side */
    }
}

/* basename() without the libgen include ordering trap */
static const char *base_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}
/* ------------------------------------------------------------------ */
/* JSON — small DOM: parse, build, dump.                              */
/*                                                                    */
/* Numbers keep the literal text they were parsed from, so values the */
/* bridge only passes through (quota percentages, thread ids, sizes)  */
/* are re-emitted byte for byte, exactly like Python's json module.    */
/* ------------------------------------------------------------------ */

typedef enum { J_NULL, J_BOOL, J_INT, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jval jval;

typedef struct {
    char *key;
    jval *val;
} jpair;

struct jval {
    jtype    t;
    int      b;        /* J_BOOL */
    char    *s;        /* J_STR text, or the literal source of J_INT/J_NUM */
    jval   **items;    /* J_ARR */
    size_t   n, cap;
    jpair   *pairs;    /* J_OBJ */
    size_t   pn, pcap;
};

static jval *jnew(jtype t)
{
    jval *v = xmalloc(sizeof(*v));
    memset(v, 0, sizeof(*v));
    v->t = t;
    return v;
}

static jval *jnull(void)
{
    return jnew(J_NULL);
}

static jval *jbool(int b)
{
    jval *v = jnew(J_BOOL);
    v->b = b ? 1 : 0;
    return v;
}

static jval *jint(long long n)
{
    jval *v = jnew(J_INT);
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lld", n);
    v->s = xstrdup(tmp);
    return v;
}

/* number from an already formatted literal ("1.5", "0") */
static jval *jnumtext(const char *lit)
{
    jval *v = jnew(strpbrk(lit, ".eE") ? J_NUM : J_INT);
    v->s = xstrdup(lit);
    return v;
}

static jval *jstr(const char *s)
{
    jval *v = jnew(J_STR);
    v->s = xstrdup(s ? s : "");
    return v;
}

/* Object key that owns a copy of s. */
static jval *jstr_own(char *s)
{
    jval *v = jnew(J_STR);
    v->s = s;
    return v;
}

static jval *jarr(void)
{
    return jnew(J_ARR);
}

static jval *jobj(void)
{
    return jnew(J_OBJ);
}

static void ja_add(jval *a, jval *v)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4;
        a->items = xrealloc(a->items, a->cap * sizeof(*a->items));
    }
    a->items[a->n++] = v;
}

static jval *jo(jval *o, const char *key)
{
    if (!o || o->t != J_OBJ || !key)
        return NULL;
    for (size_t i = 0; i < o->pn; i++)
        if (!strcmp(o->pairs[i].key, key))
            return o->pairs[i].val;
    return NULL;
}

/* Set (replace in place, keeping insertion order — like a Python dict). */
static void jo_set(jval *o, const char *key, jval *v)
{
    if (!o || o->t != J_OBJ)
        return;
    for (size_t i = 0; i < o->pn; i++) {
        if (!strcmp(o->pairs[i].key, key)) {
            o->pairs[i].val = v;
            return;
        }
    }
    if (o->pn == o->pcap) {
        o->pcap = o->pcap ? o->pcap * 2 : 8;
        o->pairs = xrealloc(o->pairs, o->pcap * sizeof(*o->pairs));
    }
    o->pairs[o->pn].key = xstrdup(key);
    o->pairs[o->pn].val = v;
    o->pn++;
}

static void jo_setstr(jval *o, const char *key, const char *s)
{
    jo_set(o, key, jstr(s));
}

static void jo_setint(jval *o, const char *key, long long n)
{
    jo_set(o, key, jint(n));
}

static void jo_setbool(jval *o, const char *key, int b)
{
    jo_set(o, key, jbool(b));
}

/* dict.pop(key) */
static jval *jo_pop(jval *o, const char *key)
{
    if (!o || o->t != J_OBJ)
        return NULL;
    for (size_t i = 0; i < o->pn; i++) {
        if (!strcmp(o->pairs[i].key, key)) {
            jval *v = o->pairs[i].val;
            memmove(&o->pairs[i], &o->pairs[i + 1],
                    (o->pn - i - 1) * sizeof(*o->pairs));
            o->pn--;
            return v;
        }
    }
    return NULL;
}

static jval *jclone(const jval *v)
{
    if (!v)
        return NULL;
    jval *r = jnew(v->t);
    r->b = v->b;
    if (v->s)
        r->s = xstrdup(v->s);
    if (v->t == J_ARR)
        for (size_t i = 0; i < v->n; i++)
            ja_add(r, jclone(v->items[i]));
    if (v->t == J_OBJ)
        for (size_t i = 0; i < v->pn; i++)
            jo_set(r, v->pairs[i].key, jclone(v->pairs[i].val));
    return r;
}

/* Python truthiness of a decoded value. */
static int jtruthy(const jval *v)
{
    if (!v || v->t == J_NULL)
        return 0;
    switch (v->t) {
    case J_BOOL: return v->b != 0;
    case J_INT:
    case J_NUM:  return strtod(v->s, NULL) != 0.0;
    case J_STR:  return v->s && v->s[0];
    case J_ARR:  return v->n != 0;
    case J_OBJ:  return v->pn != 0;
    default:     return 0;
    }
}

static const char *js(jval *o, const char *key, const char *def)
{
    jval *v = jo(o, key);
    if (v && v->t == J_STR)
        return v->s;
    return def;
}

/* int(value) for the shapes the bridge meets; ok reports success. */
static long long jtoint(jval *v, int *ok)
{
    if (ok)
        *ok = 0;
    if (!v)
        return 0;
    if (v->t == J_BOOL) {
        if (ok)
            *ok = 1;
        return v->b;
    }
    if (v->t == J_INT || v->t == J_NUM || v->t == J_STR) {
        const char *s = v->s ? v->s : "";
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
            s++;
        errno = 0;
        char *end = NULL;
        long long n = strtoll(s, &end, 10);
        if (end == s || errno == ERANGE)
            return 0;
        const char *t = end;
        while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r')
            t++;
        if (*t)
            return 0;
        if (ok)
            *ok = 1;
        return n;
    }
    return 0;
}

/* -------------------------------------------------------------- */
/* parser                                                          */
/* -------------------------------------------------------------- */

typedef struct {
    const char *s;
    size_t      n;
    size_t      i;
    int         depth;
} jparser;

static void jp_ws(jparser *p)
{
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->i++;
        else
            break;
    }
}

static jval *jp_value(jparser *p);

static int jp_hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void utf8_put(sb_t *out, unsigned cp)
{
    char b[4];
    if (cp < 0x80) {
        b[0] = (char)cp;
        sb_addn(out, b, 1);
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        sb_addn(out, b, 2);
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        sb_addn(out, b, 3);
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        sb_addn(out, b, 4);
    }
}

static int jp_utf16(jparser *p, unsigned *out)
{
    if (p->i + 6 > p->n || p->s[p->i] != '\\' || p->s[p->i + 1] != 'u')
        return -1;
    unsigned cp = 0;
    for (int k = 0; k < 4; k++) {
        int h = jp_hex(p->s[p->i + 2 + k]);
        if (h < 0)
            return -1;
        cp = (cp << 4) | (unsigned)h;
    }
    p->i += 6;
    *out = cp;
    return 0;
}

static char *jp_string(jparser *p)
{
    if (p->i >= p->n || p->s[p->i] != '"')
        raise_msg("JSONDecodeError: Expecting '\"'");
    p->i++;
    sb_t out;
    sb_init(&out);
    sb_reserve(&out, 0);
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == '"') {
            p->i++;
            return sb_take(&out);
        }
        if (c == '\\') {
            p->i++;
            if (p->i >= p->n)
                break;
            char e = p->s[p->i++];
            switch (e) {
            case '"':  sb_addc(&out, '"');  break;
            case '\\': sb_addc(&out, '\\'); break;
            case '/':  sb_addc(&out, '/');  break;
            case 'b':  sb_addc(&out, '\b'); break;
            case 'f':  sb_addc(&out, '\f'); break;
            case 'n':  sb_addc(&out, '\n'); break;
            case 'r':  sb_addc(&out, '\r'); break;
            case 't':  sb_addc(&out, '\t'); break;
            case 'u': {
                p->i--;             /* jp_utf16 expects the backslash */
                unsigned cp = 0;
                if (jp_utf16(p, &cp) != 0)
                    raise_msg("JSONDecodeError: Invalid \\uXXXX escape");
                if (cp >= 0xD800 && cp <= 0xDBFF && p->i + 6 <= p->n &&
                    p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                    jparser save = *p;
                    unsigned lo = 0;
                    if (jp_utf16(p, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else
                        *p = save;
                }
                utf8_put(&out, cp);
                break;
            }
            default:
                raise_msg("JSONDecodeError: Invalid escape");
            }
            continue;
        }
        if (c < 0x20)
            raise_msg("JSONDecodeError: Invalid control character");
        sb_addc(&out, (char)c);
        p->i++;
    }
    raise_msg("JSONDecodeError: Unterminated string");
    return NULL;
}

static jval *jp_object(jparser *p)
{
    jval *o = jobj();
    p->i++;                     /* '{' */
    jp_ws(p);
    if (p->i < p->n && p->s[p->i] == '}') {
        p->i++;
        return o;
    }
    for (;;) {
        jp_ws(p);
        if (p->i >= p->n)
            raise_msg("JSONDecodeError: Expecting property name");
        char *key = jp_string(p);
        jp_ws(p);
        if (p->i >= p->n || p->s[p->i] != ':')
            raise_msg("JSONDecodeError: Expecting ':' delimiter");
        p->i++;
        jval *v = jp_value(p);
        jo_set(o, key, v);
        free(key);
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') {
            p->i++;
            continue;
        }
        if (p->i < p->n && p->s[p->i] == '}') {
            p->i++;
            return o;
        }
        raise_msg("JSONDecodeError: Expecting ',' delimiter");
    }
}

static jval *jp_array(jparser *p)
{
    jval *a = jarr();
    p->i++;                     /* '[' */
    jp_ws(p);
    if (p->i < p->n && p->s[p->i] == ']') {
        p->i++;
        return a;
    }
    for (;;) {
        ja_add(a, jp_value(p));
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') {
            p->i++;
            continue;
        }
        if (p->i < p->n && p->s[p->i] == ']') {
            p->i++;
            return a;
        }
        raise_msg("JSONDecodeError: Expecting ',' delimiter");
    }
}

static jval *jp_number(jparser *p)
{
    size_t start = p->i;
    if (p->i < p->n && (p->s[p->i] == '-' || p->s[p->i] == '+'))
        p->i++;
    while (p->i < p->n && isdigit((unsigned char)p->s[p->i]))
        p->i++;
    if (p->i < p->n && p->s[p->i] == '.') {
        p->i++;
        while (p->i < p->n && isdigit((unsigned char)p->s[p->i]))
            p->i++;
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '-' || p->s[p->i] == '+'))
            p->i++;
        while (p->i < p->n && isdigit((unsigned char)p->s[p->i]))
            p->i++;
    }
    if (p->i == start)
        raise_msg("JSONDecodeError: Expecting value");
    char *lit = xstrndup(p->s + start, p->i - start);
    jval *v = jnumtext(lit);
    free(lit);
    return v;
}

static int jp_match(jparser *p, const char *word)
{
    size_t len = strlen(word);
    if (p->i + len > p->n || memcmp(p->s + p->i, word, len) != 0)
        return 0;
    p->i += len;
    return 1;
}

static jval *jp_value(jparser *p)
{
    if (++p->depth > 200)
        raise_msg("JSONDecodeError: Too deeply nested");
    jp_ws(p);
    if (p->i >= p->n)
        raise_msg("JSONDecodeError: Expecting value: line 1 column 1 (char 0)");
    char c = p->s[p->i];
    jval *v = NULL;
    if (c == '{')
        v = jp_object(p);
    else if (c == '[')
        v = jp_array(p);
    else if (c == '"')
        v = jstr_own(jp_string(p));
    else if (c == 't') {
        if (!jp_match(p, "true"))
            raise_msg("JSONDecodeError: Expecting value");
        v = jbool(1);
    } else if (c == 'f') {
        if (!jp_match(p, "false"))
            raise_msg("JSONDecodeError: Expecting value");
        v = jbool(0);
    } else if (c == 'n') {
        if (!jp_match(p, "null"))
            raise_msg("JSONDecodeError: Expecting value");
        v = jnull();
    } else if (c == '-' || c == '+' || isdigit((unsigned char)c)) {
        v = jp_number(p);
    } else {
        raise_msg("JSONDecodeError: Expecting value: line 1 column %zu",
                  p->i + 1);
    }
    p->depth--;
    return v;
}

static jval *json_parse(const char *text, size_t len)
{
    jparser p = { text, len, 0, 0 };
    jval *v = jp_value(&p);
    jp_ws(&p);
    if (p.i != p.n)
        raise_msg("JSONDecodeError: Extra data");
    return v;
}

/* read_json(path, fallback): any failure yields a copy of the fallback. */
static jval *read_json(const char *path, jval *fallback)
{
    sb_t buf;
    sb_init(&buf);
    int rc = read_all(path, &buf);
    if (rc != 0) {
        sb_free(&buf);
        return jclone(fallback);
    }
    jval *v = NULL;
    if (buf.n == 0) {
        sb_free(&buf);
        return jclone(fallback);
    }
    /* parse defensively: a corrupt cache must never take the bridge down */
    jmp_buf save;
    memcpy(&save, &g_raise_jmp, sizeof(save));
    char savemsg[sizeof(g_raise_msg)];
    memcpy(savemsg, g_raise_msg, sizeof(savemsg));
    if (setjmp(g_raise_jmp) == 0) {
        v = json_parse(sb_cstr(&buf), buf.n);
    } else {
        v = NULL;
    }
    memcpy(&g_raise_jmp, &save, sizeof(save));
    memcpy(g_raise_msg, savemsg, sizeof(savemsg));
    sb_free(&buf);
    return v ? v : jclone(fallback);
}

/* -------------------------------------------------------------- */
/* writer                                                          */
/* -------------------------------------------------------------- */

static void jdump_string(sb_t *o, const char *s)
{
    sb_addc(o, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  sb_adds(o, "\\\""); break;
        case '\\': sb_adds(o, "\\\\"); break;
        case '\n': sb_adds(o, "\\n");  break;
        case '\r': sb_adds(o, "\\r");  break;
        case '\t': sb_adds(o, "\\t");  break;
        case '\b': sb_adds(o, "\\b");  break;
        case '\f': sb_adds(o, "\\f");  break;
        default:
            if (*p < 0x20)
                sb_addf(o, "\\u%04x", (unsigned)*p);
            else
                sb_addc(o, (char)*p);
        }
    }
    sb_addc(o, '"');
}

static void jdump_indent(sb_t *o, int indent, int level)
{
    if (indent <= 0)
        return;
    sb_addc(o, '\n');
    for (int i = 0; i < indent * level; i++)
        sb_addc(o, ' ');
}

static void jdump(sb_t *o, const jval *v, int indent, int level)
{
    if (!v) {
        sb_adds(o, "null");
        return;
    }
    switch (v->t) {
    case J_NULL: sb_adds(o, "null"); break;
    case J_BOOL: sb_adds(o, v->b ? "true" : "false"); break;
    case J_INT:
    case J_NUM:  sb_adds(o, v->s ? v->s : "0"); break;
    case J_STR:  jdump_string(o, v->s ? v->s : ""); break;
    case J_ARR:
        if (!v->n) {
            sb_adds(o, "[]");
            break;
        }
        sb_addc(o, '[');
        for (size_t i = 0; i < v->n; i++) {
            if (i)
                sb_addc(o, ',');
            if (indent > 0)
                jdump_indent(o, indent, level + 1);
            else if (i)
                sb_addc(o, ' ');
            jdump(o, v->items[i], indent, level + 1);
        }
        if (indent > 0)
            jdump_indent(o, indent, level);
        sb_addc(o, ']');
        break;
    case J_OBJ:
        if (!v->pn) {
            sb_adds(o, "{}");
            break;
        }
        sb_addc(o, '{');
        for (size_t i = 0; i < v->pn; i++) {
            if (i)
                sb_addc(o, ',');
            if (indent > 0)
                jdump_indent(o, indent, level + 1);
            else if (i)
                sb_addc(o, ' ');
            jdump_string(o, v->pairs[i].key);
            sb_adds(o, ": ");
            jdump(o, v->pairs[i].val, indent, level + 1);
        }
        if (indent > 0)
            jdump_indent(o, indent, level);
        sb_addc(o, '}');
        break;
    }
}

/* json.dump(value, fp) — one line, ", " and ": " separators. */
static char *json_compact(const jval *v)
{
    sb_t o;
    sb_init(&o);
    jdump(&o, v, 0, 0);
    return sb_take(&o);
}

/* json.dump(value, fp, indent=2) */
static char *json_pretty(const jval *v)
{
    sb_t o;
    sb_init(&o);
    jdump(&o, v, 2, 0);
    return sb_take(&o);
}
/* ------------------------------------------------------------------ */
/* defensive JSON decode: same role as Python's try/except ValueError   */
/* ------------------------------------------------------------------ */

static jval *json_try(const char *text, size_t len)
{
    jmp_buf save;
    char savemsg[sizeof(g_raise_msg)];
    memcpy(&save, &g_raise_jmp, sizeof(save));
    memcpy(savemsg, g_raise_msg, sizeof(savemsg));
    jval *v = NULL;
    if (setjmp(g_raise_jmp) == 0)
        v = json_parse(text, len);
    else
        v = NULL;
    memcpy(&g_raise_jmp, &save, sizeof(save));
    memcpy(g_raise_msg, savemsg, sizeof(savemsg));
    return v;
}

/* ------------------------------------------------------------------ */
/* environment                                                         */
/* ------------------------------------------------------------------ */

/* Minimal ordered "KEY=VALUE" environment built from environ. */
typedef struct {
    char **v;       /* each entry "KEY=VALUE" */
    size_t n, cap;
} env_t;

extern char **environ;

static char *env_get(env_t *e, const char *key)
{
    size_t klen = strlen(key);
    for (size_t i = 0; i < e->n; i++) {
        if (!strncmp(e->v[i], key, klen) && e->v[i][klen] == '=')
            return e->v[i] + klen + 1;
    }
    return NULL;
}

static void env_set(env_t *e, const char *key, const char *val)
{
    size_t klen = strlen(key);
    char *entry = xmalloc(klen + strlen(val) + 2);
    sprintf(entry, "%s=%s", key, val);
    for (size_t i = 0; i < e->n; i++) {
        if (!strncmp(e->v[i], key, klen) && e->v[i][klen] == '=') {
            e->v[i] = entry;
            return;
        }
    }
    if (e->n + 2 > e->cap) {
        e->cap = e->cap ? e->cap * 2 : 64;
        e->v = xrealloc(e->v, e->cap * sizeof(*e->v));
    }
    e->v[e->n++] = entry;
    e->v[e->n] = NULL;
}

static void env_init(env_t *e)
{
    memset(e, 0, sizeof(*e));
    e->cap = 64;
    e->v = xmalloc(e->cap * sizeof(*e->v));
    e->v[0] = NULL;
    for (char **p = environ; p && *p; p++) {
        const char *eq = strchr(*p, '=');
        if (!eq)
            continue;
        char *key = xstrndup(*p, (size_t)(eq - *p));
        env_set(e, key, eq + 1);
        free(key);
    }
}

static void env_free(env_t *e)
{
    for (size_t i = 0; i < e->n; i++)
        free(e->v[i]);
    free(e->v);
    memset(e, 0, sizeof(*e));
}

/* cli_env(): isolated HOME/CODEX_HOME so the CLI never touches ~/.codex. */
static void build_cli_env(env_t *e)
{
    env_init(e);
    env_set(e, "CODEX_HOME", P_CX);
    env_set(e, "HOME", P_CX);
    env_set(e, "NO_COLOR", "1");
    env_set(e, "TERM", "dumb");
    env_set(e, "PATH", CLI_PATH_ENV);
}

/* /etc/proxy-client.conf → proxy variables (web managed proxy). */
static void proxy_env(env_t *dst)
{
    sb_t buf;
    sb_init(&buf);
    if (read_all(PROXY_CONF, &buf) != 0) {
        sb_free(&buf);
        return;
    }
    char *host = NULL, *port = NULL, *scheme = NULL, *no_proxy = NULL;
    char *save = NULL;
    for (char *line = strtok_r(buf.p, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        size_t len = strlen(s);
        while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r'))
            s[--len] = 0;
        if (!*s || *s == '#' || !strchr(s, '='))
            continue;
        char *eq = strchr(s, '=');
        *eq = 0;
        char *key = s, *val = eq + 1;
        size_t klen = strlen(key);
        while (klen && (key[klen - 1] == ' ' || key[klen - 1] == '\t'))
            key[--klen] = 0;
        while (*val == ' ' || *val == '\t')
            val++;
        size_t vlen = strlen(val);
        while (vlen && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
            val[--vlen] = 0;
        while (vlen && (val[0] == '"' || val[0] == '\'')) {
            memmove(val, val + 1, vlen);
            vlen--;
        }
        while (vlen && (val[vlen - 1] == '"' || val[vlen - 1] == '\''))
            val[--vlen] = 0;
        char **slot = NULL;
        if (!strcmp(key, "PROXY_HOST"))
            slot = &host;
        else if (!strcmp(key, "PROXY_PORT"))
            slot = &port;
        else if (!strcmp(key, "PROXY_SCHEME"))
            slot = &scheme;
        else if (!strcmp(key, "NO_PROXY"))
            slot = &no_proxy;
        if (slot) {
            free(*slot);
            *slot = xstrdup(val);
        }
    }
    sb_free(&buf);
    if (host && *host && port && *port) {
        char url[512];
        snprintf(url, sizeof(url), "%s://%s:%s",
                 (scheme && *scheme) ? scheme : "http", host, port);
        env_set(dst, "http_proxy", url);
        env_set(dst, "HTTP_PROXY", url);
        env_set(dst, "https_proxy", url);
        env_set(dst, "HTTPS_PROXY", url);
        env_set(dst, "all_proxy", url);
        env_set(dst, "ALL_PROXY", url);
        if (no_proxy && *no_proxy) {
            env_set(dst, "no_proxy", no_proxy);
            env_set(dst, "NO_PROXY", no_proxy);
        }
    }
    free(host);
    free(port);
    free(scheme);
    free(no_proxy);
}

/* env.setdefault(): only fill in variables that are still missing. */
static void proxy_env_default(env_t *dst)
{
    env_t extra;
    env_init(&extra);
    proxy_env(&extra);
    for (size_t i = 0; i < extra.n; i++) {
        const char *eq = strchr(extra.v[i], '=');
        if (!eq)
            continue;
        char *key = xstrndup(extra.v[i], (size_t)(eq - extra.v[i]));
        if (!env_get(dst, key))
            env_set(dst, key, eq + 1);
        free(key);
    }
    env_free(&extra);
}

/* update_env(): the updater locates its install through the real HOME. */
static void build_update_env(env_t *e)
{
    build_cli_env(e);
    env_set(e, "HOME", CLI_HOME);
    env_set(e, "CODEX_HOME", CLI_HOME "/.codex");
    proxy_env_default(e);
}

/* ------------------------------------------------------------------ */
/* privileges                                                          */
/* ------------------------------------------------------------------ */

/* Runs in the forked child, right before exec.
 *
 * The Python bridge always passes user/group/extra_groups=[] to Popen, which
 * calls setgroups() unconditionally and therefore fails with EPERM whenever
 * the bridge itself is not root.  Dropping only when we actually are root is
 * the same thing in production (systemd starts us as root) and keeps the
 * non-root case working for local testing. */
static void drop_privileges(void)
{
    if (geteuid() != 0)
        return;
    struct passwd *pw = getpwnam(CLI_USER);
    if (!pw)
        return;
    if (setgroups(0, NULL) != 0) {
        /* keep going: uid/gid below still restrict the child */
    }
    if (setgid(pw->pw_gid) != 0)
        return;
    if (setuid(pw->pw_uid) != 0)
        return;
}

/* ------------------------------------------------------------------ */
/* running a child with captured output                                */
/* ------------------------------------------------------------------ */

typedef struct {
    sb_t out;
    sb_t err;
    int  rc;             /* Python-style: exit code, or -signo */
    int  timed_out;
    int  spawn_errno;    /* nonzero when execve() failed in the child */
    int  exited;
    pid_t pid;
} proc_t;

static void proc_init(proc_t *r)
{
    memset(r, 0, sizeof(*r));
    sb_init(&r->out);
    sb_init(&r->err);
    r->rc = -1;
}

static void proc_free(proc_t *r)
{
    sb_free(&r->out);
    sb_free(&r->err);
}

static int wait_status_to_rc(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return -WTERMSIG(status);
    return -1;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/*
 * run_proc — fork/exec `argv` (argv[0] is an absolute path), optionally
 * switching to cwd, dropping to the CLI account and starting a new session.
 * The child's stdout/stderr are collected; timeout kills the process group.
 */
static int run_proc(char *const argv[], env_t *env, const char *cwd,
                    int new_session, const char *stdin_data, size_t stdin_len,
                    int timeout_ms, void (*on_start)(pid_t pid), proc_t *res)
{
    proc_init(res);

    int in_pipe[2] = { -1, -1 }, out_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 }, exec_pipe[2] = { -1, -1 };
    if (stdin_data && pipe2(in_pipe, O_CLOEXEC) != 0)
        return -1;
    if (pipe2(out_pipe, O_CLOEXEC) != 0 || pipe2(err_pipe, O_CLOEXEC) != 0 ||
        pipe2(exec_pipe, O_CLOEXEC) != 0) {
        close_fd(&in_pipe[0]); close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]); close_fd(&out_pipe[1]);
        close_fd(&err_pipe[0]); close_fd(&err_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        int err = errno;
        close_fd(&in_pipe[0]); close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]); close_fd(&out_pipe[1]);
        close_fd(&err_pipe[0]); close_fd(&err_pipe[1]);
        close_fd(&exec_pipe[0]); close_fd(&exec_pipe[1]);
        errno = err;
        return -1;
    }

    if (pid == 0) {
        if (new_session)
            setsid();
        if (stdin_data) {
            dup2(in_pipe[0], STDIN_FILENO);
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                if (devnull > 2)
                    close(devnull);
            }
        }
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        if (cwd && chdir(cwd) != 0) {
            int err = errno;
            ssize_t w = write(exec_pipe[1], &err, sizeof(err));
            (void)w;
            _exit(127);
        }
        drop_privileges();
        execve(argv[0], argv, env->v);
        int err = errno;
        ssize_t w = write(exec_pipe[1], &err, sizeof(err));
        (void)w;
        _exit(127);
    }

    close_fd(&in_pipe[0]);
    close_fd(&out_pipe[1]);      /* child keeps the write ends */
    close_fd(&err_pipe[1]);
    close_fd(&exec_pipe[1]);
    res->pid = pid;
    if (on_start)
        on_start(pid);

    if (stdin_data) {
        size_t off = 0;
        while (off < stdin_len) {
            ssize_t w = write(in_pipe[1], stdin_data + off, stdin_len - off);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            off += (size_t)w;
        }
        close_fd(&in_pipe[1]);
    }

    int out_fd = out_pipe[0], err_fd = err_pipe[0];
    long long deadline = now_millis() + (timeout_ms > 0 ? timeout_ms : 3600000);
    int killed = 0;

    for (;;) {
        if (out_fd < 0 && err_fd < 0)
            break;
        if (!killed && now_millis() > deadline) {
            killed = 1;
            res->timed_out = 1;
            if (new_session)
                kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
        }
        struct pollfd pfd[2];
        int n = 0;
        if (out_fd >= 0) {
            pfd[n].fd = out_fd;
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            n++;
        }
        if (err_fd >= 0) {
            pfd[n].fd = err_fd;
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            n++;
        }
        int wait = 200;
        if (!killed && deadline - now_millis() < wait)
            wait = (int)(deadline - now_millis());
        if (wait < 0)
            wait = 0;
        int pr = poll(pfd, (nfds_t)n, wait);
        if (pr < 0 && errno != EINTR)
            break;
        if (pr <= 0)
            continue;
        int idx = 0;
        if (out_fd >= 0) {
            if (pfd[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                char buf[8192];
                ssize_t r = read(out_fd, buf, sizeof(buf));
                if (r > 0)
                    sb_addn(&res->out, buf, (size_t)r);
                else if (r == 0 || (r < 0 && errno != EINTR))
                    close_fd(&out_fd);
            }
            idx++;
        }
        if (err_fd >= 0) {
            if (pfd[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                char buf[8192];
                ssize_t r = read(err_fd, buf, sizeof(buf));
                if (r > 0)
                    sb_addn(&res->err, buf, (size_t)r);
                else if (r == 0 || (r < 0 && errno != EINTR))
                    close_fd(&err_fd);
            }
        }
    }
    close_fd(&in_pipe[1]);
    close_fd(&out_fd);
    close_fd(&err_fd);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    res->rc = wait_status_to_rc(status);
    res->exited = 1;

    int exec_err = 0;
    ssize_t r = read(exec_pipe[0], &exec_err, sizeof(exec_err));
    close_fd(&exec_pipe[0]);
    if (r == (ssize_t)sizeof(exec_err) && exec_err)
        res->spawn_errno = exec_err;

    sb_cstr(&res->out);
    sb_cstr(&res->err);
    return 0;
}

/* PATH lookup, like shutil.which() for a plain file name. */
static char *which_exe(const char *name)
{
    const char *path = getenv("PATH");
    if (!path || !*path)
        path = CLI_PATH_ENV;
    char *copy = xstrdup(path);
    char *save = NULL;
    char *found = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", *dir ? dir : ".", name);
        if (access(full, X_OK) == 0) {
            found = xstrdup(full);
            break;
        }
    }
    free(copy);
    return found;
}

/* ------------------------------------------------------------------ */
/* popen("curl ...")                                                   */
/* ------------------------------------------------------------------ */

static char *shell_quote(const char *s)
{
    sb_t o;
    sb_init(&o);
    sb_addc(&o, '\'');
    for (const char *p = s; *p; p++) {
        if (*p == '\'')
            sb_adds(&o, "'\\''");
        else
            sb_addc(&o, *p);
    }
    sb_addc(&o, '\'');
    return sb_take(&o);
}

/* One short HTTP attempt; returns the body or "" (never raises). */
static char *http_get(const char *url, int timeout_s)
{
    char *q = shell_quote(url);
    sb_t cmd;
    sb_init(&cmd);
    sb_addf(&cmd, "curl -sS --max-time %d --connect-timeout 3 %s 2>/dev/null",
            timeout_s, q);
    free(q);
    FILE *fp = popen(sb_cstr(&cmd), "r");
    sb_free(&cmd);
    if (!fp)
        return xstrdup("");
    sb_t out;
    sb_init(&out);
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), fp)) > 0)
        sb_addn(&out, buf, r);
    pclose(fp);
    if (out.n > 200000)
        out.n = 200000;
    return sb_take(&out);
}

/* ------------------------------------------------------------------ */
/* line reader with a deadline (app-server JSON-RPC)                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int   fd;
    sb_t  buf;
    int   eof;
} linereader_t;

static void lr_init(linereader_t *lr, int fd)
{
    lr->fd = fd;
    lr->eof = 0;
    sb_init(&lr->buf);
}

/*
 * One line (without the trailing newline), or NULL on EOF/timeout.
 * A short read deadline is applied per poll() so the caller's overall
 * deadline is honoured without blocking.
 */
static char *lr_line(linereader_t *lr, long long deadline_ms)
{
    for (;;) {
        char *nl = lr->buf.p ? memchr(lr->buf.p, '\n', lr->buf.n) : NULL;
        if (nl) {
            size_t len = (size_t)(nl - lr->buf.p);
            char *line = xstrndup(lr->buf.p, len);
            size_t rest = lr->buf.n - len - 1;
            memmove(lr->buf.p, nl + 1, rest);
            lr->buf.n = rest;
            if (lr->buf.p)
                lr->buf.p[lr->buf.n] = 0;
            return line;
        }
        if (lr->eof)
            return NULL;
        long long remain = deadline_ms - now_millis();
        if (remain <= 0)
            return NULL;
        struct pollfd pfd = { lr->fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, (int)(remain > 1000 ? 1000 : remain));
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return NULL;
        }
        if (pr == 0)
            continue;
        char buf[8192];
        ssize_t r = read(lr->fd, buf, sizeof(buf));
        if (r > 0)
            sb_addn(&lr->buf, buf, (size_t)r);
        else if (r == 0 || (r < 0 && errno != EINTR))
            lr->eof = 1;
    }
}

/* ------------------------------------------------------------------ */
/* `codex app-server` JSON-RPC                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    pid_t pid;
    int   in_fd;
    linereader_t lr;
} appserver_t;

static void appserver_kill(appserver_t *as)
{
    if (as->pid > 0) {
        kill(-as->pid, SIGKILL);
        kill(as->pid, SIGKILL);
        int status;
        while (waitpid(as->pid, &status, 0) < 0 && errno == EINTR)
            ;
        as->pid = 0;
    }
    close_fd(&as->in_fd);
    sb_free(&as->lr.buf);
    as->lr.buf.p = NULL;
}

/*
 * app_server_call — one initialize + call round trip over stdio.
 * Returns the JSON "result" or NULL with errbuf filled like the Python
 * RuntimeError text.
 */
static jval *app_server_call(const char *method, jval *params, int timeout_ms,
                             char *errbuf, size_t errsize)
{
    jval *out = NULL;
    char *cli = NULL;
    jval *s = NULL;

    errbuf[0] = 0;
    /* settings() is defined later; use a forward declared helper */
    extern jval *settings(void);
    s = settings();
    const char *path = js(s, "cli_path", "");
    cli = xstrdup(path);
    if (!file_exists(cli)) {
        snprintf(errbuf, errsize, "Codex CLI not found");
        free(cli);
        return NULL;
    }

    int in_pipe[2] = { -1, -1 }, out_pipe[2] = { -1, -1 };
    if (pipe2(in_pipe, O_CLOEXEC) != 0 || pipe2(out_pipe, O_CLOEXEC) != 0) {
        close_fd(&in_pipe[0]); close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]); close_fd(&out_pipe[1]);
        snprintf(errbuf, errsize, "OSError: [Errno %d] %s", errno,
                 strerror(errno));
        free(cli);
        return NULL;
    }
    env_t env;
    build_cli_env(&env);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, STDERR_FILENO);
        drop_privileges();
        char *argv[3];
        argv[0] = cli;
        argv[1] = (char *)"app-server";
        argv[2] = NULL;
        execve(argv[0], argv, env.v);
        _exit(127);
    }
    env_free(&env);
    close_fd(&in_pipe[0]);
    close_fd(&out_pipe[1]);
    if (pid < 0) {
        close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]);
        snprintf(errbuf, errsize, "OSError: [Errno %d] %s", errno,
                 strerror(errno));
        free(cli);
        return NULL;
    }

    appserver_t as;
    as.pid = pid;
    as.in_fd = in_pipe[1];
    lr_init(&as.lr, out_pipe[0]);
    long long deadline = now_millis() + timeout_ms;

    sb_t req;
    sb_init(&req);
    sb_adds(&req,
            "{\"jsonrpc\": \"2.0\", \"id\": 1, \"method\": \"initialize\", "
            "\"params\": {\"clientInfo\": {\"name\": "
            "\"autonomous-driving-car\", \"title\": \"Board\", "
            "\"version\": \"1.0.0\"}}}\n");
    if (write(as.in_fd, req.p, req.n) < 0) {
        /* the CLI died immediately; reported by the handshake below */
    }

    int handshake = 0;
    while (now_millis() < deadline) {
        char *line = lr_line(&as.lr, deadline);
        if (!line) {
            snprintf(errbuf, errsize, "app-server handshake timed out");
            break;
        }
        jval *msg = json_try(line, strlen(line));
        free(line);
        if (!msg)
            continue;
        int ok = 0;
        long long id = jtoint(jo(msg, "id"), &ok);
        if (ok && id == 1) {
            if (jtruthy(jo(msg, "error")))
                snprintf(errbuf, errsize, "app-server refused initialize");
            else
                handshake = 1;
            break;
        }
    }
    if (!handshake) {
        if (!errbuf[0])
            snprintf(errbuf, errsize, "app-server handshake timed out");
        sb_free(&req);
        appserver_kill(&as);
        close_fd(&out_pipe[0]);
        free(cli);
        return NULL;
    }

    sb_reset(&req);
    sb_adds(&req, "{\"jsonrpc\": \"2.0\", \"method\": \"initialized\", "
                  "\"params\": {}}\n{\"jsonrpc\": \"2.0\", \"id\": 2, "
                  "\"method\": ");
    jdump_string(&req, method);
    sb_adds(&req, ", \"params\": ");
    jdump(&req, params ? params : jobj(), 0, 0);
    sb_adds(&req, "}\n");
    if (write(as.in_fd, req.p, req.n) < 0) {
        /* fall through to the read loop for the error message */
    }
    sb_free(&req);

    int answered = 0;
    while (now_millis() < deadline) {
        char *line = lr_line(&as.lr, deadline);
        if (!line) {
            snprintf(errbuf, errsize, "app-server did not answer %s", method);
            break;
        }
        jval *msg = json_try(line, strlen(line));
        free(line);
        if (!msg)
            continue;
        int ok = 0;
        long long id = jtoint(jo(msg, "id"), &ok);
        if (ok && id == 2) {
            jval *err = jo(msg, "error");
            if (jtruthy(err)) {
                char *txt = json_compact(err);
                snprintf(errbuf, errsize, "%.200s", txt);
                free(txt);
            } else {
                jval *result = jo(msg, "result");
                out = result ? jclone(result) : jobj();
                answered = 1;
            }
            break;
        }
    }
    if (!answered) {
        if (!errbuf[0])
            snprintf(errbuf, errsize, "app-server timed out on %s", method);
        out = NULL;
    }

    appserver_kill(&as);
    close_fd(&out_pipe[0]);
    free(cli);
    return out;
}
/* ------------------------------------------------------------------ */
/* settings / state / login state / auth                               */
/* ------------------------------------------------------------------ */

jval *settings(void);
jval *state(void);
jval *login_state(void);
void  write_state(jval *changes);
void  write_json_file(const char *path, const jval *value, mode_t mode);
void  ensure_dirs(void);
void  config_toml(jval *s);
void  result_out(const char *out_path, const jval *value);
void  append_record(jval *entry);
jval *read_records(int limit);

static const char *EFFORTS[] = { "minimal", "low", "medium", "high", "xhigh" };

static int effort_valid(const char *e)
{
    for (size_t i = 0; i < sizeof(EFFORTS) / sizeof(EFFORTS[0]); i++)
        if (e && !strcmp(e, EFFORTS[i]))
            return 1;
    return 0;
}

jval *settings(void)
{
    jval *s = jobj();
    jo_setstr(s, "cli_path", CLI_PATH_DEF);
    jo_setstr(s, "model", "");
    jo_setstr(s, "effort", "medium");
    jo_setstr(s, "sandbox", FORCED_SANDBOX);
    jo_setstr(s, "cli_version", "");

    jval *stored = read_json(P_SETTINGS, jobj());
    if (stored && stored->t == J_OBJ)
        for (size_t i = 0; i < stored->pn; i++)
            jo_set(s, stored->pairs[i].key,
                   jclone(stored->pairs[i].val));

    /* sandbox is pinned; a stale workdir from an old release is dropped */
    jo_setstr(s, "sandbox", FORCED_SANDBOX);
    jo_pop(s, "workdir");
    if (!effort_valid(js(s, "effort", NULL)))
        jo_setstr(s, "effort", "medium");
    jval *model = jo(s, "model");
    if (!model || model->t != J_STR)
        jo_setstr(s, "model", "");
    return s;
}

static jval *state_defaults(void)
{
    jval *d = jobj();
    jo_setbool(d, "busy", 0);
    jo_setstr(d, "job", "");
    jo_setstr(d, "thread_id", "");
    jo_setstr(d, "last_error", "");
    jo_setint(d, "started_at", 0);
    jo_setint(d, "finished_at", 0);
    return d;
}

jval *state(void)
{
    return read_json(P_STATE, state_defaults());
}

void write_state(jval *changes)
{
    jval *s = state();
    for (size_t i = 0; changes && i < changes->pn; i++)
        jo_set(s, changes->pairs[i].key, jclone(changes->pairs[i].val));
    write_json_file(P_STATE, s, 0600);
}

static jval *login_defaults(void)
{
    jval *d = jobj();
    jo_setstr(d, "state", "idle");
    jo_setstr(d, "url", "");
    jo_setstr(d, "code", "");
    jo_setstr(d, "message", "");
    jo_setint(d, "started_at", 0);
    jo_setint(d, "expires_at", 0);
    return d;
}

jval *login_state(void)
{
    return read_json(P_LOGIN, login_defaults());
}

/* Only device-code (ChatGPT) credentials count as signed in. */
static const char *auth_mode(void)
{
    if (!file_exists(P_AUTH))
        return "";
    jval *a = read_json(P_AUTH, jobj());
    if (!a || a->t != J_OBJ)
        return "";
    if (jtruthy(jo(a, "tokens")))
        return "chatgpt";
    const char *mode = js(a, "auth_mode", "");
    if (mode && !strcmp(mode, "chatgpt"))
        return "chatgpt";
    if (jtruthy(jo(a, "OPENAI_API_KEY")))
        return "";              /* legacy API-key blob: report signed out */
    return a->pn ? "unknown" : "";
}

/* ------------------------------------------------------------------ */
/* writable layout                                                     */
/* ------------------------------------------------------------------ */

static int list_records(char ***names, size_t *count)
{
    *names = NULL;
    *count = 0;
    DIR *dir = opendir(P_REC);
    if (!dir)
        return -1;
    size_t cap = 16, n = 0;
    char **v = xmalloc(cap * sizeof(*v));
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        const char *name = de->d_name;
        size_t len = strlen(name);
        if (len < 10)                       /* chat_X.txt */
            continue;
        if (strncmp(name, "chat_", 5) != 0)
            continue;
        if (strcmp(name + len - 4, ".txt") != 0)
            continue;
        if (n == cap) {
            cap *= 2;
            v = xrealloc(v, cap * sizeof(*v));
        }
        v[n++] = xstrdup(name);
    }
    closedir(dir);
    for (size_t i = 0; i + 1 < n; i++) {     /* plain insertion sort */
        for (size_t j = 0; j + 1 < n - i; j++) {
            if (strcmp(v[j], v[j + 1]) > 0) {
                char *t = v[j];
                v[j] = v[j + 1];
                v[j + 1] = t;
            }
        }
    }
    *names = v;
    *count = n;
    return 0;
}

static void free_names(char **names, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

/* One readable line: `23:18:38 你：你好`. */
static char *format_record(jval *entry)
{
    const char *role = js(entry, "role", NULL);
    char stamp[32];
    rec_stamp(js(entry, "ts", ""), stamp, sizeof(stamp));
    const char *content = js(entry, "content", "");
    sb_t o;
    sb_init(&o);
    sb_adds(&o, stamp);
    if (role && !strcmp(role, "user")) {
        sb_adds(&o, " 你：");
        sb_adds(&o, content);
        return sb_take(&o);
    }
    jval *okv = jo(entry, "ok");
    if (okv && okv->t == J_BOOL && !okv->b) {
        const char *msg = content;
        if (!*msg)
            msg = js(entry, "error", "");
        sb_adds(&o, " 错误：");
        sb_adds(&o, msg);
        return sb_take(&o);
    }
    sb_adds(&o, " Codex：");
    sb_adds(&o, content);
    return sb_take(&o);
}

/* Rewrite records left in the old JSON-lines format as readable text. */
static void migrate_records(void)
{
    char **names = NULL;
    size_t n = 0;
    if (list_records(&names, &n) != 0)
        return;
    for (size_t i = 0; i < n; i++) {
        char path[PATH_MAX];
        pj(path, sizeof(path), P_REC, names[i]);
        sb_t buf;
        sb_init(&buf);
        if (read_all(path, &buf) != 0) {
            sb_free(&buf);
            continue;
        }
        const char *first = sb_cstr(&buf);
        while (*first == ' ' || *first == '\t' || *first == '\r' ||
               *first == '\n')
            first++;
        if (*first != '{') {
            sb_free(&buf);
            continue;
        }
        sb_t text;
        sb_init(&text);
        int have = 0;
        char *save = NULL;
        for (char *line = strtok_r(buf.p, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            char *s = line;
            while (*s == ' ' || *s == '\t' || *s == '\r')
                s++;
            size_t len = strlen(s);
            while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                           s[len - 1] == '\r'))
                s[--len] = 0;
            if (!*s)
                continue;
            jval *entry = json_try(s, strlen(s));
            if (!entry || entry->t != J_OBJ)
                continue;
            const char *role = js(entry, "role", "");
            if (strcmp(role, "user") && strcmp(role, "assistant"))
                continue;
            char *line_out = format_record(entry);
            if (have)
                sb_adds(&text, "\n\n");
            sb_adds(&text, line_out);
            free(line_out);
            have = 1;
        }
        if (have)
            sb_addc(&text, '\n');
        if (write_atomic(path, sb_cstr(&text), text.n, 0644) != 0) {
            /* best effort, like the Python version */
        }
        sb_free(&text);
        sb_free(&buf);
    }
    free_names(names, n);
}

void ensure_dirs(void)
{
    const char *dirs[] = { P_CX, P_REC, P_OUT, P_INBOX };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        int created = mkdir_p(dirs[i], 0700);
        if (created == 1)
            chown_cli_user(dirs[i]);
    }

    /* legacy chat_*.jsonl is renamed in place */
    char **names = NULL;
    size_t n = 0;
    DIR *dir = opendir(P_REC);
    if (dir) {
        struct dirent *de;
        size_t cap = 8;
        names = xmalloc(cap * sizeof(*names));
        while ((de = readdir(dir)) != NULL) {
            const char *nm = de->d_name;
            size_t len = strlen(nm);
            if (len < 11 || strncmp(nm, "chat_", 5) != 0 ||
                strcmp(nm + len - 6, ".jsonl") != 0)
                continue;
            if (n == cap) {
                cap *= 2;
                names = xrealloc(names, cap * sizeof(*names));
            }
            names[n++] = xstrdup(nm);
        }
        closedir(dir);
    }
    for (size_t i = 0; i < n; i++) {
        char old_path[PATH_MAX], new_path[PATH_MAX];
        pj(old_path, sizeof(old_path), P_REC, names[i]);
        snprintf(new_path, sizeof(new_path), "%s", old_path);
        size_t len = strlen(new_path);
        new_path[len - 6] = 0;
        strcat(new_path, ".txt");
        if (!file_exists(new_path))
            rename(old_path, new_path);
        free(names[i]);
    }
    if (names)
        free(names);

    migrate_records();
}

void write_json_file(const char *path, const jval *value, mode_t mode)
{
    ensure_dirs();
    char *text = json_pretty(value);
    if (write_atomic(path, text, strlen(text), mode) != 0)
        raise_oserror(errno, path);
    free(text);
}

void result_out(const char *out_path, const jval *value)
{
    ensure_dirs();
    char *text = json_compact(value);
    if (write_atomic(out_path, text, strlen(text), 0600) != 0) {
        int err = errno;
        free(text);
        raise_oserror(err, out_path);
    }
    free(text);
}

void config_toml(jval *s)
{
    sb_t o;
    sb_init(&o);
    sb_adds(&o, "# Managed by the autonomous-driving-car web UI.\n"
                "approval_policy = \"never\"\n"
                "sandbox_mode = \"");
    sb_adds(&o, FORCED_SANDBOX);
    sb_addc(&o, '"');
    const char *model = js(s, "model", "");
    if (*model) {
        sb_adds(&o, "\nmodel = \"");
        sb_adds(&o, model);
        sb_addc(&o, '"');
    }
    const char *effort = js(s, "effort", "");
    if (*effort) {
        sb_adds(&o, "\nmodel_reasoning_effort = \"");
        sb_adds(&o, effort);
        sb_addc(&o, '"');
    }
    if (write_atomic(P_TOML, sb_cstr(&o), o.n, 0644) != 0) {
        /* best effort: a read-only home must not kill the request */
    }
    sb_free(&o);
}

/* ------------------------------------------------------------------ */
/* interaction records                                                 */
/* ------------------------------------------------------------------ */

void append_record(jval *entry)
{
    ensure_dirs();
    const char *role = js(entry, "role", NULL);
    if (role && !strcmp(role, "event"))
        return;                     /* event rows are debug only */
    if (!jo(entry, "ts")) {
        char iso[64];
        now_iso(iso, sizeof(iso));
        jo_setstr(entry, "ts", iso);
    }
    char day[16], path[PATH_MAX * 2];
    fmt_localtime(now_epoch(), "%Y%m%d", day, sizeof(day));
    snprintf(path, sizeof(path), "%s/chat_%s.txt", P_REC, day);

    int ok = 0;
    long long size = file_size(path, &ok);
    int empty = !ok || size == 0;

    char *line = format_record(entry);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
        if (!empty) {
            ssize_t w = write(fd, "\n", 1);
            (void)w;
        }
        size_t len = strlen(line);
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(fd, line + off, len - off);
            if (w <= 0)
                break;
            off += (size_t)w;
        }
        ssize_t w = write(fd, "\n", 1);
        (void)w;
        fsync(fd);
        close(fd);
    }
    free(line);
}

/* Parse the readable transcript back into role/content rows. */
jval *read_records(int limit)
{
    jval *rows = jarr();
    char **names = NULL;
    size_t n = 0;
    if (list_records(&names, &n) != 0)
        return rows;

    size_t start = n > 2 ? n - 2 : 0;
    for (size_t i = start; i < n; i++) {
        char path[PATH_MAX];
        pj(path, sizeof(path), P_REC, names[i]);
        sb_t buf;
        sb_init(&buf);
        if (read_all(path, &buf) != 0) {
            sb_free(&buf);
            continue;
        }
        sb_cstr(&buf);
        jval *cur = NULL;
        size_t off = 0;
        while (off < buf.n) {
            /* one line, minus the trailing "\n" only (like rstrip("\n")) */
            size_t end = off;
            while (end < buf.n && buf.p[end] != '\n')
                end++;
            size_t len = end - off;
            /* NUL terminated copy: the regexes below work on the line only */
            char *line = xstrndup(buf.p + off, len);
            off = (end < buf.n) ? end + 1 : buf.n + 1;

            /* ^(\d\d:\d\d:\d\d)\s+(你|Codex|错误)[：:]\s?(.*)$ */
            const char *head = NULL;
            size_t head_len = 0;
            const char *body = NULL;
            if (len >= 8 && isdigit((unsigned char)line[0]) &&
                isdigit((unsigned char)line[1]) && line[2] == ':' &&
                isdigit((unsigned char)line[3]) &&
                isdigit((unsigned char)line[4]) && line[5] == ':' &&
                isdigit((unsigned char)line[6]) &&
                isdigit((unsigned char)line[7])) {
                size_t k = 8;
                if (k < len && isspace((unsigned char)line[k])) {
                    while (k < len && isspace((unsigned char)line[k]))
                        k++;
                    if (len - k >= 3 && !memcmp(line + k, "你", 3)) {
                        head = "你";
                        head_len = 3;
                    } else if (len - k >= 5 && !memcmp(line + k, "Codex", 5)) {
                        head = "Codex";
                        head_len = 5;
                    } else if (len - k >= 6 && !memcmp(line + k, "错误", 6)) {
                        head = "错误";
                        head_len = 6;
                    }
                    if (head) {
                        size_t m = k + head_len;
                        if (m < len && (line[m] == ':' ||
                                        (len - m >= 3 &&
                                         !memcmp(line + m, "：", 3)))) {
                            m += (line[m] == ':') ? 1 : 3;
                            if (m < len && isspace((unsigned char)line[m]))
                                m++;
                            body = line + m;
                        } else {
                            head = NULL;
                        }
                    }
                }
            }

            if (head && body) {
                if (cur)
                    ja_add(rows, cur);
                char stamp[16];
                memcpy(stamp, line, 8);
                stamp[8] = 0;
                cur = jobj();
                if (!strcmp(head, "你")) {
                    jo_setstr(cur, "role", "user");
                    jo_setstr(cur, "content", body);
                    jo_setstr(cur, "ts", stamp);
                    jo_setbool(cur, "ok", 1);
                } else if (!strcmp(head, "错误")) {
                    jo_setstr(cur, "role", "assistant");
                    jo_setstr(cur, "content", body);
                    jo_setstr(cur, "ts", stamp);
                    jo_setbool(cur, "ok", 0);
                    jo_setstr(cur, "error", body);
                } else {
                    jo_setstr(cur, "role", "assistant");
                    jo_setstr(cur, "content", body);
                    jo_setstr(cur, "ts", stamp);
                    jo_setbool(cur, "ok", 1);
                }
            } else if (cur) {
                const char *content = js(cur, "content", "");
                sb_t merged;
                sb_init(&merged);
                sb_adds(&merged, content);
                sb_addc(&merged, '\n');
                sb_addn(&merged, line, len);
                jo_setstr(cur, "content", sb_cstr(&merged));
                sb_free(&merged);
            }
            free(line);
        }
        if (cur)
            ja_add(rows, cur);
        sb_free(&buf);
    }
    free_names(names, n);

    if (limit > 0 && rows->n > (size_t)limit) {
        size_t drop = rows->n - (size_t)limit;
        memmove(rows->items, rows->items + drop,
                ((size_t)limit) * sizeof(*rows->items));
        rows->n = (size_t)limit;
    }
    return rows;
}

/* Only user/assistant rows survive into the UI. */
static void filter_dialog(jval *rows)
{
    size_t w = 0;
    for (size_t i = 0; i < rows->n; i++) {
        const char *role = js(rows->items[i], "role", "");
        if (!strcmp(role, "user") || !strcmp(role, "assistant"))
            rows->items[w++] = rows->items[i];
    }
    rows->n = w;
}

static void keep_last(jval *rows, size_t limit)
{
    if (rows->n > limit) {
        size_t drop = rows->n - limit;
        memmove(rows->items, rows->items + drop, limit * sizeof(*rows->items));
        rows->n = limit;
    }
}
/* ------------------------------------------------------------------ */
/* shared helpers for the commands                                     */
/* ------------------------------------------------------------------ */

static int cli_available(const char *path)
{
    return path && *path && file_exists(path);
}

/* Python-ish str() for the handful of JSON shapes that reach config. */
static char *str_of(jval *v)
{
    if (!v)
        return xstrdup("None");
    switch (v->t) {
    case J_STR:
    case J_INT:
    case J_NUM:
        return xstrdup(v->s ? v->s : "");
    case J_BOOL:
        return xstrdup(v->b ? "True" : "False");
    case J_NULL:
        return xstrdup("None");
    default:
        return json_compact(v);
    }
}

static char *strip_dup(const char *s)
{
    const char *a = s;
    while (*a == ' ' || *a == '\t' || *a == '\n' || *a == '\r' || *a == '\f' ||
           *a == '\v')
        a++;
    size_t len = strlen(a);
    while (len && (a[len - 1] == ' ' || a[len - 1] == '\t' || a[len - 1] == '\n' ||
                   a[len - 1] == '\r' || a[len - 1] == '\f' || a[len - 1] == '\v'))
        len--;
    return xstrndup(a, len);
}

static const char *read_text(const char *path)
{
    if (!path || !*path)
        return "";
    sb_t buf;
    sb_init(&buf);
    if (read_all(path, &buf) != 0) {
        sb_free(&buf);
        return "";
    }
    return sb_take(&buf);
}

/* `path, "--version"` → first line of its output, 80 chars. */
static char *cli_version(const char *path)
{
    if (!path || !*path)
        return xstrdup("");
    char *argv[3];
    argv[0] = (char *)path;
    argv[1] = (char *)"--version";
    argv[2] = NULL;
    env_t env;
    build_cli_env(&env);
    proc_t r;
    char *ret = xstrdup("");
    if (run_proc(argv, &env, NULL, 0, NULL, 0, 10000, NULL, &r) == 0 && !r.spawn_errno) {
        const char *src = r.out.n ? sb_cstr(&r.out) : sb_cstr(&r.err);
        char *text = strip_dup(src);
        size_t len = 0;
        while (text[len] && text[len] != '\n' && text[len] != '\r')
            len++;
        if (len > 80)
            len = 80;
        free(ret);
        ret = xstrndup(text, len);
        free(text);
    }
    proc_free(&r);
    env_free(&env);
    return ret;
}

/* Python slicing name[start:end] with clamping. */
static char *slice_dup(const char *s, size_t start, size_t end)
{
    size_t len = strlen(s);
    if (start > len)
        start = len;
    if (end > len)
        end = len;
    if (end < start)
        end = start;
    return xstrndup(s + start, end - start);
}

static int record_name_ok(const char *name)
{
    /* ^chat_\d{8}\.txt$ */
    if (!name || strncmp(name, "chat_", 5) != 0)
        return 0;
    for (int i = 0; i < 8; i++)
        if (!isdigit((unsigned char)name[5 + i]))
            return 0;
    return !strcmp(name + 13, ".txt");
}

/* OSError text the Python version returns in "error" fields ([:160]). */
static void oserror_text(char *out, size_t size, int err, const char *path)
{
    char tmp[PATH_MAX * 2 + 256];
    if (path)
        snprintf(tmp, sizeof(tmp), "[Errno %d] %s: '%s'", err, strerror(err),
                 path);
    else
        snprintf(tmp, sizeof(tmp), "[Errno %d] %s", err, strerror(err));
    size_t len = strlen(tmp);
    if (len > 160)
        len = 160;
    if (len > size - 1)
        len = size - 1;
    memcpy(out, tmp, len);
    out[len] = 0;
}

/* ------------------------------------------------------------------ */
/* status                                                              */
/* ------------------------------------------------------------------ */

static jval *build_status(void)
{
    jval *s = settings();
    jval *st = state();
    jval *lg = login_state();
    const char *mode = auth_mode();

    if (!file_exists(P_AUTH) && !strcmp(js(lg, "state", ""), "success")) {
        jo_setstr(lg, "state", "idle");
        jo_setstr(lg, "message", "");
    }

    jval *rows = read_records(400);
    filter_dialog(rows);
    keep_last(rows, 60);

    char **names = NULL;
    size_t nfiles = 0;
    long long bytes = 0;
    if (list_records(&names, &nfiles) == 0) {
        for (size_t i = 0; i < nfiles; i++) {
            char path[PATH_MAX];
            pj(path, sizeof(path), P_REC, names[i]);
            int ok = 0;
            long long size = file_size(path, &ok);
            if (ok)
                bytes += size;
        }
        free_names(names, nfiles);
    }

    jval *o = jobj();
    jo_setbool(o, "ok", 1);
    jo_setbool(o, "cli_found", cli_available(js(s, "cli_path", "")));
    jo_setstr(o, "cli_path", js(s, "cli_path", ""));
    jo_setstr(o, "cli_version", js(s, "cli_version", ""));
    jo_setbool(o, "logged_in", mode && *mode && strcmp(mode, "none") != 0);
    jo_setstr(o, "auth_mode", (mode && *mode) ? mode : "none");
    jo_set(o, "login", lg);

    jval *view = jobj();
    jo_setstr(view, "sandbox", FORCED_SANDBOX);
    jo_setstr(view, "model", js(s, "model", ""));
    jo_setstr(view, "effort", js(s, "effort", "medium"));
    jo_set(o, "settings", view);

    jo_setbool(o, "busy", jtruthy(jo(st, "busy")));
    jo_setstr(o, "job", js(st, "job", ""));

    jval *tid = jo(st, "thread_id");
    jo_set(o, "thread_id", tid ? jclone(tid) : jstr(""));
    jval *lerr = jo(st, "last_error");
    jo_set(o, "last_error", lerr ? jclone(lerr) : jstr(""));
    jval *sa = jo(st, "started_at");
    jo_set(o, "started_at", sa ? jclone(sa) : jint(0));
    jval *fa = jo(st, "finished_at");
    jo_set(o, "finished_at", fa ? jclone(fa) : jint(0));

    jo_set(o, "messages", rows);

    jval *rec = jobj();
    jo_setint(rec, "files", (long long)nfiles);
    jo_setint(rec, "bytes", bytes);
    jo_set(o, "records", rec);
    return o;
}

/* ------------------------------------------------------------------ */
/* record listing / export                                             */
/* ------------------------------------------------------------------ */

static jval *record_files(void)
{
    jval *files = jarr();
    char **names = NULL;
    size_t n = 0;
    if (list_records(&names, &n) != 0)
        return files;
    for (size_t k = n; k > 0; k--) {
        const char *name = names[k - 1];          /* reverse=True */
        char path[PATH_MAX];
        pj(path, sizeof(path), P_REC, name);
        struct stat st;
        if (stat(path, &st) != 0) {
            int err = errno;
            char msg[256];
            oserror_text(msg, sizeof(msg), err, path);
            free_names(names, n);
            raise_msg("OSError: %s", msg);
        }
        char *day = slice_dup(name, 5, 13);
        char *d1 = slice_dup(day, 0, 4);
        char *d2 = slice_dup(day, 4, 6);
        char *d3 = slice_dup(day, 6, 8);
        char date[128];
        snprintf(date, sizeof(date), "%s-%s-%s", d1, d2, d3);
        free(day);
        free(d1);
        free(d2);
        free(d3);

        char mtime[64];
        fmt_localtime((long long)st.st_mtime, "%Y-%m-%d %H:%M:%S", mtime,
                      sizeof(mtime));

        jval *row = jobj();
        jo_setstr(row, "name", name);
        jo_setstr(row, "date", date);
        jo_setint(row, "size", (long long)st.st_size);
        jo_setstr(row, "mtime", mtime);
        ja_add(files, row);
    }
    free_names(names, n);
    return files;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static void cmd_status(const char *out)
{
    result_out(out, build_status());
}

static void cmd_history(const char *out, const char *arg)
{
    int ok = 0;
    long long after = jtoint(jstr(arg && *arg ? arg : "0"), &ok);
    if (!ok)
        after = 0;
    jval *rows = read_records(500);
    jval *o = jobj();
    jo_setbool(o, "ok", 1);
    jo_set(o, "messages", rows);
    jo_setint(o, "total", (long long)rows->n);
    jo_setint(o, "after", after);
    result_out(out, o);
}

static void cmd_records(const char *out)
{
    jval *o = jobj();
    jo_setbool(o, "ok", 1);
    jo_set(o, "files", record_files());
    result_out(out, o);
}

static void cmd_record(const char *out, const char *arg)
{
    const char *name = base_of(arg ? arg : "");
    if (!record_name_ok(name)) {
        jval *o = jobj();
        jo_setbool(o, "ok", 0);
        jo_setstr(o, "error", "invalid record name");
        result_out(out, o);
        return;
    }
    char path[PATH_MAX];
    pj(path, sizeof(path), P_REC, name);
    if (!file_exists(path)) {
        jval *o = jobj();
        jo_setbool(o, "ok", 0);
        jo_setstr(o, "error", "record not found");
        result_out(out, o);
        return;
    }
    int ok = 0;
    long long size = file_size(path, &ok);
    jval *o = jobj();
    jo_setbool(o, "ok", 1);
    jo_setstr(o, "path", path);
    jo_setstr(o, "name", name);
    jo_setint(o, "size", size);
    result_out(out, o);
}

static void cmd_records_delete(const char *out, const char *arg)
{
    const char *name = base_of(arg ? arg : "");
    if (!record_name_ok(name)) {
        jval *o = jobj();
        jo_setbool(o, "ok", 0);
        jo_setstr(o, "error", "invalid record name");
        result_out(out, o);
        return;
    }
    char path[PATH_MAX];
    pj(path, sizeof(path), P_REC, name);
    if (unlink(path) != 0) {
        char msg[256];
        oserror_text(msg, sizeof(msg), errno, path);
        jval *o = jobj();
        jo_setbool(o, "ok", 0);
        jo_setstr(o, "error", msg);
        result_out(out, o);
        return;
    }
    jval *o = jobj();
    jo_setbool(o, "ok", 1);
    jo_setstr(o, "deleted", name);
    result_out(out, o);
}

static void cmd_records_clear(const char *out)
{
    long long removed = 0;
    char **names = NULL;
    size_t n = 0;
    if (list_records(&names, &n) == 0) {
        for (size_t i = 0; i < n; i++) {
            char path[PATH_MAX];
            pj(path, sizeof(path), P_REC, names[i]);
            if (unlink(path) == 0)
                removed++;
        }
        free_names(names, n);
    }
    jval *o = jobj();
    jo_setbool(o, "ok", 1);
    jo_setint(o, "deleted", removed);
    result_out(out, o);
}

static void cmd_export(const char *out)
{
    char stamp[32];
    fmt_localtime(now_epoch(), "%Y%m%d_%H%M%S", stamp, sizeof(stamp));
    char name[128];
    snprintf(name, sizeof(name), "codex_chat_%s.txt", stamp);
    char path[PATH_MAX * 2];
    snprintf(path, sizeof(path), "%s/.export_%s", P_REC, name);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        char msg[256];
        oserror_text(msg, sizeof(msg), errno, path);
        raise_msg("OSError: %s", msg);
    }

    long long count = 0;
    char **names = NULL;
    size_t n = 0;
    if (list_records(&names, &n) == 0) {
        for (size_t i = 0; i < n; i++) {
            char src[PATH_MAX];
            pj(src, sizeof(src), P_REC, names[i]);
            sb_t buf;
            sb_init(&buf);
            if (read_all(src, &buf) != 0) {
                sb_free(&buf);
                continue;
            }
            sb_cstr(&buf);
            size_t off = 0;
            while (off < buf.n) {
                size_t end = off;
                while (end < buf.n && buf.p[end] != '\n')
                    end++;
                size_t len = end - off;
                int has_nl = end < buf.n;
                const char *line = buf.p + off;
                off = has_nl ? end + 1 : buf.n;
                int nonblank = 0;
                for (size_t k = 0; k < len; k++) {
                    unsigned char c = (unsigned char)line[k];
                    if (!isspace(c)) {
                        nonblank = 1;
                        break;
                    }
                }
                if (!nonblank)
                    continue;
                size_t w = 0;
                while (w < len) {
                    ssize_t r = write(fd, line + w, len - w);
                    if (r <= 0)
                        break;
                    w += (size_t)r;
                }
                /* the source line keeps its trailing newline; add it when
                   the file's last line had none */
                ssize_t nl = write(fd, "\n", 1);
                (void)nl;
                count++;
            }
            sb_free(&buf);
        }
        free_names(names, n);
    }
    fsync(fd);
    close(fd);

    int ok = 0;
    long long size = file_size(path, &ok);
    jval *o = jobj();
    jo_setbool(o, "ok", 1);
    jo_setstr(o, "path", path);
    jo_setstr(o, "name", name);
    jo_setint(o, "size", size);
    jo_setint(o, "records", count);
    result_out(out, o);
}

static void cmd_config(const char *out, const char *input)
{
    const char *raw = read_text(input);
    jval *payload = NULL;
    if (!*raw) {
        payload = jobj();
    } else {
        payload = json_parse(raw, strlen(raw));   /* JSONDecodeError on junk */
        if (!payload || payload->t != J_OBJ)
            raise_msg("TypeError: argument of type '%.20s' is not a mapping",
                      payload && payload->t == J_ARR ? "list" : "str");
    }

    jval *s = settings();
    const char *str_keys[] = { "cli_path", "model" };
    for (size_t i = 0; i < 2; i++) {
        jval *v = jo(payload, str_keys[i]);
        if (!v)
            continue;
        char *text = str_of(v);
        char *cut = xstrndup(text, strlen(text) > 512 ? 512 : strlen(text));
        char *clean = strip_dup(cut);
        jo_setstr(s, str_keys[i], clean);
        free(text);
        free(cut);
        free(clean);
    }
    if (jo(payload, "effort")) {
        char *text = str_of(jo(payload, "effort"));
        char *clean = strip_dup(text);
        for (char *p = clean; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        jo_setstr(s, "effort", effort_valid(clean) ? clean : "medium");
        free(text);
        free(clean);
    }
    if (!*js(s, "cli_version", "") && cli_available(js(s, "cli_path", ""))) {
        char *ver = cli_version(js(s, "cli_path", ""));
        jo_setstr(s, "cli_version", ver);
        free(ver);
    }
    write_json_file(P_SETTINGS, s, 0600);
    config_toml(s);

    jval *o = jobj();
    jo_setbool(o, "ok", 1);
    jo_set(o, "status", build_status());
    result_out(out, o);
}

static void cmd_reset(const char *out)
{
    jval *st = state();
    if (jtruthy(jo(st, "busy"))) {
        jval *o = jobj();
        jo_setbool(o, "ok", 0);
        jo_setstr(o, "error", "Codex is still answering");
        result_out(out, o);
        return;
    }
    jval *ch = jobj();
    jo_setstr(ch, "thread_id", "");
    jo_setstr(ch, "last_error", "");
    jo_setstr(ch, "job", "");
    write_state(ch);

    jval *o = jobj();
    jo_setbool(o, "ok", 1);
    jo_set(o, "status", build_status());
    result_out(out, o);
}

static void cmd_stop(const char *out)
{
    jval *st = state();
    int ok = 0;
    long long pid = jtoint(jo(st, "child_pid"), &ok);
    if (!ok)
        pid = 0;
    int killed = 0;
    if (pid > 0) {
        if (kill(-(pid_t)pid, SIGTERM) == 0)
            killed = 1;
        else if (kill((pid_t)pid, SIGTERM) == 0)
            killed = 1;
    }
    jval *ch = jobj();
    jo_setbool(ch, "busy", 0);
    jo_setint(ch, "child_pid", 0);
    jo_setstr(ch, "last_error", killed ? "Stopped by user" : "");
    jo_setint(ch, "finished_at", now_epoch());
    write_state(ch);

    jval *o = jobj();
    jo_setbool(o, "ok", 1);
    jo_setbool(o, "stopped", killed);
    result_out(out, o);
}

static void cmd_logout(const char *out)
{
    cmd_stop(out);
    unlink(P_AUTH);
    config_toml(settings());

    jval *lg = jobj();
    jo_setstr(lg, "state", "idle");
    jo_setstr(lg, "url", "");
    jo_setstr(lg, "code", "");
    jo_setstr(lg, "message", "");
    jo_setint(lg, "started_at", 0);
    jo_setint(lg, "expires_at", 0);
    write_json_file(P_LOGIN, lg, 0600);

    jval *ch = jobj();
    jo_setstr(ch, "thread_id", "");
    jo_setstr(ch, "last_error", "");
    write_state(ch);

    jval *o = jobj();
    jo_setbool(o, "ok", 1);
    jo_set(o, "status", build_status());
    result_out(out, o);
}
/* ------------------------------------------------------------------ */
/* versions and the npm registry                                       */
/* ------------------------------------------------------------------ */

/* \d+\.\d+\.\d+(?:[-.][0-9A-Za-z.]+)? */
static char *version_of(const char *text)
{
    if (!text)
        return xstrdup("");
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        size_t p = i;
        int parts = 0;
        size_t digits = 0;
        while (parts < 3) {
            size_t d = 0;
            while (p + d < len && isdigit((unsigned char)text[p + d]))
                d++;
            if (!d)
                break;
            digits = d;
            p += d;
            parts++;
            if (parts == 3)
                break;
            if (p < len && text[p] == '.')
                p++;
            else
                break;
        }
        if (parts != 3 || !digits)
            continue;
        size_t end = p;
        if (end < len && (text[end] == '-' || text[end] == '.')) {
            size_t q = end + 1;
            while (q < len && (isalnum((unsigned char)text[q]) ||
                               text[q] == '.'))
                q++;
            if (q > end + 1)
                end = q;
        }
        return xstrndup(text + i, end - i);
    }
    return xstrdup("");
}

/* tuple(int(x) for x in re.findall(r"\d+", text)[:3] + [0,0,0][:3]) */
static void version_key(const char *text, long long out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (!text)
        return;
    size_t len = strlen(text);
    int n = 0;
    for (size_t i = 0; i < len && n < 3; i++) {
        if (!isdigit((unsigned char)text[i]))
            continue;
        long long v = 0;
        while (i < len && isdigit((unsigned char)text[i])) {
            if (v < 100000000LL)
                v = v * 10 + (text[i] - '0');
            i++;
        }
        out[n++] = v;
    }
}

static int version_gt(const char *a, const char *b)
{
    long long ka[3], kb[3];
    version_key(a, ka);
    version_key(b, kb);
    for (int i = 0; i < 3; i++) {
        if (ka[i] != kb[i])
            return ka[i] > kb[i];
    }
    return 0;
}

static const char *NPM_ENDPOINTS[2] = {
    "https://registry.npmjs.org/@openai/codex/latest",
    "https://registry.npmjs.org/-/package/@openai/codex/dist-tags",
};

/* One short fetch attempt; an unreachable registry simply yields {}. */
static jval *fetch_json(const char *url, int timeout_s)
{
    char *body = http_get(url, timeout_s);
    jval *v = NULL;
    if (body && *body)
        v = json_try(body, strlen(body));
    free(body);
    return (v && v->t == J_OBJ) ? v : NULL;
}

/* The board proxy drops half the TLS handshakes: try both endpoints twice. */
static char *npm_latest_version(void)
{
    for (int i = 0; i < 4; i++) {
        const char *url = NPM_ENDPOINTS[(i == 0 || i == 3) ? 0 : 1];
        jval *data = fetch_json(url, 4);
        if (!data)
            continue;
        const char *value = js(data, "version", "");
        if (!*value)
            value = js(data, "latest", "");
        char *v = version_of(value);
        if (*v)
            return v;
        free(v);
    }
    return xstrdup("");
}

/* ------------------------------------------------------------------ */
/* update check / install / progress                                   */
/* ------------------------------------------------------------------ */

static void cmd_check_update(const char *out)
{
    jval *s = settings();
    const char *cli = js(s, "cli_path", "");
    char *cur = version_of(js(s, "cli_version", ""));
    if (!*cur) {
        free(cur);
        cur = cli_version(cli);
        char *tmp = version_of(cur);
        free(cur);
        cur = tmp;
    }
    char *latest = npm_latest_version();
    int stale = 0;
    char path_cache[PATH_MAX];
    pj(path_cache, sizeof(path_cache), P_CX, "update_cache.json");

    if (*latest) {
        jval *cache = jobj();
        jo_setstr(cache, "latest", latest);
        jo_setint(cache, "checked_at", now_epoch());
        write_json_file(path_cache, cache, 0644);
    } else {
        jval *cache = read_json(path_cache, jobj());
        free(latest);
        latest = version_of(js(cache, "latest", ""));
        stale = *latest ? 1 : 0;
    }

    jval *o = jobj();
    if (!*latest) {
        jo_setbool(o, "ok", 0);
        jo_setstr(o, "current", cur);
        jo_setstr(o, "latest", "");
        jo_setbool(o, "cli_found", cli_available(cli));
        jo_setbool(o, "update_available", 0);
        jo_setbool(o, "stale", 0);
        jo_setstr(o, "error",
                  "Cannot reach npm registry (check the board proxy)");
        result_out(out, o);
        return;
    }
    jo_setbool(o, "ok", 1);
    jo_setstr(o, "current", cur);
    jo_setstr(o, "latest", latest);
    jo_setbool(o, "cli_found", cli_available(cli));
    jo_setbool(o, "update_available", *cur && version_gt(latest, cur));
    jo_setbool(o, "stale", stale);
    jo_setstr(o, "source", "npm registry: @openai/codex");
    jo_setstr(o, "update_command", UPDATE_COMMAND);
    jo_setint(o, "checked_at", now_epoch());
    result_out(out, o);
    free(cur);
    free(latest);
}

/* Bytes the installer has pulled so far, plus the tarball path. */
static void update_download_state(long long *best_out, char *path_out, size_t path_size)
{
    long long best = 0;
    path_out[0] = 0;

    char progress[PATH_MAX];
    pj(progress, sizeof(progress), P_REC, "codex_update_progress.txt");
    sb_t buf;
    sb_init(&buf);
    if (read_all(progress, &buf) == 0) {
        int ok = 0;
        long long v = jtoint(jstr(sb_cstr(&buf)), &ok);
        if (ok && v > 0)
            best = v;
    }
    sb_free(&buf);

    char tmpdir[PATH_MAX];
    pj(tmpdir, sizeof(tmpdir), P_REC, "tmp");

    /* os.walk(UPDATE_TMPDIR) — depth-first, any *.tar.gz */
    char *stack[64];
    size_t sp = 0;
    stack[sp++] = xstrdup(tmpdir);
    while (sp) {
        char *dir_path = stack[--sp];
        DIR *dir = opendir(dir_path);
        if (!dir) {
            free(dir_path);
            continue;
        }
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            char full[PATH_MAX];
            snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);
            struct stat st;
            if (lstat(full, &st) != 0)
                continue;
            if (S_ISDIR(st.st_mode)) {
                if (sp < sizeof(stack) / sizeof(stack[0]))
                    stack[sp++] = xstrdup(full);
                continue;
            }
            size_t nlen = strlen(de->d_name);
            if (nlen < 7)
                continue;
            const char *tail = de->d_name + nlen - 7;
            if (strcasecmp(tail, ".tar.gz") != 0)
                continue;
            if ((long long)st.st_size > best) {
                best = (long long)st.st_size;
                snprintf(path_out, path_size, "%s", full);
            }
        }
        closedir(dir);
        free(dir_path);
    }
    *best_out = best;
}

/* Name of an already running update unit, if any. */
static char *running_update_unit(void)
{
    char *systemctl = which_exe("systemctl");
    if (!systemctl)
        return xstrdup("");
    char *argv[7];
    argv[0] = systemctl;
    argv[1] = (char *)"list-units";
    argv[2] = (char *)"--all";
    argv[3] = (char *)"--no-legend";
    argv[4] = (char *)"--plain";
    argv[5] = (char *)"codex-update-*.service";
    argv[6] = NULL;
    env_t env;
    env_init(&env);
    proc_t r;
    char *unit = xstrdup("");
    if (run_proc(argv, &env, NULL, 0, NULL, 0, 10000, NULL, &r) == 0 &&
        !r.spawn_errno) {
        char *save = NULL;
        for (char *line = strtok_r((char *)sb_cstr(&r.out), "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            /* UNIT LOAD ACTIVE SUB DESCRIPTION */
            char *fields[5];
            size_t nf = 0;
            char *save2 = NULL;
            for (char *tok = strtok_r(line, " \t", &save2); tok && nf < 5;
                 tok = strtok_r(NULL, " \t", &save2))
                fields[nf++] = tok;
            if (nf < 4)
                continue;
            if (strncmp(fields[0], "codex-update-", 13) != 0)
                continue;
            if (strcmp(fields[3], "running") != 0)
                continue;
            free(unit);
            unit = xstrdup(fields[0]);
            break;
        }
    }
    proc_free(&r);
    env_free(&env);
    free(systemctl);
    return unit;
}

static void cmd_update_status(const char *out)
{
    jval *s = settings();
    const char *cli = js(s, "cli_path", "");
    char *unit = running_update_unit();

    char log_path[PATH_MAX];
    pj(log_path, sizeof(log_path), P_REC, "codex_update.log");
    const char *log = read_text(log_path);

    long long done = 0;
    char tarball[PATH_MAX];
    update_download_state(&done, tarball, sizeof(tarball));

    long long total = UPDATE_TOTAL_DEFAULT;
    const char *found = strstr(log, "out of ");
    if (found) {
        const char *p = found + 7;
        long long v = 0;
        int any = 0;
        while (isdigit((unsigned char)*p)) {
            v = v * 10 + (*p - '0');
            p++;
            any = 1;
        }
        if (any && !strncmp(p, " bytes", 6))
            total = v;
    }
    int percent = 0;
    if (total > 0) {
        long long pct = done * 100 / total;
        if (pct > 100)
            pct = 100;
        percent = (int)pct;
    }

    /* last three non-blank lines (blank test on a copy: Python only filters,
       it does not strip the lines it keeps) */
    char *tail[3] = { NULL, NULL, NULL };
    {
        char *copy = xstrdup(log);
        char *save = NULL;
        for (char *line = strtok_r(copy, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            char *probe = strip_dup(line);
            int blank = !*probe;
            free(probe);
            if (blank)
                continue;
            free(tail[0]);
            tail[0] = tail[1];
            tail[1] = tail[2];
            tail[2] = xstrdup(line);
            size_t tl = strlen(tail[2]);
            while (tl && tail[2][tl - 1] == '\r')
                tail[2][--tl] = 0;
        }
        free(copy);
    }
    sb_t joined;
    sb_init(&joined);
    for (int i = 0; i < 3; i++) {
        if (!tail[i])
            continue;
        if (joined.n)
            sb_addc(&joined, '\n');
        sb_adds(&joined, tail[i]);
    }
    const char *joined_text = sb_cstr(&joined);
    size_t jlen = strlen(joined_text);
    const char *log_tail = jlen > 600 ? joined_text + jlen - 600 : joined_text;

    const char *ver = js(s, "cli_version", "");
    char *ver_owned = NULL;
    if (!*ver) {
        ver_owned = cli_version(cli);
        ver = ver_owned;
    }

    jval *o = jobj();
    jo_setbool(o, "ok", 1);
    jo_setbool(o, "running", *unit != 0);
    jo_setstr(o, "unit", unit);
    jo_setint(o, "downloaded", done);
    jo_setint(o, "total", total);
    jo_setint(o, "percent", percent);
    jo_setstr(o, "phase", tail[2] ? tail[2] : "");
    jo_setstr(o, "log", log_tail);
    jo_setstr(o, "cli_version", ver);
    jo_setstr(o, "path", tarball);
    result_out(out, o);

    for (int i = 0; i < 3; i++)
        free(tail[i]);
    sb_free(&joined);
    free(ver_owned);
    free(unit);
}

/*
 * systemd-run the updater outside the service sandbox; returns immediately.
 * The reporter samples the host-side tarball into the recordings directory
 * because the service runs with PrivateTmp.
 */
static void start_update_outside_sandbox(const char *cli, char *unit_out,
                                         size_t unit_size, int *rc_out,
                                         char **output_out, char *log_out,
                                         size_t log_size)
{
    char log_path[PATH_MAX];
    pj(log_path, sizeof(log_path), P_REC, "codex_update.log");
    snprintf(log_out, log_size, "%s", log_path);

    char tmpdir[PATH_MAX];
    pj(tmpdir, sizeof(tmpdir), P_REC, "tmp");
    mkdir_p(tmpdir, 0777);
    if (chmod(tmpdir, 0777) != 0) {
        /* best effort */
    }

    char progress[PATH_MAX];
    pj(progress, sizeof(progress), P_REC, "codex_update_progress.txt");

    snprintf(unit_out, unit_size, "codex-update-%lld", now_epoch());

    sb_t shell;
    sb_init(&shell);
    sb_addf(&shell,
            "(for _ in $(seq 1 1800); do "
            "f=$(ls -t /tmp/tmp.*/codex-package-aarch64-unknown-linux-musl.tar.gz "
            "2>/dev/null | head -1); "
            "if [ -n \"$f\" ]; then stat -c %%s \"$f\" > %s 2>/dev/null; fi; "
            "sleep 2; done) & REPORTER=$!; "
            "for i in 1 2 3; do %s update && break; echo \"retry $i\"; sleep 5; done; "
            "kill $REPORTER 2>/dev/null",
            progress, cli);
    char *reporter = sb_take(&shell);
    sb_t script;
    sb_init(&script);
    sb_addf(&script, "{ %s; } > %s 2>&1", reporter, log_path);
    free(reporter);

    char *systemd_run = which_exe("systemd-run");
    if (!systemd_run)
        raise_msg("FileNotFoundError: [Errno 2] No such file or directory: "
                  "'systemd-run'");

    char *argv[48];
    size_t n = 0;
    argv[n++] = systemd_run;
    argv[n++] = (char *)"--collect";
    argv[n++] = (char *)"--quiet";
    argv[n++] = (char *)"--unit";
    argv[n++] = unit_out;
    argv[n++] = (char *)"--uid";
    argv[n++] = (char *)CLI_USER;
    argv[n++] = (char *)"--gid";
    argv[n++] = (char *)CLI_USER;
    char *setenvs[16];
    size_t ns = 0;
    char buf_home[64], buf_codex[64], buf_path[128], buf_tmp[PATH_MAX + 16];
    snprintf(buf_home, sizeof(buf_home), "HOME=%s", CLI_HOME);
    snprintf(buf_codex, sizeof(buf_codex), "CODEX_HOME=%s/.codex", CLI_HOME);
    snprintf(buf_path, sizeof(buf_path), "PATH=%s", CLI_PATH_ENV);
    snprintf(buf_tmp, sizeof(buf_tmp), "TMPDIR=%s", tmpdir);
    setenvs[ns++] = buf_home;
    setenvs[ns++] = buf_codex;
    setenvs[ns++] = buf_path;
    setenvs[ns++] = (char *)"TERM=dumb";
    setenvs[ns++] = (char *)"NO_COLOR=1";
    setenvs[ns++] = buf_tmp;

    env_t px;
    env_init(&px);
    proxy_env(&px);
    for (size_t i = 0; i < px.n && ns < 16; i++)
        setenvs[ns++] = px.v[i];

    for (size_t i = 0; i < ns; i++) {
        argv[n++] = (char *)"--setenv";
        argv[n++] = setenvs[i];
    }
    argv[n++] = (char *)"/bin/sh";
    argv[n++] = (char *)"-c";
    argv[n++] = (char *)sb_cstr(&script);
    argv[n] = NULL;

    env_t env;
    env_init(&env);
    proc_t r;
    int spawn_rc = run_proc(argv, &env, NULL, 0, NULL, 0, 20000, NULL, &r);
    *rc_out = (spawn_rc == 0) ? r.rc : -1;
    sb_t combined;
    sb_init(&combined);
    if (spawn_rc == 0) {
        sb_addn(&combined, sb_cstr(&r.out), r.out.n);
        sb_addn(&combined, sb_cstr(&r.err), r.err.n);
    }
    *output_out = sb_take(&combined);
    proc_free(&r);
    env_free(&env);
    env_free(&px);
    sb_free(&script);
    free(systemd_run);
}

static void cmd_update_install(const char *out)
{
    jval *s = settings();
    const char *cli = js(s, "cli_path", "");
    if (!cli_available(cli)) {
        jval *o = jobj();
        jo_setbool(o, "ok", 0);
        jo_setstr(o, "error", "Codex CLI not found");
        result_out(out, o);
        return;
    }
    char *before = version_of(js(s, "cli_version", ""));
    if (!*before) {
        free(before);
        char *raw = cli_version(cli);
        before = version_of(raw);
        free(raw);
    }

    char log_path[PATH_MAX];
    pj(log_path, sizeof(log_path), P_REC, "codex_update.log");

    char *systemd_run = which_exe("systemd-run");
    if (geteuid() == 0 && systemd_run) {
        free(systemd_run);
        char *existing = running_update_unit();
        if (*existing) {
            jval *o = jobj();
            jo_setbool(o, "ok", 1);
            jo_setbool(o, "started", 1);
            jo_setstr(o, "before", before);
            jo_setstr(o, "version", before);
            jo_setstr(o, "unit", existing);
            jo_setstr(o, "log", log_path);
            jo_setstr(o, "note", "an update is already running");
            jo_setstr(o, "update_command", UPDATE_COMMAND);
            result_out(out, o);
            free(existing);
            free(before);
            return;
        }
        free(existing);
        char unit[128];
        char *output = NULL;
        int rc = -1;
        start_update_outside_sandbox(cli, unit, sizeof(unit), &rc, &output,
                                     log_path, sizeof(log_path));
        jval *o = jobj();
        jo_setbool(o, "ok", rc == 0);
        jo_setbool(o, "started", rc == 0);
        jo_setstr(o, "before", before);
        jo_setstr(o, "version", before);
        jo_setstr(o, "unit", unit);
        jo_setstr(o, "log", log_path);
        size_t olen = output ? strlen(output) : 0;
        /* output.strip()[-500:] */
        char *stripped = strip_dup(output ? output : "");
        size_t slen = strlen(stripped);
        jo_setstr(o, "output", slen > 500 ? stripped + slen - 500 : stripped);
        jo_setstr(o, "note", "update started in the background");
        jo_setstr(o, "update_command", UPDATE_COMMAND);
        result_out(out, o);
        free(stripped);
        free(output);
        free(before);
        (void)olen;
        return;
    }
    free(systemd_run);

    char *argv[3];
    argv[0] = (char *)cli;
    argv[1] = (char *)"update";
    argv[2] = NULL;
    env_t env;
    build_update_env(&env);
    proc_t r;
    int spawn = run_proc(argv, &env, CLI_HOME, 0, NULL, 0,
                         UPDATE_TIMEOUT_S * 1000, NULL, &r);
    env_free(&env);

    jval *o = jobj();
    if (spawn != 0 || r.spawn_errno) {
        int err = r.spawn_errno;
        char msg[400];
        if (err == ENOENT)
            snprintf(msg, sizeof(msg),
                     "FileNotFoundError: [Errno 2] No such file or directory: '%s'",
                     cli);
        else if (err == EACCES)
            snprintf(msg, sizeof(msg),
                     "PermissionError: [Errno 13] Permission denied: '%s'", cli);
        else
            snprintf(msg, sizeof(msg), "OSError: [Errno %d] %s", err,
                     strerror(err));
        jo_setbool(o, "ok", 0);
        jo_setstr(o, "before", before);
        jo_setstr(o, "version", before);
        jo_setstr(o, "error", msg);
        result_out(out, o);
        proc_free(&r);
        free(before);
        return;
    }
    if (r.timed_out) {
        jo_setbool(o, "ok", 0);
        jo_setstr(o, "before", before);
        jo_setstr(o, "version", before);
        jo_setstr(o, "error", "codex update timed out");
        result_out(out, o);
        proc_free(&r);
        free(before);
        return;
    }
    int rc = r.rc;
    /* (stdout + stderr).strip()[-4000:] */
    sb_t both;
    sb_init(&both);
    sb_addn(&both, sb_cstr(&r.out), r.out.n);
    sb_addn(&both, sb_cstr(&r.err), r.err.n);
    char *stripped = strip_dup(sb_cstr(&both));
    size_t slen = strlen(stripped);
    char *shown = slen > 4000 ? xstrdup(stripped + slen - 4000) : xstrdup(stripped);
    free(stripped);
    sb_free(&both);
    proc_free(&r);

    char *after = cli_version(cli);
    char *latest = *after ? version_of(after) : xstrdup(before);
    if (*after) {
        jo_setstr(s, "cli_version", after);
        write_json_file(P_SETTINGS, s, 0600);
    }
    int changed = *latest && strcmp(latest, before) != 0 &&
                  version_gt(latest, before);

    jo_setbool(o, "ok", rc == 0 && changed);
    jo_setint(o, "exit_code", rc);
    jo_setstr(o, "before", before);
    jo_setstr(o, "version", latest);
    jo_setstr(o, "cli_version", after);
    jo_setbool(o, "changed", changed);
    jo_setstr(o, "output", shown);
    jo_setstr(o, "update_command", UPDATE_COMMAND);
    result_out(out, o);
    free(shown);
    free(after);
    free(latest);
    free(before);
}

/* ------------------------------------------------------------------ */
/* model catalog                                                       */
/* ------------------------------------------------------------------ */

static jval *cache_read(const char *path, int ttl)
{
    jval *data = read_json(path, jobj());
    if (!data || data->t != J_OBJ || !data->pn)
        return NULL;
    int ok = 0;
    long long at = jtoint(jo(data, "cached_at"), &ok);
    if (!ok)
        at = 0;
    if ((now_epoch() - at) < ttl)
        return data;
    return NULL;
}

static jval *cache_write(const char *path, jval *value)
{
    jval *v = jclone(value);
    jo_setint(v, "cached_at", now_epoch());
    write_json_file(path, v, 0644);
    return v;
}

static void cmd_models(const char *out)
{
    jval *s = settings();
    char cache_path[PATH_MAX];
    pj(cache_path, sizeof(cache_path), P_CX, "models_cache.json");

    jval *cached = cache_read(cache_path, MODELS_TTL);
    if (cached && jtruthy(jo(cached, "models"))) {
        jval *payload = jclone(cached);
        jo_pop(payload, "cached_at");
        jo_setstr(payload, "selected", js(s, "model", ""));
        jo_setbool(payload, "cached", 1);
        result_out(out, payload);
        return;
    }

    const char *cli = js(s, "cli_path", "");
    if (!cli_available(cli)) {
        jval *o = jobj();
        jo_setbool(o, "ok", 0);
        jo_set(o, "models", jarr());
        jo_setstr(o, "error", "Codex CLI not found");
        result_out(out, o);
        return;
    }

    char *argv[4];
    argv[0] = (char *)cli;
    argv[1] = (char *)"debug";
    argv[2] = (char *)"models";
    argv[3] = NULL;
    env_t env;
    build_cli_env(&env);
    proc_t r;
    int spawn = run_proc(argv, &env, NULL, 0, NULL, 0, 45000, NULL, &r);
    env_free(&env);

    jval *o = jobj();
    if (spawn != 0 || r.spawn_errno || r.timed_out) {
        char msg[400];
        if (r.timed_out)
            snprintf(msg, sizeof(msg),
                     "TimeoutExpired: Command '['%s', 'debug', 'models'] timed "
                     "out after 45 seconds", cli);
        else if (r.spawn_errno == ENOENT)
            snprintf(msg, sizeof(msg),
                     "FileNotFoundError: [Errno 2] No such file or directory: '%s'",
                     cli);
        else if (r.spawn_errno == EACCES)
            snprintf(msg, sizeof(msg),
                     "PermissionError: [Errno 13] Permission denied: '%s'", cli);
        else
            snprintf(msg, sizeof(msg), "OSError: [Errno %d] %s",
                     (int)r.spawn_errno, strerror((int)r.spawn_errno));
        if (strlen(msg) > 200)
            msg[200] = 0;
        jo_setbool(o, "ok", 0);
        jo_set(o, "models", jarr());
        jo_setstr(o, "error", msg);
        result_out(out, o);
        proc_free(&r);
        return;
    }

    jval *catalog = json_try(sb_cstr(&r.out), r.out.n);
    if (!catalog || catalog->t != J_OBJ) {
        char *errtext = strip_dup(r.err.n ? sb_cstr(&r.err)
                                           : "cannot parse model catalog");
        if (strlen(errtext) > 200)
            errtext[200] = 0;
        jo_setbool(o, "ok", 0);
        jo_set(o, "models", jarr());
        jo_setstr(o, "error", errtext);
        result_out(out, o);
        free(errtext);
        proc_free(&r);
        return;
    }
    proc_free(&r);

    jval *models = jarr();
    jval *list = jo(catalog, "models");
    if (list && list->t == J_ARR) {
        for (size_t i = 0; i < list->n; i++) {
            jval *m = list->items[i];
            if (!m || m->t != J_OBJ)
                continue;
            const char *vis = js(m, "visibility", "");
            if (*vis ? strcmp(vis, "list") != 0 : 0)
                continue;
            jval *api = jo(m, "supported_in_api");
            if (api && !jtruthy(api))
                continue;
            char *slug = strip_dup(js(m, "slug", ""));
            if (!*slug) {
                free(slug);
                continue;
            }
            const char *name = js(m, "display_name", "");
            jval *efforts = jarr();
            jval *levels = jo(m, "supported_reasoning_levels");
            if (levels && levels->t == J_ARR) {
                for (size_t k = 0; k < levels->n; k++) {
                    jval *lvl = levels->items[k];
                    if (!lvl || lvl->t != J_OBJ)
                        continue;
                    jval *eff = jo(lvl, "effort");
                    if (!jtruthy(eff))
                        continue;
                    ja_add(efforts, jstr(eff && eff->s ? eff->s : ""));
                }
            }
            jval *row = jobj();
            jo_setstr(row, "slug", slug);
            jo_setstr(row, "name", *name ? name : slug);
            jo_setstr(row, "default_effort", js(m, "default_reasoning_level", ""));
            jo_set(row, "efforts", efforts);
            ja_add(models, row);
            free(slug);
        }
    }

    jval *stored = jobj();
    jo_setbool(stored, "ok", 1);
    jo_set(stored, "models", jclone(models));
    jo_setstr(stored, "source", "codex debug models");
    jval *payload = cache_write(cache_path, stored);
    jo_pop(payload, "cached_at");
    jo_setstr(payload, "selected", js(s, "model", ""));
    result_out(out, payload);
}

/* ------------------------------------------------------------------ */
/* quota                                                               */
/* ------------------------------------------------------------------ */

/* {"used_percent": .., "window_minutes": .., "resets_at": ..} */
static jval *quota_window(jval *value)
{
    if (!value || value->t != J_OBJ)
        return jnull();
    jval *w = jobj();
    jval *up = jo(value, "usedPercent");
    jo_set(w, "used_percent", up ? jclone(up) : jnull());
    jval *wm = jo(value, "windowDurationMins");
    jo_set(w, "window_minutes", wm ? jclone(wm) : jnull());
    jval *ra = jo(value, "resetsAt");
    jo_set(w, "resets_at", ra ? jclone(ra) : jnull());
    return w;
}

static void cmd_quota(const char *out)
{
    if (!*auth_mode()) {
        jval *o = jobj();
        jo_setbool(o, "ok", 0);
        jo_setstr(o, "error", "not signed in");
        jo_setbool(o, "signed_in", 0);
        result_out(out, o);
        return;
    }
    char cache_path[PATH_MAX];
    pj(cache_path, sizeof(cache_path), P_CX, "quota_cache.json");

    jval *cached = cache_read(cache_path, QUOTA_TTL);
    if (cached) {
        jval *payload = jclone(cached);
        jo_pop(payload, "cached_at");
        jo_setbool(payload, "cached", 1);
        result_out(out, payload);
        return;
    }

    char errbuf[400];
    jval *r = app_server_call("account/rateLimits/read", jobj(),
                              APP_SERVER_TIMEOUT_MS, errbuf, sizeof(errbuf));
    if (!r) {
        jval *stale = read_json(cache_path, jobj());
        if (stale && stale->t == J_OBJ && stale->pn) {
            jo_pop(stale, "cached_at");
            jo_setbool(stale, "cached", 1);
            jo_setbool(stale, "stale", 1);
            result_out(out, stale);
            return;
        }
        jval *o = jobj();
        jo_setbool(o, "ok", 0);
        jo_setbool(o, "signed_in", 1);
        char msg[480];
        /* "%s: %s" % (type(exc).__name__, str(exc)[:160]) */
        snprintf(msg, sizeof(msg), "RuntimeError: %.160s", errbuf);
        jo_setstr(o, "error", msg);
        result_out(out, o);
        return;
    }

    jval *limits = jo(r, "rateLimits");
    if (!limits || limits->t != J_OBJ)
        limits = jobj();
    jval *credits = jo(limits, "credits");
    if (!credits || credits->t != J_OBJ)
        credits = jobj();

    jval *stored = jobj();
    jo_setbool(stored, "ok", 1);
    jo_setbool(stored, "signed_in", 1);
    jo_setstr(stored, "plan", js(limits, "planType", ""));
    jo_setbool(stored, "allowed", jtruthy(jo(r, "ordinaryUsageAllowed")));
    jo_set(stored, "primary", quota_window(jo(limits, "primary")));
    jo_set(stored, "secondary", quota_window(jo(limits, "secondary")));
    jval *cred = jobj();
    jo_setbool(cred, "has", jtruthy(jo(credits, "hasCredits")));
    jo_setbool(cred, "unlimited", jtruthy(jo(credits, "unlimited")));
    jval *bal = jo(credits, "balance");
    jo_set(cred, "balance", bal ? jclone(bal) : jnull());
    jo_set(stored, "credits", cred);
    jo_setstr(stored, "reached", js(limits, "rateLimitReachedType", ""));

    jval *rc = jo(r, "rateLimitResetCredits");
    jval *avail = (rc && rc->t == J_OBJ) ? jo(rc, "availableCount") : NULL;
    jo_set(stored, "reset_credits", avail ? jclone(avail) : jint(0));
    jo_setint(stored, "fetched_at", now_epoch());

    jval *payload = cache_write(cache_path, stored);
    jo_pop(payload, "cached_at");
    result_out(out, payload);
}
/* ------------------------------------------------------------------ */
/* terminal text helpers (device login)                                */
/* ------------------------------------------------------------------ */

/*
 * Strip what the Python bridge strips before matching:
 *   \x1b\[[0-9;?]*[A-Za-z]                (CSI colour/cursor)
 *   \x1b\][^\x07\x1b]*(?:\x07|\x1b\\)     (OSC-8 hyperlinks)
 */
static char *ansi_strip(const char *in, size_t len)
{
    sb_t out;
    sb_init(&out);
    sb_reserve(&out, len);
    size_t i = 0;
    while (i < len) {
        if ((unsigned char)in[i] == 0x1b && i + 1 < len) {
            if (in[i + 1] == '[') {
                size_t j = i + 2;
                while (j < len && (isdigit((unsigned char)in[j]) || in[j] == ';' ||
                                   in[j] == '?'))
                    j++;
                if (j < len && isalpha((unsigned char)in[j])) {
                    i = j + 1;
                    continue;
                }
            } else if (in[i + 1] == ']') {
                size_t j = i + 2;
                while (j < len && (unsigned char)in[j] != 0x07 &&
                       (unsigned char)in[j] != 0x1b)
                    j++;
                if (j < len && (unsigned char)in[j] == 0x07) {
                    i = j + 1;
                    continue;
                }
                if (j + 1 < len && (unsigned char)in[j] == 0x1b && in[j + 1] == '\\') {
                    i = j + 2;
                    continue;
                }
            }
        }
        sb_addc(&out, in[i]);
        i++;
    }
    return sb_take(&out);
}

/* https://<run>/device<not a letter><rest of the non-space run> */
static char *find_device_url(const char *text)
{
    const char *p = text;
    while ((p = strstr(p, "https://")) != NULL) {
        size_t run = 0;
        while (p[run] && !isspace((unsigned char)p[run]))
            run++;
        /* the regex greedily backtracks to the last "/device" in the run */
        size_t hit = 0;
        int found = 0;
        for (size_t k = run; k >= 7; k--) {
            if (!memcmp(p + k - 7, "/device", 7)) {
                size_t after = k;
                if (after >= run || !isalpha((unsigned char)p[after])) {
                    hit = k;
                    found = 1;
                    break;
                }
            }
        }
        if (found) {
            size_t end = run;
            while (end && strchr(".,;)", p[end - 1]))
                end--;
            (void)hit;
            return xstrndup(p, end);
        }
        p += 8;
    }
    /* fall back to the auth host, but never expose an API endpoint */
    p = text;
    while ((p = strstr(p, "https://")) != NULL) {
        size_t run = 0;
        while (p[run] && !isspace((unsigned char)p[run]))
            run++;
        size_t host_end = 8;
        while (host_end < run && p[host_end] != '/')
            host_end++;
        char *host = xstrndup(p + 8, host_end - 8);
        int is_auth = strstr(host, "auth.openai.com") != NULL;
        free(host);
        if (is_auth) {
            char *whole = xstrndup(p, run);
            int api = strstr(whole, "/api/") != NULL;
            free(whole);
            if (!api) {
                size_t end = run;
                while (end && strchr(".,;)", p[end - 1]))
                    end--;
                return xstrndup(p, end);
            }
        }
        p += 8;
    }
    return xstrdup("");
}

static int is_word_char(unsigned char c)
{
    return isalnum(c) || c == '_';
}

/* \b([A-Z0-9]{4,}-[A-Z0-9]{4,})\b */
static char *find_device_code(const char *text)
{
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        if (!(isupper((unsigned char)text[i]) || isdigit((unsigned char)text[i])))
            continue;
        if (i > 0 && is_word_char((unsigned char)text[i - 1]))
            continue;
        size_t j = i;
        while (j < len && (isupper((unsigned char)text[j]) ||
                           isdigit((unsigned char)text[j])))
            j++;
        size_t first_len = j - i;
        if (first_len < 4)
            continue;
        /* a longer run cannot be split: only "-" may follow the first group */
        if (j >= len || text[j] != '-')
            continue;
        size_t k = j + 1;
        size_t start2 = k;
        while (k < len && (isupper((unsigned char)text[k]) ||
                           isdigit((unsigned char)text[k])))
            k++;
        if (k - start2 < 4)
            continue;
        if (k < len && is_word_char((unsigned char)text[k]))
            continue;
        return xstrndup(text + i, k - i);
    }
    return xstrdup("");
}

/* Keep the last `limit` bytes of a growing string. */
static void keep_tail(char **buf, size_t *len, const char *chunk, size_t n,
                      size_t limit)
{
    size_t total = *len + n;
    char *merged = xmalloc(total + 1);
    memcpy(merged, *buf, *len);
    memcpy(merged + *len, chunk, n);
    merged[total] = 0;
    free(*buf);
    if (total > limit) {
        memmove(merged, merged + (total - limit), limit);
        merged[limit] = 0;
        total = limit;
    }
    *buf = merged;
    *len = total;
}

/* ------------------------------------------------------------------ */
/* detached workers                                                    */
/* ------------------------------------------------------------------ */

static void background(void (*fn)(void *), void *arg)
{
    pid_t pid = fork();
    if (pid != 0)
        return;
    setsid();
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > 2)
            close(devnull);
    }
    static int reporting;
    if (setjmp(g_raise_jmp) == 0) {
        reporting = 0;
        fn(arg);
    } else if (!reporting) {
        /* a detached worker must never die silently */
        reporting = 1;
        jval *ch = jobj();
        jo_setbool(ch, "busy", 0);
        char msg[460];
        snprintf(msg, sizeof(msg), "worker: %.400s", g_raise_msg);
        jo_setstr(ch, "last_error", msg);
        jo_setint(ch, "finished_at", now_epoch());
        write_state(ch);
        jval *ev = jobj();
        jo_setstr(ev, "role", "event");
        jo_setstr(ev, "event", "worker_error");
        jo_setstr(ev, "content", msg + 8);
        jo_setbool(ev, "ok", 0);
        append_record(ev);
    }
    _exit(0);
}

/* ------------------------------------------------------------------ */
/* device-code login                                                   */
/* ------------------------------------------------------------------ */

static void write_login(const char *state, const char *url, const char *code,
                        const char *message, long long started_at,
                        long long expires_at)
{
    jval *lg = jobj();
    jo_setstr(lg, "state", state);
    jo_setstr(lg, "url", url);
    jo_setstr(lg, "code", code);
    jo_setstr(lg, "message", message);
    jo_setint(lg, "started_at", started_at);
    jo_setint(lg, "expires_at", expires_at);
    write_json_file(P_LOGIN, lg, 0600);
}

typedef struct {
    char *cli;
} login_arg;

/* forkpty() equivalent built from posix_openpt (no libutil needed). */
static pid_t spawn_login_pty(const char *cli, int *master_out)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0)
        return -1;
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);
        return -1;
    }
    char slave_name[256];
    if (ptsname_r(master, slave_name, sizeof(slave_name)) != 0) {
        close(master);
        return -1;
    }

    env_t env;
    build_cli_env(&env);
    pid_t pid = fork();
    if (pid < 0) {
        env_free(&env);
        close(master);
        return -1;
    }
    if (pid == 0) {
        setsid();
        int slave = open(slave_name, O_RDWR);
        if (slave < 0)
            _exit(127);
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > 2)
            close(slave);
        close(master);
        drop_privileges();
        char *argv[4];
        argv[0] = (char *)cli;
        argv[1] = (char *)"login";
        argv[2] = (char *)"--device-auth";
        argv[3] = NULL;
        execve(argv[0], argv, env.v);
        _exit(127);
    }
    env_free(&env);
    *master_out = master;
    return pid;
}

static void device_login_worker(void *vp)
{
    login_arg *a = vp;
    long long started = now_epoch();
    write_login("starting", "", "", "", started, started + 900);

    int master = -1;
    pid_t pid = spawn_login_pty(a->cli, &master);
    if (pid < 0) {
        char msg[320];
        snprintf(msg, sizeof(msg), "%s",
                 "OSError: [Errno 5] Input/output error");
        write_login("error", "", "", msg, now_epoch(), 0);
        return;
    }

    char *clean = xstrdup("");
    size_t clean_len = 0;
    char *tail = xstrdup("");
    size_t tail_len = 0;
    char *url = xstrdup("");
    char *code = xstrdup("");
    int child_done = 0;
    int child_status = 0;

    while (now_epoch() - started < 900) {
        struct pollfd pfd = { master, POLLIN, 0 };
        int ready = poll(&pfd, 1, 0);
        if (ready > 0) {
            char buf[4096];
            ssize_t r = read(master, buf, sizeof(buf));
            if (r > 0) {
                char *plain = ansi_strip(buf, (size_t)r);
                keep_tail(&clean, &clean_len, plain, strlen(plain), 8000);
                free(plain);
                keep_tail(&tail, &tail_len, buf, (size_t)r, 1200);
                if (!*url) {
                    free(url);
                    url = find_device_url(clean);
                }
                if (!*code) {
                    free(code);
                    code = find_device_code(clean);
                }
                if (*url || *code) {
                    write_login("pending", url, code,
                                "Waiting for browser authorisation", started,
                                started + 900);
                }
                continue;
            }
            /* pty closed: the child is finishing, let the wait below reap it */
            break;
        }
        pid_t w = waitpid(pid, &child_status, WNOHANG);
        if (w == pid)
            child_done = 1;
        if (child_done)
            break;
        /* wait up to a second for terminal output */
        int pr = poll(&pfd, 1, 1000);
        if (pr <= 0)
            continue;
        char buf[4096];
        ssize_t r = read(master, buf, sizeof(buf));
        if (r <= 0)
            break;
        char *plain = ansi_strip(buf, (size_t)r);
        keep_tail(&clean, &clean_len, plain, strlen(plain), 8000);
        free(plain);
        keep_tail(&tail, &tail_len, buf, (size_t)r, 1200);
        if (!*url) {
            free(url);
            url = find_device_url(clean);
        }
        if (!*code) {
            free(code);
            code = find_device_code(clean);
        }
        if (*url || *code) {
            write_login("pending", url, code,
                        "Waiting for browser authorisation", started,
                        started + 900);
        }
    }
    close(master);

    int rc = -1;
    if (child_done && WIFEXITED(child_status))
        rc = WEXITSTATUS(child_status);
    else if (child_done && WIFSIGNALED(child_status))
        rc = -WTERMSIG(child_status);
    if (!child_done) {
        long long deadline = now_millis() + 5000;
        int status = 0;
        pid_t w = 0;
        while (now_millis() < deadline) {
            w = waitpid(pid, &status, WNOHANG);
            if (w == pid)
                break;
            struct timespec ts = { 0, 20 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        if (w == pid)
            rc = wait_status_to_rc(status);
        else {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            rc = -1;
        }
    }

    char *clean_tail = ansi_strip(tail, strlen(tail));
    for (char *p = clean_tail; *p; p++)
        if (*p == '\r')
            *p = ' ';
    char *clean_final = strip_dup(clean_tail);
    free(clean_tail);

    if (!*url) {
        free(url);
        url = find_device_url(clean);
    }
    if (!*code) {
        free(code);
        code = find_device_code(clean);
        if (!*code) {
            free(code);
            code = find_device_code(clean_final);
        }
    }

    int logged = file_exists(P_AUTH);
    const char *final = (rc == 0 || logged) ? "success" : "error";
    char message[400];
    if (!strcmp(final, "success")) {
        snprintf(message, sizeof(message), "Device login finished");
    } else {
        size_t len = strlen(clean_final);
        const char *tailp = len > 300 ? clean_final + len - 300 : clean_final;
        if (!*tailp)
            snprintf(message, sizeof(message), "Device login failed");
        else
            snprintf(message, sizeof(message), "%s", tailp);
    }
    write_login(final, url, code, message, started, 0);

    jval *s = settings();
    if (!*js(s, "cli_version", "")) {
        char *ver = cli_version(a->cli);
        jo_setstr(s, "cli_version", ver);
        free(ver);
        write_json_file(P_SETTINGS, s, 0600);
    }
    config_toml(s);

    free(clean);
    free(tail);
    free(url);
    free(code);
    free(clean_final);
}

static void cmd_login_device(const char *out)
{
    jval *s = settings();
    const char *cli = js(s, "cli_path", "");
    if (!cli_available(cli)) {
        jval *o = jobj();
        jo_setbool(o, "ok", 0);
        jo_setstr(o, "error", "Codex CLI not found");
        result_out(out, o);
        return;
    }
    config_toml(s);
    long long now = now_epoch();
    write_login("starting", "", "", "", now, now + 900);

    login_arg *a = xmalloc(sizeof(*a));
    a->cli = xstrdup(cli);
    background(device_login_worker, a);

    jval *o = jobj();
    jo_setbool(o, "ok", 1);
    result_out(out, o);
}

/* ------------------------------------------------------------------ */
/* chat turn                                                           */
/* ------------------------------------------------------------------ */

/* First 1200 bytes of a UTF-8 string, on a character boundary. */
static char *truncate_chars(const char *s, size_t limit)
{
    size_t len = strlen(s);
    size_t chars = 0, i = 0;
    while (i < len && chars < limit) {
        unsigned char c = (unsigned char)s[i];
        size_t step = 1;
        if (c >= 0xF0)
            step = 4;
        else if (c >= 0xE0)
            step = 3;
        else if (c >= 0xC0)
            step = 2;
        if (i + step > len)
            break;
        i += step;
        chars++;
    }
    return xstrndup(s, i);
}

static char *build_prompt(const char *message)
{
    jval *hist = read_records(20);
    filter_dialog(hist);
    keep_last(hist, 6);
    if (!hist->n)
        return xstrdup(message);

    sb_t o;
    sb_init(&o);
    sb_adds(&o, "Previous conversation (for context only):");
    for (size_t i = 0; i < hist->n; i++) {
        jval *row = hist->items[i];
        const char *role = js(row, "role", "");
        const char *content = js(row, "content", "");
        sb_addc(&o, '\n');
        sb_adds(&o, strcmp(role, "user") ? "Codex: " : "User: ");
        char *cut = truncate_chars(content, 1200);
        sb_adds(&o, cut);
        free(cut);
    }
    sb_adds(&o, "\n\nCurrent message:\n");
    sb_adds(&o, message);
    return sb_take(&o);
}

typedef struct {
    char *job;
    char *message;
    char *cli;
} chat_arg;

/* Publish the worker pid as soon as it exists, so `stop` can kill it. */
static void chat_on_start(pid_t pid)
{
    jval *ch = jobj();
    jo_setint(ch, "child_pid", (long long)pid);
    write_state(ch);
}

static void chat_worker(void *vp)
{
    chat_arg *a = vp;
    long long started_ms = now_millis();

    jval *st0 = state();
    jval *rec = jobj();
    jo_setstr(rec, "role", "user");
    jo_setstr(rec, "content", a->message);
    jo_setstr(rec, "job", a->job);
    jo_setstr(rec, "session", js(st0, "thread_id", ""));
    jo_setbool(rec, "ok", 1);
    append_record(rec);

    jval *s = settings();
    config_toml(s);

    char reply_file[PATH_MAX * 2];
    snprintf(reply_file, sizeof(reply_file), "%s/reply_%s.txt", P_OUT, a->job);

    char *argv[12];
    size_t n = 0;
    argv[n++] = a->cli;
    argv[n++] = (char *)"exec";
    argv[n++] = (char *)"--json";
    argv[n++] = (char *)"--skip-git-repo-check";
    argv[n++] = (char *)"-s";
    argv[n++] = (char *)FORCED_SANDBOX;
    argv[n++] = (char *)"-C";
    argv[n++] = (char *)P_ROOT;
    argv[n++] = (char *)"-o";
    argv[n++] = reply_file;
    argv[n++] = (char *)"-";
    argv[n] = NULL;

    char *thread_id = xstrdup(js(st0, "thread_id", ""));
    char *error = xstrdup("");
    char *reply = xstrdup("");
    jval *events = jarr();              /* {type, line[:500]} */
    char **raw_lines = NULL;
    size_t raw_n = 0, raw_cap = 0;

    char *prompt = build_prompt(a->message);
    env_t env;
    build_cli_env(&env);
    proc_t r;
    int spawn = run_proc(argv, &env, NULL, 1, prompt, strlen(prompt), 300000,
                         chat_on_start, &r);
    env_free(&env);
    free(prompt);

    if (spawn != 0 || r.spawn_errno) {
        int err = r.spawn_errno;
        if (err == ENOENT)
            free(error), error = xstrdup("FileNotFoundError: [Errno 2] No such "
                                        "file or directory: codex");
        else if (err)
            free(error), error = xstrdup(strerror(err));
        else
            free(error), error = xstrdup("cannot start codex");
    }

    int rc = r.rc;
    if (r.timed_out) {
        free(error);
        error = xstrdup("Codex timed out after 300 s");
    }

    /* the CLI announces the thread and the failure in its event stream */
    if (r.out.n) {
        char *copy = xstrdup(sb_cstr(&r.out));
        char *save = NULL;
        for (char *line = strtok_r(copy, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            char *clean = strip_dup(line);
            if (!*clean) {
                free(clean);
                continue;
            }
            if (raw_n == raw_cap) {
                raw_cap = raw_cap ? raw_cap * 2 : 16;
                raw_lines = xrealloc(raw_lines, raw_cap * sizeof(*raw_lines));
            }
            raw_lines[raw_n++] = xstrdup(clean);

            jval *ev = json_try(clean, strlen(clean));
            if (!ev || ev->t != J_OBJ) {
                free(clean);
                continue;
            }
            const char *etype = js(ev, "type", "");
            if (!strcmp(etype, "thread.started")) {
                const char *tid = js(ev, "thread_id", "");
                if (*tid) {
                    free(thread_id);
                    thread_id = xstrdup(tid);
                }
            } else if (!strcmp(etype, "error") || !strcmp(etype, "turn.failed")) {
                jval *info = jo(ev, "error");
                const char *msg = js(ev, "message", "");
                if (!*msg && info && info->t == J_OBJ)
                    msg = js(info, "message", "");
                if (!*msg)
                    msg = "turn failed";
                free(error);
                error = xstrdup(msg);
            }
            jval *row = jobj();
            jo_setstr(row, "type", etype);
            char *shortline = truncate_chars(clean, 500);
            jo_setstr(row, "line", shortline);
            free(shortline);
            ja_add(events, row);
            free(clean);
        }
        free(copy);
    }

    if (!*error && rc != 0) {
        char *tail = strip_dup(r.err.n ? sb_cstr(&r.err) : "");
        size_t len = strlen(tail);
        const char *p = len > 300 ? tail + len - 300 : tail;
        free(error);
        if (*p) {
            error = xstrdup(p);
        } else {
            char buf[96];
            snprintf(buf, sizeof(buf), "codex exec exited with %d", rc);
            error = xstrdup(buf);
        }
        free(tail);
    }

    sb_t reply_buf;
    sb_init(&reply_buf);
    if (read_all(reply_file, &reply_buf) == 0) {
        const char *text = sb_cstr(&reply_buf);
        char *clean = strip_dup(text);
        free(reply);
        reply = clean;
    }
    sb_free(&reply_buf);

    if (!*reply) {
        for (size_t i = raw_n; i > 0; i--) {
            jval *obj = json_try(raw_lines[i - 1], strlen(raw_lines[i - 1]));
            if (!obj || obj->t != J_OBJ)
                continue;
            jval *item = jo(obj, "item");
            if (!item || item->t != J_OBJ)
                continue;
            const char *itype = js(item, "type", "");
            if (strcmp(itype, "agent_message") != 0)
                continue;
            const char *text = js(item, "text", "");
            char *clean = strip_dup(text);
            free(reply);
            reply = clean;
            if (*reply)
                break;
        }
    }

    for (size_t i = 0; i < events->n && i < 40; i++) {
        jval *ev = events->items[i];
        jval *row = jobj();
        jo_setstr(row, "role", "event");
        jo_setstr(row, "event", js(ev, "type", ""));
        jo_setstr(row, "content", js(ev, "line", ""));
        jo_setstr(row, "job", a->job);
        jo_setbool(row, "ok", !*error);
        append_record(row);
    }

    jval *done = jobj();
    jo_setstr(done, "role", "assistant");
    jo_setstr(done, "content",
              *reply ? reply : (*error ? error : "(no reply)"));
    jo_setstr(done, "job", a->job);
    jo_setstr(done, "session", thread_id);
    jo_setint(done, "elapsed_ms", now_millis() - started_ms);
    jo_setbool(done, "ok", !*error);
    jo_setstr(done, "error", error);
    append_record(done);

    jval *ch = jobj();
    jo_setbool(ch, "busy", 0);
    jo_setstr(ch, "job", a->job);
    jo_setstr(ch, "thread_id", thread_id);
    jo_setint(ch, "child_pid", 0);
    jo_setstr(ch, "last_error", error);
    jo_setint(ch, "finished_at", now_epoch());
    write_state(ch);

    unlink(reply_file);

    for (size_t i = 0; i < raw_n; i++)
        free(raw_lines[i]);
    free(raw_lines);
    free(reply);
    free(error);
    free(thread_id);
    proc_free(&r);
}

static void cmd_chat_start(const char *out, const char *input)
{
    const char *raw = read_text(input);
    char *message = strip_dup(raw);
    if (!*message) {
        free(message);
        jval *o = jobj();
        jo_setbool(o, "ok", 0);
        jo_setstr(o, "error", "empty message");
        result_out(out, o);
        return;
    }
    {
        char *cut = truncate_chars(message, 16000);
        free(message);
        message = cut;
    }
    const char *mode = auth_mode();
    if (!mode || !*mode || !strcmp(mode, "none")) {
        free(message);
        jval *o = jobj();
        jo_setbool(o, "ok", 0);
        jo_setstr(o, "error",
                  "Codex CLI is not signed in. Sign in on /config first.");
        result_out(out, o);
        return;
    }
    jval *s = settings();
    const char *cli = js(s, "cli_path", "");
    if (!cli_available(cli)) {
        free(message);
        jval *o = jobj();
        jo_setbool(o, "ok", 0);
        jo_setstr(o, "error", "Codex CLI not found");
        result_out(out, o);
        return;
    }
    jval *st = state();
    if (jtruthy(jo(st, "busy"))) {
        int ok = 0;
        long long started_at = jtoint(jo(st, "started_at"), &ok);
        if (!ok)
            started_at = 0;
        if (now_epoch() - started_at < 330) {
            free(message);
            jval *o = jobj();
            jo_setbool(o, "ok", 0);
            jo_setstr(o, "error", "Codex is still answering");
            result_out(out, o);
            return;
        }
    }

    char stamp[16];
    fmt_localtime(now_epoch(), "%H%M%S", stamp, sizeof(stamp));
    unsigned char rnd[4];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t r = read(fd, rnd, sizeof(rnd));
        (void)r;
        close(fd);
    } else {
        long long t = now_millis();
        memcpy(rnd, &t, sizeof(rnd));
    }
    char job[64];
    snprintf(job, sizeof(job), "%s-%02x%02x%02x%02x", stamp, rnd[0], rnd[1],
             rnd[2], rnd[3]);

    jval *ch = jobj();
    jo_setbool(ch, "busy", 1);
    jo_setstr(ch, "job", job);
    jo_setstr(ch, "last_error", "");
    jo_setint(ch, "started_at", now_epoch());
    write_state(ch);

    chat_arg *a = xmalloc(sizeof(*a));
    a->job = xstrdup(job);
    a->message = message;
    a->cli = xstrdup(cli);
    background(chat_worker, a);

    jval *o = jobj();
    jo_setbool(o, "ok", 1);
    jo_setstr(o, "job", job);
    result_out(out, o);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

typedef void (*cmd_fn)(const char *out, const char *input, const char *arg);

static void cmd_status_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    (void)arg;
    cmd_status(out);
}

static void cmd_check_update_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    (void)arg;
    cmd_check_update(out);
}

static void cmd_update_install_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    (void)arg;
    cmd_update_install(out);
}

static void cmd_update_status_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    (void)arg;
    cmd_update_status(out);
}

static void cmd_models_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    (void)arg;
    cmd_models(out);
}

static void cmd_quota_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    (void)arg;
    cmd_quota(out);
}

static void cmd_config_t(const char *out, const char *input, const char *arg)
{
    (void)arg;
    cmd_config(out, input);
}

static void cmd_login_device_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    (void)arg;
    cmd_login_device(out);
}

static void cmd_chat_start_t(const char *out, const char *input, const char *arg)
{
    (void)arg;
    cmd_chat_start(out, input);
}

static void cmd_reset_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    (void)arg;
    cmd_reset(out);
}

static void cmd_stop_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    (void)arg;
    cmd_stop(out);
}

static void cmd_logout_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    (void)arg;
    cmd_logout(out);
}

static void cmd_history_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    cmd_history(out, arg);
}

static void cmd_records_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    (void)arg;
    cmd_records(out);
}

static void cmd_export_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    (void)arg;
    cmd_export(out);
}

static void cmd_record_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    cmd_record(out, arg);
}

static void cmd_records_clear_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    (void)arg;
    cmd_records_clear(out);
}

static void cmd_records_delete_t(const char *out, const char *input, const char *arg)
{
    (void)input;
    cmd_records_delete(out, arg);
}

static const struct {
    const char *name;
    cmd_fn      fn;
} COMMANDS[] = {
    { "status",         cmd_status_t },
    { "check-update",   cmd_check_update_t },
    { "update-install", cmd_update_install_t },
    { "update-status",  cmd_update_status_t },
    { "models",         cmd_models_t },
    { "quota",          cmd_quota_t },
    { "config",         cmd_config_t },
    { "login-device",   cmd_login_device_t },
    { "chat-start",     cmd_chat_start_t },
    { "reset",          cmd_reset_t },
    { "stop",           cmd_stop_t },
    { "logout",         cmd_logout_t },
    { "history",        cmd_history_t },
    { "records",        cmd_records_t },
    { "export",         cmd_export_t },
    { "record",         cmd_record_t },
    { "records-clear",  cmd_records_clear_t },
    { "records-delete", cmd_records_delete_t },
};

static void usage(void)
{
    fputs("usage: codex-bridge <command> --out PATH [--input PATH] [--arg STR]\n",
          stderr);
}

int main(int argc, char **argv)
{
    init_paths();

    const char *cmd = NULL, *out = NULL, *input = NULL, *arg = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(a, "--input") && i + 1 < argc)
            input = argv[++i];
        else if (!strcmp(a, "--arg") && i + 1 < argc)
            arg = argv[++i];
        else if (!strncmp(a, "--out=", 6))
            out = a + 6;
        else if (!strncmp(a, "--input=", 8))
            input = a + 8;
        else if (!strncmp(a, "--arg=", 6))
            arg = a + 6;
        else if (a[0] != '-' && !cmd)
            cmd = a;
        else {
            usage();
            return 2;
        }
    }
    if (!cmd || !out || !*out) {
        usage();
        return 2;
    }

    cmd_fn fn = NULL;
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (!strcmp(COMMANDS[i].name, cmd)) {
            fn = COMMANDS[i].fn;
            break;
        }
    }
    if (!fn) {
        fprintf(stderr, "codex-bridge: unknown command '%s'\n", cmd);
        return 2;
    }

    /* Anything raised after this point is reported in the result file. */
    static int in_handler;
    if (setjmp(g_raise_jmp) != 0) {
        if (in_handler) {          /* the error report itself failed */
            fprintf(stderr, "codex-bridge: %s\n", g_raise_msg);
            return 1;
        }
        in_handler = 1;
        jval *o = jobj();
        jo_setbool(o, "ok", 0);
        jo_setstr(o, "error", g_raise_msg);
        result_out(out, o);
        return 1;
    }

    ensure_dirs();
    fn(out, input, arg);
    return 0;
}
