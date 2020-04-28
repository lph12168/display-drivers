// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021-2022, Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_crtc.h>

#include "msm_drv.h"
#include "msm_kms.h"
#include "sde_connector.h"
#include "dp_drm.h"
#include "dp_mst_drm.h"
#include "dp_debug.h"

#define DP_MST_DEBUG(fmt, ...) DP_DEBUG(fmt, ##__VA_ARGS__)

struct dp_bond_bridge {
	struct drm_bridge base;
	struct drm_encoder *encoder;
	struct dp_display *display;
	struct dp_bridge *bridges[MAX_DP_BOND_NUM];
	u32 bridge_num;
	enum dp_bond_type type;
	u32 bond_mask;
};

struct dp_bond_mgr {
	struct drm_private_obj obj;
	struct dp_bond_bridge bond_bridge[DP_BOND_MAX];
};

struct dp_bond_mgr_state {
	struct drm_private_state base;
	struct drm_connector *connector[DP_BOND_MAX];
	u32 bond_mask[DP_BOND_MAX];
	u32 connector_mask;
};

struct dp_bond_info {
	struct dp_bond_mgr *bond_mgr;
	struct dp_bond_bridge *bond_bridge[DP_BOND_MAX];
	u32 bond_idx;
};

#define to_dp_bridge(x)     container_of((x), struct dp_bridge, base)

#define to_dp_bond_bridge(x)     container_of((x), struct dp_bond_bridge, base)

#define to_dp_bond_mgr(x)     container_of((x), struct dp_bond_mgr, base)

#define to_dp_bond_mgr_state(x) \
		container_of((x), struct dp_bond_mgr_state, base)

static struct drm_private_state *dp_bond_duplicate_mgr_state(
		struct drm_private_obj *obj)
{
	struct dp_bond_mgr_state *state;

	state = kmemdup(obj->state, sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_private_obj_duplicate_state(obj, &state->base);

	return &state->base;
}

static void dp_bond_destroy_mgr_state(struct drm_private_obj *obj,
		struct drm_private_state *state)
{
	struct dp_bond_mgr_state *bond_state =
			to_dp_bond_mgr_state(state);

	kfree(bond_state);
}

static const struct drm_private_state_funcs dp_bond_mgr_state_funcs = {
	.atomic_duplicate_state = dp_bond_duplicate_mgr_state,
	.atomic_destroy_state = dp_bond_destroy_mgr_state,
};

static struct dp_bond_mgr_state *dp_bond_get_mgr_atomic_state(
		struct drm_atomic_state *state, struct dp_bond_mgr *mgr)
{
	return to_dp_bond_mgr_state(
		drm_atomic_get_private_obj_state(state, &mgr->obj));
}

void convert_to_drm_mode(const struct dp_display_mode *dp_mode,
				struct drm_display_mode *drm_mode)
{
	u32 flags = 0;

	memset(drm_mode, 0, sizeof(*drm_mode));

	drm_mode->hdisplay = dp_mode->timing.h_active;
	drm_mode->hsync_start = drm_mode->hdisplay +
				dp_mode->timing.h_front_porch;
	drm_mode->hsync_end = drm_mode->hsync_start +
			      dp_mode->timing.h_sync_width;
	drm_mode->htotal = drm_mode->hsync_end + dp_mode->timing.h_back_porch;
	drm_mode->hskew = dp_mode->timing.h_skew;

	drm_mode->vdisplay = dp_mode->timing.v_active;
	drm_mode->vsync_start = drm_mode->vdisplay +
				dp_mode->timing.v_front_porch;
	drm_mode->vsync_end = drm_mode->vsync_start +
			      dp_mode->timing.v_sync_width;
	drm_mode->vtotal = drm_mode->vsync_end + dp_mode->timing.v_back_porch;

	drm_mode->clock = dp_mode->timing.pixel_clk_khz;

	if (dp_mode->timing.h_active_low)
		flags |= DRM_MODE_FLAG_NHSYNC;
	else
		flags |= DRM_MODE_FLAG_PHSYNC;

	if (dp_mode->timing.v_active_low)
		flags |= DRM_MODE_FLAG_NVSYNC;
	else
		flags |= DRM_MODE_FLAG_PVSYNC;

	drm_mode->flags = flags;

	drm_mode->type = 0x48;
	drm_mode_set_name(drm_mode);
}

static int dp_bridge_attach(struct drm_bridge *dp_bridge,
				enum drm_bridge_attach_flags flags)
{
	struct dp_bridge *bridge = to_dp_bridge(dp_bridge);

	if (!dp_bridge) {
		DP_ERR("Invalid params\n");
		return -EINVAL;
	}

	DP_DEBUG("[%d] attached\n", bridge->id);

	return 0;
}

static void dp_bridge_pre_enable(struct drm_bridge *drm_bridge)
{
	int rc = 0;
	struct dp_bridge *bridge;
	struct dp_display *dp;

	if (!drm_bridge) {
		DP_ERR("Invalid params\n");
		return;
	}

	bridge = to_dp_bridge(drm_bridge);
	dp = bridge->display;

	if (!bridge->connector) {
		DP_ERR("Invalid connector\n");
		return;
	}

	if (!bridge->dp_panel) {
		DP_ERR("Invalid dp_panel\n");
		return;
	}

	/*
	 * Non-bond mode, associated with the CRTC,
	 * set non-bond mode to the display
	 */
	if (bridge->base.encoder->crtc != NULL)
		dp->set_phy_bond_mode(dp, DP_PHY_BOND_MODE_NONE);

	/* By this point mode should have been validated through mode_fixup */
	rc = dp->set_mode(dp, bridge->dp_panel, &bridge->dp_mode);
	if (rc) {
		DP_ERR("[%d] failed to perform a mode set, rc=%d\n",
		       bridge->id, rc);
		return;
	}

	rc = dp->prepare(dp, bridge->dp_panel);
	if (rc) {
		DP_ERR("[%d] DP display prepare failed, rc=%d\n",
		       bridge->id, rc);
		return;
	}

	/* for SST force stream id, start slot and total slots to 0 */
	dp->set_stream_info(dp, bridge->dp_panel, 0, 0, 0, 0, 0);

	rc = dp->enable(dp, bridge->dp_panel);
	if (rc)
		DP_ERR("[%d] DP display enable failed, rc=%d\n",
		       bridge->id, rc);
}

static void dp_bridge_enable(struct drm_bridge *drm_bridge)
{
	int rc = 0;
	struct dp_bridge *bridge;
	struct dp_display *dp;

	if (!drm_bridge) {
		DP_ERR("Invalid params\n");
		return;
	}

	bridge = to_dp_bridge(drm_bridge);
	if (!bridge->connector) {
		DP_ERR("Invalid connector\n");
		return;
	}

	if (!bridge->dp_panel) {
		DP_ERR("Invalid dp_panel\n");
		return;
	}

	dp = bridge->display;

	rc = dp->post_enable(dp, bridge->dp_panel);
	if (rc)
		DP_ERR("[%d] DP display post enable failed, rc=%d\n",
		       bridge->id, rc);
}

static void dp_bridge_disable(struct drm_bridge *drm_bridge)
{
	int rc = 0;
	struct dp_bridge *bridge;
	struct dp_display *dp;

	if (!drm_bridge) {
		DP_ERR("Invalid params\n");
		return;
	}

	bridge = to_dp_bridge(drm_bridge);
	if (!bridge->connector) {
		DP_ERR("Invalid connector\n");
		return;
	}

	if (!bridge->dp_panel) {
		DP_ERR("Invalid dp_panel\n");
		return;
	}

	dp = bridge->display;

	if (!dp) {
		DP_ERR("dp is null\n");
		return;
	}

	if (dp)
		sde_connector_helper_bridge_disable(bridge->connector);

	rc = dp->pre_disable(dp, bridge->dp_panel);
	if (rc) {
		DP_ERR("[%d] DP display pre disable failed, rc=%d\n",
		       bridge->id, rc);
	}
}

static void dp_bridge_post_disable(struct drm_bridge *drm_bridge)
{
	int rc = 0;
	struct dp_bridge *bridge;
	struct dp_display *dp;

	if (!drm_bridge) {
		DP_ERR("Invalid params\n");
		return;
	}

	bridge = to_dp_bridge(drm_bridge);
	if (!bridge->connector) {
		DP_ERR("Invalid connector\n");
		return;
	}

	if (!bridge->dp_panel) {
		DP_ERR("Invalid dp_panel\n");
		return;
	}

	dp = bridge->display;

	rc = dp->disable(dp, bridge->dp_panel);
	if (rc) {
		DP_ERR("[%d] DP display disable failed, rc=%d\n",
		       bridge->id, rc);
		return;
	}

	rc = dp->unprepare(dp, bridge->dp_panel);
	if (rc) {
		DP_ERR("[%d] DP display unprepare failed, rc=%d\n",
		       bridge->id, rc);
		return;
	}
}

static void dp_bridge_mode_set(struct drm_bridge *drm_bridge,
				const struct drm_display_mode *mode,
				const struct drm_display_mode *adjusted_mode)
{
	struct dp_bridge *bridge;
	struct dp_display *dp;

	if (!drm_bridge || !mode || !adjusted_mode) {
		DP_ERR("Invalid params\n");
		return;
	}

	bridge = to_dp_bridge(drm_bridge);
	if (!bridge->connector) {
		DP_ERR("Invalid connector\n");
		return;
	}

	if (!bridge->dp_panel) {
		DP_ERR("Invalid dp_panel\n");
		return;
	}

	dp = bridge->display;

	dp->convert_to_dp_mode(dp, bridge->dp_panel, adjusted_mode,
			&bridge->dp_mode);
}

static bool dp_bridge_mode_fixup(struct drm_bridge *drm_bridge,
				  const struct drm_display_mode *mode,
				  struct drm_display_mode *adjusted_mode)
{
	bool ret = true;
	struct dp_display_mode dp_mode;
	struct dp_bridge *bridge;
	struct dp_display *dp;

	if (!drm_bridge || !mode || !adjusted_mode) {
		DP_ERR("Invalid params\n");
		ret = false;
		goto end;
	}

	bridge = to_dp_bridge(drm_bridge);
	if (!bridge->connector) {
		DP_ERR("Invalid connector\n");
		ret = false;
		goto end;
	}

	if (!bridge->dp_panel) {
		DP_ERR("Invalid dp_panel\n");
		ret = false;
		goto end;
	}

	dp = bridge->display;

	dp->convert_to_dp_mode(dp, bridge->dp_panel, mode, &dp_mode);
	convert_to_drm_mode(&dp_mode, adjusted_mode);
end:
	return ret;
}

static const struct drm_bridge_funcs dp_bridge_ops = {
	.attach       = dp_bridge_attach,
	.mode_fixup   = dp_bridge_mode_fixup,
	.pre_enable   = dp_bridge_pre_enable,
	.enable       = dp_bridge_enable,
	.disable      = dp_bridge_disable,
	.post_disable = dp_bridge_post_disable,
	.mode_set     = dp_bridge_mode_set,
};

static inline bool dp_bond_is_tile_mode(const struct drm_display_mode *mode)
{
	return !!(mode->flags & DRM_MODE_FLAG_CLKDIV2);
}

static inline void
dp_bond_split_tile_timing(struct drm_display_mode *mode, int num_h_tile)
{
	mode->hdisplay /= num_h_tile;
	mode->hsync_start /= num_h_tile;
	mode->hsync_end /= num_h_tile;
	mode->htotal /= num_h_tile;
	mode->hskew /= num_h_tile;
	mode->clock /= num_h_tile;
	mode->flags &= ~DRM_MODE_FLAG_CLKDIV2;
}

static inline void
dp_bond_merge_tile_timing(struct drm_display_mode *mode, int num_h_tile)
{
	mode->hdisplay *= num_h_tile;
	mode->hsync_start *= num_h_tile;
	mode->hsync_end *= num_h_tile;
	mode->htotal *= num_h_tile;
	mode->hskew *= num_h_tile;
	mode->clock *= num_h_tile;
	mode->flags |= DRM_MODE_FLAG_CLKDIV2;
}

static bool dp_bond_bridge_mode_fixup(struct drm_bridge *drm_bridge,
				  const struct drm_display_mode *mode,
				  struct drm_display_mode *adjusted_mode)
{
	struct dp_bond_bridge *bridge;
	struct drm_display_mode tmp;
	struct dp_display_mode dp_mode;
	struct dp_display *dp;
	bool ret = true;

	if (!drm_bridge || !mode || !adjusted_mode) {
		pr_err("Invalid params\n");
		ret = false;
		goto end;
	}

	bridge = to_dp_bond_bridge(drm_bridge);

	dp = bridge->display;

	if (!dp->bridge->dp_panel) {
		pr_err("Invalid dp_panel\n");
		ret = false;
		goto end;
	}

	tmp = *mode;
	dp_bond_split_tile_timing(&tmp, dp->base_connector->num_h_tile);
	dp->convert_to_dp_mode(dp, dp->bridge->dp_panel, &tmp, &dp_mode);
	convert_to_drm_mode(&dp_mode, adjusted_mode);
	dp_bond_merge_tile_timing(adjusted_mode,
			dp->base_connector->num_h_tile);
end:
	return ret;
}

static void dp_bond_bridge_pre_enable(struct drm_bridge *drm_bridge)
{
	struct dp_bond_bridge *bridge;
	int i;

	if (!drm_bridge) {
		pr_err("Invalid params\n");
		return;
	}

	bridge = to_dp_bond_bridge(drm_bridge);

	/* Set the corresponding bond mode to bonded displays */
	for (i = 0; i < bridge->bridge_num; i++) {
		enum dp_phy_bond_mode mode;

		if (i == 0) {
			switch (bridge->type)
			{
			case DP_BOND_DUAL_PHY:
			case DP_BOND_TRIPLE_PHY:
				mode = DP_PHY_BOND_MODE_PLL_MASTER;
				break;
			case DP_BOND_DUAL_PCLK:
			case DP_BOND_TRIPLE_PCLK:
				mode = DP_PHY_BOND_MODE_PCLK_MASTER;
				break;
			default:
				mode = DP_PHY_BOND_MODE_NONE;
				break;
			}
		} else {
			switch (bridge->type)
			{
			case DP_BOND_DUAL_PHY:
			case DP_BOND_TRIPLE_PHY:
				mode = DP_PHY_BOND_MODE_PLL_SLAVE;
				break;
			case DP_BOND_DUAL_PCLK:
			case DP_BOND_TRIPLE_PCLK:
				mode = DP_PHY_BOND_MODE_PCLK_SLAVE;
				break;
			default:
				mode = DP_PHY_BOND_MODE_NONE;
				break;
			}
		}
		if (bridge->bridges[i]->display)
			bridge->bridges[i]->display->set_phy_bond_mode(
					bridge->bridges[i]->display, mode);
	}

	/* In the order of from master PHY to slave PHY */
	for (i = 0; i < bridge->bridge_num; i++)
		drm_bridge_chain_pre_enable(&bridge->bridges[i]->base);
}

static void dp_bond_bridge_enable(struct drm_bridge *drm_bridge)
{
	struct dp_bond_bridge *bridge;
	int i;

	if (!drm_bridge) {
		pr_err("Invalid params\n");
		return;
	}

	bridge = to_dp_bond_bridge(drm_bridge);

	/* In the order of from master PHY to slave PHY */
	for (i = 0; i < bridge->bridge_num; i++)
		drm_bridge_chain_enable(&bridge->bridges[i]->base);
}

static void dp_bond_bridge_disable(struct drm_bridge *drm_bridge)
{
	struct dp_bond_bridge *bridge;
	int i;

	if (!drm_bridge) {
		pr_err("Invalid params\n");
		return;
	}

	bridge = to_dp_bond_bridge(drm_bridge);

	/* In the order of from slave PHY to master PHY */
	for (i = bridge->bridge_num - 1; i >= 0; i--)
		drm_bridge_chain_disable(&bridge->bridges[i]->base);
}

static void dp_bond_bridge_post_disable(struct drm_bridge *drm_bridge)
{
	struct dp_bond_bridge *bridge;
	int i;

	if (!drm_bridge) {
		pr_err("Invalid params\n");
		return;
	}

	bridge = to_dp_bond_bridge(drm_bridge);

	/* In the order of from slave PHY to master PHY */
	for (i = bridge->bridge_num - 1; i >= 0; i--)
		drm_bridge_chain_post_disable(&bridge->bridges[i]->base);
}

static void dp_bond_bridge_mode_set(struct drm_bridge *drm_bridge,
				const struct drm_display_mode *mode,
				const struct drm_display_mode *adjusted_mode)
{
	struct dp_bond_bridge *bridge;
	struct drm_display_mode tmp;
	int i;

	if (!drm_bridge) {
		pr_err("Invalid params\n");
		return;
	}

	bridge = to_dp_bond_bridge(drm_bridge);

	tmp = *adjusted_mode;
	dp_bond_split_tile_timing(&tmp, bridge->bridge_num);

	for (i = 0; i < bridge->bridge_num; i++)
		drm_bridge_chain_mode_set(&bridge->bridges[i]->base, &tmp, &tmp);
}

static const struct drm_bridge_funcs dp_bond_bridge_ops = {
	.mode_fixup   = dp_bond_bridge_mode_fixup,
	.pre_enable   = dp_bond_bridge_pre_enable,
	.enable       = dp_bond_bridge_enable,
	.disable      = dp_bond_bridge_disable,
	.post_disable = dp_bond_bridge_post_disable,
	.mode_set     = dp_bond_bridge_mode_set,
};

static inline
enum dp_bond_type dp_bond_get_bond_type(struct drm_connector *connector)
{
	struct sde_connector *c_conn = to_sde_connector(connector);
	struct dp_display *dp_display = c_conn->display;
	struct dp_bond_info *bond_info = dp_display->dp_bond_prv_info;
	enum dp_bond_type type;

	if (!dp_display->dp_bond_prv_info || !connector->has_tile)
		return DP_BOND_MAX;

	for (type = 0 ; type < DP_BOND_MAX; type++)
		if ((num_bond_dp[type] == connector->num_h_tile)
			&& bond_info->bond_bridge[type]
			&& bond_info->bond_bridge[type]->bridge_num == num_bond_dp[type])
			break;

	return type;
}

static inline bool dp_bond_is_primary(struct dp_display *dp_display,
		enum dp_bond_type type)
{
	struct dp_bond_info *bond_info = dp_display->dp_bond_prv_info;
	struct dp_bond_bridge *bond_bridge;

	if (!bond_info)
		return false;

	bond_bridge = bond_info->bond_bridge[type];
	if (!bond_bridge)
		return false;

	return bond_bridge->display == dp_display;
}

static void dp_bond_fixup_tile_mode(struct drm_connector *connector)
{
	struct sde_connector *c_conn = to_sde_connector(connector);
	struct dp_display *dp_display = c_conn->display;
	struct drm_display_mode *mode, *newmode;
	struct list_head tile_modes;
	enum dp_bond_type type;
	struct dp_bond_info *bond_info;
	struct dp_bond_bridge *bond_bridge;
	int i;

	/* checks supported tiling mode */
	type = dp_bond_get_bond_type(connector);
	if (type == DP_BOND_MAX)
		return;

	INIT_LIST_HEAD(&tile_modes);

	list_for_each_entry(mode, &connector->probed_modes, head) {
		if (!dp_display->force_bond_mode &&
			(mode->hdisplay != connector->tile_h_size ||
			mode->vdisplay != connector->tile_v_size))
			continue;

		newmode = drm_mode_duplicate(connector->dev, mode);
		if (!newmode)
			break;

		dp_bond_merge_tile_timing(newmode, connector->num_h_tile);
		newmode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_set_name(newmode);

		list_add_tail(&newmode->head, &tile_modes);
	}

	list_for_each_entry_safe(mode, newmode, &tile_modes, head) {
		list_del(&mode->head);
		list_add_tail(&mode->head, &connector->probed_modes);
	}

	/* update display info for sibling connectors */
	bond_info = dp_display->dp_bond_prv_info;
	bond_bridge = bond_info->bond_bridge[type];
	for (i = 0; i < bond_bridge->bridge_num; i++) {
		if (bond_bridge->bridges[i]->connector == connector)
			continue;
		bond_bridge->bridges[i]->connector->display_info =
				connector->display_info;
	}
}

static bool dp_bond_check_connector(struct drm_connector *connector,
		enum dp_bond_type type)
{
	struct sde_connector *c_conn = to_sde_connector(connector);
	struct dp_display *dp_display = c_conn->display, *p;
	struct dp_bond_info *bond_info = dp_display->dp_bond_prv_info;
	struct dp_bond_bridge *bond_bridge;
	struct drm_connector *p_conn;
	int i;

	bond_bridge = bond_info->bond_bridge[type];
	if (!bond_bridge)
		return false;

	for (i = 0; i < bond_bridge->bridge_num; i++) {
		if (bond_bridge->bridges[i]->connector == connector)
			continue;

		p = bond_bridge->bridges[i]->display;
		if (!p->is_sst_connected)
			return false;

		if (dp_display->force_bond_mode) {
			if (p->force_bond_mode)
				continue;
			else
				return false;
		}

		p_conn = p->base_connector;
		if (!p_conn->has_tile || !p_conn->tile_group ||
			p_conn->tile_group->id != connector->tile_group->id)
			return false;
	}

	return true;
}

static void dp_bond_check_force_mode(struct drm_connector *connector)
{
	struct sde_connector *c_conn = to_sde_connector(connector);
	struct dp_display *dp_display = c_conn->display;
	enum dp_bond_type type, preferred_type = DP_BOND_MAX;

	if (!dp_display->dp_bond_prv_info || !dp_display->force_bond_mode)
		return;

	if (connector->has_tile && connector->tile_group)
		return;

	connector->has_tile = false;

	for (type = DP_BOND_DUAL_PHY; type < DP_BOND_MAX; type++) {
		if (!dp_bond_check_connector(connector, type))
			continue;

		preferred_type = type;
	}

	if (preferred_type == DP_BOND_MAX)
		return;

	connector->has_tile = true;
	connector->num_h_tile = num_bond_dp[preferred_type];
	connector->num_v_tile = 1;
}

int dp_connector_config_hdr(struct drm_connector *connector, void *display,
	struct sde_connector_state *c_state)
{
	struct dp_display *dp = display;
	struct sde_connector *sde_conn;

	if (!display || !c_state || !connector) {
		DP_ERR("invalid params\n");
		return -EINVAL;
	}

	sde_conn = to_sde_connector(connector);
	if (!sde_conn->drv_panel) {
		DP_ERR("invalid dp panel\n");
		return -EINVAL;
	}

	return dp->config_hdr(dp, sde_conn->drv_panel, &c_state->hdr_meta,
			c_state->dyn_hdr_meta.dynamic_hdr_update);
}

int dp_connector_set_colorspace(struct drm_connector *connector,
	void *display)
{
	struct dp_display *dp_display = display;
	struct sde_connector *sde_conn;

	if (!dp_display || !connector)
		return -EINVAL;

	sde_conn = to_sde_connector(connector);
	if (!sde_conn->drv_panel) {
		pr_err("invalid dp panel\n");
		return -EINVAL;
	}

	return dp_display->set_colorspace(dp_display,
		sde_conn->drv_panel, connector->state->colorspace);
}

int dp_connector_post_init(struct drm_connector *connector, void *display)
{
	int rc;
	struct dp_display *dp_display = display;
	struct sde_connector *sde_conn;

	if (!dp_display || !connector)
		return -EINVAL;

	dp_display->base_connector = connector;
	dp_display->bridge->connector = connector;

	if (dp_display->post_init) {
		rc = dp_display->post_init(dp_display);
		if (rc)
			goto end;
	}

	sde_conn = to_sde_connector(connector);
	dp_display->bridge->dp_panel = sde_conn->drv_panel;

	rc = dp_mst_init(dp_display);

	if (dp_display->dsc_cont_pps)
		sde_conn->ops.update_pps = NULL;

end:
	return rc;
}

int dp_connector_get_mode_info(struct drm_connector *connector,
		const struct drm_display_mode *drm_mode,
		struct msm_sub_mode *sub_mode,
		struct msm_mode_info *mode_info,
		void *display, const struct msm_resource_caps_info *avail_res)
{
	const u32 single_intf = 1;
	const u32 no_enc = 0;
	struct msm_display_topology *topology;
	struct sde_connector *sde_conn;
	struct dp_panel *dp_panel;
	struct dp_display_mode dp_mode;
	struct dp_display *dp_disp = display;
	struct msm_drm_private *priv;
	struct msm_resource_caps_info avail_dp_res;
	int rc = 0;

	if (!drm_mode || !mode_info || !avail_res ||
			!avail_res->max_mixer_width || !connector || !display ||
			!connector->dev || !connector->dev->dev_private) {
		DP_ERR("invalid params\n");
		return -EINVAL;
	}

	if (dp_bond_is_tile_mode(drm_mode)) {
		struct drm_display_mode tmp;

		tmp = *drm_mode;
		dp_bond_split_tile_timing(&tmp, connector->num_h_tile);

		/* Get single tile mode info */
		rc = dp_connector_get_mode_info(connector, &tmp, sub_mode, mode_info,
				display, avail_res);
		if (rc)
			return rc;

		mode_info->topology.num_intf *= connector->num_h_tile;
		mode_info->topology.num_lm *= connector->num_h_tile;
		mode_info->topology.num_enc *= connector->num_h_tile;
		return 0;
	}

	memset(mode_info, 0, sizeof(*mode_info));

	sde_conn = to_sde_connector(connector);
	dp_panel = sde_conn->drv_panel;
	priv = connector->dev->dev_private;

	topology = &mode_info->topology;

	rc = dp_disp->get_available_dp_resources(dp_disp, avail_res,
			&avail_dp_res);
	if (rc) {
		DP_ERR("error getting max dp resources. rc:%d\n", rc);
		return rc;
	}

	rc = msm_get_mixer_count(priv, drm_mode, &avail_dp_res,
			&topology->num_lm);
	if (rc) {
		DP_ERR("error getting mixer count. rc:%d\n", rc);
		return rc;
	}

	topology->num_enc = no_enc;
	topology->num_intf = single_intf;

	mode_info->frame_rate = drm_mode_vrefresh(drm_mode);
	mode_info->vtotal = drm_mode->vtotal;

	mode_info->wide_bus_en = dp_panel->widebus_en;

	dp_disp->convert_to_dp_mode(dp_disp, dp_panel, drm_mode, &dp_mode);

	if (dp_mode.timing.comp_info.enabled) {
		memcpy(&mode_info->comp_info,
			&dp_mode.timing.comp_info,
			sizeof(mode_info->comp_info));

		topology->num_enc = topology->num_lm;
		topology->comp_type = mode_info->comp_info.comp_type;
	}

	return 0;
}

int dp_connector_get_info(struct drm_connector *connector,
		struct msm_display_info *info, void *data)
{
	struct dp_display *display = data;

	if (!info || !display || !display->drm_dev) {
		DP_ERR("invalid params\n");
		return -EINVAL;
	}

	info->intf_type = DRM_MODE_CONNECTOR_DisplayPort;

	info->num_of_h_tiles = 1;
	info->h_tile_instance[0] = 0;
	info->is_connected = display->is_sst_connected;
	info->curr_panel_mode = MSM_DISPLAY_VIDEO_MODE;
	info->capabilities = MSM_DISPLAY_CAP_VID_MODE | MSM_DISPLAY_CAP_EDID |
		MSM_DISPLAY_CAP_HOT_PLUG;

	return 0;
}

enum drm_connector_status dp_connector_detect(struct drm_connector *conn,
		bool force,
		void *display)
{
	enum drm_connector_status status = connector_status_unknown;
	struct msm_display_info info;
	struct dp_display *dp_display;
	int rc;

	if (!conn || !display)
		return status;

	/* get display dp_info */
	memset(&info, 0x0, sizeof(info));
	rc = dp_connector_get_info(conn, &info, display);
	if (rc) {
		DP_ERR("failed to get display info, rc=%d\n", rc);
		return connector_status_disconnected;
	}

	if (info.capabilities & MSM_DISPLAY_CAP_HOT_PLUG)
		status = (info.is_connected ? connector_status_connected :
					      connector_status_disconnected);
	else
		status = connector_status_connected;

	conn->display_info.width_mm = info.width_mm;
	conn->display_info.height_mm = info.height_mm;

	/*
	 * hide tiled connectors so only primary connector
	 * is reported to user
	 */
	dp_display = display;
	if (dp_display->dp_bond_prv_info &&
			status == connector_status_connected) {
		enum dp_bond_type type;

		dp_bond_check_force_mode(conn);

		type = dp_bond_get_bond_type(conn);
		if (type == DP_BOND_MAX)
			return status;

		if (!dp_bond_is_primary(dp_display, type)) {
			if (dp_bond_check_connector(conn, type))
				status = connector_status_disconnected;
		}

		if (dp_display->force_bond_mode) {
			if (!dp_bond_check_connector(conn, type))
				status = connector_status_disconnected;
		}
	}

	return status;
}

void dp_connector_post_open(struct drm_connector *connector, void *display)
{
	struct dp_display *dp;

	if (!display) {
		DP_ERR("invalid input\n");
		return;
	}

	dp = display;

	if (dp->post_open)
		dp->post_open(dp);
}


int dp_drm_bond_bridge_init(void *display,
	struct drm_encoder *encoder,
	enum dp_bond_type type,
	struct dp_display_bond_displays *bond_displays)
{
	struct dp_display *dp_display = display, *bond_display;
	struct dp_bond_info *bond_info;
	struct dp_bond_bridge *bridge;
	struct dp_bond_mgr *mgr;
	struct dp_bond_mgr_state *state;
	struct msm_drm_private *priv;
	struct sde_kms *sde_kms;
	int i, rc;

	if (type < 0 || type >= DP_BOND_MAX || !bond_displays ||
		bond_displays->dp_display_num >= MAX_DP_BOND_NUM)
		return -EINVAL;

	priv = dp_display->drm_dev->dev_private;
	sde_kms = to_sde_kms(priv->kms);

	mgr = sde_kms->dp_bond_mgr;
	if (!mgr) {
		mgr = devm_kzalloc(dp_display->drm_dev->dev,
				sizeof(*mgr), GFP_KERNEL);
		if (!mgr)
			return -ENOMEM;

		state = kzalloc(sizeof(*state), GFP_KERNEL);
		if (state == NULL)
			return -ENOMEM;

		drm_atomic_private_obj_init(dp_display->drm_dev, &mgr->obj,
					    &state->base,
					    &dp_bond_mgr_state_funcs);
		sde_kms->dp_bond_mgr = mgr;
	}

	for (i = 0; i < bond_displays->dp_display_num; i++) {
		bond_display = bond_displays->dp_display[i];
		if (!bond_display->dp_bond_prv_info) {
			bond_info = devm_kzalloc(
				dp_display->drm_dev->dev,
				sizeof(*bond_info), GFP_KERNEL);
			if (!bond_info)
				return -ENOMEM;
			bond_info->bond_mgr = mgr;
			bond_info->bond_idx = drm_connector_index(
					bond_display->base_connector);
			bond_display->dp_bond_prv_info = bond_info;
		}
	}

	bond_info = dp_display->dp_bond_prv_info;
	if (!bond_info)
		return -EINVAL;

	bridge = &mgr->bond_bridge[type];
	if (bridge->display) {
		pr_err("bond bridge already inited\n");
		return -EINVAL;
	}

	bridge->encoder = encoder;
	bridge->base.funcs = &dp_bond_bridge_ops;
	bridge->base.encoder = encoder;
	bridge->display = display;
	bridge->type = type;
	bridge->bridge_num = bond_displays->dp_display_num;

	for (i = 0; i < bridge->bridge_num; i++) {
		bond_display = bond_displays->dp_display[i];
		bond_info = bond_display->dp_bond_prv_info;
		bond_info->bond_bridge[type] = bridge;
		bridge->bond_mask |= (1 << bond_info->bond_idx);
		bridge->bridges[i] = bond_display->bridge;
	}

	rc = drm_bridge_attach(encoder, &bridge->base, NULL, 0);
	if (rc) {
		pr_err("failed to attach bridge, rc=%d\n", rc);
		return rc;
	}

	priv->bridges[priv->num_bridges++] = &bridge->base;

	return 0;
}

struct drm_encoder *dp_connector_atomic_best_encoder(
		struct drm_connector *connector, void *display,
		struct drm_connector_state *state)
{
	struct dp_display *dp_display = display;
	struct drm_crtc_state *crtc_state;
	struct sde_connector *sde_conn = to_sde_connector(connector);
	struct dp_bond_info *bond_info;
	struct dp_bond_bridge *bond_bridge;
	struct dp_bond_mgr *bond_mgr;
	struct dp_bond_mgr_state *bond_state;
	enum dp_bond_type type;

	/* return if bond mode is not supported */
	if (!dp_display->dp_bond_prv_info)
		return sde_conn->encoder;

	/* get current mode */
	crtc_state = drm_atomic_get_new_crtc_state(state->state, state->crtc);

	/* return encoder in state if there is no switch needed */
	if (state->best_encoder) {
		if (dp_bond_is_tile_mode(&crtc_state->mode)) {
			if (state->best_encoder != sde_conn->encoder)
				return state->best_encoder;
		} else {
			if (state->best_encoder == sde_conn->encoder)
				return state->best_encoder;
		}
	}

	bond_info = dp_display->dp_bond_prv_info;
	bond_mgr = bond_info->bond_mgr;
	bond_state = dp_bond_get_mgr_atomic_state(state->state, bond_mgr);
	if (IS_ERR(bond_state))
		return NULL;

	/* clear bond connector */
	for (type = 0; type < DP_BOND_MAX; type++) {
		if (bond_state->connector[type] != connector) {
			if (bond_state->bond_mask[type] &
					(1 << bond_info->bond_idx)) {
				pr_debug("single encoder is in use\n");
				return NULL;
			}
			continue;
		}

		bond_bridge = bond_info->bond_bridge[type];
		bond_state->connector_mask &= ~bond_bridge->bond_mask;
		bond_state->bond_mask[type] = 0;
		bond_state->connector[type] = NULL;
		break;
	}

	/* clear single connector */
	bond_state->connector_mask &= ~(1 << bond_info->bond_idx);

	if (dp_bond_is_tile_mode(&crtc_state->mode)) {
		type = dp_bond_get_bond_type(connector);
		if (type == DP_BOND_MAX)
			return NULL;

		if (!dp_bond_check_connector(connector, type))
			return NULL;

		bond_bridge = bond_info->bond_bridge[type];
		if (bond_state->connector_mask & bond_bridge->bond_mask) {
			pr_debug("bond encoder is in use\n");
			return NULL;
		}

		bond_state->connector_mask |= bond_bridge->bond_mask;
		bond_state->bond_mask[type] = bond_bridge->bond_mask;
		bond_state->connector[type] = connector;
		return bond_bridge->encoder;
	}

	bond_state->connector_mask |= (1 << bond_info->bond_idx);
	return sde_conn->encoder;
}

int dp_connector_atomic_check(struct drm_connector *connector,
		void *display, struct drm_atomic_state *a_state)
{
	struct drm_connector_state *c_state;
	struct drm_connector_state *old_state;
	struct sde_connector *sde_conn;
	struct drm_crtc *old_crtc;
	struct drm_crtc_state *crtc_state;
	struct dp_display *dp_display = display;
	struct dp_bond_info *bond_info;
	struct dp_bond_bridge *bond_bridge;
	struct dp_bond_mgr_state *bond_state;

	if (!connector || !display || !a_state)
		return -EINVAL;

	c_state = drm_atomic_get_new_connector_state(a_state, connector);
	old_state = drm_atomic_get_old_connector_state(a_state, connector);
	if (!old_state || !c_state)
		return -EINVAL;

	sde_conn = to_sde_connector(connector);

	/*
	 * Marking the colorspace has been changed
	 * the flag shall be checked in the pre_kickoff
	 * to configure the new colorspace in HW
	 */
	if (c_state->colorspace != old_state->colorspace) {
		DP_DEBUG("colorspace has been updated\n");
		sde_conn->colorspace_updated = true;
	}

	/* return if bond mode is not supported */
	if (!dp_display->dp_bond_prv_info)
		return 0;

	old_crtc = old_state->crtc;
	if (!old_crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(a_state, old_crtc);

	if (drm_atomic_crtc_needs_modeset(crtc_state) &&
			!c_state->crtc) {
		bond_info = dp_display->dp_bond_prv_info;
		bond_state = dp_bond_get_mgr_atomic_state(a_state,
				bond_info->bond_mgr);
		if (IS_ERR(bond_state))
			return PTR_ERR(bond_state);

		/* clear single state */
		if (old_state->best_encoder ==
				dp_display->bridge->base.encoder) {
			bond_state->connector_mask &=
				~(1 << bond_info->bond_idx);
			return 0;
		}

		/* clear bond state */
		bond_bridge = to_dp_bond_bridge(list_first_entry(
					&old_state->best_encoder->bridge_chain,
					struct drm_bridge, chain_node));

		bond_state->connector[bond_bridge->type] = NULL;
		bond_state->bond_mask[bond_bridge->type] = 0;
		bond_state->connector_mask &= ~bond_bridge->bond_mask;
	}

	return 0;
}

int dp_connector_get_modes(struct drm_connector *connector,
		void *display, const struct msm_resource_caps_info *avail_res)
{
	int rc = 0;
	struct dp_display *dp;
	struct dp_display_mode *dp_mode = NULL;
	struct drm_display_mode *m, drm_mode;
	struct sde_connector *sde_conn;

	if (!connector || !display)
		return 0;

	sde_conn = to_sde_connector(connector);
	if (!sde_conn->drv_panel) {
		DP_ERR("invalid dp panel\n");
		return 0;
	}

	dp = display;

	dp_mode = kzalloc(sizeof(*dp_mode),  GFP_KERNEL);
	if (!dp_mode)
		return 0;

	/* pluggable case assumes EDID is read when HPD */
	if (dp->is_sst_connected) {
		rc = dp->get_modes(dp, sde_conn->drv_panel, dp_mode);
		if (!rc)
			DP_ERR("failed to get DP sink modes, rc=%d\n", rc);

		if (dp_mode->timing.pixel_clk_khz) { /* valid DP mode */
			memset(&drm_mode, 0x0, sizeof(drm_mode));
			convert_to_drm_mode(dp_mode, &drm_mode);
			m = drm_mode_duplicate(connector->dev, &drm_mode);
			if (!m) {
				DP_ERR("failed to add mode %ux%u\n",
				       drm_mode.hdisplay,
				       drm_mode.vdisplay);
				kfree(dp_mode);
				return 0;
			}
			m->width_mm = connector->display_info.width_mm;
			m->height_mm = connector->display_info.height_mm;
			drm_mode_probed_add(connector, m);
		}

		if (dp->dp_bond_prv_info)
			dp_bond_fixup_tile_mode(connector);
	} else {
		DP_ERR("No sink connected\n");
	}
	kfree(dp_mode);

	return rc;
}

int dp_connnector_set_info_blob(struct drm_connector *connector,
		void *info, void *display, struct msm_mode_info *mode_info)
{
	struct dp_display *dp_display = display;
	const char *display_type = NULL;

	dp_display->get_display_type(dp_display, &display_type);
	sde_kms_info_add_keystr(info,
		"display type", display_type);

	return 0;
}

int dp_drm_bridge_init(void *data, struct drm_encoder *encoder,
	u32 max_mixer_count, u32 max_dsc_count)
{
	int rc = 0;
	struct dp_bridge *bridge;
	struct drm_device *dev;
	struct dp_display *display = data;
	struct msm_drm_private *priv = NULL;

	bridge = kzalloc(sizeof(*bridge), GFP_KERNEL);
	if (!bridge) {
		rc = -ENOMEM;
		goto error;
	}

	dev = display->drm_dev;
	bridge->display = display;
	bridge->base.funcs = &dp_bridge_ops;
	bridge->base.encoder = encoder;

	priv = dev->dev_private;

	rc = drm_bridge_attach(encoder, &bridge->base, NULL, 0);
	if (rc) {
		DP_ERR("failed to attach bridge, rc=%d\n", rc);
		goto error_free_bridge;
	}

	rc = display->request_irq(display);
	if (rc) {
		DP_ERR("request_irq failed, rc=%d\n", rc);
		goto error_free_bridge;
	}

	priv->bridges[priv->num_bridges++] = &bridge->base;
	display->bridge = bridge;
	display->max_mixer_count = max_mixer_count;
	display->max_dsc_count = max_dsc_count;

	return 0;
error_free_bridge:
	kfree(bridge);
error:
	return rc;
}

void dp_drm_bridge_deinit(void *data)
{
	struct dp_display *display = data;
	struct dp_bridge *bridge = display->bridge;

	kfree(bridge);
}

enum drm_mode_status dp_connector_mode_valid(struct drm_connector *connector,
		struct drm_display_mode *mode, void *display,
		const struct msm_resource_caps_info *avail_res)
{
	int rc = 0, vrefresh;
	struct dp_display *dp_disp;
	struct sde_connector *sde_conn;
	struct msm_resource_caps_info avail_dp_res;
	struct dp_panel *dp_panel;

	if (!mode || !display || !connector) {
		DP_ERR("invalid params\n");
		return MODE_ERROR;
	}

	sde_conn = to_sde_connector(connector);
	if (!sde_conn->drv_panel) {
		DP_ERR("invalid dp panel\n");
		return MODE_ERROR;
	}

	dp_disp = display;
	dp_panel = sde_conn->drv_panel;

	vrefresh = drm_mode_vrefresh(mode);

	rc = dp_disp->get_available_dp_resources(dp_disp, avail_res,
			&avail_dp_res);
	if (rc) {
		DP_ERR("error getting max dp resources. rc:%d\n", rc);
		return MODE_ERROR;
	}

	if (dp_panel->mode_override && (mode->hdisplay != dp_panel->hdisplay ||
			mode->vdisplay != dp_panel->vdisplay ||
			vrefresh != dp_panel->vrefresh ||
			mode->picture_aspect_ratio != dp_panel->aspect_ratio))
		return MODE_BAD;

	if (dp_bond_is_tile_mode(mode)) {
		struct drm_display_mode tmp;
		enum dp_bond_type type;

		type = dp_bond_get_bond_type(connector);
		if (type == DP_BOND_MAX)
			return MODE_BAD;

		if (!dp_bond_check_connector(connector, type)) {
			pr_debug("mode:%s requires multi ports\n", mode->name);
			return MODE_BAD;
		}

		tmp = *mode;
		dp_bond_split_tile_timing(&tmp, connector->num_h_tile);

		return dp_disp->validate_mode(dp_disp,
				sde_conn->drv_panel, &tmp, &avail_dp_res);
	}

	return dp_disp->validate_mode(dp_disp, sde_conn->drv_panel,
			mode, &avail_dp_res);
}

int dp_connector_update_pps(struct drm_connector *connector,
		char *pps_cmd, void *display)
{
	struct dp_display *dp_disp;
	struct sde_connector *sde_conn;

	if (!display || !connector) {
		DP_ERR("invalid params\n");
		return -EINVAL;
	}

	sde_conn = to_sde_connector(connector);
	if (!sde_conn->drv_panel) {
		DP_ERR("invalid dp panel\n");
		return MODE_ERROR;
	}

	dp_disp = display;
	return dp_disp->update_pps(dp_disp, connector, pps_cmd);
}

int dp_connector_install_properties(void *display, struct drm_connector *conn)
{
	struct dp_display *dp_display = display;
	struct drm_connector *base_conn;
	int rc;

	if (!display || !conn) {
		DP_ERR("invalid params\n");
		return -EINVAL;
	}

	base_conn = dp_display->base_connector;

	/*
	 * Create the property on the base connector during probe time and then
	 * attach the same property onto new connector objects created for MST
	 */
	if (!base_conn->colorspace_property) {
		/* This is the base connector. create the drm property */
		rc = drm_mode_create_dp_colorspace_property(base_conn);
		if (rc)
			return rc;
	} else {
		conn->colorspace_property = base_conn->colorspace_property;
	}

	drm_object_attach_property(&conn->base, conn->colorspace_property, 0);

	return 0;
}
