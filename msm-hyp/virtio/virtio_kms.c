/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/sort.h>
#include <drm/drm_atomic.h>
#include <linux/virtio_config.h>
#include <soc/qcom/boot_stats.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_atomic_helper.h>

#include "msm_hyp_trace.h"
#include "msm_hyp_utils.h"
#include "virtio_kms.h"
#include "virtio_ext.h"
#include "virtgpu_vq.h"
#include <linux/habmm.h>
#define CLIENT_ID_LEN_IN_CHARS 5

#define DISPLAY_DEVICE_MAX_WIDTH      10240
#define DISPLAY_DEVICE_MAX_HEIGHT     4096

#define MAX_HORZ_DECIMATION    4
#define MAX_VERT_DECIMATION    4
#define SSPP_UNITY_SCALE       1
#define MAX_NUM_LIMIT_PAIRS    16
#define MAX_MDP_CLK_KHZ        412500

#define VIRTIO_DEBUG 1

struct limit_val_pair {
	const char *str;
	uint32_t val;
};

struct limit_constraints {
	uint32_t sdma_width;
	struct limit_val_pair pairs[MAX_NUM_LIMIT_PAIRS];
};

static struct limit_constraints constraints_table[] = {
	{
		/* SA6155 */
		1080,
		{
			{"sspp_linewidth_usecases", 3},
			{"vig",   0x1},
			{"dma",   0x2},
			{"scale", 0x4},
			{"sspp_linewidth_values", 3},
			{"limit_usecase", 0x1},
			{"limit_value",  2160},
			{"limit_usecase", 0x5},
			{"limit_value",  2160},
			{"limit_usecase", 0x2},
			{"limit_value",  2160},
		}
	},
	{
		/* SA8155/SA8195 */
		2048,
		{
			{"sspp_linewidth_usecases", 3},
			{"vig",   0x1},
			{"dma",   0x2},
			{"scale", 0x4},
			{"sspp_linewidth_values", 3},
			{"limit_usecase", 0x1},
			{"limit_value",  2560},
			{"limit_usecase", 0x5},
			{"limit_value",  2560},
			{"limit_usecase", 0x2},
			{"limit_value",  4096},
		}
	},
	{
		/* SA8295 */
		2560,
		{
			{"sspp_linewidth_usecases", 3},
			{"vig",   0x1},
			{"dma",   0x2},
			{"scale", 0x4},
			{"sspp_linewidth_values", 3},
			{"limit_usecase", 0x1},
			{"limit_value",  2560},
			{"limit_usecase", 0x5},
			{"limit_value",  2560},
			{"limit_usecase", 0x2},
			{"limit_value",  5120},
		}
	},
};

static const char * const disp_order_str[] = {
	"primary",
	"secondary",
	"tertiary",
	"quaternary",
	"quinary",
	"senary",
	"septenary",
	"octonary",
};
static int virtio_kms_create_framebuffer(struct virtio_kms *kms,
		struct msm_hyp_framebuffer *fb);

static int virtio_kms_scanout_init(struct virtio_kms *kms, uint32_t scanout);

static const char* virtio_get_drm_format_string(uint32_t drm_format) {
 switch (drm_format) {
   case DRM_FORMAT_ABGR1555:
     return "DRM_FORMAT_ABGR1555";
   case DRM_FORMAT_ABGR2101010:
     return "DRM_FORMAT_ABGR2101010";
   case DRM_FORMAT_ABGR4444:
     return "DRM_FORMAT_ABGR4444";
   case DRM_FORMAT_ABGR8888:
     return "DRM_FORMAT_ABGR8888";
   case DRM_FORMAT_ARGB1555:
     return "DRM_FORMAT_ARGB1555";
   case DRM_FORMAT_ARGB2101010:
     return "DRM_FORMAT_ARGB2101010";
   case DRM_FORMAT_ARGB4444:
     return "DRM_FORMAT_ARGB4444";
   case DRM_FORMAT_ARGB8888:
     return "DRM_FORMAT_ARGB8888";
   case DRM_FORMAT_AYUV:
     return "DRM_FORMAT_AYUV";
   case DRM_FORMAT_BGR233:
     return "DRM_FORMAT_BGR233";
   case DRM_FORMAT_BGR565:
     return "DRM_FORMAT_BGR565";
   case DRM_FORMAT_BGR888:
     return "DRM_FORMAT_BGR888";
   case DRM_FORMAT_BGRA1010102:
     return "DRM_FORMAT_BGRA1010102";
   case DRM_FORMAT_BGRA4444:
     return "DRM_FORMAT_BGRA4444";
   case DRM_FORMAT_BGRA5551:
     return "DRM_FORMAT_BGRA5551";
   case DRM_FORMAT_BGRA8888:
     return "DRM_FORMAT_BGRA8888";
   case DRM_FORMAT_BGRX1010102:
     return "DRM_FORMAT_BGRX1010102";
   case DRM_FORMAT_BGRX4444:
     return "DRM_FORMAT_BGRX4444";
   case DRM_FORMAT_BGRX5551:
     return "DRM_FORMAT_BGRX5551";
   case DRM_FORMAT_BGRX8888:
     return "DRM_FORMAT_BGRX8888";
   case DRM_FORMAT_C8:
     return "DRM_FORMAT_C8";
   case DRM_FORMAT_GR88:
     return "DRM_FORMAT_GR88";
   case DRM_FORMAT_NV12:
     return "DRM_FORMAT_NV12";
   case DRM_FORMAT_NV21:
     return "DRM_FORMAT_NV21";
   case DRM_FORMAT_R8:
     return "DRM_FORMAT_R8";
   case DRM_FORMAT_RG88:
     return "DRM_FORMAT_RG88";
   case DRM_FORMAT_RGB332:
     return "DRM_FORMAT_RGB332";
   case DRM_FORMAT_RGB565:
     return "DRM_FORMAT_RGB565";
   case DRM_FORMAT_RGB888:
     return "DRM_FORMAT_RGB888";
   case DRM_FORMAT_RGBA1010102:
     return "DRM_FORMAT_RGBA1010102";
   case DRM_FORMAT_RGBA4444:
     return "DRM_FORMAT_RGBA4444";
   case DRM_FORMAT_RGBA5551:
     return "DRM_FORMAT_RGBA5551";
   case DRM_FORMAT_RGBA8888:
     return "DRM_FORMAT_RGBA8888";
   case DRM_FORMAT_RGBX1010102:
     return "DRM_FORMAT_RGBX1010102";
   case DRM_FORMAT_RGBX4444:
     return "DRM_FORMAT_RGBX4444";
   case DRM_FORMAT_RGBX5551:
     return "DRM_FORMAT_RGBX5551";
   case DRM_FORMAT_RGBX8888:
      return "DRM_FORMAT_RGBX8888";
    case DRM_FORMAT_UYVY:
      return "DRM_FORMAT_UYVY";
    case DRM_FORMAT_VYUY:
      return "DRM_FORMAT_VYUY";
    case DRM_FORMAT_XBGR1555:
      return "DRM_FORMAT_XBGR1555";
    case DRM_FORMAT_XBGR2101010:
      return "DRM_FORMAT_XBGR2101010";
    case DRM_FORMAT_XBGR4444:
      return "DRM_FORMAT_XBGR4444";
    case DRM_FORMAT_XBGR8888:
      return "DRM_FORMAT_XBGR8888";
    case DRM_FORMAT_XRGB1555:
      return "DRM_FORMAT_XRGB1555";
    case DRM_FORMAT_XRGB2101010:
      return "DRM_FORMAT_XRGB2101010";
    case DRM_FORMAT_XRGB4444:
      return "DRM_FORMAT_XRGB4444";
    case DRM_FORMAT_XRGB8888:
      return "DRM_FORMAT_XRGB8888";
    case DRM_FORMAT_YUYV:
      return "DRM_FORMAT_YUYV";
    case DRM_FORMAT_YVU420:
      return "DRM_FORMAT_YVU420";
    case DRM_FORMAT_YVYU:
      return "DRM_FORMAT_YVYU";
  }
  return "Unknown";
}

static const struct {
         uint32_t drm_fmt;
         uint32_t virtio_fmt;
 } drm_virtio_formats[] = {
 #ifdef __BIG_ENDIAN
         {DRM_FORMAT_XRGB8888, VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM},
         {DRM_FORMAT_ARGB8888, VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM},
         {DRM_FORMAT_BGRX8888, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM},
         {DRM_FORMAT_BGRA8888, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM},
         {DRM_FORMAT_RGBX8888, VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM},
         {DRM_FORMAT_RGBA8888, VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM},
         {DRM_FORMAT_XBGR8888, VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM},
         {DRM_FORMAT_ABGR8888, VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM},
 #else
         {DRM_FORMAT_XRGB8888, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM},
         {DRM_FORMAT_ARGB8888, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM},
         {DRM_FORMAT_BGRX8888, VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM},
         {DRM_FORMAT_BGRA8888, VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM},
         {DRM_FORMAT_RGBX8888, VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM},
         {DRM_FORMAT_RGBA8888, VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM},
         {DRM_FORMAT_XBGR8888, VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM},
         {DRM_FORMAT_ABGR8888, VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM},
 #endif
         {0,0}
 };

 uint32_t get_drm_format(uint32_t virtio_format)
{
        uint32_t format = 0;
        int i = 0;
        while (drm_virtio_formats[i].virtio_fmt || drm_virtio_formats[i].drm_fmt) {
                if (virtio_format == drm_virtio_formats[i].virtio_fmt) {
                        format = drm_virtio_formats[i].drm_fmt;
                        break;
                }
                i++;
        }

        WARN_ON(format == 0);
        return format;
}

uint32_t virtio_gpu_translate_format(uint32_t drm_fourcc)
{
        uint32_t format = 0;
        int i = 0;

        while (drm_virtio_formats[i].virtio_fmt || drm_virtio_formats[i].drm_fmt) {
                if (drm_fourcc == drm_virtio_formats[i].drm_fmt) {
                        format = drm_virtio_formats[i].virtio_fmt;
                        break;
                }
                i++;
        }

        WARN_ON(format == 0);

        return format;
}
static int virtio_kms_connector_detect_ctx(struct drm_connector *connector,
			  struct drm_modeset_acquire_ctx *ctx,
			  bool force)
{
	struct msm_hyp_connector *c = to_msm_hyp_connector(connector);
	struct virtio_connector_info_priv *priv = container_of(c->info,
			struct virtio_connector_info_priv, base);
#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_connector_detect_ctx called\n");
	pr_err("virtio_kms_connector_detect_ctx done %d\n", priv->connector_status);
#endif
	return priv->connector_status;
}

static struct drm_encoder *virtio_kms_connector_best_encoder(
		struct drm_connector *connector)
{
	struct msm_hyp_connector *c_conn = to_msm_hyp_connector(connector);

#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_connector_best_encoder Called\n");
	pr_err("virtio_kms_connector_best_encoder Done\n");
#endif
	return &c_conn->encoder;
}

static int virtio_kms_connector_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *m;
	struct msm_hyp_connector *c_conn;
	struct virtio_connector_info_priv *priv;
	uint32_t i;

#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_connector_get_modes called\n");
#endif
	c_conn = to_msm_hyp_connector(connector);

	priv = container_of(c_conn->info,
                          struct virtio_connector_info_priv, base);
	for (i = 0; i < priv->mode_count; i++) {
		m = drm_mode_duplicate(connector->dev,
				&priv->modes[i]);
		if (!m)
			return i;
		drm_mode_probed_add(connector, m);
	}

//	msm_hyp_connector_init_edid(connector, priv->panel_name);

	if (c_conn->info->display_info.width_mm > 0 &&
				c_conn->info->display_info.height_mm > 0) {
		connector->display_info.width_mm =
					c_conn->info->display_info.width_mm;
		connector->display_info.height_mm =
					c_conn->info->display_info.height_mm;
	}

#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_connector_get_modes done %d\n",  priv->mode_count);
#endif
	return priv->mode_count;
}
static const struct drm_connector_helper_funcs virtio_conn_helper_funcs = {
	.detect_ctx = virtio_kms_connector_detect_ctx,
	.get_modes = virtio_kms_connector_get_modes,
	.best_encoder = virtio_kms_connector_best_encoder,
};

static void virtio_kms_bridge_mode_set(struct drm_bridge *drm_bridge,
		const struct drm_display_mode *mode,
		const struct drm_display_mode *adjusted_mode)
{
	struct msm_hyp_connector *connector;
	struct virtio_connector_info_priv *priv;
	struct virtio_gpu_rect dest_rect = {0,0,0,0};
	int i, mode_index = 0;
	uint32_t scanout;// = priv->scanout;
	int rc = 0;

#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_bridge_mode_set called\n");
#endif
	connector = container_of(drm_bridge, struct msm_hyp_connector, bridge);
	priv = container_of(connector->info, struct virtio_connector_info_priv, base);
	scanout = priv->scanout;

	for (i = 0; i < priv->mode_count; i++) {
		mode = &priv->modes[i];
		if ((adjusted_mode->hdisplay == mode->hdisplay) &&
		    (adjusted_mode->vdisplay == mode->vdisplay)) {
//			mode_index = *mode->private;
			dest_rect.width = mode->hdisplay;
			dest_rect.height = mode->vdisplay;
			dest_rect.x = 0;
			dest_rect.y = 0;
			break;
		}
	}
/*	if (mode_index < 0) {
		pr_err("mode set failed %d for mode h-%d v-%d",
				priv->scanout,
				adjusted_mode->hdisplay,
				adjusted_mode->vdisplay);
		mode = NULL;
		return;
	}*/
	priv->mode_index = 0;//mode_index;
	priv->mode_rect.width = mode->hdisplay;
	priv->mode_rect.height = mode->vdisplay;
	priv->mode_rect.x = 0;
	priv->mode_rect.y = 0;

	rc = virtio_gpu_cmd_set_scanout_properties(priv->kms,
			scanout,
			0x7680,//FALSE,
			mode_index,
			0,
			dest_rect);
	if (rc) {
		pr_err("scanout set properties for mode failed %d\n",
				mode_index);
	}

#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_bridge_mode_set done\n");
#endif
}

static void virtio_kms_bridge_enable(struct drm_bridge *drm_bridge)
{
	struct msm_hyp_connector *connector;
	struct virtio_connector_info_priv *priv;
	struct virtio_gpu_rect dest_rect;
	uint32_t scanout;

#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_bridge_enable called\n");
#endif
	connector = container_of(drm_bridge, struct msm_hyp_connector, bridge);
	priv = container_of(connector->info,struct virtio_connector_info_priv, base);
	dest_rect.width = priv->mode_rect.width;
        dest_rect.height = priv->mode_rect.height;
        dest_rect.x = priv->mode_rect.x,
        dest_rect.y = priv->mode_rect.y;
	scanout = priv->scanout;
	virtio_gpu_cmd_set_scanout_properties(priv->kms,
			scanout,
			0x7683,//TRUE,
			priv->mode_index,
			0,
			dest_rect);

#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_bridge_enable done\n");
#endif
}

static void virtio_kms_bridge_disable(struct drm_bridge *drm_bridge)
{
	struct msm_hyp_connector *connector;
	struct virtio_connector_info_priv *priv;
	struct virtio_gpu_rect dest_rect;
	uint32_t scanout;

#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_bridge_disable called\n");
#endif
	connector = container_of(drm_bridge, struct msm_hyp_connector, bridge);
	priv = container_of(connector->info, struct virtio_connector_info_priv, base);
	dest_rect.width = priv->mode_rect.width;
        dest_rect.height = priv->mode_rect.height;
        dest_rect.x = priv->mode_rect.x,
        dest_rect.y = priv->mode_rect.y;

        scanout = priv->scanout;
	virtio_gpu_cmd_set_scanout_properties(priv->kms,
			scanout,
			0x7680,//FALSE,
			priv->mode_index,
			0,
			dest_rect);

#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_bridge_disable done\n");
#endif
}

static const struct drm_bridge_funcs virtio_bridge_ops = {
	.enable       = virtio_kms_bridge_enable,
	.disable      = virtio_kms_bridge_disable,
	.mode_set     = virtio_kms_bridge_mode_set,
};

static int virtio_kms_connector_get_type(
		uint32_t port_type,
		uint32_t scanout,
		char *name)
{
	int connector_type;

	switch (port_type) {
	case VIRTIO_PORT_TYPE_INTERNAL:
	case VIRTIO_PORT_TYPE_HDMI:
		connector_type = DRM_MODE_CONNECTOR_DSI;
		snprintf(name, PANEL_NAME_LEN, "%s_%d", "HDMI", scanout);
		break;
	case VIRTIO_PORT_TYPE_DSI:
		connector_type = DRM_MODE_CONNECTOR_DSI;
		snprintf(name, PANEL_NAME_LEN, "%s_%d", "DSI", scanout);
		break;
	case VIRTIO_PORT_TYPE_DP:
		connector_type = DRM_MODE_CONNECTOR_DisplayPort;
		snprintf(name, PANEL_NAME_LEN, "%s_%d", "DP", scanout);
		break;
	default:
		connector_type = DRM_MODE_CONNECTOR_Unknown;
		snprintf(name, PANEL_NAME_LEN, "%s_%d", "Unknown", scanout);
		break;
	}

	pr_debug("%s - port_type = %x name = %s\n", __func__, port_type, name);

	return connector_type;
}

static int virtio_kms_get_connector_infos(struct msm_hyp_kms *hyp_kms,
		struct msm_hyp_connector_info **connector_infos,
		int *connector_num)
{
	struct virtio_kms *kms = to_virtio_kms(hyp_kms);
	struct drm_device *ddev = kms->dev;
	int i;
	uint64_t j;
	struct virtio_connector_info_priv *priv;
	struct virtio_display_modes *info;
	struct drm_display_mode *mode;
	struct scanout_sttrib *attr;

	if (!connector_infos) {
		*connector_num = kms->num_scanouts;
		return 0;
	}
	if (!ddev) {
		pr_err("ddev failed \n");
		return 0;
	}

	ddev->mode_config.min_width = 0;
	ddev->mode_config.max_width = DISPLAY_DEVICE_MAX_WIDTH;
	ddev->mode_config.min_height = 0;
	ddev->mode_config.max_height = DISPLAY_DEVICE_MAX_HEIGHT;

	for (i = 0; i < kms->num_scanouts; i++) {
		priv = kzalloc(sizeof(*priv), GFP_KERNEL);
		if (!priv)
			return -ENOMEM;

		attr = &kms->outputs[i].attr;
		info = &kms->outputs[i].info[0];
		priv->connector_status = attr->connection_status ?
				connector_status_connected :
				connector_status_disconnected;
		priv->base.connector_type =
			virtio_kms_connector_get_type(attr->type,
					i,
					priv->panel_name);
		priv->base.display_info.width_mm = attr->width_mm;
		priv->base.display_info.height_mm = attr->height_mm;
		priv->scanout = i;
		priv->base.possible_crtcs = 1 << i;
		if (!kms->outputs[i].num_modes) {
			kfree(priv);
			pr_err("number of modes 0\n");
			return -EINVAL;
		}

		if (kms->outputs[i].num_modes > 0) {
			priv->modes = kcalloc(kms->outputs[i].num_modes,
					sizeof(struct drm_display_mode),
					GFP_KERNEL);
			if (!priv->modes) {
				pr_err("Mode allocation failed\n");
				kfree(priv);
				return -ENOMEM;
			}
		}

		for (j = 0; j < kms->outputs[i].num_modes; j++) {
			mode = &priv->modes[j];
			mode->hdisplay = info[j].r.width;
			mode->vdisplay = info[j].r.height;
//			mode->vrefresh = info[j].refresh;
//TODO find a way to pass mode index
//			mode->private = (int *)j;
			mode->hsync_end = mode->hdisplay;
			mode->htotal = mode->hdisplay;
			mode->hsync_start = mode->hdisplay;
			mode->vsync_end = mode->vdisplay;
			mode->vtotal = mode->vdisplay;
			mode->vsync_start = mode->vdisplay;
			//TODO : find a way for teh referesh rate
			mode->clock = 60 * //mode->vrefresh *
					mode->vtotal *
					mode->htotal /
					1000LL;

			drm_mode_set_name(mode);

		}
		priv->mode_count = kms->outputs[i].num_modes;

		if (i < ARRAY_SIZE(disp_order_str))
			priv->base.display_type = disp_order_str[i];

		priv->base.connector_funcs = &virtio_conn_helper_funcs;
		priv->base.bridge_funcs = &virtio_bridge_ops;
		priv->kms = kms;
		connector_infos[i] = &priv->base;
	}
	return 0;
}

static bool virtio_kms_plane_is_rect_changed(struct drm_plane_state *pre,
	struct drm_plane_state *cur, bool src)
{
	bool ret = false;

	if (src) {
		if ((pre->src_x != cur->src_x) ||
			(pre->src_y != cur->src_y) ||
			(pre->src_w != cur->src_w) ||
			(pre->src_h != cur->src_h))
			ret = true;
	} else {
		if ((pre->crtc_x != cur->crtc_x) ||
			(pre->crtc_y != cur->crtc_y) ||
			(pre->crtc_w != cur->crtc_w) ||
			(pre->crtc_h != cur->crtc_h))
			ret = true;
	}

	return ret;
}

static int virtio_kms_plane_cmp(const void *a, const void *b)
{
	struct msm_hyp_plane_state *pa = *(struct msm_hyp_plane_state **)a;
	struct msm_hyp_plane_state *pb = *(struct msm_hyp_plane_state **)b;
	int rc = 0;

	if (pa->zpos != pb->zpos)
		rc = pa->zpos - pb->zpos;
	else
		rc = pa->base.crtc_x - pb->base.crtc_x;

	return rc;
}

static void virtio_kms_plane_zpos_adj_fe(struct drm_crtc *crtc,
		struct drm_atomic_state *old_state)
{
	struct drm_device *ddev = crtc->dev;
	int cnt = 0;
	struct drm_plane *plane;
	struct drm_plane_state *old_plane_state;
	struct msm_hyp_plane *p;
	struct msm_hyp_plane_state *old_pstate, *new_pstate;
	struct drm_crtc_state *old_crtc_state;
	bool zpos_update = false;
	struct msm_hyp_plane_state *sorted_pstate[VIRTIO_GPU_MAX_PLANES];
	struct virtio_plane_info_priv *priv;
	int i, rc;
	struct msm_hyp_crtc *c = to_msm_hyp_crtc(crtc);
	struct virtio_crtc_info_priv *crtc_priv = container_of(c->info,
			struct virtio_crtc_info_priv,
			base);
	struct plane_properties prop;

	drm_for_each_plane_mask(plane, ddev, crtc->state->plane_mask) {
		new_pstate = to_msm_hyp_plane_state(plane->state);
		sorted_pstate[cnt++] = new_pstate;

		if (zpos_update)
			continue;

		old_plane_state = drm_atomic_get_old_plane_state(
				old_state, plane);
		if (old_plane_state) {
			old_pstate = to_msm_hyp_plane_state(old_plane_state);
			if (old_pstate->zpos != new_pstate->zpos)
				zpos_update = true;
		}
	}
	old_crtc_state = drm_atomic_get_old_crtc_state(old_state, crtc);

	if (cnt && (zpos_update || (old_crtc_state->plane_mask !=
			crtc->state->plane_mask))) {
		sort(sorted_pstate, cnt, sizeof(sorted_pstate[0]),
				virtio_kms_plane_cmp, NULL);
		for (i = 0; i < cnt; i++) {
			p = to_msm_hyp_plane(sorted_pstate[i]->base.plane);
			priv = container_of(p->info,
					struct virtio_plane_info_priv, base);

			prop.z_order = i + 1;
			prop.mask |= Z_ORDER;
			rc = virtio_gpu_cmd_set_plane_properties(priv->kms,
				crtc_priv->scanout,
				priv->plane_id,
				prop);
			if (rc) {
				pr_err("set plane properties failed \n");
			}
		}
	}
}

static void virtio_kms_plane_atomic_update(struct drm_plane *plane,
		struct drm_atomic_state *old_atomic_state)
{
	struct msm_hyp_plane *p;
	struct virtio_plane_info_priv *plane_priv;
	struct msm_hyp_framebuffer *fb;
	struct virtio_framebuffer_priv *fb_priv;
	struct drm_plane_state *old_state;
	struct msm_hyp_plane_state *old_pstate, *new_pstate;
	struct plane_properties prop;
	struct msm_hyp_crtc *crtc;
	struct virtio_crtc_info_priv *crtc_priv;
	int rc = 0;
	struct virtio_kms *kms;

#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_plane_atomic_update called\n");
#endif
	p = to_msm_hyp_plane(plane);
	plane_priv = container_of(p->info, struct virtio_plane_info_priv, base);
	kms = plane_priv ? plane_priv->kms : NULL;

	if (!kms) {
		pr_err("kms failed \n");
		return;
	}

        old_state = drm_atomic_get_old_plane_state(old_atomic_state, plane);
	new_pstate = to_msm_hyp_plane_state(plane->state);
	old_pstate = to_msm_hyp_plane_state(old_state);

	memset(&prop, 0x00, sizeof(struct plane_properties));

	if (!plane->state->crtc) {

#ifdef VIRTIO_DEBUG
		pr_err("virtio_kms_plane_atomic_update crtc removed\n");
#endif
		crtc = to_msm_hyp_crtc(old_state->crtc);
		crtc_priv = container_of(crtc->info,
				struct virtio_crtc_info_priv,
				base);

		rc = virtio_gpu_cmd_set_plane(kms, crtc_priv->scanout,
				plane_priv->plane_id, 0);
		if (rc) {
			pr_err("set plane properties failed \n");
		}
	} else if (!plane->state->fb) {
		crtc = to_msm_hyp_crtc(plane->state->crtc);
		crtc_priv = container_of(crtc->info,
				struct virtio_crtc_info_priv,
				base);

#ifdef VIRTIO_DEBUG
		pr_err("virtio_kms_plane_atomic_update fb removed plane id %d\n",plane_priv->plane_id);
#endif
		rc = virtio_gpu_cmd_set_plane(kms,
				crtc_priv->scanout,
				plane_priv->plane_id,
				0);
		if (rc) {
			pr_err("set plane properties failed %d\n", plane_priv->plane_id);
		}
	} else {
		fb = to_msm_hyp_fb(plane->state->fb);
		fb_priv = container_of(fb->info,
				struct virtio_framebuffer_priv,
				base);
		crtc = to_msm_hyp_crtc(plane->state->crtc);
		crtc_priv = container_of(crtc->info,
					struct virtio_crtc_info_priv,
					base);
		if (!fb_priv || !crtc_priv) {
			pr_err("Something failed in commit\n");
			return;
		}

		if ((old_state->crtc != plane->state->crtc) ||
			(old_state->fb != plane->state->fb)) {
			fb_priv->secure = new_pstate->fb_mode == SDE_DRM_FB_SEC ?
				true : false;
			rc = virtio_kms_create_framebuffer(kms,	fb);
			if (rc)
				pr_err("create frame buffer failed\n");

			rc = virtio_gpu_cmd_set_plane(kms,
					crtc_priv->scanout,
					plane_priv->plane_id,
					fb_priv->hw_res_handle);
			if (rc)
				pr_err("set plane failed \n");
		}
	}

	if (virtio_kms_plane_is_rect_changed(old_state, plane->state, true)) {

#ifdef VIRTIO_DEBUG
		pr_err("virtio_kms_plane_atomic_update send src_rect %d %d %d %d\n",
				plane->state->src_x >> 16,
				plane->state->src_y >> 16,
				plane->state->src_w >> 16,
				plane->state->src_h >> 16);
#endif
		prop.src_rect.x = plane->state->src_x >> 16;
		prop.src_rect.y = plane->state->src_y >> 16;
		prop.src_rect.width = plane->state->src_w >> 16;
		prop.src_rect.height = plane->state->src_h >> 16;
		prop.mask |= SRC_RECT;
	}

	if (virtio_kms_plane_is_rect_changed(old_state, plane->state, false)) {

#ifdef VIRTIO_DEBUG
		pr_err("virtio_kms_plane_atomic_update send dest_rect %d %d %d %d\n",
				plane->state->crtc_x,
				plane->state->crtc_y,
				plane->state->crtc_w,
				plane->state->crtc_h);
#endif
		prop.dst_rect.x = plane->state->crtc_x;
		prop.dst_rect.y = plane->state->crtc_y;
		prop.dst_rect.width = plane->state->crtc_w;
		prop.dst_rect.height = plane->state->crtc_h;
		prop.mask |= DST_RECT;
	}

	if (old_pstate->alpha != new_pstate->alpha || !plane_priv->committed) {
		prop.global_alpha = new_pstate->alpha;
		 prop.mask |= GLOBAL_ALPHA;
	}

	rc = virtio_gpu_cmd_set_plane_properties(kms,
			crtc_priv->scanout,
			plane_priv->plane_id,
			prop);
	if (rc) {
		pr_err("set plane properties failed \n");
	}

	plane_priv->committed = true;
}

static const struct drm_plane_helper_funcs virtio_plane_helper_funcs = {
	.atomic_update = virtio_kms_plane_atomic_update,
};

static int virtio_kms_get_plane_infos(struct msm_hyp_kms *hyp_kms,
		struct msm_hyp_plane_info **plane_infos,
		int *plane_num)
{
	struct virtio_kms *kms = to_virtio_kms(hyp_kms);
	struct virtio_plane_info_priv *priv;
	int i, j, pipe_cnt = 0;
	int fmt_idx = 0;
	uint32_t *formats;
	uint32_t num_formats = 0;
	uint32_t plane_type;

	if (!kms || !plane_num)
		return -EINVAL;

	if (!plane_infos) {
		*plane_num = 0;
		for (i = 0; i < kms->num_scanouts; i++)
			*plane_num += kms->outputs[i].plane_cnt;
		return 0;
	}
	for (i = 0; i < kms->num_scanouts; i++) {

		for (j = 0; j < kms->outputs[i].plane_cnt; j++) {
			priv = kzalloc(sizeof(struct virtio_plane_info_priv), GFP_KERNEL);
			if (priv == NULL) {
				return -ENOMEM;
			}

			if (j == 0)
				plane_type = DRM_PLANE_TYPE_PRIMARY;
			else
				plane_type = DRM_PLANE_TYPE_OVERLAY;

//			plane_type = kms->outputs[i].plane_caps[j].plane_type;

			priv->plane_type = plane_type;
			priv->base.plane_type = plane_type;
			priv->scanout = i;
			num_formats = kms->outputs[i].plane_caps[j].num_formats;
			formats = kms->outputs[i].plane_caps[j].formats;

			if (!num_formats) {
				pr_err("formats for plane ID %d\
						for san out %d failed\n",
						j, i);
				kfree(priv);
				return -EINVAL;
			}
			priv->base.format_types = kcalloc(num_formats, sizeof(uint32_t),
							GFP_KERNEL);
			if (priv->base.format_types == NULL) {
				pr_err("base.format_types Memory allocation failed\n");
				return -ENOMEM;
			}

			for (fmt_idx = 0; fmt_idx < num_formats; fmt_idx++) {
				priv->base.format_types[fmt_idx] =
					get_drm_format(formats[fmt_idx]);
#ifdef VIRTIO_DEBUG
				pr_err("Format %s\n", virtio_get_drm_format_string(priv->base.format_types[fmt_idx]));
#endif
			}
			priv->base.format_count = num_formats;
			//TODO : check for the support of scaling and csc
			priv->base.support_scale = false;
			priv->base.support_csc = false;
			priv->base.possible_crtcs = 1 << i;
			priv->base.maxdwnscale = SSPP_UNITY_SCALE;
			priv->base.maxupscale = SSPP_UNITY_SCALE;
			priv->base.maxhdeciexp = MAX_HORZ_DECIMATION;
			priv->base.maxvdeciexp = MAX_VERT_DECIMATION;
			priv->base.max_width =
				kms->outputs[i].plane_caps[j].max_width;
			priv->base.max_bandwidth = 4500000000;

			priv->base.plane_funcs = &virtio_plane_helper_funcs;
			priv->kms = kms;
			priv->plane_id = kms->outputs[i].plane_caps[j].plane_id;
			plane_infos[j + pipe_cnt] = &priv->base;
		}
		pipe_cnt += kms->outputs[i].plane_cnt;
	}
	return 0;
}

static void _virtio_kms_set_crtc_limit(struct virtio_kms *kms,
		struct virtio_crtc_info_priv *crtc_priv)
{
	struct limit_constraints *constraints = NULL;
	struct limit_val_pair *pair;
	char buf[16];
	int i;

	for (i = 0; i < ARRAY_SIZE(constraints_table); i++) {
		if (constraints_table[i].sdma_width == kms->max_sdma_width) {
			constraints = &constraints_table[i];
			break;
		}
	}

	if (!constraints)
		return;

	for (i = 0; i < MAX_NUM_LIMIT_PAIRS; i++) {
		pair = &constraints->pairs[i];

		if (!pair->str)
			break;

		snprintf(buf, sizeof(buf), "%d", pair->val);
		msm_hyp_prop_info_add_keystr(&crtc_priv->extra_info,
				pair->str, buf);
	}

	crtc_priv->base.extra_caps = crtc_priv->extra_info.data;
}


static int virtio_kms_get_crtc_infos(struct msm_hyp_kms *hyp_kms,
		struct msm_hyp_crtc_info **crtc_infos,
		int *crtc_num)
{
	struct virtio_kms *kms = to_virtio_kms(hyp_kms);
	struct virtio_crtc_info_priv *priv;
	int i;
	int plane_cnt = 0;

	if (!kms || !crtc_num)
		return -EINVAL;

	if (!crtc_infos) {
		*crtc_num = kms->num_scanouts;
		return 0;
	}

	for (i = 0; i < kms->num_scanouts; i++) {
		priv = kzalloc(sizeof(*priv), GFP_KERNEL);
		if (priv == NULL) {
			return -ENOMEM;
		}

		//get priv->base.max_blendstages
		priv->base.primary_plane_index = plane_cnt;
		plane_cnt = kms->outputs[i].plane_cnt;

		/* these values should read from host */
		priv->base.max_mdp_clk = 412500000LL;
		priv->base.qseed_type = "qseed3";
		priv->base.smart_dma_rev = "smart_dma_v2p5";
		priv->base.has_hdr = false;
		priv->base.max_bandwidth_low = 9600000000LL;
		priv->base.max_bandwidth_high = 9600000000LL;
		priv->base.has_src_split = true;
		priv->scanout = i;
		priv->kms = kms;
		_virtio_kms_set_crtc_limit(kms, priv);
		crtc_infos[i] = &priv->base;
	}
	return 0;
}

static int virtio_kms_get_mode_info(struct msm_hyp_kms *kms,
		const struct drm_display_mode *mode,
		struct msm_hyp_mode_info *modeinfo)
{
	pr_err("virtio_kms_get_mode_info called\n");
	modeinfo->num_lm = (mode->clock > MAX_MDP_CLK_KHZ) ? 2 : 1;
	modeinfo->num_enc = 0;
	modeinfo->num_intf = 1;

	return 0;
}

#if 0
static void virtio_gpu_resource_id_put(struct virtio_kms *kms, uint32_t id)
{
	return;
}
#endif

static void virtio_gpu_resource_id_get(uint32_t *resid)
{
	static atomic_t seqno = ATOMIC_INIT(1);
	int handle = atomic_inc_return(&seqno);
	*resid = handle + 1;
}

static int virtio_kms_create_framebuffer(struct virtio_kms *kms,
		struct msm_hyp_framebuffer *fb)
{
	struct virtio_framebuffer_priv *fb_priv;
	struct dma_buf *dma_buf;
	uint32_t client_id;
	struct virtio_mem_info *mem;
	uint32_t export_id = 0;
	uint32_t export_flags = 0;
	int32_t handle;
	int ret = 0;
	uint32_t fence = 0;
	uint32_t modifiers = 0;
//	struct dma_buf_map map;
//	char *ptr;
//	int i;

	if (!fb) {
		if (!fb->bo) {
			pr_err("no bo attached to fb\n");
			return -EINVAL;
		}
	}
#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_create_framebuffer called \n");
	pr_err("create: FB ID: %d (%pK)", fb->base.base.id, fb);
#endif

	fb_priv = container_of(fb->info, struct virtio_framebuffer_priv, base);
	client_id = fb_priv->kms->client_id;
        mem = &fb_priv->mem;
	handle =  fb_priv->kms->channel[client_id].hab_socket[CHANNEL_BUFFERS];

//	if (!fb_priv->created) {
		if (fb->bo->import_attach) {
#ifdef VIRTIO_DEBUG
			pr_err(" virtio_kms_create_framebuffer import_attach\n");
#endif
			dma_buf = fb->bo->import_attach->dmabuf;
			get_dma_buf(dma_buf);
			/*
			pr_err(" framebuffer dma_buf_vmap started \n");
			dma_buf_begin_cpu_access(dma_buf, DMA_BIDIRECTIONAL);
			ret =  dma_buf_vmap(dma_buf, &map);
			if (ret)
				pr_err(" mmap failed for dma_buf_vmap\n");
			else {
				ptr = (char *)map.vaddr;
				if (!ptr) {
					pr_err(" no memry map for da buffer\n");
				}
				else {
					for (i = 0; i < 50; ) {
						pr_err("framebuffer data %x %x %x %x %x \n", ptr[i], ptr[i+1], ptr[i+2], ptr[i+3], ptr[i+4]);
						i = i + 5;
					}
				}
			}
			pr_err("framebuffer dma_buf_vmap done %p\n", map.vaddr);
			dma_buf_vunmap(dma_buf, &map);
		  	dma_buf_end_cpu_access(dma_buf, DMA_BIDIRECTIONAL);
			*/
		} else if (fb->bo->dma_buf) {

#ifdef VIRTIO_DEBUG
			pr_err(" virtio_kms_create_framebuffer dma_buf \n");
#endif
			dma_buf = fb->bo->dma_buf;
			get_dma_buf(dma_buf);
		} else {

#ifdef VIRTIO_DEBUG
			pr_err("virtio_kms_create_framebuffer drm_gem_prime_export\n");
#endif
			dma_buf = drm_gem_prime_export(fb->bo, 0);
			if (IS_ERR(dma_buf))
				return PTR_ERR(dma_buf);
		}

		memset((char *)mem, 0x00,
				sizeof(struct virtio_mem_info));
		mem->size	= fb->bo->size;
		mem->buffer	= (void *)dma_buf;
		export_flags |= HABMM_EXPIMP_FLAGS_DMABUF;
		ret = habmm_export(
			handle,
			mem->buffer,
			(uint32_t)mem->size,
			&export_id,
			export_flags);

		if (ret) {
			pr_err("framebuffer habmm export failed\n");
			dma_buf_put(dma_buf);
			goto error;
		}

		pr_err("framebuffer fack resource_attach_backing returned \n");

		mem->shmem_id = export_id;
#ifdef VIRTIO_DEBUG
		pr_err("framebuffer drm_gem_prime_export habmm_export done %d\n", mem->shmem_id);
#endif
		dma_buf_put(dma_buf);
//	}


	virtio_gpu_resource_id_get(&fb_priv->hw_res_handle);
	//fb hight and width are filled in drm_helper_mode_fill_fb_struct
	//TODO : get the fence
	ret = virtio_gpu_cmd_resource_create_2D(fb_priv->kms,
			fb_priv->hw_res_handle,
			VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM,//fb_priv->format,
			fb->base.width,
			fb->base.height,
			fence);
	if (ret) {
		pr_err("resource_create_2D failed\n");
		goto error;
	}
	//TODO : number of fb modifier
	if (fb_priv->secure)
		modifiers |= SECURE_SOURCE;

	ret = virtio_gpu_cmd_set_resource_info(fb_priv->kms,
			fb_priv->hw_res_handle,
			modifiers,
			fb->base.offsets,
			fb->base.pitches,
			fb_priv->format);
	if (ret) {
		pr_err("set_resource_info failed\n");
		goto error;
	}

	ret = virtio_gpu_cmd_resource_attach_backing(fb_priv->kms,
			fb_priv->hw_res_handle,
			mem->shmem_id,
			mem->size);
	if (ret) {
		pr_err("resource_attach_backing failed\n");
	}
#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_create_framebuffer done\n");
#endif

	fb_priv->created = true;
error:
	return ret;
}

static void virtio_kms_destroy_framebuffer(struct drm_framebuffer *framebuffer)
{
	struct msm_hyp_framebuffer *fb;
	struct virtio_framebuffer_priv *fb_priv;
	int32_t rc = 0;
	uint32_t client_id;
	struct virtio_mem_info *mem;
	int32_t handle;
	uint32_t unexport_flags = 0;
	struct dma_buf *dma_buf;
//	char *ptr;
//	int i;
//	struct dma_buf_map map;

#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_destroy_framebuffer called\n");
#endif
	fb = to_msm_hyp_fb(framebuffer);
	fb_priv = container_of(fb->info, struct virtio_framebuffer_priv, base);
	client_id = fb_priv->kms->client_id;
	mem = &fb_priv->mem;
	handle = fb_priv->kms->channel[client_id].hab_socket[CHANNEL_BUFFERS];

#ifdef VIRTIO_DEBUG
	pr_err("framebuffer create: FB ID: %d (%pK)", fb->base.base.id, fb);
#endif
	virtio_gpu_cmd_resource_detach_backing(fb_priv->kms,
			fb_priv->hw_res_handle);

	unexport_flags |= HABMM_EXPIMP_FLAGS_DMABUF;//HABMM_EXPIMP_FLAGS_FD;
	pr_err("framebuffer habmm_unexport %d\n", mem->shmem_id);
	rc = habmm_unexport(
			handle,
			mem->shmem_id,
			unexport_flags);
	if (rc) {
		pr_err("framebuffer habmm_unexport failed");
	}

	virtio_gpu_cmd_resource_unref(fb_priv->kms,
			fb_priv->hw_res_handle);

        dma_buf = (struct dma_buf *) mem->buffer;

	//dump the incoming data
	/*
	dma_buf_begin_cpu_access(dma_buf, DMA_BIDIRECTIONAL);
	rc =  dma_buf_vmap(dma_buf, &map);
	if (rc)
		pr_err(" mmap failed for dma_buf_vmap\n");
	else {
		ptr = (char *)map.vaddr;
		if (!ptr)
			pr_err(" no memry map for da buffer\n");
		for (i = 0; i < 50; ) {
			pr_err("framebuffer unexport data %x %x %x %x %x\n",
				ptr[i], ptr[i+1], ptr[i+2], ptr[i+3], ptr[i+4]);
			i = i + 5;
		}
	}

	pr_err("framebuffer dma_buf_vmap done %p\n", map.vaddr);
	dma_buf_vunmap(dma_buf, &map);
	dma_buf_end_cpu_access(dma_buf, DMA_BIDIRECTIONAL);
	*/

#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_destroy_framebuffer donei %d\n", fb_priv->hw_res_handle);
#endif
	kfree(fb_priv);
	fb->info = NULL;
}

static int virtio_kms_get_framebuffer_info(struct msm_hyp_kms *hyp_kms,
		struct drm_framebuffer *framebuffer,
		struct msm_hyp_framebuffer_info **fb_info)
{
	struct virtio_framebuffer_priv *fb_priv;
	uint32_t format;
	struct virtio_kms *kms = to_virtio_kms(hyp_kms);

	format = virtio_gpu_translate_format(framebuffer->format->format);
	if (format == 0) {
		return -EINVAL;
	}

	fb_priv = kzalloc(sizeof(*fb_priv), GFP_KERNEL);
	if (!fb_priv)
		return -ENOMEM;

	fb_priv->base.destroy = virtio_kms_destroy_framebuffer;
	fb_priv->format = format;
	fb_priv->mem.shmem_id = 0;
	fb_priv->kms = kms;
	*fb_info = &fb_priv->base;
	return 0;
}

static void virtio_kms_commit(struct msm_hyp_kms *kms,
		struct drm_atomic_state *old_state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	struct msm_hyp_crtc *c;
	struct virtio_crtc_info_priv *priv;
	int i;
	bool async = true;

	if (!old_state)
		return;
#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_commit called\n");
#endif
	for_each_new_crtc_in_state(old_state, crtc, crtc_state, i) {
		c = to_msm_hyp_crtc(crtc);
		priv = container_of(c->info,
				struct virtio_crtc_info_priv,
				base);

		if (crtc_state->active) {
			pr_err("virtio_kms_plane_zpos_adj_fe called \n");
			virtio_kms_plane_zpos_adj_fe(crtc, old_state);
		}

		priv->kms->outputs[priv->scanout].crtc = crtc;
		virtio_gpu_cmd_event_control(priv->kms,
				priv->scanout,
				VIRTIO_COMMIT_COMPLETE,
				true);

		virtio_gpu_cmd_scanout_flush(priv->kms,
				priv->scanout,
				async);
	}
#ifdef VIRTIO_DEBUG
	pr_err("virtio_kms_commit done\n");
#endif
}

static void virtio_kms_enable_vblank(struct msm_hyp_kms *hyp_kms,
		struct drm_crtc *crtc)
{
	struct msm_hyp_crtc *c;
	struct virtio_crtc_info_priv *priv;
	struct virtio_kms *kms;

	c = to_msm_hyp_crtc(crtc);
	priv = container_of(c->info, struct virtio_crtc_info_priv, base);
        kms = to_virtio_kms(hyp_kms);

	kms->outputs[priv->scanout].vblank_enabled = true;
	virtio_gpu_cmd_event_control(priv->kms,
			priv->scanout,
			VIRTIO_VSYNC,
			true);
}

static void virtio_kms_disable_vblank(struct msm_hyp_kms *hyp_kms,
		struct drm_crtc *crtc)
{
	struct msm_hyp_crtc *c;
	struct virtio_crtc_info_priv *priv;
	struct virtio_kms *kms;

	c = to_msm_hyp_crtc(crtc);
	priv = container_of(c->info, struct virtio_crtc_info_priv, base);
	kms = to_virtio_kms(hyp_kms);
	kms->outputs[priv->scanout].vblank_enabled = false;
	virtio_gpu_cmd_event_control(priv->kms,
			priv->scanout,
			VIRTIO_VSYNC,
			false);
}

static const struct msm_hyp_kms_funcs virtio_kms_funcs = {
	.get_connector_infos = virtio_kms_get_connector_infos,
	.get_plane_infos = virtio_kms_get_plane_infos,
	.get_crtc_infos = virtio_kms_get_crtc_infos,
	.get_mode_info = virtio_kms_get_mode_info,
	.get_framebuffer_info = virtio_kms_get_framebuffer_info,
	.commit = virtio_kms_commit,
	.enable_vblank = virtio_kms_enable_vblank,
	.disable_vblank = virtio_kms_disable_vblank,
};

/*
static void virtio_kms_get_capsets(struct virtio_kms *kms,
		int num_capsets)
{
	int i, ret;

	kms->capsets = kcalloc(num_capsets,
			 sizeof(struct virtio_gpu_drv_capset),
				 GFP_KERNEL);
       if (!kms->capsets) {
		DRM_ERROR("failed to allocate cap sets\n");
		return;
	}
       for (i = 0; i < num_capsets; i++) {
		virtio_cmd_get_capset_info(kms, i);
		ret = wait_event_timeout(kms->resp_wq,
				 kms->capsets[i].id > 0, 5 * HZ);
		if (ret == 0) {
			pr_err("timed out waiting for cap set %d\n", i);
			spin_lock(&kms->display_info_lock);
			kfree(kms->capsets);
			kms->capsets = NULL;
			spin_unlock(&kms->display_info_lock);
			return;
		}
		pr_debug("cap set %d: id %d, max-version %d, max-size %d\n",
			i, kms->capsets[i].id,
			kms->capsets[i].max_version,
			kms->capsets[i].max_size);
 	}
	kms->num_capsets = num_capsets;
}
*/
static int _virtio_kms_hw_deinit(struct virtio_kms *kms)
{
	uint32_t scanout, plane;
	uint32_t plane_id = 0;
	int rc = 0;
	uint32_t num_planes = 0;
	struct virtio_kms_output *output;

	for (scanout = 0; scanout < kms->num_scanouts; scanout++) {
		num_planes = kms->outputs[scanout].plane_cnt;
		output = &kms->outputs[scanout];
		for (plane = 0; plane < num_planes; plane++) {
			plane_id = output->plane_caps[plane].plane_id;
			rc = virtio_gpu_cmd_plane_destroy(kms,
					scanout,
					plane_id);
			if (rc) {
				pr_err("plane destroy failed %d\n", plane_id);
			}
		}
	}
	return rc;
}

static int _virtio_kms_hw_init(struct virtio_kms *kms)
{
	int rc = 0;
	uint32_t scanout;

//	if (virtio_has_feature(kms->vdev, VIRTIO_GPU_F_EDID)) {
//		kms->has_edid = true;
//		DRM_INFO("EDID support available.\n");
//	}
//	virtio_has_feature(kms->vdev, VIRTIO_GPU_F_VENDOR);

	init_waitqueue_head(&kms->resp_wq);
	spin_lock_init(&kms->display_info_lock);

	//virtio_kms_get_capsets(kms, kms->num_capsets);

	rc = virtio_gpu_cmd_get_display_info(kms);
	if (rc) {
		pr_err("get_display_info failed\n");
		goto error;
	}

	for (scanout = 0; scanout < kms->num_scanouts; scanout++) {
		rc = virtio_kms_scanout_init(kms, scanout);
		if (rc) {
			 pr_err("scanout init failed %d\n", scanout);
		}
	}
error:
	return rc;
}

static int virtio_kms_scanout_init(struct virtio_kms *kms, uint32_t scanout)
{
	int rc = 0;
	uint32_t num_planes = 0;
	uint32_t plane;
	uint32_t plane_id = 0;

	if (scanout >= VIRTIO_GPU_MAX_SCANOUTS) {
		pr_err(" Wrong Scanout ID\n");
		goto error;
	}

	if (kms->has_edid)
		virtio_gpu_cmd_get_edid(kms, scanout);

	rc = virtio_gpu_cmd_get_display_info_ext(kms, scanout);
	if (rc) {
		pr_err("get_display_info_ext failed %d\n",
				scanout);
		goto error;
	}

	rc = virtio_gpu_cmd_get_scanout_attributes(kms, scanout);
	if (rc) {
		goto error;
	}

	rc = virtio_gpu_cmd_get_scanout_planes(kms, scanout);
	if (rc) {
		goto error;
	}

	num_planes = kms->outputs[scanout].plane_cnt;

	if (!num_planes)
		pr_err("No planes passed\n");

	for (plane = 0; plane < num_planes; plane++) {
		plane_id = kms->outputs[scanout].plane_caps[plane].plane_id;
		rc = virtio_gpu_cmd_plane_create(kms,
				scanout,
				plane_id);
		if (rc) {
			pr_err("Plane creation failed plane-id %d\n",
					plane_id);
			continue;
		}
		rc = virtio_gpu_cmd_get_plane_caps(kms,
				scanout,
				plane_id);
		if (rc) {
			pr_err("virtio_gpu_cmd_get_plane_caps failed\n");
			goto error;
		}

		rc = virtio_gpu_cmd_get_plane_properties(kms,
				scanout,
				plane_id);
		if (rc) {
			pr_err("virtio_gpu_cmd_get_plane_properties failed \n");
			goto error;
		}
	}
error:
	return rc;
}

#if 0
static int _virtio_kms_parse_client_id(struct device_node *node,
		uint32_t *client_id)
{
	int len = 0;
	int ret = 0;
	const char *client_id_str;

	client_id_str = of_get_property(node, "qcom,client-id", &len);
	if (!client_id_str || len != CLIENT_ID_LEN_IN_CHARS) {
		pr_err("client_id_str len(%d) is invalid\n", len);
		ret = -EINVAL;
	} else {
		/* Try node as a hex value */
		ret = kstrtouint(client_id_str, 16, client_id);
		if (ret) {
			/* Otherwise, treat at 4cc code */
			*client_id = fourcc_code(client_id_str[0],
					client_id_str[1],
					client_id_str[2],
					client_id_str[3]);

			ret = 0;
		}
	}

	return ret;
}

#endif


static int virtio_gpu_hab_open(struct virtio_kms *kms)
{
	int ret = 0;
	uint32_t client_id = kms->client_id;
	if (!kms)
		pr_err("kms NULL\n");
#ifdef VIRTIO_DEBUG
	pr_err("virtio: hab open mmid %d\n",kms->mmid_cmd);
#endif
	ret = habmm_socket_open(
			&kms->channel[client_id].hab_socket[CHANNEL_CMD],
			kms->mmid_cmd,
			-1,
			0);
	if (!ret) {
		pr_info("virtio: hab socket open mmid %d OK\n", kms->mmid_cmd);

	} else {
		pr_err("hab open failed mmid %d ret %d\n", kms->mmid_cmd, ret);
		goto exit;
	}
	mutex_init(&kms->channel[client_id].hab_lock[CHANNEL_CMD]);

#ifdef VIRTIO_DEBUG
	pr_err("virtio: hab open mmid %d\n",kms->mmid_event);
#endif
	ret = habmm_socket_open(
			&kms->channel[client_id].hab_socket[CHANNEL_EVENTS],
			kms->mmid_event,
			-1,
			0);
	if (!ret) {
		pr_info("virtio: hab socket open mmid %d OK\n", kms->mmid_event);
	} else {
		pr_err("hab open failed mmid %d ret %d\n", kms->mmid_event, ret);
	}

	mutex_init(&kms->channel[client_id].hab_lock[CHANNEL_EVENTS]);

#ifdef VIRTIO_DEBUG
	pr_err("virtio: hab open mmid %d\n",kms->mmid_buffer);
#endif
	ret = habmm_socket_open(
			&kms->channel[client_id].hab_socket[CHANNEL_BUFFERS],
			kms->mmid_buffer,
			-1,
			0);
	if (!ret) {
		pr_info("virtio: hab socket open mmid %d OK\n", kms->mmid_buffer);

	} else {
		pr_err("hab open failed mmid %d ret %d\n",
				kms->mmid_buffer,
				ret);
		ret = habmm_socket_close(
			kms->channel[client_id].hab_socket[CHANNEL_CMD]);
		if (ret)
			pr_err("hab closed failed mmid %d ret %d\n",
					kms->mmid_buffer, ret);

	}
	mutex_init(&kms->channel[client_id].hab_lock[CHANNEL_BUFFERS]);
exit:
	return ret;
}

static int virtio_kms_service_hpd(struct virtio_kms *kms, uint32_t scanout)
{
	int rc = 0;
	rc = virtio_kms_scanout_init(kms, scanout);
	if (rc) {
		 pr_err("scanout init failed %d\n", scanout);
	}

	return 0;
}

static void virtio_kms_vsync(struct virtio_kms *kms, uint32_t scanout)
{
	struct drm_crtc *crtc = kms->outputs[scanout].crtc;
	msm_hyp_crtc_vblank_done(crtc);

	if (kms->outputs[scanout].vblank_enabled) {
		virtio_gpu_cmd_event_control(kms,
				scanout,
				VIRTIO_VSYNC,
				true);
	}
}

static void virtio_kms_service_commit_done(
		struct virtio_kms *kms,
		uint32_t scanout)
{
	struct drm_crtc *crtc = kms->outputs[scanout].crtc;
	virtio_gpu_cmd_event_control(kms,
				scanout,
				VIRTIO_COMMIT_COMPLETE,
				false);

	msm_hyp_crtc_commit_done(crtc);
}

void  virtio_kms_event_handler(struct virtio_kms *kms,
		uint32_t scanout,
		uint32_t num_event,
		uint32_t event_type)
{
	switch (event_type) {

	case VIRTIO_VSYNC:
		virtio_kms_vsync(kms, scanout);
	break;

	case VIRTIO_COMMIT_COMPLETE:
		virtio_kms_service_commit_done(kms, scanout);
	break;

	case VIRTIO_HPD:
		virtio_kms_service_hpd(kms, scanout);
	break;

	default:
		pr_err("Undefine event received %d\n",event_type);
	}
}

static int virtio_kms_bind(struct device *dev,
		struct device *master,
                void *data)
{
        struct virtio_kms *kms = dev_get_drvdata(dev);
        struct drm_device *drm_dev = dev_get_drvdata(master);
	if (!kms) {
		pr_err("virtio_kms_bind failed ");
		return 0;
	}
        kms->dev = drm_dev;
        msm_hyp_set_kms(drm_dev, &kms->base);

        return 0;
}

static void virtio_kms_unbind(struct device *dev,
		struct device *master,
                void *data)
{
        struct virtio_kms *kms = dev_get_drvdata(dev);

        msm_hyp_set_kms(kms->dev, NULL);
}

static const struct component_ops virtio_kms_comp_ops = {
        .bind = virtio_kms_bind,
        .unbind = virtio_kms_unbind,
};

static int virtio_kms_probe(struct platform_device *pdev)
{
        struct device *dev = &pdev->dev;
        struct virtio_kms *kms;
        int ret;
//        char marker_buff[MARKER_BUFF_LENGTH] = {0};


        kms = devm_kzalloc(dev, sizeof(*kms), GFP_KERNEL);
        if (!kms)
                return -ENOMEM;

//        ret = _virtio_kms_parse_client_id(dev->of_node, &kms->client_id);
//        if (ret)
//                return ret;

	kms->client_id = 0;

	kms->mmid_cmd = MM_DISP_1;
	kms->mmid_event = MM_DISP_3;
	kms->mmid_buffer = MM_DISP_2;

//	ret = _virtio_kms_parse_capsets(dev->of_node, &kms->num_capsets);
//	if (ret)
//		return ret;

	ret = virtio_gpu_hab_open(kms);
	if (ret)
		return ret;

	kms->stop = false;
	kthread_run(virtio_gpu_event_kthread, kms, "virtio gpu kthread");

        ret = _virtio_kms_hw_init(kms);
        if (ret)
                return ret;

	pr_debug("numbr of scanouts %d for client %x\n", kms->num_scanouts, kms->client_id);
        kms->base.funcs = &virtio_kms_funcs;

        platform_set_drvdata(pdev, kms);

        ret = component_add(&pdev->dev, &virtio_kms_comp_ops);
        if (ret) {
		pr_err("component add failed, rc=%d\n", ret);
		return ret;
	}
  //       snprintf(marker_buff, sizeof(marker_buff),
  //              "kernel_fe: virtio_kms probe client %x", kms->client_id);
//        place_marker(marker_buff);

        return 0;
}

static int virtio_kms_remove(struct platform_device *pdev)
{
	//TODO: implement remove
	int ret;
	struct virtio_kms *kms = platform_get_drvdata(pdev);

	ret = _virtio_kms_hw_deinit(kms);
	if (ret) {
		pr_err("deinit failed \n");
	}
	return 0;
}

static const struct platform_device_id virtio_kms_id[] = {
        { "virtio-kms", 0 },
        { }
};

static const struct of_device_id dt_match[] = {
        { .compatible = "qcom,virtio-kms" },
        {}
};
static struct platform_driver virtio_kms_driver = {
        .probe      = virtio_kms_probe,
        .remove     = virtio_kms_remove,
        .driver     = {
                .name   = "virtio_kms",
                .of_match_table = dt_match,
        },
        .id_table   = virtio_kms_id,
};

void virtio_kms_register(void)
{
        platform_driver_register(&virtio_kms_driver);
}

void virtio_kms_unregister(void)
{
        platform_driver_unregister(&virtio_kms_driver);
}
