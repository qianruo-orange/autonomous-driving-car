/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise policy capabilities and kernel-like min/max constraints in scratch. */
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
static int fake_write(const char *, const char *, const char *);
#define CPUFREQ_ROOT "cpufreq"
#define CPUFREQ_CONFIG_PATH "config/cpu_freq.conf"
#define CPUFREQ_WRITE fake_write
#include "http/http_api_cpufreq.c"

static int response_code, writes, error_logs, restored_logs;
static char response[CPUFREQ_JSON], order[4096];
static const char *fail_policy, *fail_file, *ignore_file;
static int thermal_cap;

void _log_info(const char *file, int line, const char *format, ...)
{ (void)file; (void)line; if (strstr(format, "restored from")) restored_logs++; }
void _log_error(const char *file, int line, const char *format, ...)
{ (void)file; (void)line; (void)format; error_logs++; }
void http_send_response(int fd, int code, const char *status, const char *mime, const void *body, size_t length)
{ (void)fd; (void)status; (void)mime; response_code = code; assert(length < sizeof(response)); if (length) memcpy(response, body, length); response[length] = '\0'; }

static void put(const char *path, const char *text)
{
    FILE *output = fopen(path, "w"); assert(output);
    assert(fputs(text, output) >= 0 && fclose(output) == 0);
}
static void put_policy(const char *id, const char *name, const char *text)
{
    char path[PATH_MAX]; assert(!policy_path(path, sizeof(path), id, name)); put(path, text);
}
static long frequency(const char *id, const char *file)
{ long value; assert(!read_frequency(id, file, &value)); return value; }
static int fake_write(const char *id, const char *file, const char *text)
{
    writes++;
    size_t used = strlen(order);
    assert(snprintf(order + used, sizeof(order) - used, "%s/%s=%s;", id, file, text) < (int)(sizeof(order) - used));
    if (fail_file && !strcmp(file, fail_file) && (!fail_policy || !strcmp(id, fail_policy))) {
        fail_file = NULL; errno = EIO; return -1;
    }
    if (ignore_file && !strcmp(file, ignore_file)) { ignore_file = NULL; return 0; }
    if (!strcmp(file, "scaling_min_freq")) {
        long value; assert(!parse_frequency(text, &value));
        if (value > frequency(id, "scaling_max_freq")) { errno = EINVAL; return -1; }
    }
    if (!strcmp(file, "scaling_max_freq")) {
        long value; assert(!parse_frequency(text, &value));
        if (value < frequency(id, "scaling_min_freq")) { errno = EINVAL; return -1; }
        if (thermal_cap && value > thermal_cap) {
            char limited[32]; snprintf(limited, sizeof(limited), "%d", thermal_cap);
            return sysfs_write(id, file, limited);
        }
    }
    return sysfs_write(id, file, text);
}
static void reset_trace(void)
{ writes = 0; order[0] = '\0'; fail_policy = fail_file = ignore_file = NULL; thermal_cap = 0; }
static void post(const char *form, int expected)
{
    http_cpufreq_post(NULL, 0, "POST", "/api/cpu_freq", form);
    assert(response_code == expected);
}
static void initial_policy(const char *id, const char *cpus, const char *frequencies, const char *maximum)
{
    char path[128]; snprintf(path, sizeof(path), "%s/%s", CPUFREQ_ROOT, id); assert(!mkdir(path, 0700));
    put_policy(id, "related_cpus", cpus);
    put_policy(id, "scaling_governor", "ondemand\n");
    put_policy(id, "scaling_available_governors", "ondemand performance powersave\n");
    put_policy(id, "scaling_available_frequencies", frequencies);
    put_policy(id, "cpuinfo_min_freq", "408000\n");
    put_policy(id, "cpuinfo_max_freq", maximum);
    put_policy(id, "scaling_min_freq", "408000\n");
    put_policy(id, "scaling_max_freq", "600000\n");
    put_policy(id, "scaling_cur_freq", "600000\n");
}
static void remove_policy(const char *id)
{
    char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", CPUFREQ_ROOT, id);
    DIR *directory = opendir(path); assert(directory); struct dirent *entry;
    while ((entry = readdir(directory))) if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
        char child[PATH_MAX]; assert(snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) < (int)sizeof(child)); assert(!unlink(child));
    }
    closedir(directory); assert(!rmdir(path));
}

int main(void)
{
    char scratch[] = "/tmp/cpufreq-test-XXXXXX"; assert(mkdtemp(scratch));
    int original = open(".", O_RDONLY | O_DIRECTORY); assert(original >= 0 && !chdir(scratch));
    assert(!mkdir("cpufreq", 0700) && !mkdir("config", 0700));
    initial_policy("policy0", "0 1 2 3\n", "408000 600000 1200000 1608000 2016000\n", "2016000\n");
    initial_policy("policy4", "4 5 6 7\n", "408000 600000 1200000 1800000 2208000\n", "2208000\n");
    http_cpufreq_get(NULL, 0, "GET", "/api/cpu_freq", NULL);
    assert(response_code == 200 && strstr(response, "\"policies\":[") && strstr(response, "\"id\":\"policy0\"") && strstr(response, "\"id\":\"policy4\""));
    assert(strstr(response, "\"related_cpus\":\"4 5 6 7\"") && strstr(response, "2208000"));
    assert(strstr(response, "\"saved\":false"));
    const char *bad[] = {
        "governor=performance&max_freq=2208000", /* All-policy validation before any governor write. */
        "policy=policy0&governor=performance&min_freq=2208000",
        "policy=policy0&min_freq=600000junk", "policy=policy0&min_freq=+600000",
        "policy=policy0&min_freq=0", "policy=policy0&min_freq=99999999999999999999999999",
        "policy=policy0&min_freq=1608000&max_freq=600000",
        "policy=policy0&governor=performance&governor=ondemand", "policy=policy0&max_freq=",
        "policy=policy999&max_freq=600000", "policy=..%2Fpolicy0&max_freq=600000",
        "policy=policy0%00ignored&max_freq=600000", "policy=policy0", "bad=parameter",
        "policy=policy0&max_freq=600000&ignored", "policy=policy0&ignored&max_freq=600000",
        "policy=policy0&max_freq=600000&"
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) { reset_trace(); post(bad[i], 400); assert(!writes && access(CPUFREQ_CONFIG_PATH, F_OK)); }

    reset_trace(); post("policy=policy4&governor=performance&min_freq=1800000&max_freq=2208000", 200);
    assert(strstr(order, "policy4/scaling_max_freq=2208000;policy4/scaling_min_freq=1800000;") == order);
    assert(writes == 3 && frequency("policy4", "scaling_min_freq") == 1800000 && frequency("policy0", "scaling_max_freq") == 600000);
    assert(strstr(response, "\"persistent\":true"));
    reset_trace(); post("policy=policy4&min_freq=408000&max_freq=1200000", 200);
    assert(!strcmp(order, "policy4/scaling_min_freq=408000;policy4/scaling_max_freq=1200000;"));
    reset_trace(); post("policy=policy0&governor=performance&max_freq=1608000", 200);
    assert(writes == 2); /* One policy, no repeated writes through per-CPU symlinks. */
    int existed; cpu_request_t saved[CPUFREQ_POLICIES];
    int count = load_saved(saved, &existed); assert(existed && count == 2);
    assert(saved[request_index(saved, count, "policy4")].maximum == 1200000);

    reset_trace(); fail_policy = "policy4"; fail_file = "scaling_governor";
    post("policy=all&governor=ondemand&min_freq=408000&max_freq=600000", 500);
    assert(frequency("policy0", "scaling_max_freq") == 1608000 && frequency("policy4", "scaling_max_freq") == 1200000);
    char governor[64]; assert(!read_text("policy0", "scaling_governor", governor, sizeof(governor)) && !strcmp(governor, "performance"));
    count = load_saved(saved, &existed); assert(count == 2 && saved[request_index(saved, count, "policy0")].maximum == 1608000);

    reset_trace(); ignore_file = "scaling_max_freq";
    post("policy=policy0&max_freq=600000", 500); /* No false success after a rejected readback. */
    assert(frequency("policy0", "scaling_max_freq") == 1608000);
    reset_trace(); ignore_file = "scaling_min_freq";
    post("policy=policy0&min_freq=600000", 500);
    assert(frequency("policy0", "scaling_min_freq") == 408000 && frequency("policy0", "scaling_max_freq") == 1608000);

    reset_trace(); thermal_cap = 1200000;
    post("policy=policy4&max_freq=2208000", 200);
    assert(frequency("policy4", "scaling_max_freq") == 1200000);
    http_cpufreq_get(NULL, 0, "GET", "/api/cpu_freq", NULL);
    assert(strstr(response, "\"requested_max_freq\":2208000") && strstr(response, "\"limited\":true"));
    count = load_saved(saved, &existed); assert(saved[request_index(saved, count, "policy4")].maximum == 2208000);

    reset_trace(); put_policy("policy0", "scaling_governor", "ondemand"); put_policy("policy0", "scaling_max_freq", "600000");
    put_policy("policy4", "scaling_governor", "ondemand"); put_policy("policy4", "scaling_max_freq", "600000");
    http_cpufreq_restore(); assert(restored_logs == 1 && frequency("policy0", "scaling_max_freq") == 1608000 && frequency("policy4", "scaling_max_freq") == 2208000);

    reset_trace(); assert(!rename("config", "config.off"));
    post("policy=policy0&max_freq=1200000", 500); /* Atomic config creation failure rolls hardware back. */
    assert(frequency("policy0", "scaling_max_freq") == 1608000 && !rename("config.off", "config"));
    put(CPUFREQ_CONFIG_PATH, "policy0 powersave 408000 1200000\npolicy4 performance 408000 9999999\n");
    reset_trace(); int before_errors = error_logs;
    http_cpufreq_restore(); assert(!writes && error_logs == before_errors + 1);
    put(CPUFREQ_CONFIG_PATH, "policy0 performance 408000 1200000\npolicy0 powersave 408000 600000\n");
    reset_trace(); http_cpufreq_restore(); assert(!writes);
    assert(!unlink(CPUFREQ_CONFIG_PATH));

    /* /dev/full rejects stdio flush, verifying fclose failure is propagated. */
    assert(!symlink("/dev/full", "cpufreq/policy0/full"));
    assert(sysfs_write("policy0", "full", "performance") < 0);
    remove_policy("policy0"); remove_policy("policy4");
    assert(!rmdir("cpufreq") && !rmdir("config"));
    assert(!fchdir(original)); close(original); assert(!rmdir(scratch));
    puts("PASS: heterogeneous CPU policies, complete validation, ordered writes, error rollback, thermal readback and persistent startup restore");
    return 0;
}
