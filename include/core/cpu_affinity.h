/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CPU_AFFINITY_H
#define CPU_AFFINITY_H

#include <sched.h>

/* CPU_INHERIT leaves a thread's mask untouched, i.e. the whole permitted set.
 * LITTLE and BIG are the fixed core tables for RK3576: cpu0-3 and cpu4-7. */
typedef enum { CPU_INHERIT, CPU_LITTLE, CPU_BIG } cpu_group_t;

/* Initialize before binding main so workers can expand from inherited masks
 * into the fixed groups. CPUs outside the process's permitted set are dropped;
 * if either group ends up empty, both fall back to the permitted set. */
int cpu_affinity_init(void);
/* Callers exposing cpu_set_t must define _GNU_SOURCE before system headers.
 * Copy the LITTLE/BIG mask without binding any thread. INHERIT is handled by
 * the caller. Return -1 with errno on failure; leave *set intact. */
int cpu_affinity_get(cpu_group_t group, cpu_set_t *set);

#endif
