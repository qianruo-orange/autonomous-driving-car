/* SPDX-License-Identifier: GPL-3.0-or-later */
/* CPUFreq controls operate once per policy domain, preserving requested limits. */
#include "http/http_internal.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CPUFREQ_ROOT
#define CPUFREQ_ROOT "/sys/devices/system/cpu/cpufreq"
#endif
#ifndef CPUFREQ_CONFIG_PATH
#define CPUFREQ_CONFIG_PATH "config/cpu_freq.conf"
#endif
#define CPUFREQ_POLICIES 64
#define CPUFREQ_FREQS 64
#define CPUFREQ_GOVS 16
#define CPUFREQ_JSON 131072

typedef struct { char id[32], governor[64]; long minimum, maximum; } cpu_request_t;
typedef struct {
    cpu_request_t actual;
    char related[256], governors[CPUFREQ_GOVS][64];
    long frequencies[CPUFREQ_FREQS], current, hardware_min, hardware_max;
    int nfreqs, ngovs;
} cpu_policy_t;
static pthread_mutex_t cpufreq_lock = PTHREAD_MUTEX_INITIALIZER;

static int policy_id_valid(const char *id)
{
    if (!id || strncmp(id, "policy", 6) || !id[6] || strlen(id) >= 32) return 0;
    for (const char *p = id + 6; *p; p++) if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}
static int token_valid(const char *text)
{
    if (!text || !*text || strlen(text) >= 64) return 0;
    for (; *text; text++) if (!isalnum((unsigned char)*text) && *text != '_' && *text != '-') return 0;
    return 1;
}
static int parse_frequency(const char *text, long *value)
{
    if (!text || !*text) return -1;
    for (const char *p = text; *p; p++) if (!isdigit((unsigned char)*p)) return -1;
    errno = 0; char *end;
    long result = strtol(text, &end, 10);
    if (errno || *end || result <= 0) return -1;
    *value = result; return 0;
}
static int policy_path(char *out, size_t size, const char *id, const char *file)
{
    int n = snprintf(out, size, "%s/%s/%s", CPUFREQ_ROOT, id, file);
    if (n < 0 || (size_t)n >= size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}
static int read_text(const char *id, const char *file, char *text, size_t size)
{
    char path[PATH_MAX];
    if (policy_path(path, sizeof(path), id, file)) return -1;
    FILE *input = fopen(path, "r");
    if (!input) return -1;
    int result = fgets(text, (int)size, input) ? 0 : -1;
    if (!result && !strchr(text, '\n') && !feof(input) && fgetc(input) != EOF) { errno = EOVERFLOW; result = -1; }
    int saved_errno = errno;
    if (fclose(input) && !result) { result = -1; saved_errno = errno; }
    if (!result) { size_t n = strlen(text); while (n && isspace((unsigned char)text[n - 1])) text[--n] = '\0'; }
    errno = saved_errno; return result;
}
static int read_frequency(const char *id, const char *file, long *value)
{
    char text[64];
    return read_text(id, file, text, sizeof(text)) || parse_frequency(text, value) ? -1 : 0;
}
static int sysfs_write(const char *id, const char *file, const char *text)
{
    char path[PATH_MAX];
    if (policy_path(path, sizeof(path), id, file)) return -1;
    FILE *output = fopen(path, "w");
    if (!output) return -1;
    int result = fputs(text, output) < 0 ? -1 : 0;
    int saved_errno = errno;
    /* A buffered sysfs write can fail only when fclose flushes it. */
    if (fclose(output)) { result = -1; saved_errno = errno; }
    errno = saved_errno; return result;
}
#ifndef CPUFREQ_WRITE
#define CPUFREQ_WRITE sysfs_write
#endif
static int write_frequency(const char *id, const char *file, long value)
{
    char text[32]; snprintf(text, sizeof(text), "%ld", value);
    return CPUFREQ_WRITE(id, file, text);
}
static int read_policy(const char *id, cpu_policy_t *policy)
{
    memset(policy, 0, sizeof(*policy));
    snprintf(policy->actual.id, sizeof(policy->actual.id), "%s", id);
    if (read_text(id, "scaling_governor", policy->actual.governor, sizeof(policy->actual.governor)) ||
        !token_valid(policy->actual.governor) || read_frequency(id, "scaling_min_freq", &policy->actual.minimum) ||
        read_frequency(id, "scaling_max_freq", &policy->actual.maximum)) return -1;
    if (read_text(id, "related_cpus", policy->related, sizeof(policy->related))) policy->related[0] = '\0';
    for (char *p = policy->related; *p; p++) {
        if (isspace((unsigned char)*p)) *p = ' ';
        else if (!isdigit((unsigned char)*p) && *p != '-' && *p != ',') return -1;
    }
    (void)read_frequency(id, "scaling_cur_freq", &policy->current);
    char list[4096], *save;
    if (!read_text(id, "scaling_available_governors", list, sizeof(list)))
        for (char *p = strtok_r(list, " \t", &save); p; p = strtok_r(NULL, " \t", &save)) {
            if (!token_valid(p) || policy->ngovs == CPUFREQ_GOVS) return -1;
            snprintf(policy->governors[policy->ngovs++], 64, "%s", p);
        }
    if (!policy->ngovs) snprintf(policy->governors[policy->ngovs++], 64, "%s", policy->actual.governor);
    if (!read_text(id, "scaling_available_frequencies", list, sizeof(list)))
        for (char *p = strtok_r(list, " \t", &save); p; p = strtok_r(NULL, " \t", &save)) {
            if (policy->nfreqs == CPUFREQ_FREQS || parse_frequency(p, &policy->frequencies[policy->nfreqs])) return -1;
            policy->nfreqs++;
        }
    policy->hardware_min = policy->actual.minimum; policy->hardware_max = policy->actual.maximum;
    if (policy->nfreqs) {
        policy->hardware_min = policy->hardware_max = policy->frequencies[0];
        for (int i = 1; i < policy->nfreqs; i++) {
            if (policy->frequencies[i] < policy->hardware_min) policy->hardware_min = policy->frequencies[i];
            if (policy->frequencies[i] > policy->hardware_max) policy->hardware_max = policy->frequencies[i];
        }
    }
    (void)read_frequency(id, "cpuinfo_min_freq", &policy->hardware_min);
    (void)read_frequency(id, "cpuinfo_max_freq", &policy->hardware_max);
    return 0;
}
static int policy_compare(const void *left, const void *right)
{
    const cpu_policy_t *a = left, *b = right;
    unsigned long x = strtoul(a->actual.id + 6, NULL, 10), y = strtoul(b->actual.id + 6, NULL, 10);
    return x < y ? -1 : x > y ? 1 : strcmp(a->actual.id, b->actual.id);
}
static int read_policies(cpu_policy_t *policies)
{
    DIR *directory = opendir(CPUFREQ_ROOT);
    if (!directory) return -1;
    int count = 0, result = 0; struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!policy_id_valid(entry->d_name)) continue;
        if (count == CPUFREQ_POLICIES || read_policy(entry->d_name, &policies[count])) { result = -1; break; }
        count++;
    }
    int saved_errno = errno; closedir(directory); errno = saved_errno;
    if (result || !count) return -1;
    qsort(policies, (size_t)count, sizeof(*policies), policy_compare); return count;
}
static int request_index(const cpu_request_t *requests, int count, const char *id)
{
    for (int i = 0; i < count; i++) if (!strcmp(requests[i].id, id)) return i;
    return -1;
}
/* Versioned text file: policy governor minimum_kHz maximum_kHz. */
static int load_saved(cpu_request_t *saved, int *exists)
{
    *exists = 0;
    FILE *input = fopen(CPUFREQ_CONFIG_PATH, "r");
    if (!input) return errno == ENOENT ? 0 : -1;
    *exists = 1; int count = 0, result = 0;
    char line[512], id[32], governor[64], minimum[64], maximum[64], extra;
    while (fgets(line, sizeof(line), input)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (count == CPUFREQ_POLICIES || sscanf(line, "%31s %63s %63s %63s %c", id, governor, minimum, maximum, &extra) != 4 ||
            !policy_id_valid(id) || !token_valid(governor) || request_index(saved, count, id) >= 0) { result = -1; break; }
        cpu_request_t *request = &saved[count]; memset(request, 0, sizeof(*request));
        snprintf(request->id, sizeof(request->id), "%s", id); snprintf(request->governor, sizeof(request->governor), "%s", governor);
        if (parse_frequency(minimum, &request->minimum) || parse_frequency(maximum, &request->maximum) || request->minimum > request->maximum) { result = -1; break; }
        count++;
    }
    if (ferror(input)) result = -1;
    int saved_errno = errno;
    if (fclose(input)) { result = -1; saved_errno = errno; }
    errno = saved_errno; return result ? -1 : count;
}
static int persist_requests(const cpu_request_t *saved, int count)
{
    char temporary[PATH_MAX], directory[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.tmpXXXXXX", CPUFREQ_CONFIG_PATH) >= (int)sizeof(temporary)) return -1;
    int descriptor = mkstemp(temporary);
    if (descriptor < 0) return -1;
    FILE *output = fdopen(descriptor, "w");
    if (!output) { close(descriptor); unlink(temporary); return -1; }
    int result = fputs("# CPUFreq policy settings v1 (kHz)\n", output) < 0 ? -1 : 0;
    for (int i = 0; i < count && !result; i++)
        if (fprintf(output, "%s %s %ld %ld\n", saved[i].id, saved[i].governor, saved[i].minimum, saved[i].maximum) < 0) result = -1;
    if (fflush(output) || fsync(descriptor) || fchmod(descriptor, 0644)) result = -1;
    int saved_errno = errno;
    if (fclose(output)) { result = -1; saved_errno = errno; }
    if (!result && rename(temporary, CPUFREQ_CONFIG_PATH)) { result = -1; saved_errno = errno; }
    if (result) unlink(temporary);
    else {
        snprintf(directory, sizeof(directory), "%s", CPUFREQ_CONFIG_PATH);
        char *slash = strrchr(directory, '/'); if (slash) *slash = '\0'; else strcpy(directory, ".");
        int parent = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent < 0 || fsync(parent)) { result = -1; saved_errno = errno; }
        if (parent >= 0 && close(parent) && !result) { result = -1; saved_errno = errno; }
    }
    errno = saved_errno; return result;
}
static int request_valid(const cpu_policy_t *policy, const cpu_request_t *request)
{
    if (request->minimum > request->maximum || request->minimum < policy->hardware_min || request->maximum > policy->hardware_max) return 0;
    int governor_found = 0, minimum_found = !policy->nfreqs, maximum_found = !policy->nfreqs;
    for (int i = 0; i < policy->ngovs; i++) if (!strcmp(request->governor, policy->governors[i])) governor_found = 1;
    for (int i = 0; i < policy->nfreqs; i++) {
        if (request->minimum == policy->frequencies[i]) minimum_found = 1;
        if (request->maximum == policy->frequencies[i]) maximum_found = 1;
    }
    return governor_found && minimum_found && maximum_found;
}
static int apply_request(const cpu_request_t *request)
{
    cpu_policy_t current;
    if (read_policy(request->id, &current)) return -1;
    long minimum = current.actual.minimum, maximum = current.actual.maximum;
    /* Widen the previous range before narrowing either boundary. */
    if (request->minimum > maximum) {
        if (write_frequency(request->id, "scaling_max_freq", request->maximum)) return -1;
        maximum = request->maximum;
    }
    if (request->maximum < minimum) {
        if (write_frequency(request->id, "scaling_min_freq", request->minimum)) return -1;
        minimum = request->minimum;
    }
    if (minimum != request->minimum && write_frequency(request->id, "scaling_min_freq", request->minimum)) return -1;
    if (maximum != request->maximum && write_frequency(request->id, "scaling_max_freq", request->maximum)) return -1;
    if (strcmp(current.actual.governor, request->governor) && CPUFREQ_WRITE(request->id, "scaling_governor", request->governor)) return -1;
    /* A kernel thermal cap can legitimately reduce the effective maximum. */
    if (read_policy(request->id, &current) || strcmp(current.actual.governor, request->governor) ||
        current.actual.minimum > current.actual.maximum || current.actual.maximum > request->maximum ||
        (current.actual.maximum >= request->minimum && current.actual.minimum != request->minimum)) {
        errno = EIO; return -1;
    }
    return 0;
}
static int append_json(char *json, int offset, const char *format, ...)
{
    if (offset < 0 || offset >= CPUFREQ_JSON) return -1;
    va_list arguments; va_start(arguments, format);
    int n = vsnprintf(json + offset, CPUFREQ_JSON - (size_t)offset, format, arguments); va_end(arguments);
    return n < 0 || n >= CPUFREQ_JSON - offset ? -1 : offset + n;
}
static int policy_json(char *json, int offset, const cpu_policy_t *policy, const cpu_request_t *saved)
{
    const cpu_request_t *actual = &policy->actual, *requested = saved ? saved : actual;
    offset = append_json(json, offset, "\"id\":\"%s\",\"related_cpus\":\"%s\",\"governor\":\"%s\",\"governors\":[", actual->id, policy->related, actual->governor);
    for (int i = 0; i < policy->ngovs; i++) offset = append_json(json, offset, "%s\"%s\"", i ? "," : "", policy->governors[i]);
    offset = append_json(json, offset, "],\"frequencies\":[");
    for (int i = 0; i < policy->nfreqs; i++) offset = append_json(json, offset, "%s%ld", i ? "," : "", policy->frequencies[i]);
    return append_json(json, offset, "],\"min_freq\":%ld,\"max_freq\":%ld,\"cur_freq\":%ld,\"hardware_min_freq\":%ld,\"hardware_max_freq\":%ld,"
        "\"requested_min_freq\":%ld,\"requested_max_freq\":%ld,\"requested_governor\":\"%s\",\"saved\":%s,\"limited\":%s",
        actual->minimum, actual->maximum, policy->current, policy->hardware_min, policy->hardware_max,
        requested->minimum, requested->maximum, requested->governor, saved ? "true" : "false", actual->maximum < requested->maximum ? "true" : "false");
}
void http_cpufreq_get(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri; (void)req;
    cpu_policy_t policies[CPUFREQ_POLICIES]; cpu_request_t saved[CPUFREQ_POLICIES]; char json[CPUFREQ_JSON];
    pthread_mutex_lock(&cpufreq_lock);
    int count = read_policies(policies), exists, nsaved = load_saved(saved, &exists);
    if (count < 0) { pthread_mutex_unlock(&cpufreq_lock); http_err(fd, 503, "Unavailable", "CPU frequency policies unavailable"); return; }
    if (nsaved < 0) nsaved = 0;
    int index = request_index(saved, nsaved, policies[0].actual.id);
    int offset = policy_json(json, append_json(json, 0, "{"), &policies[0], index >= 0 ? &saved[index] : NULL);
    offset = append_json(json, offset, ",\"policies\":[");
    for (int i = 0; i < count; i++) {
        offset = append_json(json, offset, "%s{", i ? "," : "");
        index = request_index(saved, nsaved, policies[i].actual.id);
        offset = policy_json(json, offset, &policies[i], index >= 0 ? &saved[index] : NULL);
        offset = append_json(json, offset, "}");
    }
    offset = append_json(json, offset, "]}"); pthread_mutex_unlock(&cpufreq_lock);
    if (offset < 0) http_err(fd, 500, "Error", "CPU policy response is too large"); else http_ok_json(fd, json, (size_t)offset);
}
static int rollback_policies(const cpu_policy_t *original, int count, const int *selected)
{
    int failed = 0;
    for (int i = count - 1; i >= 0; i--) if (selected[i] && apply_request(&original[i].actual)) failed = 1;
    return failed;
}
void http_cpufreq_post(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req)
{
    (void)app; (void)uri;
    if (!method || strcmp(method, "POST")) { http_err(fd, 405, "Method Not Allowed", NULL); return; }
    http_form_field_t fields[8]; const char *body = http_body_start(req);
    if (!body || strlen(body) > 1024 || strstr(body, "%00")) { http_err(fd, 400, "Bad Request", "Invalid CPU settings"); return; }
    /* Validate segment boundaries before the shared form parser sees them. */
    int segments = 0;
    for (const char *segment = body; *segment;) {
        const char *amp = strchr(segment, '&');
        size_t length = amp ? (size_t)(amp - segment) : strlen(segment);
        const char *equals = memchr(segment, '=', length);
        if (!equals || equals == segment || equals == segment + length - 1 || ++segments > 4 ||
            (amp && !amp[1])) goto invalid;
        segment = amp ? amp + 1 : segment + length;
    }
    int nfields = http_form_parse(req, fields, 8);
    const char *id = NULL, *governor = NULL, *minimum = NULL, *maximum = NULL;
    if (nfields <= 0 || nfields > 4) goto invalid;
    for (int i = 0; i < nfields; i++) {
        const char **target = !strcmp(fields[i].key, "policy") ? &id : !strcmp(fields[i].key, "governor") ? &governor :
            !strcmp(fields[i].key, "min_freq") ? &minimum : !strcmp(fields[i].key, "max_freq") ? &maximum : NULL;
        if (!target || *target || !fields[i].val[0]) goto invalid;
        *target = fields[i].val;
    }
    if ((!governor && !minimum && !maximum) || (governor && !token_valid(governor)) ||
        (id && strcmp(id, "all") && !policy_id_valid(id))) goto invalid;
    long new_minimum = 0, new_maximum = 0;
    if ((minimum && parse_frequency(minimum, &new_minimum)) || (maximum && parse_frequency(maximum, &new_maximum))) goto invalid;
    cpu_policy_t policies[CPUFREQ_POLICIES]; cpu_request_t desired[CPUFREQ_POLICIES], saved[CPUFREQ_POLICIES], old_saved[CPUFREQ_POLICIES];
    int selected[CPUFREQ_POLICIES] = {0};
    pthread_mutex_lock(&cpufreq_lock);
    int count = read_policies(policies), existed = 0, nsaved = load_saved(saved, &existed), targets = 0;
    if (count < 0 || nsaved < 0) { pthread_mutex_unlock(&cpufreq_lock); http_err(fd, 500, "Error", "Unable to read CPU settings"); return; }
    int old_count = nsaved; memcpy(old_saved, saved, (size_t)nsaved * sizeof(*saved));
    for (int i = 0; i < count; i++) {
        if (id && strcmp(id, "all") && strcmp(id, policies[i].actual.id)) continue;
        selected[i] = 1; targets++;
        int previous = request_index(saved, nsaved, policies[i].actual.id);
        desired[i] = previous >= 0 ? saved[previous] : policies[i].actual;
        if (governor) snprintf(desired[i].governor, sizeof(desired[i].governor), "%s", governor);
        if (minimum) desired[i].minimum = new_minimum;
        if (maximum) desired[i].maximum = new_maximum;
        if (!request_valid(&policies[i], &desired[i])) { pthread_mutex_unlock(&cpufreq_lock); goto invalid; }
        if (previous < 0) {
            if (nsaved == CPUFREQ_POLICIES) { pthread_mutex_unlock(&cpufreq_lock); http_err(fd, 500, "Error", "Too many saved CPU policies"); return; }
            previous = nsaved++;
        }
        saved[previous] = desired[i];
    }
    if (!targets) { pthread_mutex_unlock(&cpufreq_lock); goto invalid; }
    for (int i = 0; i < count; i++) if (selected[i] && apply_request(&desired[i])) {
        int failed = rollback_policies(policies, count, selected); pthread_mutex_unlock(&cpufreq_lock);
        http_err(fd, 500, "Error", failed ? "CPU setting failed; rollback incomplete" : "CPU setting failed; previous settings restored"); return;
    }
    if (persist_requests(saved, nsaved)) {
        int failed = rollback_policies(policies, count, selected);
        if (existed ? persist_requests(old_saved, old_count) : (unlink(CPUFREQ_CONFIG_PATH) && errno != ENOENT)) failed = 1;
        pthread_mutex_unlock(&cpufreq_lock);
        http_err(fd, 500, "Error", failed ? "CPU settings were not saved; rollback incomplete" : "Unable to persist CPU settings; previous settings restored"); return;
    }
    pthread_mutex_unlock(&cpufreq_lock);
    char json[128]; int length = snprintf(json, sizeof(json), "{\"result\":\"ok\",\"changed\":%d,\"persistent\":true}", targets);
    http_ok_json(fd, json, (size_t)length); return;
invalid:
    http_err(fd, 400, "Bad Request", "Invalid or unsupported CPU policy settings");
}
void http_cpufreq_restore(void)
{
    cpu_policy_t policies[CPUFREQ_POLICIES]; cpu_request_t saved[CPUFREQ_POLICIES]; int selected[CPUFREQ_POLICIES] = {0}, exists;
    pthread_mutex_lock(&cpufreq_lock);
    int nsaved = load_saved(saved, &exists);
    if (!nsaved) { pthread_mutex_unlock(&cpufreq_lock); return; }
    int count = nsaved < 0 ? -1 : read_policies(policies);
    if (count < 0) goto failed;
    for (int i = 0; i < count; i++) {
        int index = request_index(saved, nsaved, policies[i].actual.id);
        if (index < 0) continue;
        if (!request_valid(&policies[i], &saved[index])) goto failed;
        selected[i] = index + 1;
    }
    for (int i = 0; i < count; i++) if (selected[i] && apply_request(&saved[selected[i] - 1])) {
        (void)rollback_policies(policies, count, selected); goto failed;
    }
    pthread_mutex_unlock(&cpufreq_lock); LOG_INFO("CPU frequency settings restored from %s", CPUFREQ_CONFIG_PATH); return;
failed:
    pthread_mutex_unlock(&cpufreq_lock); LOG_ERROR("CPU frequency settings could not be restored from %s; keeping kernel settings", CPUFREQ_CONFIG_PATH);
}
