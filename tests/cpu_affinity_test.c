/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tests never inspect or change the running test process's affinity. */
#include "core/cpu_affinity.c"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

void _log_info(const char *file, int line, const char *format, ...)
{ (void)file; (void)line; (void)format; }
void _log_error(const char *file, int line, const char *format, ...)
{ (void)file; (void)line; (void)format; }

static void put(const char *root, int cpu, const char *attribute, const char *value)
{
    char directory[PATH_MAX], path[PATH_MAX];
    snprintf(directory, sizeof(directory), "%s/cpu%d", root, cpu);
    assert(!mkdir(directory, 0700) || errno == EEXIST);
    snprintf(directory, sizeof(directory), "%s/cpu%d/cpufreq", root, cpu);
    assert(!mkdir(directory, 0700) || errno == EEXIST);
    assert(snprintf(path, sizeof(path), "%s/cpu%d/%s", root, cpu, attribute) < (int)sizeof(path));
    FILE *output = fopen(path, "w"); assert(output);
    assert(fputs(value, output) >= 0 && !fclose(output));
}
static void assert_group(const cpu_set_t *set, int a, int b)
{
    assert(CPU_COUNT(set) == (a == b ? 1 : 2));
    assert(CPU_ISSET(a, set) && CPU_ISSET(b, set));
}
static void remove_cpu(const char *root, int cpu)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/cpu%d/cpu_capacity", root, cpu);
    assert(!unlink(path) || errno == ENOENT);
    snprintf(path, sizeof(path), "%s/cpu%d/cpufreq/cpuinfo_max_freq", root, cpu);
    assert(!unlink(path) || errno == ENOENT);
    snprintf(path, sizeof(path), "%s/cpu%d/cpufreq", root, cpu); assert(!rmdir(path));
    snprintf(path, sizeof(path), "%s/cpu%d", root, cpu); assert(!rmdir(path));
}

int main(void)
{
    cpu_set_t allowed, heavy, light;
    unsigned long long values[CPU_SETSIZE] = {0};
    CPU_ZERO(&allowed); CPU_SET(2, &allowed); CPU_SET(7, &allowed);
    values[2] = 400; values[7] = 1024;
    assert(split_metrics(&allowed, values, &heavy, &light));
    assert_group(&heavy, 7, 7); assert_group(&light, 2, 2);
    CPU_SET(11, &allowed); values[11] = 768;
    assert(split_metrics(&allowed, values, &heavy, &light));
    assert_group(&heavy, 7, 11); assert_group(&light, 2, 2);
    values[2] = values[7] = values[11] = 512;
    assert(!split_metrics(&allowed, values, &heavy, &light));
    assert(CPU_EQUAL(&allowed, &heavy) && CPU_EQUAL(&allowed, &light));

    char scratch[] = "/tmp/cpu-affinity-test-XXXXXX"; assert(mkdtemp(scratch));
    put(scratch, 2, "cpu_capacity", "400\n");
    put(scratch, 7, "cpu_capacity", "1024\n");
    put(scratch, 11, "cpu_capacity", "768\n");
    /* Opposite frequency ordering proves complete capacity takes priority. */
    put(scratch, 2, "cpufreq/cpuinfo_max_freq", "2208000\n");
    put(scratch, 7, "cpufreq/cpuinfo_max_freq", "1608000\n");
    put(scratch, 11, "cpufreq/cpuinfo_max_freq", "1608000\n");
    assert(!strcmp(detect_groups(scratch, &allowed, &heavy, &light), "cpu_capacity"));
    assert_group(&heavy, 7, 11); assert_group(&light, 2, 2);
    put(scratch, 11, "cpu_capacity", "invalid\n");
    assert(!strcmp(detect_groups(scratch, &allowed, &heavy, &light), "cpuinfo_max_freq"));
    assert_group(&heavy, 2, 2); assert_group(&light, 7, 11);
    put(scratch, 7, "cpufreq/cpuinfo_max_freq", "invalid\n");
    assert(strstr(detect_groups(scratch, &allowed, &heavy, &light), "unknown topology"));
    assert(CPU_EQUAL(&allowed, &heavy) && CPU_EQUAL(&allowed, &light));
    /* Excluded CPUs with missing data must not affect allowed groups. */
    CPU_CLR(11, &allowed);
    assert(!strcmp(detect_groups(scratch, &allowed, &heavy, &light), "cpu_capacity"));
    assert_group(&heavy, 7, 7); assert_group(&light, 2, 2);
    CPU_ZERO(&allowed); CPU_SET(11, &allowed);
    assert(strstr(detect_groups(scratch, &allowed, &heavy, &light), "unknown topology"));
    assert_group(&heavy, 11, 11); assert_group(&light, 11, 11);
    remove_cpu(scratch, 2); remove_cpu(scratch, 7); remove_cpu(scratch, 11);
    assert(!rmdir(scratch));
    puts("PASS: capacity/frequency topology, sparse allowed CPUs, three-tier, homogeneous and missing-data fallback");
    return 0;
}
