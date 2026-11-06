/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CPU_AFFINITY_H
#define CPU_AFFINITY_H

#include <sched.h>

/* CPU_ALL is the full permitted set. SCHED_DEADLINE admission requires a task's
 * affinity to cover the whole root domain, so deadline threads may only use it.
 * CPU_BIG_PAIR is the two lowest-numbered heavy CPUs: the decode pair needs a
 * small, fixed set, and on this kernel DL and a narrowed affinity are mutually
 * exclusive (EPERM/EBUSY), so its threads stay FIFO. */
typedef enum { CPU_INHERIT, CPU_LITTLE, CPU_BIG, CPU_BIG_PAIR, CPU_ALL } cpu_group_t;

/* Initialize before binding main so workers can expand from inherited masks
 * into the original permitted groups. Discovery uses CPU capacity, then static
 * hardware maximum frequency. Homogeneous/unknown platforms retain all initially
 * permitted CPUs for both classes. No core numbers or SoC names are assumed. */
int cpu_affinity_init(void);
/* Callers exposing cpu_set_t must define _GNU_SOURCE before system headers.
 * Copy a discovered LITTLE/BIG mask without binding any thread. INHERIT is
 * handled by the caller. Return -1 with errno on failure; leave *set intact. */
int cpu_affinity_get(cpu_group_t group, cpu_set_t *set);

#endif
