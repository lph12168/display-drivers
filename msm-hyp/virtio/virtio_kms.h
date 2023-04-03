/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __VIRTIO_KMS_H__
#define __VIRTIO_KMS_H__
#include <linux/virtio_gpu.h>
#include <msm_drv_hyp.h>
#include "virtio_ext.h"
#define PANEL_NAME_LEN 13
#define VIRTIO_MAX_CLIENTS	10
#define MARKER_BUFF_LENGTH 256

#define to_virtio_kms(x)\
		container_of((x), struct virtio_kms, base)


enum virtio_channel_ids {
	CHANNEL_CMD,
	CHANNEL_EVENTS,
	CHANNEL_BUFFERS,
	MAX_CHANNELS
};
struct scanout_sttrib {
	uint32_t type;
	uint32_t connection_status;
	uint32_t width_mm;
	uint32_t height_mm;
};

struct virtio_plane_caps {
	uint32_t plane_id;
	uint32_t plane_type;
	uint32_t max_width;
	uint32_t max_height;
	uint32_t num_formats;
	uint32_t formats[VIRTIO_GPU_MAX_PIXEL_FORMATS];
	uint32_t max_scale;
	uint32_t zorder;
};

struct virtio_display_modes {
	struct virtio_gpu_rect r;
	uint32_t refresh;
	uint32_t flags;
};

struct virtio_kms_output {
	int index;
	struct virtio_display_modes info[VIRTIO_GPU_MAX_MODES]; //modes
	uint32_t num_modes;
	struct scanout_sttrib attr;
	bool enabled;
	uint32_t type;
	struct edid *edid;
	uint32_t plane_cnt;
	struct virtio_plane_caps plane_caps[VIRTIO_GPU_MAX_PLANES];
	struct drm_crtc *crtc;
	bool vblank_enabled;
};

struct channel_map {
	int32_t hab_socket[MAX_CHANNELS];
	struct mutex hab_lock[MAX_CHANNELS];
};

struct virtio_kms {
	struct msm_hyp_kms base;
	struct channel_map channel[VIRTIO_MAX_CLIENTS];
	uint32_t mmid_cmd;
	uint32_t mmid_buffer;
	uint32_t mmid_event;
	bool stop;
        struct drm_device *dev;
        uint32_t client_id;
	struct virtio_device *vdev;
	wait_queue_head_t resp_wq;
	uint32_t max_sdma_width;

	/* current display info */
	spinlock_t display_info_lock;
	bool display_info_pending;

	uint32_t num_capsets;
	struct virtio_gpu_drv_capset *capsets;
	uint32_t num_scanouts;
	struct virtio_kms_output outputs[VIRTIO_GPU_MAX_SCANOUTS];
	bool has_edid;
};

struct virtio_mem_info {
	void *buffer;
	uint32_t size;
	uint64_t shmem_id;
};

struct virtio_framebuffer_priv {
	struct msm_hyp_framebuffer_info base;
	struct virtio_kms *kms;
	uint32_t format;
	uint32_t hw_res_handle;
	struct virtio_mem_info mem;
	bool created;
	bool secure;
};

struct virtio_connector_info_priv {
	struct msm_hyp_connector_info base;
	struct virtio_kms *kms;
	struct drm_crtc *crtc;
	int connector_status;
	uint32_t scanout;
	uint32_t mode_count;
	struct drm_display_mode *modes;
	char panel_name[PANEL_NAME_LEN];
	struct virtio_gpu_rect mode_rect;
	uint32_t mode_index;
};

struct virtio_crtc_info_priv {
	struct msm_hyp_crtc_info base;
	struct virtio_kms *kms;
	bool vblank_enable;
	int scanout;
	struct msm_hyp_prop_blob_info extra_info;
};

struct virtio_plane_info_priv {
	struct msm_hyp_plane_info base;
	struct virtio_kms *kms;
	uint32_t  plane_type;
	uint32_t plane_id;
	uint32_t scanout;
	bool committed;
};

void  virtio_kms_event_handler(struct virtio_kms *kms,
		uint32_t scanout,
		uint32_t num_event,
		uint32_t event_type);

#endif //_VIRTIO_KMS_H__
