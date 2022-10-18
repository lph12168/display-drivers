# SPDX-License-Identifier: GPL-2.0-only

ifeq ($(ENABLE_HYP),true)
    PRODUCT_PACKAGES += msm_hyp.ko
else
    PRODUCT_PACKAGES += msm_drm.ko
endif
