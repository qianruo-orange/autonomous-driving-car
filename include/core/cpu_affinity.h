/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CPU_AFFINITY_H
#define CPU_AFFINITY_H

/* Initialize before binding main so workers can expand from inherited masks
 * into the original permitted groups. Discovery uses CPU capacity, then static
 * hardware maximum frequency. Homogeneous/unknown platforms retain all initially
 * permitted CPUs for both classes. No core numbers or SoC names are assumed. */
int cpu_affinity_init(void);
/* Bind the calling thread. Return 0 on success, -1 on failure. errno carries
 * the pthread error; failed calls leave the scheduler's current mask intact. */
int cpu_bind_big(void);
int cpu_bind_little(void);

#endif
