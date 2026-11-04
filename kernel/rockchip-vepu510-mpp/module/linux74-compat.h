/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ROCKCHIP_MPP_LINUX74_COMPAT_H
#define ROCKCHIP_MPP_LINUX74_COMPAT_H

#include <linux/types.h>

struct device;

/* Implemented by the matching kernel-side recovery helpers. */
int rockchip_pmu_idle_request(struct device *dev, bool idle);

#endif
