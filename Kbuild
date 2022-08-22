# SPDX-License-Identifier: GPL-2.0-only

ifeq ($(DISPLAY_ROOT),)
DISPLAY_ROOT=$(srctree)/techpack/display
endif

ifeq (y, $(findstring y, $(CONFIG_ARCH_SA8155) $(CONFIG_ARCH_SA6155) $(CONFIG_ARCH_SA8195)))
	include $(DISPLAY_ROOT)/config/augen3disp.conf
	LINUX_INC += -include $(DISPLAY_ROOT)/config/augen3dispconf.h
export DISPLAY_ROOT=$(srctree)/techpack/display
export KERNEL_SRC=$(srctree)
endif

ifeq (y, $(findstring y, $(CONFIG_ARCH_SA8155) $(CONFIG_ARCH_SA6155) $(CONFIG_ARCH_SA8195) \
		$(CONFIG_ARCH_DIREWOLF) $(CONFIG_ARCH_LEMANS)))
LINUXINCLUDE += -I$(DISPLAY_ROOT)/include/uapi/display \
		-I$(DISPLAY_ROOT)/include
USERINCLUDE += -I$(DISPLAY_ROOT)/include/uapi/display
CONFIG_DRM_MSM=y
endif

ifeq (y, $(findstring y, $(CONFIG_QTI_QUIN_GVM)))
include $(DISPLAY_ROOT)/config/gvmgen3disp.conf
LINUXINCLUDE += -include $(DISPLAY_ROOT)/config/gvmgen3dispconf.h
endif

obj-$(CONFIG_DRM_MSM) += msm/
obj-$(CONFIG_DRM_MSM_HYP) += msm-hyp/
