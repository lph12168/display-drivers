# SPDX-License-Identifier: GPL-2.0-only

ifeq ($(DISPLAY_ROOT),)
DISPLAY_ROOT=$(srctree)/techpack/display
endif

ifeq (y, $(findstring y, $(CONFIG_ARCH_SA8155) $(CONFIG_ARCH_SA6155) $(CONFIG_ARCH_SA8195)))
	include $(DISPLAY_ROOT)/config/augen3disp.conf
	LINUX_INC += -include $(DISPLAY_ROOT)/config/augen3dispconf.h
endif

ifeq (y, $(findstring y, $(CONFIG_ARCH_LEMANS)))
	include $(DISPLAY_ROOT)/config/augen4disp.conf
	LINUX_INC += -include $(DISPLAY_ROOT)/config/augen4dispconf.h
endif

LINUXINCLUDE    += \
		   -I$(DISPLAY_ROOT)/include/uapi/display \
		   -I$(DISPLAY_ROOT)/include \
		   -I$(DISPLAY_ROOT)/include/linux
USERINCLUDE     += -I$(DISPLAY_ROOT)/include/uapi/display

ifeq (y, $(findstring y, $(CONFIG_QTI_QUIN_GVM)))
include $(DISPLAY_ROOT)/config/gvmgen3disp.conf
LINUXINCLUDE += -include $(DISPLAY_ROOT)/config/gvmgen3dispconf.h
endif

obj-$(CONFIG_DRM_MSM) += msm/
obj-$(CONFIG_DRM_MSM_HYP) += msm-hyp/
obj-$(CONFIG_DRM_MSM_CFG) += msm-cfg/
