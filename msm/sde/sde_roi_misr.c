// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <drm/drm_encoder.h>
#include "sde_encoder.h"
#include "sde_hw_roi_misr.h"
#include "sde_hw_dspp.h"
#include "sde_trace.h"
#include "sde_core_irq.h"
#include "sde_roi_misr.h"
#include "sde_roi_misr_helper.h"
#include "sde_fence_misr.h"
#include "msm_drv.h"

static void sde_roi_misr_work(struct kthread_work *work);

void sde_roi_misr_init(struct sde_crtc *sde_crtc)
{
	struct sde_misr_crtc_data *roi_misr_data;

	roi_misr_data = &sde_crtc->roi_misr_data;
	atomic_set(&roi_misr_data->cfg_refcount, 0);
	spin_lock_init(&roi_misr_data->misr_lock);
	kthread_init_work(&roi_misr_data->misr_event.work,
			sde_roi_misr_work);

	sde_misr_fence_ctx_init(sde_crtc);
}

static inline struct sde_kms *_sde_misr_get_kms(struct drm_crtc *crtc)
{
	struct msm_drm_private *priv;

	if (!crtc || !crtc->dev || !crtc->dev->dev_private) {
		SDE_ERROR("invalid crtc\n");
		return NULL;
	}

	priv = crtc->dev->dev_private;
	if (!priv || !priv->kms) {
		SDE_ERROR("invalid kms\n");
		return NULL;
	}

	return to_sde_kms(priv->kms);
}

int sde_roi_misr_cfg_set(struct drm_crtc_state *state,
		void __user *usr_ptr)
{
	struct drm_crtc *crtc;
	struct sde_crtc_state *cstate;
	struct sde_roi_misr_usr_cfg *roi_misr_cfg;
	struct sde_drm_roi_misr_v1 roi_misr_info;

	if (!state) {
		SDE_ERROR("invalid args\n");
		return -EINVAL;
	}

	if (!usr_ptr) {
		SDE_DEBUG("roi misr cleared\n");
		return 0;
	}

	cstate = to_sde_crtc_state(state);
	crtc = cstate->base.crtc;
	roi_misr_cfg = &cstate->misr_state.roi_misr_cfg;

	if (copy_from_user(&roi_misr_info, usr_ptr, sizeof(roi_misr_info))) {
		SDE_ERROR("crtc%d: failed to copy roi_v1 data\n", DRMID(crtc));
		return -EINVAL;
	}

	/* return directly if the roi_misr commit is empty */
	if (roi_misr_info.roi_rect_num == 0)
		return 0;

	if (roi_misr_info.roi_rect_num > ROI_MISR_MAX_ROIS_PER_CRTC) {
		SDE_ERROR("invalid roi_rect_num(%u)\n",
				roi_misr_info.roi_rect_num);
		return -EINVAL;
	}

	if (!roi_misr_info.roi_ids || !roi_misr_info.roi_rects) {
		SDE_ERROR("crtc%d: misr data pointer is NULL\n", DRMID(crtc));
		return -EINVAL;
	}

	roi_misr_cfg->user_fence_fd_addr = roi_misr_info.fence_fd_ptr;
	if (!roi_misr_cfg->user_fence_fd_addr) {
		SDE_ERROR("crtc%d: fence fd address error\n", DRMID(crtc));
		return -EINVAL;
	}

	roi_misr_cfg->roi_rect_num = roi_misr_info.roi_rect_num;

	if (copy_from_user(roi_misr_cfg->roi_ids,
		(void __user *)roi_misr_info.roi_ids,
		sizeof(int) * roi_misr_info.roi_rect_num)) {
		SDE_ERROR("crtc%d: failed to copy roi_ids data\n", DRMID(crtc));
		return -EINVAL;
	}

	if (copy_from_user(roi_misr_cfg->roi_rects,
		(void __user *)roi_misr_info.roi_rects,
		sizeof(struct sde_rect) * roi_misr_info.roi_rect_num)) {
		SDE_ERROR("crtc%d: failed to copy roi_rects data\n",
				DRMID(crtc));
		return -EINVAL;
	}

	/**
	 * if user don't set golden value, always set all
	 * golden values to 0xFFFFFFFF as default value
	 */
	if (!roi_misr_info.roi_golden_value) {
		memset(roi_misr_cfg->roi_golden_value, 0xFF,
			sizeof(roi_misr_cfg->roi_golden_value));
	} else if (copy_from_user(roi_misr_cfg->roi_golden_value,
		(void __user *)roi_misr_info.roi_golden_value,
		sizeof(uint32_t) * roi_misr_info.roi_rect_num)) {
		SDE_ERROR("crtc%d: failed to copy roi_golden_value data\n",
				DRMID(crtc));
		return -EINVAL;
	}

	if (roi_misr_info.roi_rect_num)
		cstate->post_commit_fence_mask |= BIT(SDE_SUB_FENCE_ROI_MISR);

	return 0;
}

int sde_roi_misr_get_mode_info(struct drm_connector *connector,
		const struct drm_display_mode *drm_mode,
		struct msm_mode_info *mode_info,
		struct sde_roi_misr_mode_info *misr_mode_info,
		void *display)
{
	struct sde_connector *sde_conn = NULL;
	struct msm_drm_private *priv;
	struct sde_kms *sde_kms;
	struct drm_clip_rect *roi_range;
	enum sde_rm_topology_name topology_name;
	int all_roi_num;
	int num_misrs, misr_width;
	int roi_factor, roi_id;
	int i;
	int ret = 0;

	if (!connector || !drm_mode || !mode_info
			|| !misr_mode_info || !display) {
		pr_err("%s: invalid params\n", __func__);
		return -EINVAL;
	}

	sde_conn = to_sde_connector(connector);
	priv = connector->dev->dev_private;
	sde_kms = to_sde_kms(priv->kms);

	/**
	 * Call sde_connector's operation to get misr mode
	 * info if callback function is registered, otherwise
	 * do default calculation.
	 */
	if (sde_conn->ops.get_roi_misr_mode_info) {
		ret = sde_conn->ops.get_roi_misr_mode_info(connector,
				mode_info, misr_mode_info, display);
		if (ret)
			pr_err("failed to get roi misr mode info\n");

		return ret;
	}

	topology_name = sde_rm_get_topology_name(&sde_kms->rm,
			mode_info->topology);
	num_misrs = sde_rm_get_roi_misr_num(&sde_kms->rm, topology_name);
	misr_width = drm_mode->hdisplay / num_misrs;
	all_roi_num = num_misrs * ROI_MISR_MAX_ROIS_PER_MISR;
	roi_factor = TOPOLOGY_3DMUX_MODE(topology_name)
			? 2 * ROI_MISR_MAX_ROIS_PER_MISR
			: ROI_MISR_MAX_ROIS_PER_MISR;

	misr_mode_info->mixer_width = drm_mode->hdisplay
			/ mode_info->topology.num_lm;
	misr_mode_info->num_misrs = num_misrs;
	misr_mode_info->misr_width = misr_width;

	for (i = 0; i < all_roi_num; i++) {
		roi_id = roi_factor * SDE_ROI_MISR_GET_HW_IDX(i)
				+ SDE_ROI_MISR_GET_ROI_IDX(i);

		roi_range = &misr_mode_info->roi_range[roi_id];
		roi_range->x1 = misr_width * SDE_ROI_MISR_GET_HW_IDX(i);
		roi_range->y1 = 0;
		roi_range->x2 = roi_range->x1 + misr_width - 1;
		roi_range->y2 = drm_mode->vdisplay - 1;
	}

	return 0;
}

void sde_roi_misr_populate_roi_range(
		struct sde_connector *c_conn,
		struct sde_kms_info *info,
		struct drm_display_mode *mode,
		struct msm_mode_info *mode_info)
{
	struct msm_drm_private *priv;
	struct sde_kms *sde_kms;
	char misr_prop_name[20] = {0};
	char misr_prop_value[30] = {0};
	struct drm_clip_rect *roi_range;
	struct sde_rect roi_rect;
	struct sde_roi_misr_mode_info misr_mode_info = {0};
	int topology_idx;
	int range_data_idx;
	int roi_misr_num;
	int roi_factor, roi_id;
	int i;

	priv = c_conn->base.dev->dev_private;
	sde_kms = to_sde_kms(priv->kms);
	topology_idx = (int)sde_rm_get_topology_name(&sde_kms->rm,
				mode_info->topology);
	if (topology_idx >= SDE_RM_TOPOLOGY_MAX) {
		pr_err("%s: invalid topology\n", __func__);
		return;
	}

	if (sde_roi_misr_get_mode_info(&c_conn->base, mode,
			mode_info, &misr_mode_info, c_conn->display))
		return;

	roi_misr_num = misr_mode_info.num_misrs;
	roi_factor = TOPOLOGY_3DMUX_MODE(topology_idx)
			? 2 * ROI_MISR_MAX_ROIS_PER_MISR
			: ROI_MISR_MAX_ROIS_PER_MISR;

	for (i = 0; i < roi_misr_num * ROI_MISR_MAX_ROIS_PER_MISR; i++) {
		range_data_idx = SDE_ROI_MISR_GET_HW_IDX(i);
		roi_id = roi_factor * range_data_idx
				+ SDE_ROI_MISR_GET_ROI_IDX(i);
		roi_range = &misr_mode_info.roi_range[roi_id];

		roi_rect.x = roi_range->x1;
		roi_rect.y = roi_range->y1;
		roi_rect.w = roi_range->x2 - roi_range->x1 + 1;
		roi_rect.h = roi_range->y2 - roi_range->y1 + 1;

		/* Skip invalid range info due to the range table is not continuous */
		if (!roi_rect.w || !roi_rect.h)
			continue;

		snprintf(misr_prop_name, sizeof(misr_prop_name),
				"misr_roi_%d", roi_id);
		snprintf(misr_prop_value, sizeof(misr_prop_value),
				"(%d,%d,%d,%d)",
				roi_rect.x, roi_rect.y,
				roi_rect.w, roi_rect.h);

		sde_kms_info_add_keystr(info, misr_prop_name,
				misr_prop_value);
	}
}

int sde_roi_misr_check_rois(struct drm_crtc_state *state)
{
	struct sde_crtc_state *crtc_state;
	struct sde_roi_misr_usr_cfg *roi_misr_cfg;
	struct sde_roi_misr_mode_info *misr_mode_info;
	struct drm_clip_rect *roi_range;
	int roi_id;
	int i;

	if (!state)
		return -EINVAL;

	crtc_state = to_sde_crtc_state(state);
	roi_misr_cfg = &crtc_state->misr_state.roi_misr_cfg;
	misr_mode_info = &crtc_state->misr_mode_info;

	/**
	 * if user_fence_fd_addr is NULL, that means
	 * user has not set the ROI_MISR property
	 */
	if (!roi_misr_cfg->user_fence_fd_addr)
		return 0;

	/**
	 * user can't get roi range through mode_properties
	 * if no available roi misr in current topology,
	 * so user shouldn't set ROI_MISR info
	 */
	if (!misr_mode_info->num_misrs) {
		SDE_ERROR("roi misr is not supported on this topology\n");
		return -EINVAL;
	}

	if (roi_misr_cfg->roi_rect_num >
		misr_mode_info->num_misrs * ROI_MISR_MAX_ROIS_PER_MISR) {
		SDE_ERROR("roi_rect_num(%d) is invalid\n",
				roi_misr_cfg->roi_rect_num);
		return -EINVAL;
	}

	for (i = 0; i < roi_misr_cfg->roi_rect_num; ++i) {
		roi_id = roi_misr_cfg->roi_ids[i];
		roi_range = &misr_mode_info->roi_range[roi_id];

		if (roi_misr_cfg->roi_rects[i].x1 < roi_range->x1
			|| roi_misr_cfg->roi_rects[i].y1 < roi_range->y1
			|| roi_misr_cfg->roi_rects[i].x2 > roi_range->x2
			|| roi_misr_cfg->roi_rects[i].y2 > roi_range->y2) {
			SDE_ERROR("error rect_info[%d]: {%d,%d,%d,%d}\n",
				roi_id,
				roi_misr_cfg->roi_rects[i].x1,
				roi_misr_cfg->roi_rects[i].y1,
				roi_misr_cfg->roi_rects[i].x2,
				roi_misr_cfg->roi_rects[i].y2);

			return -EINVAL;
		}
	}

	return 0;
}

static void sde_roi_misr_event_cb(void *data)
{
	struct drm_crtc *crtc;
	struct sde_crtc *sde_crtc;
	struct msm_drm_private *priv;
	struct sde_crtc_misr_event *misr_event;
	u32 crtc_id;

	if (!data) {
		SDE_ERROR("invalid data parameters\n");
		return;
	}

	crtc = (struct drm_crtc *)data;
	if (!crtc || !crtc->dev || !crtc->dev->dev_private) {
		SDE_ERROR("invalid crtc parameters\n");
		return;
	}

	sde_crtc = to_sde_crtc(crtc);
	priv = crtc->dev->dev_private;
	crtc_id = drm_crtc_index(crtc);

	misr_event = &sde_crtc->roi_misr_data.misr_event;
	misr_event->crtc = crtc;
	kthread_queue_work(&priv->event_thread[crtc_id].worker,
			&misr_event->work);
}

static void sde_roi_misr_work(struct kthread_work *work)
{
	struct sde_crtc_misr_event *misr_event;
	struct drm_crtc *crtc;
	struct sde_crtc *sde_crtc;

	if (!work) {
		SDE_ERROR("invalid work handle\n");
		return;
	}

	misr_event = container_of(work, struct sde_crtc_misr_event, work);
	if (!misr_event->crtc || !misr_event->crtc->state) {
		SDE_ERROR("invalid crtc\n");
		return;
	}

	crtc = misr_event->crtc;
	sde_crtc = to_sde_crtc(crtc);

	SDE_ATRACE_BEGIN("crtc_roi_misr_event");

	sde_post_commit_signal_sub_fence(
			&sde_crtc->post_commit_fence_ctx,
			SDE_SUB_FENCE_ROI_MISR);

	SDE_ATRACE_END("crtc_roi_misr_event");
}

static void sde_roi_misr_roi_calc(struct sde_crtc *sde_crtc,
		struct sde_crtc_state *cstate)
{
	struct sde_roi_misr_mode_info *misr_mode_info;
	struct sde_roi_misr_usr_cfg *roi_misr_cfg;
	struct sde_roi_misr_hw_cfg *roi_misr_hw_cfg;
	int roi_id;
	int misr_idx;
	int misr_roi_idx;
	int i;

	misr_mode_info = &cstate->misr_mode_info;
	roi_misr_cfg = &cstate->misr_state.roi_misr_cfg;

	memset(sde_crtc->roi_misr_data.roi_misr_hw_cfg, 0,
		sizeof(sde_crtc->roi_misr_data.roi_misr_hw_cfg));

	for (i = 0; i < roi_misr_cfg->roi_rect_num; ++i) {
		roi_id = roi_misr_cfg->roi_ids[i];
		misr_idx = SDE_ROI_MISR_GET_HW_IDX(roi_id);
		roi_misr_hw_cfg =
			&sde_crtc->roi_misr_data.roi_misr_hw_cfg[misr_idx];
		misr_roi_idx = SDE_ROI_MISR_GET_ROI_IDX(roi_id);

		/**
		 * convert global roi coordinate to the relative
		 * coordinate of MISR module.
		 */
		roi_misr_hw_cfg->misr_roi_rect[misr_roi_idx].x =
			roi_misr_cfg->roi_rects[i].x1
			% misr_mode_info->misr_width;
		roi_misr_hw_cfg->misr_roi_rect[misr_roi_idx].y =
			roi_misr_cfg->roi_rects[i].y1;
		roi_misr_hw_cfg->misr_roi_rect[misr_roi_idx].w =
			roi_misr_cfg->roi_rects[i].x2
			- roi_misr_cfg->roi_rects[i].x1 + 1;
		roi_misr_hw_cfg->misr_roi_rect[misr_roi_idx].h =
			roi_misr_cfg->roi_rects[i].y2
			- roi_misr_cfg->roi_rects[i].y1 + 1;

		roi_misr_hw_cfg->golden_value[misr_roi_idx] =
			roi_misr_cfg->roi_golden_value[i];

		/* always set frame_count to one */
		roi_misr_hw_cfg->frame_count[misr_roi_idx] = 1;

		roi_misr_hw_cfg->roi_mask |= BIT(misr_roi_idx);
	}
}

static void sde_roi_misr_dspp_roi_calc(struct sde_crtc *sde_crtc,
		struct sde_crtc_state *cstate)
{
	const int dual_mixer = 2;
	struct sde_rect roi_info;
	struct sde_rect *left_rect, *right_rect;
	struct sde_roi_misr_hw_cfg *l_dspp_hw_cfg, *r_dspp_hw_cfg;
	struct sde_roi_misr_mode_info *misr_mode_info;
	int mixer_width;
	int num_misrs;
	int lms_per_misr;
	int l_idx, r_idx;
	int i, j;

	misr_mode_info = &cstate->misr_mode_info;
	mixer_width = misr_mode_info->mixer_width;
	num_misrs = misr_mode_info->num_misrs;
	lms_per_misr = cstate->num_mixers / num_misrs;

	for (i = 0; i < num_misrs; ++i) {
		/**
		 * Convert MISR rect info to DSPP bypass rect
		 * this rect coordinate has been converted to
		 * every MISR's coordinate, so we can use it
		 * directly. Left and right are abstract concepts,
		 * not specific LM, it is based on one MISR.
		 *
		 * if not in merge mode, only left can be used.
		 *
		 * if in merge mode, left & right are based on
		 * the same MISR.
		 */
		l_idx = (lms_per_misr == dual_mixer)
				? lms_per_misr * i : i;
		l_dspp_hw_cfg =
			&sde_crtc->roi_misr_data.roi_misr_hw_cfg[l_idx];

		r_idx = l_idx + 1;
		r_dspp_hw_cfg =
			&sde_crtc->roi_misr_data.roi_misr_hw_cfg[r_idx];

		for (j = 0; j < ROI_MISR_MAX_ROIS_PER_MISR; ++j) {
			if (!(l_dspp_hw_cfg->roi_mask & BIT(i)))
				continue;

			roi_info = l_dspp_hw_cfg->misr_roi_rect[j];
			left_rect = &l_dspp_hw_cfg->dspp_roi_rect[j];
			right_rect = &r_dspp_hw_cfg->dspp_roi_rect[j];

			if ((roi_info.x + roi_info.w <= mixer_width)) {
				left_rect->x = roi_info.x;
				left_rect->y = roi_info.y;
				left_rect->w = roi_info.w;
				left_rect->h = roi_info.h;
				l_dspp_hw_cfg->dspp_roi_mask |= BIT(j);
			} else if (roi_info.x >= mixer_width) {
				right_rect->x = roi_info.x - mixer_width;
				right_rect->y = roi_info.y;
				right_rect->w = roi_info.w;
				right_rect->h = roi_info.h;
				r_dspp_hw_cfg->dspp_roi_mask |= BIT(j);
			} else if (lms_per_misr == dual_mixer) {
				left_rect->x = roi_info.x;
				left_rect->y = roi_info.y;
				left_rect->w = mixer_width - left_rect->x;
				left_rect->h = roi_info.h;
				l_dspp_hw_cfg->dspp_roi_mask |= BIT(j);

				right_rect->x = 0;
				right_rect->y = roi_info.y;
				right_rect->w = roi_info.w - left_rect->w;
				right_rect->h = roi_info.h;
				r_dspp_hw_cfg->dspp_roi_mask |= BIT(j);
			}
		}
	}
}

static bool sde_roi_misr_dspp_is_used(struct sde_crtc *sde_crtc)
{
	int i;

	for (i = 0; i < sde_crtc->num_mixers; ++i)
		if (sde_crtc->mixers[i].hw_dspp)
			return true;

	return false;
}

void sde_roi_misr_setup(struct drm_crtc *crtc)
{
	struct sde_crtc *sde_crtc;
	struct sde_crtc_state *cstate;
	struct sde_hw_ctl *hw_ctl;
	struct sde_hw_roi_misr *hw_misr;
	struct sde_hw_dspp *hw_dspp;
	struct sde_roi_misr_hw_cfg *misr_hw_cfg;
	struct sde_hw_intf_cfg_v1 dsc_cfg = {0};
	int i;

	sde_crtc = to_sde_crtc(crtc);
	cstate = to_sde_crtc_state(crtc->state);

	sde_roi_misr_roi_calc(sde_crtc, cstate);

	if (sde_roi_misr_dspp_is_used(sde_crtc))
		sde_roi_misr_dspp_roi_calc(sde_crtc, cstate);

	sde_encoder_register_roi_misr_callback(
			sde_crtc->mixers[0].encoder,
			sde_roi_misr_event_cb, crtc);

	hw_ctl = sde_crtc->mixers[0].hw_ctl;

	for (i = 0; i < sde_crtc->num_mixers; ++i) {
		hw_dspp = sde_crtc->mixers[i].hw_dspp;
		hw_misr = sde_crtc->mixers[i].hw_roi_misr;
		misr_hw_cfg =
			&sde_crtc->roi_misr_data.roi_misr_hw_cfg[i];

		if (!hw_misr)
			continue;

		hw_misr->ops.setup_roi_misr(hw_misr, misr_hw_cfg);
		hw_ctl->ops.update_bitmask(hw_ctl, SDE_HW_FLUSH_DSC,
				hw_misr->idx, true);

		if (hw_dspp && hw_dspp->ops.setup_roi_misr) {
			hw_dspp->ops.setup_roi_misr(hw_dspp,
					misr_hw_cfg->dspp_roi_mask,
					misr_hw_cfg->dspp_roi_rect);

			hw_ctl->ops.update_bitmask_dspp(hw_ctl,
					hw_dspp->idx, true);
		}

		dsc_cfg.dsc[dsc_cfg.dsc_count++] = (enum sde_dsc)hw_misr->idx;

		SDE_DEBUG("crtc%d: setup roi misr, index(%d),",
				crtc->base.id, i);
		SDE_DEBUG("roi_mask(%x), hw_lm_id %d, hw_misr_id %d\n",
				misr_hw_cfg->roi_mask,
				sde_crtc->mixers[i].hw_lm->idx,
				hw_misr->idx);
	}

	if (hw_ctl->ops.update_intf_cfg && dsc_cfg.dsc_count)
		hw_ctl->ops.update_intf_cfg(hw_ctl, &dsc_cfg, true);
}

void sde_roi_misr_hw_reset(struct sde_encoder_phys *phys_enc)
{
	struct sde_hw_roi_misr *hw_roi_misr;
	int i;

	for (i = 0; i < phys_enc->roi_misr_num; i++) {
		hw_roi_misr = phys_enc->hw_roi_misr[i];
		if (!hw_roi_misr->ops.reset_roi_misr)
			continue;

		hw_roi_misr->ops.reset_roi_misr(hw_roi_misr);
		phys_enc->hw_ctl->ops.update_bitmask(
				phys_enc->hw_ctl, SDE_HW_FLUSH_DSC,
				hw_roi_misr->idx, true);
	}
}

void sde_roi_misr_setup_irq_hw_idx(struct sde_encoder_phys *phys_enc)
{
	struct sde_hw_roi_misr *hw_roi_misr;
	struct sde_encoder_irq *irq;
	int mismatch_idx, intr_offset;
	int i, j;

	for (i = 0; i < phys_enc->roi_misr_num; i++) {
		hw_roi_misr = phys_enc->hw_roi_misr[i];
		intr_offset = SDE_ROI_MISR_GET_INTR_OFFSET(hw_roi_misr->idx);

		for (j = 0; j < ROI_MISR_MAX_ROIS_PER_MISR; j++) {
			mismatch_idx = MISR_ROI_MISMATCH_BASE_IDX
				+ intr_offset * ROI_MISR_MAX_ROIS_PER_MISR + j;

			irq = &phys_enc->irq[mismatch_idx];
			if (irq->irq_idx < 0)
				irq->hw_idx = hw_roi_misr->idx;
		}
	}
}

int sde_roi_misr_irq_control(struct sde_encoder_phys *phys_enc,
		int base_irq_idx, int roi_idx, bool enable)
{
	struct sde_encoder_irq *irq;
	int irq_tbl_idx;
	int ret;

	if (!phys_enc) {
		SDE_ERROR("invalid parameters\n");
		return -EINVAL;
	}

	irq_tbl_idx = base_irq_idx + roi_idx;
	irq = &phys_enc->irq[irq_tbl_idx];

	if ((irq->irq_idx >= 0) && enable) {
		SDE_DEBUG(
			"skipping already registered irq %s type %d\n",
			irq->name, irq->intr_type);
		return 0;
	}

	if ((irq->irq_idx < 0) && (!enable))
		return 0;

	irq->irq_idx = sde_core_irq_idx_lookup(phys_enc->sde_kms,
			irq->intr_type, irq->hw_idx) + roi_idx;

	SDE_DEBUG("hw_idx(%d) roi_idx(%d) irq_idx(%d) enable(%d)\n",
			irq->hw_idx, roi_idx, irq->irq_idx, enable);

	if (enable) {
		ret = sde_core_irq_register_callback(phys_enc->sde_kms,
				irq->irq_idx,
				&phys_enc->irq[irq->intr_idx].cb);
		if (ret) {
			SDE_ERROR("failed to register IRQ[%d]\n",
				irq->irq_idx);
			return ret;
		}

		ret = sde_core_irq_enable(phys_enc->sde_kms,
				&irq->irq_idx, 1);
		if (ret) {
			SDE_ERROR("enable irq[%d] error %d\n",
				irq->irq_idx, ret);

			sde_core_irq_unregister_callback(
				phys_enc->sde_kms, irq->irq_idx,
				&phys_enc->irq[irq->intr_idx].cb);
			return ret;
		}
	} else {
		sde_core_irq_disable(phys_enc->sde_kms,
			&irq->irq_idx, 1);

		sde_core_irq_unregister_callback(
			phys_enc->sde_kms, irq->irq_idx,
			&phys_enc->irq[irq->intr_idx].cb);

		irq->irq_idx = -EINVAL;
	}

	return 0;
}

