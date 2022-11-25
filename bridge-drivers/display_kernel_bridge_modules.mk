# Build display kernel bridge driver
BRIDGE_KERNEL_MODULES :=

ifeq ($(call is-board-platform-in-list, kalama kona), true)
  BRIDGE_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/lt9611uxc.ko
endif
