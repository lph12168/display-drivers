# Android makefile for display kernel modules
LOCAL_PATH := $(call my-dir)

ifeq ($(ENABLE_HYP),true)
#TODO: Enable back when dependencies are merged
#    include $(LOCAL_PATH)/msm-hyp/Android.mk
#    LOCAL_PATH := $(LOCAL_PATH)/../
#    include $(LOCAL_PATH)/msm-cfg/Android.mk
else
    include $(LOCAL_PATH)/msm/Android.mk
endif
