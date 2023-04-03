 /*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __VIRTIOGPU_VQ_H__
#define __VIRTIOGPU_VQ_H__

#include "virtio_kms.h"

struct plane_properties {
	uint64_t mask;
	uint32_t z_order;
	uint32_t global_alpha;
	uint32_t blend_mode;
	uint32_t color_space;
	uint32_t colorimetry;
	uint32_t color_range;
	uint32_t hue;
	uint32_t saturation;
	uint32_t contrast;
	uint32_t brightness;
	struct virtio_gpu_rect src_rect;
	struct virtio_gpu_rect dst_rect;
};

int virtio_gpu_cmd_set_scanout_pic_adjust(struct virtio_kms *kms,
	uint32_t scanout,
	uint32_t hue,
	uint32_t saturation,
	uint32_t contrast,
	uint32_t brightness);

int virtio_gpu_cmd_set_scanout_properties(struct virtio_kms *kms,
		uint32_t scanout,
		uint32_t power_mode,
		uint32_t mode_index,
		uint32_t rotation,
		struct virtio_gpu_rect dest_rect);

int virtio_gpu_cmd_set_scanout(struct virtio_kms *kms,
		uint32_t scanout,
		uint32_t res_id,
		struct virtio_gpu_rect dst_rect);

int virtio_gpu_cmd_resource_create_2D(struct virtio_kms *kms,
		uint32_t res_id,
		uint32_t format,
		uint32_t width,
		uint32_t height,
		uint32_t fence);

int virtio_gpu_cmd_resource_create_2D(struct virtio_kms *kms,
		uint32_t res_id,
		uint32_t format,
		uint32_t width,
		uint32_t height,
		uint32_t fence);

int virtio_gpu_cmd_resource_detach_backing(struct virtio_kms *kms,
		uint32_t resource_id);

int virtio_gpu_cmd_resource_attach_backing(struct virtio_kms *kms,
		uint32_t resource_id,
		uint32_t shmem_id,
		uint32_t size);

int virtio_gpu_cmd_resource_unref(struct virtio_kms *kms,
		uint32_t resource_id);

int virtio_gpu_cmd_plane_flush(struct virtio_kms *kms,
		uint32_t scanout,
		uint32_t plane_id,
		bool sync);

int virtio_gpu_cmd_scanout_flush(struct virtio_kms *kms,
		uint32_t scanout,
		bool sync);

int virtio_gpu_cmd_event_wait(struct virtio_kms *kms,
		uint32_t scanout,
		uint32_t max_num_events);

int virtio_gpu_cmd_event_control(struct virtio_kms *kms,
		uint32_t scanout,
		uint32_t event_type,
		bool enable);

int virtio_gpu_cmd_get_edid(struct virtio_kms *kms,
		uint32_t scanout);

int virtio_gpu_cmd_get_display_info(struct virtio_kms *kms);

int virtio_gpu_cmd_get_display_info_ext(struct virtio_kms *kms,
		uint32_t scanout);

int virtio_gpu_cmd_get_scanout_attributes(struct virtio_kms *kms,
		uint32_t scanout);

int virtio_gpu_cmd_get_scanout_planes(struct virtio_kms *kms,
		uint32_t scanout);

int virtio_gpu_cmd_get_plane_caps(struct virtio_kms *kms,
		uint32_t scanout,
		uint32_t plane_id);

int virtio_gpu_cmd_get_plane_properties(struct virtio_kms *kms,
		uint32_t scanout,
		uint32_t plane_id);

int virtio_gpu_cmd_set_resource_info(struct virtio_kms *kms,
		uint32_t resource_id,
		uint32_t modifiers,
		uint32_t *offsets,
		uint32_t *pitchs,
		uint32_t ext_format);

int virtio_gpu_cmd_set_plane(struct virtio_kms *kms,
		uint32_t scanout,
		uint32_t plane_id,
		uint32_t res_id);

int virtio_gpu_cmd_plane_create(struct virtio_kms *kms,
		uint32_t scanout,
		uint32_t plane_id);

int virtio_gpu_cmd_plane_destroy(struct virtio_kms *kms,
		uint32_t scanout,
		uint32_t plane_id);

int virtio_gpu_cmd_set_plane_properties(struct virtio_kms *kms,
		uint32_t scanout,
		uint32_t plane_id,
		struct plane_properties prop);

int virtio_gpu_event_kthread(void *d);

int virtio_gpu_cmd_resource_attach_backing_test(struct virtio_kms *kms,
		uint32_t resource_id,
		uint32_t shmem_id,
		uint32_t size,
		uint32_t handle, bool resp);

#endif //__VIRTIOGPU_VQ_H__
