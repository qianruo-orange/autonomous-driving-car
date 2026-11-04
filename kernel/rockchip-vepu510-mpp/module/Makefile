# SPDX-License-Identifier: GPL-2.0-only
obj-m += rockchip_vepu510.o
rockchip_vepu510-y := mpp_service.o mpp_common.o mpp_iommu.o mpp_rkvenc2.o
ccflags-y += -I$(src)/include
ccflags-y += -DCONFIG_ROCKCHIP_MPP_SERVICE=1 -DCONFIG_ROCKCHIP_MPP_RKVENC2=1 -DCONFIG_CPU_RK3576=1
ccflags-y += -DMPP_VERSION=\"linux74-vepu510-prototype\"
