 /*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __VIRTIO_EXT_H__
#define __VIRTIO_EXT_H__

#include <linux/virtio_gpu.h>

#pragma pack(push, 1)
#define VIRTIO_GPU_MAX_MODIFIERS 4
#define VIRTIO_GPU_MAX_EVENTS 4
#define VIRTIO_GPU_MAX_PIXEL_FORMATS 32
#define VIRTIO_GPU_MAX_PLANES 16
#define VIRTIO_GPU_MAX_MODES 8

#define VIRTIO_GPU_PORT_DSI 1
#define VIRTIO_GPU_PORT_DP 2
#define VIRTIO_GPU_PORT_VIRTUAL 3

enum virtio_gpu_ctrl_type_ext {
	VIRTIO_GPU_CMD_GET_DISPLAY_INFO_EXT = 0x904,
	VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING_EXT,
	VIRTIO_GPU_CMD_GET_SCANOUT_ATTRIBUTES,
	VIRTIO_GPU_CMD_SET_SCANOUT_PROPERTIES,
	VIRTIO_GPU_CMD_GET_SCANOUT_PLANES,
	VIRTIO_GPU_CMD_PLANE_CREATE,
	VIRTIO_GPU_CMD_GET_PLANES_CAPS,
	VIRTIO_GPU_CMD_GET_PLANE_PROPERTIES,
	VIRTIO_GPU_CMD_PLANE_DESTROY,
	VIRTIO_GPU_CMD_SET_PLANE,
	VIRTIO_GPU_CMD_SET_PLANE_PROPERTIES,
	VIRTIO_GPU_CMD_SET_RESOURCE_INFO,
	VIRTIO_GPU_CMD_WAIT_FOR_VSYNC,
	VIRTIO_GPU_CMD_SET_PLANE_HDR,
	VIRTIO_GPU_CMD_SET_PIC_ADJUST,
	VIRTIO_GPU_CMD_PLANE_FLUSH,
	VIRTIO_GPU_CMD_SCANOUT_FLUSH,
	VIRTIO_GPU_CMD_FULL_FLUSH,
	VIRTIO_GPU_CMD_EVENT_CONTROL,
	VIRTIO_GPU_CMD_WAIT_EVENTS,

	VIRTIO_GPU_RESP_EXTENTION_START = 0x1300,
	VIRTIO_GPU_RESP_ERR_UNSUPPORTED_COMMAND,
	VIRTIO_GPU_RESP_ERR_BACKING_SWAP_NOT_SUPPORTED,
	VIRTIO_GPU_RESP_ERR_BACKING_IN_USE,
	VIRTIO_GPU_RESP_OK_DISPLAY_INFO_EXT,
	VIRTIO_GPU_RESP_OK_SCANOUT_ATTRIBUTES,
	VIRTIO_GPU_RESP_OK_SET_SCANOUT_PROPERTIES,
	VIRTIO_GPU_RESP_OK_GET_SCANOUT_PLANES,
	VIRTIO_GPU_RESP_OK_GET_PLANES_CAPS,
	VIRTIO_GPU_RESP_OK_PLANE_CREATE,
	VIRTIO_GPU_RESP_OK_PLANE_DESTROY,
	VIRTIO_GPU_RESP_OK_GET_PLANE_PROPERTIES,
	VIRTIO_GPU_RESP_OK_SET_PLANE_PROPERTIES,
	VIRTIO_GPU_RESP_OK_SET_PLANE,
	VIRTIO_GPU_RESP_OK_SCANOUT_FLUSH,
	VIRTIO_GPU_RESP_OK_PLANE_FLUSH,
	VIRTIO_GPU_RESP_OK_FULL_FLUSH,
	VIRTIO_GPU_RESP_OK_WAIT_FOR_EVENTS,
	VIRTIO_GPU_RESP_OK_SET_PIC_ADJUST,
	VIRTIO_GPU_RESP_EXTENTION_END

};

enum display_event_types {
	VIRTIO_VSYNC,
	VIRTIO_COMMIT_COMPLETE,
	VIRTIO_HPD,
	VIRTIO_MAX_EVENT
};

enum display_port_type {
	VIRTIO_PORT_TYPE_INTERNAL,
	VIRTIO_PORT_TYPE_HDMI,
	VIRTIO_PORT_TYPE_DSI,
	VIRTIO_PORT_TYPE_DP,
	VIRTIO_PORT_TYPE_MAX
};

struct virtio_gpu_get_display_info_ext {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 padding;
};

struct virtio_gpu_resp_display_info_ext {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	struct virtio_gpu_display_ext {
		struct virtio_gpu_rect r;
		__le32 refresh;
		__le32 enabled;
		__le32 flags;
	} pmodes[VIRTIO_GPU_MAX_MODES];
	__le32 padding;
};

struct virtio_gpu_get_scanout_attributes {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 padding;
};

struct virtio_gpu_resp_scanout_atttributes {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 type;
	__le32 connection_status;
	__le32 width_mm;
	__le32 height_mm;
	__le32 padding;
};

struct virtio_gpu_get_scanout_planes {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 padding;
};

struct virtio_gpu_resp_scanout_planes {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 num_planes;
	__le32 plane_ids[VIRTIO_GPU_MAX_PLANES];
	__le32 padding;
};

struct virtio_gpu_get_planes_caps {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 plane_id;
	__le32 padding;
};

struct virtio_gpu_resp_planes_caps {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_plane_caps {
		__le32 scanout_id;
		__le32 plane_id;
		__le32 plane_type;
		__le32 max_width;
		__le32 max_height;
		__le32 num_formats;
		__le32 formats[VIRTIO_GPU_MAX_PIXEL_FORMATS];
		__le32 max_scale;
	}caps;
	__le32 padding;
};

struct virtio_gpu_set_scanout_properties {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 power_mode;
	__le32 mode_index;
	__le32 rotation;
	struct virtio_gpu_rect r; //dest_rect
	__le32 padding;
};

struct virtio_gpu_resp_scanout_properties {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 error_code;
	__le32 padding;
};

struct virtio_gpu_set_plane {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_rect r;
	__le32 scanout_id;
	__le32 plane_id;
	__le32 resource_id;
	__le32 padding;
};

struct virtio_gpu_resp_set_plane {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 plane_id;
	__le32 error_code;
	__le32 padding;
};

struct virtio_gpu_create_plane {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 plane_id;
	__le32 padding;
};

struct virtio_gpu_resp_plane_create {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 plane_id;
	__le32 error_code;
	__le32 padding;
};

struct virtio_gpu_plane_destroy {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 plane_id;
	__le32 padding;
};

struct virtio_gpu_resp_plane_destroy {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 plane_id;
	__le32 error_code;
	__le32 padding;
};

enum plane_property_mask {
	Z_ORDER = (1<<0),
	GLOBAL_ALPHA = (1<<1),
	BLEND_MODE = (1<<2),
	SRC_RECT = (1<<3),
	DST_RECT = (1<<4),
	COLOR_SPACE = (1<<5),
	COLORIMETRY = (1<<6),
	COLOR_RANGE = (1<<7),
	HUE = (1<<8),
	SATURATION = (1<<9),
	CONTRAST = (1<<10),
	BRIGHTNESS = (1<<11),
};

struct virtio_gpu_set_plane_properties {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 plane_id;
	__le64 mask;
	__le32 z_order;
	__le32 global_alpha;
	__le32 blend_mode;
	struct virtio_gpu_rect src_rect;
	struct virtio_gpu_rect dst_rect;
	__le32 color_space;
	__le32 colorimetry;
	__le32 color_range;
	__le32 hue;
	__le32 saturation;
	__le32 contrast;
	__le32 brightness;
	__le32 padding;
};

struct virtio_gpu_resp_plane_properties {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 plane_id;
	__le32 error_code;
	__le32 padding;
};

struct virtio_gpu_get_plane_properties {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 plane_id;
	__le32 padding;
};

struct virtio_gpu_resp_get_plane_properties {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 plane_id;
	__le32 zorder;
	__le32 padding;
};

struct virtio_gpu_set_plane_hdr {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 plane_id;
//	u8 hdr_metadata[…];
	__le32 padding;
};

struct virtio_gpu_plane_flush {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 plane_id;
	__le32 async_mode;
	__le32 padding;
};

struct virtio_gpu_resp_plane_flush {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 plane_id;
	__le32 error_code;
	__le32 padding;
};

struct virtio_gpu_scanout_flush {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 async_mode;
	__le32 padding;
};

struct virtio_gpu_resp_scanout_flush {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 error_code;
	__le32 padding;
};

struct virtio_gpu_full_flush {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 async_mode;
	__le32 padding;
};

struct virtio_gpu_resp_full_flush {
	struct virtio_gpu_ctrl_hdr hdr;
	 __le32 error_code;
	 __le32 padding;
};

struct virtio_gpu_event_control {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 event_type;
	__le32 enable;
	__le32 padding;
};

struct virtio_gpu_wait_events {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 max_num_events;
	__le32 padding;
};

struct virtio_gpu_resp_event {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_event_count_one {
		__le32 enabled;
		__le32 vsync_count;
		__le32 commit_count;
		__le32 hpd_count;
	}scanout[VIRTIO_GPU_MAX_SCANOUTS];
	__le32 padding;
};

struct virtio_gpu_resource_attach_backing_ext {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 resource_id;
	__le64 shmem_id;
	__le32 size;
	__le32 padding;
};

struct virtio_gpu_set_scanout_pic_adjust {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 hue;
	__le32 saturation;
	__le32 contrast;
	__le32 brightness;
	__le32 padding;
};

struct virtio_gpu_resp_scanout_pic_adjust {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 scanout_id;
	__le32 error_code;
	__le32 padding;
};

enum resource_modifier_mask {
	SECURE_SOURCE = (1<<0),
	COMPRESSED_SOURCE = (1<<1),
};

struct virtio_gpu_set_resource_info {
	struct virtio_gpu_ctrl_hdr hdr;
	__le32 resource_id;
	__le32 modifiers;
	__le32 strides[4];
	__le32 offsets[4];
	__le32 ext_format;
	__le32 padding;
};
#pragma pack(pop)
#endif //__VIRTIO_EXT_H__

