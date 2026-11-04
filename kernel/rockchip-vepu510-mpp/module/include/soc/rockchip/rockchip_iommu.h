/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ROCKCHIP_IOMMU_RECOVERY_H
#define ROCKCHIP_IOMMU_RECOVERY_H

struct device;

/* The caller must quiesce its engine and disable the IOMMU IRQ first. */
int rockchip_iommu_reset(struct device *dev);

/* For a fault callback invoked with the IOMMU clocks already enabled. */
void rockchip_iommu_mask_irq(struct device *dev);

#endif
