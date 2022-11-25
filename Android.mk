# Android makefile for display kernel modules

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

DISPLAY_DLKM_ENABLE := true
ifeq ($(TARGET_KERNEL_DLKM_DISABLE), true)
	ifeq ($(TARGET_KERNEL_DLKM_DISPLAY_OVERRIDE), false)
		DISPLAY_DLKM_ENABLE := false
	endif
endif

ifeq ($(DISPLAY_DLKM_ENABLE),  true)
	include $(LOCAL_PATH)/msm/Android.mk
	include $(LOCAL_PATH)/../bridge-drivers/Android.mk
endif
