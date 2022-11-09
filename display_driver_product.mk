# SPDX-License-Identifier: GPL-2.0-only

ifeq ($(ENABLE_HYP),true)
#TODO: Enable back when dependencies are merged
#    PRODUCT_PACKAGES += msm_hyp.ko
#    PRODUCT_PACKAGES += msm_cfg.ko
else
    PRODUCT_PACKAGES += msm_drm.ko
endif
