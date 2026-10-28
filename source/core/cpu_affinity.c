/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include "core/cpu_affinity.h"
#include "core/log.h"

#define CPU_AFFINITY_ROOT "/sys/devices/system/cpu"

static pthread_once_t affinity_once = PTHREAD_ONCE_INIT;
static cpu_set_t heavy_set, light_set;
static int affinity_ready;
static atomic_int heavy_error_logged, light_error_logged;

/* Only initially permitted CPUs participate. Do not infer core numbers from
 * a SoC name or from thermally capped scaling frequencies. */
static int read_metrics(const char *root, const cpu_set_t *allowed, const char *attribute,
                        unsigned long long values[CPU_SETSIZE])
{
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, allowed)) continue;
        char path[PATH_MAX], extra;
        int n = snprintf(path, sizeof(path), "%s/cpu%d/%s", root, cpu, attribute);
        if (n < 0 || (size_t)n >= sizeof(path)) return -1;
        FILE *input = fopen(path, "r");
        if (!input) return -1;
        int fields = fscanf(input, "%llu %c", &values[cpu], &extra);
        int failed = ferror(input);
        if (fclose(input)) failed = 1;
        if (failed || fields != 1 || !values[cpu]) return -1;
    }
    return 0;
}

/* Middle and highest capability tiers can both contribute to heavy work. */
static int split_metrics(const cpu_set_t *allowed, const unsigned long long values[CPU_SETSIZE],
                         cpu_set_t *heavy, cpu_set_t *light)
{
    unsigned long long minimum = ULLONG_MAX, maximum = 0;
    int count = 0;
    *heavy = *light = *allowed;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) if (CPU_ISSET(cpu, allowed)) {
        if (!values[cpu]) return 0;
        if (values[cpu] < minimum) minimum = values[cpu];
        if (values[cpu] > maximum) maximum = values[cpu];
        count++;
    }
    if (count < 2 || minimum == maximum) return 0;
    unsigned long long midpoint = minimum + (maximum - minimum) / 2u;
    CPU_ZERO(heavy); CPU_ZERO(light);
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) if (CPU_ISSET(cpu, allowed)) {
        if (values[cpu] > midpoint) CPU_SET(cpu, heavy);
        else CPU_SET(cpu, light);
    }
    return 1;
}

static const char *detect_groups(const char *root, const cpu_set_t *allowed,
                                 cpu_set_t *heavy, cpu_set_t *light)
{
    unsigned long long values[CPU_SETSIZE] = {0};
    *heavy = *light = *allowed;
    if (!read_metrics(root, allowed, "cpu_capacity", values) &&
        split_metrics(allowed, values, heavy, light)) return "cpu_capacity";
    memset(values, 0, sizeof(values));
    if (!read_metrics(root, allowed, "cpufreq/cpuinfo_max_freq", values) &&
        split_metrics(allowed, values, heavy, light)) return "cpuinfo_max_freq";
    *heavy = *light = *allowed;
    return "shared allowed CPUs (homogeneous or unknown topology)";
}

static void describe_set(const cpu_set_t *set, char *text, size_t size)
{
    size_t used = 0;
    text[0] = '\0';
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) if (CPU_ISSET(cpu, set)) {
        int n = snprintf(text + used, size - used, "%s%d", used ? "," : "", cpu);
        if (n < 0 || (size_t)n >= size - used) break;
        used += (size_t)n;
    }
}

static void initialize_affinity(void)
{
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) || !CPU_COUNT(&allowed)) {
        LOG_ERROR("cpu: cannot read allowed CPU set; keeping scheduler affinity: %s", strerror(errno));
        return;
    }
    const char *source = detect_groups(CPU_AFFINITY_ROOT, &allowed, &heavy_set, &light_set);
    affinity_ready = 1;
    char heavy[4096], light[4096];
    describe_set(&heavy_set, heavy, sizeof(heavy));
    describe_set(&light_set, light, sizeof(light));
    LOG_INFO("cpu: heavy CPUs [%s], light CPUs [%s], detected by %s", heavy, light, source);
}

int cpu_affinity_init(void)
{
    int result = pthread_once(&affinity_once, initialize_affinity);
    if (result) { errno = result; return -1; }
    return affinity_ready ? 0 : -1;
}

static int bind_group(const cpu_set_t *set, atomic_int *logged, const char *name)
{
    if (cpu_affinity_init()) return -1;
    int result = pthread_setaffinity_np(pthread_self(), sizeof(*set), set);
    if (result) {
        if (!atomic_exchange(logged, 1))
            LOG_ERROR("cpu: cannot bind %s work: %s", name, strerror(result));
        errno = result;
        return -1;
    }
    return 0;
}

int cpu_bind_big(void) { return bind_group(&heavy_set, &heavy_error_logged, "heavy"); }
int cpu_bind_little(void) { return bind_group(&light_set, &light_error_logged, "light"); }
