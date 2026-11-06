/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include "core/cpu_affinity.h"
#include "core/log.h"

/* RK3576: cpu0-3 are the four Cortex-A53 cores, cpu4-7 the four Cortex-A72
 * cores. The split is fixed by the SoC; porting to another board edits these
 * two tables rather than probing capacity or max frequency at runtime. */
static const int big_cpus[] = {4, 5, 6, 7};
static const int little_cpus[] = {0, 1, 2, 3};

static pthread_once_t affinity_once = PTHREAD_ONCE_INIT;
static cpu_set_t big_set, little_set;
static int affinity_ready, affinity_error;

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

/* Only CPUs this process may actually run on are kept. A container or a cpuset
 * that confines the service to the little cores leaves the big mask empty; both
 * groups then fall back to the permitted set so threads can still be created. */
static void initialize_affinity(void)
{
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed)) affinity_error = errno;
    else if (!CPU_COUNT(&allowed)) affinity_error = EINVAL;
    if (affinity_error) {
        LOG_ERROR("cpu: cannot read allowed CPU set; keeping scheduler affinity: %s", strerror(affinity_error));
        return;
    }
    CPU_ZERO(&big_set);
    CPU_ZERO(&little_set);
    for (size_t i = 0; i < sizeof(big_cpus) / sizeof(big_cpus[0]); i++)
        if (CPU_ISSET(big_cpus[i], &allowed)) CPU_SET(big_cpus[i], &big_set);
    for (size_t i = 0; i < sizeof(little_cpus) / sizeof(little_cpus[0]); i++)
        if (CPU_ISSET(little_cpus[i], &allowed)) CPU_SET(little_cpus[i], &little_set);
    if (!CPU_COUNT(&big_set) || !CPU_COUNT(&little_set))
        big_set = little_set = allowed;
    affinity_ready = 1;
    char big[128], little[128];
    describe_set(&big_set, big, sizeof(big));
    describe_set(&little_set, little, sizeof(little));
    LOG_INFO("cpu: RK3576 big CPUs [%s], little CPUs [%s]", big, little);
}

int cpu_affinity_init(void)
{
    int result = pthread_once(&affinity_once, initialize_affinity);
    if (result) { errno = result; return -1; }
    if (!affinity_ready) { errno = affinity_error ? affinity_error : EIO; return -1; }
    return 0;
}

int cpu_affinity_get(cpu_group_t group, cpu_set_t *set)
{
    if (!set || (group != CPU_LITTLE && group != CPU_BIG)) {
        errno = EINVAL;
        return -1;
    }
    if (cpu_affinity_init()) return -1;
    *set = group == CPU_BIG ? big_set : little_set;
    return 0;
}
