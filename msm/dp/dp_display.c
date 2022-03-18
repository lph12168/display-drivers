// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt)	"[drm-dp] %s: " fmt, __func__

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/component.h>
#include <linux/of_irq.h>
#include <linux/extcon.h>
#include <linux/soc/qcom/fsa4480-i2c.h>

#include "sde_connector.h"

#include "msm_drv.h"
#include "dp_hpd.h"
#include "dp_parser.h"
#include "dp_power.h"
#include "dp_catalog.h"
#include "dp_aux.h"
#include "dp_link.h"
#include "dp_panel.h"
#include "dp_ctrl.h"
#include "dp_audio.h"
#include "dp_display.h"
#include "sde_hdcp.h"
#include "dp_debug.h"
#include "dp_mst_sim.h"

#define DP_MST_DEBUG(fmt, ...) pr_debug(fmt, ##__VA_ARGS__)

#define MAX_DP_BOOT_DISPLAY	1
#define MAX_CMDLINE_PARAM_LEN	512

struct dp_display_boot_param {
	char name[MAX_CMDLINE_PARAM_LEN];
	char *boot_param;
	bool boot_disp_en;
	struct device_node *node;
	void *disp;
};

static char dp_display_0[MAX_CMDLINE_PARAM_LEN];
static struct dp_display_boot_param boot_displays[MAX_DP_BOOT_DISPLAY] = {
	{.boot_param = dp_display_0},
};

#define HPD_STRING_SIZE 30
#define MAX_DP_NAME_SIZE 8

static struct dp_display *g_dp_display[MAX_DP_ACTIVE_DISPLAY];

struct dp_hdcp_dev {
	void *fd;
	struct sde_hdcp_ops *ops;
	enum sde_hdcp_version ver;
};

struct dp_hdcp {
	void *data;
	struct sde_hdcp_ops *ops;

	u32 source_cap;

	struct dp_hdcp_dev dev[HDCP_VERSION_MAX];
};

struct dp_mst {
	bool mst_active;

	bool drm_registered;
	struct dp_mst_drm_cbs cbs;
};

struct dp_display_private {
	char name[MAX_DP_NAME_SIZE];
	int irq;

	/* state variables */
	bool core_initialized;
	bool power_on;
	bool is_connected;

	atomic_t aborted;

	struct platform_device *pdev;
	struct device_node *aux_switch_node;
	struct msm_dp_aux_bridge *aux_bridge;
	struct dentry *root;

	struct dp_hpd     *hpd;
	struct dp_parser  *parser;
	struct dp_power   *power;
	struct dp_catalog *catalog;
	struct dp_aux     *aux;
	struct dp_link    *link;
	struct dp_panel   *panel;
	struct dp_ctrl    *ctrl;
	struct dp_debug   *debug;

	struct dp_panel *active_panels[DP_STREAM_MAX];
	struct dp_hdcp hdcp;

	struct dp_hpd_cb hpd_cb;
	struct dp_display_mode mode;
	struct dp_display dp_display;
	struct msm_drm_private *priv;

	struct workqueue_struct *wq;
	struct delayed_work hdcp_cb_work;
	struct work_struct connect_work;
	struct work_struct attention_work;
	struct mutex session_lock;
	bool suspended;
	bool hdcp_delayed_off;
	bool hdcp_abort;

	u32 active_stream_cnt;
	struct dp_mst mst;

	u32 tot_dsc_blks_in_use;

	bool process_hpd_connect;

	struct notifier_block usb_nb;

	u32 cell_idx;
	u32 intf_idx[DP_STREAM_MAX];
	u32 phy_idx;

	enum dp_phy_bond_mode phy_bond_mode;
	struct drm_connector *bond_primary;

	struct device *msm_hdcp_dev;

	struct sde_power_client *cont_splash_client;
};

static const struct of_device_id dp_dt_match[] = {
	{.compatible = "qcom,dp-display"},
	{}
};

static inline bool dp_display_is_hdcp_enabled(struct dp_display_private *dp)
{
	return dp->link->hdcp_status.hdcp_version && dp->hdcp.ops;
}

static irqreturn_t dp_display_irq(int irq, void *dev_id)
{
	struct dp_display_private *dp = dev_id;

	if (!dp) {
		pr_err("invalid data\n");
		return IRQ_NONE;
	}

	/* DP HPD isr */
	if (dp->hpd->type ==  DP_HPD_LPHW)
		dp->hpd->isr(dp->hpd);

	/* DP controller isr */
	dp->ctrl->isr(dp->ctrl);

	/* DP aux isr */
	dp->aux->isr(dp->aux);

	/* HDCP isr */
	if (dp_display_is_hdcp_enabled(dp) && dp->hdcp.ops->isr) {
		if (dp->hdcp.ops->isr(dp->hdcp.data))
			pr_err("dp_hdcp_isr failed\n");
	}

	return IRQ_HANDLED;
}
static bool dp_display_is_ds_bridge(struct dp_panel *panel)
{
	return (panel->dpcd[DP_DOWNSTREAMPORT_PRESENT] &
		DP_DWN_STRM_PORT_PRESENT);
}

static bool dp_display_is_sink_count_zero(struct dp_display_private *dp)
{
	return dp_display_is_ds_bridge(dp->panel) &&
		(dp->link->sink_count.count == 0);
}

static bool dp_display_is_ready(struct dp_display_private *dp)
{
	return dp->hpd->hpd_high && dp->is_connected &&
		!dp_display_is_sink_count_zero(dp) &&
		dp->hpd->alt_mode_cfg_done;
}

static void dp_display_audio_enable(struct dp_display_private *dp, bool enable)
{
	struct dp_panel *dp_panel;
	int idx = 0;

	for (idx = DP_STREAM_0; idx < DP_STREAM_MAX; idx++) {
		if (!dp->active_panels[idx])
			continue;

		dp_panel = dp->active_panels[idx];

		if (dp_panel->audio_supported) {
			if (enable) {
				dp_panel->audio->bw_code =
					dp->link->link_params.bw_code;
				dp_panel->audio->lane_count =
					dp->link->link_params.lane_count;
				dp_panel->audio->on(dp_panel->audio);
			} else {
				dp_panel->audio->off(dp_panel->audio);
			}
		}
	}
}

static void dp_display_update_hdcp_status(struct dp_display_private *dp,
					bool reset)
{
	if (reset) {
		dp->link->hdcp_status.hdcp_state = HDCP_STATE_INACTIVE;
		dp->link->hdcp_status.hdcp_version = HDCP_VERSION_NONE;
	}

	memset(dp->debug->hdcp_status, 0, sizeof(dp->debug->hdcp_status));

	snprintf(dp->debug->hdcp_status, sizeof(dp->debug->hdcp_status),
		"%s: %s\ncaps: %d\n",
		sde_hdcp_version(dp->link->hdcp_status.hdcp_version),
		sde_hdcp_state_name(dp->link->hdcp_status.hdcp_state),
		dp->hdcp.source_cap);
}

static void dp_display_update_hdcp_info(struct dp_display_private *dp)
{
	void *fd = NULL;
	struct dp_hdcp_dev *dev = NULL;
	struct sde_hdcp_ops *ops = NULL;
	int i = HDCP_VERSION_2P2;

	dp_display_update_hdcp_status(dp, true);

	dp->hdcp.data = NULL;
	dp->hdcp.ops = NULL;

	if (dp->debug->hdcp_disabled || dp->debug->sim_mode)
		return;

	while (i) {
		dev = &dp->hdcp.dev[i];
		ops = dev->ops;
		fd = dev->fd;

		i >>= 1;

		if (!(dp->hdcp.source_cap & dev->ver))
			continue;

		if (ops->sink_support(fd)) {
			dp->hdcp.data = fd;
			dp->hdcp.ops = ops;
			dp->link->hdcp_status.hdcp_version = dev->ver;
			break;
		}
	}

	pr_debug("HDCP version supported: %s\n",
		sde_hdcp_version(dp->link->hdcp_status.hdcp_version));
}

static void dp_display_check_source_hdcp_caps(struct dp_display_private *dp)
{
	int i;
	struct dp_hdcp_dev *hdcp_dev = dp->hdcp.dev;

	if (dp->debug->hdcp_disabled) {
		pr_debug("hdcp disabled\n");
		return;
	}

	for (i = 0; i < HDCP_VERSION_MAX; i++) {
		struct dp_hdcp_dev *dev = &hdcp_dev[i];
		struct sde_hdcp_ops *ops = dev->ops;
		void *fd = dev->fd;

		if (!fd || !ops)
			continue;

		if (ops->set_mode && ops->set_mode(fd, dp->mst.mst_active))
			continue;

		if (!(dp->hdcp.source_cap & dev->ver) &&
				ops->feature_supported &&
				ops->feature_supported(fd))
			dp->hdcp.source_cap |= dev->ver;
	}

	dp_display_update_hdcp_status(dp, false);
}

static void dp_display_hdcp_register_streams(struct dp_display_private *dp)
{
	int rc;
	size_t i;
	struct sde_hdcp_ops *ops = dp->hdcp.ops;
	void *data = dp->hdcp.data;

	if (dp_display_is_ready(dp) && dp->mst.mst_active && ops &&
			ops->register_streams){
		struct stream_info streams[DP_STREAM_MAX];
		int index = 0;

		pr_debug("Registering all active panel streams with HDCP\n");
		for (i = DP_STREAM_0; i < DP_STREAM_MAX; i++) {
			if (!dp->active_panels[i])
				continue;
			streams[index].stream_id = i;
			streams[index].virtual_channel =
				dp->active_panels[i]->vcpi;
			index++;
		}

		if (index > 0) {
			rc = ops->register_streams(data, index, streams);
			if (rc)
				pr_err("failed to register streams. rc = %d\n",
					rc);
		}
	}
}

static void dp_display_hdcp_deregister_stream(struct dp_display_private *dp,
		enum dp_stream_id stream_id)
{
	if (dp->hdcp.ops->deregister_streams) {
		struct stream_info stream = {stream_id,
				dp->active_panels[stream_id]->vcpi};

		pr_debug("Deregistering stream within HDCP library");
		dp->hdcp.ops->deregister_streams(dp->hdcp.data, 1, &stream);
	}
}

static void dp_display_abort_hdcp(struct dp_display_private *dp,
		bool abort)
{
	u32 i = HDCP_VERSION_2P2;
	struct dp_hdcp_dev *dev = NULL;

	while (i) {
		dev = &dp->hdcp.dev[i];
		i >>= 1;
		if (!(dp->hdcp.source_cap & dev->ver))
			continue;

		dev->ops->abort(dev->fd, abort);
	}
}

static void dp_display_hdcp_cb_work(struct work_struct *work)
{
	struct dp_display_private *dp;
	struct delayed_work *dw = to_delayed_work(work);
	struct sde_hdcp_ops *ops;
	struct dp_link_hdcp_status *status;
	void *data;
	int rc = 0;
	u32 hdcp_auth_state;
	u8 sink_status = 0;

	dp = container_of(dw, struct dp_display_private, hdcp_cb_work);

	if (!dp->power_on || !dp->is_connected || atomic_read(&dp->aborted) ||
			dp->hdcp_abort)
		return;

	if (dp->suspended) {
		pr_debug("System suspending. Delay HDCP operations\n");
		queue_delayed_work(dp->wq, &dp->hdcp_cb_work, HZ);
		return;
	}

	if (dp->hdcp_delayed_off) {
		if (dp->hdcp.ops && dp->hdcp.ops->off)
			dp->hdcp.ops->off(dp->hdcp.data);
		dp_display_update_hdcp_status(dp, true);
		dp->hdcp_delayed_off = false;
	}

	if (dp->debug->hdcp_wait_sink_sync) {
		drm_dp_dpcd_readb(dp->aux->drm_aux, DP_SINK_STATUS,
				&sink_status);
		sink_status &= (DP_RECEIVE_PORT_0_STATUS |
				DP_RECEIVE_PORT_1_STATUS);
		if (sink_status < 1) {
			pr_debug("Sink not synchronized. Queuing again then exiting\n");
			queue_delayed_work(dp->wq, &dp->hdcp_cb_work, HZ);
			return;
		}
	}

	status = &dp->link->hdcp_status;

	if (status->hdcp_state == HDCP_STATE_INACTIVE) {
		dp_display_check_source_hdcp_caps(dp);
		dp_display_update_hdcp_info(dp);

		if (dp_display_is_hdcp_enabled(dp)) {
			if (dp->hdcp.ops && dp->hdcp.ops->on &&
					dp->hdcp.ops->on(dp->hdcp.data)) {
				dp_display_update_hdcp_status(dp, true);
				return;
			}
		} else {
			dp_display_update_hdcp_status(dp, true);
			return;
		}
	}

	rc = dp->catalog->ctrl.read_hdcp_status(&dp->catalog->ctrl);
	if (rc >= 0) {
		hdcp_auth_state = (rc >> 20) & 0x3;
		pr_debug("hdcp auth state %d\n", hdcp_auth_state);
	}

	ops = dp->hdcp.ops;
	data = dp->hdcp.data;

	pr_debug("%s: %s\n", sde_hdcp_version(status->hdcp_version),
		sde_hdcp_state_name(status->hdcp_state));

	dp_display_update_hdcp_status(dp, false);

	if (status->hdcp_state != HDCP_STATE_AUTHENTICATED &&
		dp->debug->force_encryption && ops && ops->force_encryption)
		ops->force_encryption(data, dp->debug->force_encryption);

	switch (status->hdcp_state) {
	case HDCP_STATE_INACTIVE:
		dp_display_hdcp_register_streams(dp);
		if (dp->hdcp.ops && dp->hdcp.ops->authenticate)
			rc = dp->hdcp.ops->authenticate(data);
		if (!rc)
			status->hdcp_state = HDCP_STATE_AUTHENTICATING;
		break;
	case HDCP_STATE_AUTH_FAIL:
		if (dp_display_is_ready(dp) && dp->power_on) {
			if (ops && ops->on && ops->on(data)) {
				dp_display_update_hdcp_status(dp, true);
				return;
			}
			dp_display_hdcp_register_streams(dp);
			status->hdcp_state = HDCP_STATE_AUTHENTICATING;
			if (ops && ops->reauthenticate) {
				rc = ops->reauthenticate(data);
				if (rc)
					pr_err("failed rc=%d\n", rc);
			}
		} else {
			pr_debug("not reauthenticating, cable disconnected\n");
		}
		break;
	default:
		dp_display_hdcp_register_streams(dp);
		break;
	}
}

static void dp_display_notify_hdcp_status_cb(void *ptr,
		enum sde_hdcp_state state)
{
	struct dp_display_private *dp = ptr;

	if (!dp) {
		pr_err("invalid input\n");
		return;
	}

	dp->link->hdcp_status.hdcp_state = state;

	queue_delayed_work(dp->wq, &dp->hdcp_cb_work, HZ/4);
}

static void dp_display_deinitialize_hdcp(struct dp_display_private *dp)
{
	if (!dp) {
		pr_err("invalid input\n");
		return;
	}

	sde_dp_hdcp2p2_deinit(dp->hdcp.data);
}

static int dp_display_initialize_hdcp(struct dp_display_private *dp)
{
	struct sde_hdcp_init_data hdcp_init_data;
	struct dp_parser *parser;
	void *fd;
	int rc = 0;

	if (!dp) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	parser = dp->parser;

	hdcp_init_data.client_id     = HDCP_CLIENT_DP;
	hdcp_init_data.client_index  = dp->cell_idx;
	hdcp_init_data.drm_aux       = dp->aux->drm_aux;
	hdcp_init_data.cb_data       = (void *)dp;
	hdcp_init_data.workq         = dp->wq;
	hdcp_init_data.sec_access    = true;
	hdcp_init_data.notify_status = dp_display_notify_hdcp_status_cb;
	hdcp_init_data.dp_ahb        = &parser->get_io(parser, "dp_ahb")->io;
	hdcp_init_data.dp_aux        = &parser->get_io(parser, "dp_aux")->io;
	hdcp_init_data.dp_link       = &parser->get_io(parser, "dp_link")->io;
	hdcp_init_data.dp_p0         = &parser->get_io(parser, "dp_p0")->io;
	hdcp_init_data.qfprom_io     = &parser->get_io(parser,
						"qfprom_physical")->io;
	hdcp_init_data.hdcp_io       = &parser->get_io(parser,
						"hdcp_physical")->io;
	hdcp_init_data.revision      = &dp->panel->link_info.revision;
	hdcp_init_data.msm_hdcp_dev  = dp->msm_hdcp_dev;
	hdcp_init_data.forced_encryption = parser->has_force_encryption;
	fd = sde_hdcp_1x_init(&hdcp_init_data);
	if (IS_ERR_OR_NULL(fd)) {
		pr_err("Error initializing HDCP 1.x\n");
		rc = -EINVAL;
		goto error;
	}

	dp->hdcp.dev[HDCP_VERSION_1X].fd = fd;
	dp->hdcp.dev[HDCP_VERSION_1X].ops = sde_hdcp_1x_get(fd);
	dp->hdcp.dev[HDCP_VERSION_1X].ver = HDCP_VERSION_1X;
	pr_debug("HDCP 1.3 initialized\n");

	fd = sde_dp_hdcp2p2_init(&hdcp_init_data);
	if (IS_ERR_OR_NULL(fd)) {
		pr_err("Error initializing HDCP 2.x\n");
		rc = -EINVAL;
		goto error;
	}

	dp->hdcp.dev[HDCP_VERSION_2P2].fd = fd;
	dp->hdcp.dev[HDCP_VERSION_2P2].ops = sde_dp_hdcp2p2_get(fd);
	dp->hdcp.dev[HDCP_VERSION_2P2].ver = HDCP_VERSION_2P2;
	pr_debug("HDCP 2.2 initialized\n");

	return 0;
error:
	dp_display_deinitialize_hdcp(dp);

	return rc;
}

static int dp_display_get_cell_info(struct dp_display_private *dp)
{
	struct device_node *of_node = dp->pdev->dev.of_node;
	int i, next = 0;

	for (i = 0; i < DP_STREAM_MAX; i++) {
		dp->intf_idx[i] = next;
		of_property_read_u32_index(of_node,
				"qcom,intf-index", i, &dp->intf_idx[i]);
		next = dp->intf_idx[i] + 1;
	}

	of_property_read_u32(of_node,
			"qcom,phy-index", &dp->phy_idx);

	of_property_read_u32(of_node,
			"cell-index", &dp->cell_idx);

	return 0;
}

static int dp_display_bind(struct device *dev, struct device *master,
		void *data)
{
	int rc = 0;
	struct dp_display_private *dp;
	struct drm_device *drm;
	struct platform_device *pdev = to_platform_device(dev);

	if (!dev || !pdev || !master) {
		pr_err("invalid param(s), dev %pK, pdev %pK, master %pK\n",
				dev, pdev, master);
		rc = -EINVAL;
		goto end;
	}

	drm = dev_get_drvdata(master);
	dp = platform_get_drvdata(pdev);
	if (!drm || !dp) {
		pr_err("invalid param(s), drm %pK, dp %pK\n",
				drm, dp);
		rc = -EINVAL;
		goto end;
	}

	dp->dp_display.drm_dev = drm;
	dp->priv = drm->dev_private;

	dp_display_get_cell_info(dp);
end:
	return rc;
}

static void dp_display_unbind(struct device *dev, struct device *master,
		void *data)
{
	struct dp_display_private *dp;
	struct platform_device *pdev = to_platform_device(dev);

	if (!dev || !pdev) {
		pr_err("invalid param(s)\n");
		return;
	}

	dp = platform_get_drvdata(pdev);
	if (!dp) {
		pr_err("Invalid params\n");
		return;
	}

	if (dp->power)
		(void)dp->power->power_client_deinit(dp->power);
	if (dp->aux)
		(void)dp->aux->drm_aux_deregister(dp->aux);
	dp_display_deinitialize_hdcp(dp);
}

static const struct component_ops dp_display_comp_ops = {
	.bind = dp_display_bind,
	.unbind = dp_display_unbind,
};

static void dp_display_send_hpd_event(struct dp_display_private *dp)
{
	struct drm_device *dev = NULL;
	struct drm_connector *connector;
	char name[HPD_STRING_SIZE], status[HPD_STRING_SIZE],
		bpp[HPD_STRING_SIZE], pattern[HPD_STRING_SIZE];
	char *envp[6];

	if (dp->mst.mst_active) {
		pr_debug("skip notification for mst mode\n");
		return;
	}

	connector = dp->dp_display.base_connector;

	if (!connector) {
		pr_err("DP%d connector not set\n", dp->cell_idx);
		return;
	}

	connector->status = connector->funcs->detect(connector, false);

	dev = connector->dev;

	snprintf(name, HPD_STRING_SIZE, "name=%s", connector->name);
	snprintf(status, HPD_STRING_SIZE, "status=%s",
		drm_get_connector_status_name(connector->status));
	snprintf(bpp, HPD_STRING_SIZE, "bpp=%d",
		dp_link_bit_depth_to_bpp(
		dp->link->test_video.test_bit_depth));
	snprintf(pattern, HPD_STRING_SIZE, "pattern=%d",
		dp->link->test_video.test_video_pattern);

	pr_debug("[%s]:[%s] [%s] [%s]\n", name, status, bpp, pattern);
	envp[0] = name;
	envp[1] = status;
	envp[2] = bpp;
	envp[3] = pattern;
	envp[4] = "HOTPLUG=1";
	envp[5] = NULL;
	kobject_uevent_env(&dev->primary->kdev->kobj, KOBJ_CHANGE,
			envp);
}

static void dp_display_send_hpd_notification(struct dp_display_private *dp)
{
	bool hpd = dp->is_connected;

	dp->aux->state |= DP_STATE_NOTIFICATION_SENT;

	if (!dp->mst.mst_active)
		dp->dp_display.is_sst_connected = hpd;
	else
		dp->dp_display.is_sst_connected = false;

	dp_display_send_hpd_event(dp);
}

static void dp_display_send_force_connect_event(struct dp_display_private *dp)
{
	struct drm_device *dev = NULL;
	struct drm_connector *connector;
	char name[HPD_STRING_SIZE];
	char *envp[5];

	connector = dp->dp_display.base_connector;

	if (!connector) {
		pr_err("DP%d connector not set\n", dp->cell_idx);
		return;
	}

	dev = connector->dev;

	snprintf(name, HPD_STRING_SIZE, "name=%s", connector->name);

	envp[0] = name;
	envp[1] = dp->hpd->hpd_high ? "status=connected" : "status=disconnected";
	if ((dp->aux->state & DP_STATE_TRAIN_1_SUCCEEDED) &&
			(dp->aux->state & DP_STATE_TRAIN_2_SUCCEEDED))
		envp[2] = "link=ready";
	else if ((dp->aux->state & DP_STATE_TRAIN_1_FAILED) ||
			(dp->aux->state & DP_STATE_TRAIN_2_FAILED))
		envp[2] = "link=failed";
	else if ((dp->aux->state & DP_STATE_TRAIN_1_STARTED) ||
			(dp->aux->state & DP_STATE_TRAIN_2_STARTED))
		envp[2] = "link=training";
	else
		envp[2] = "link=not_ready";
	envp[3] = (dp->aux->state & DP_STATE_CTRL_POWERED_ON) ?
			"stream=ON" : "stream=OFF";
	envp[4] = NULL;
	pr_info("[%s]:[%s] [%s] [%s]\n", name, envp[1], envp[2], envp[3]);

	kobject_uevent_env(&dev->primary->kdev->kobj, KOBJ_CHANGE, envp);
}

static void dp_display_update_mst_state(struct dp_display_private *dp,
					bool state)
{
	dp->mst.mst_active = state;
	dp->panel->mst_state = state;
}

static void dp_display_process_mst_hpd_high(struct dp_display_private *dp,
						bool mst_probe)
{
	bool is_mst_receiver;
	const int clear_mstm_ctrl_timeout = 100000;
	u8 old_mstm_ctrl;
	int ret;

	if (!dp->parser->has_mst || !dp->mst.drm_registered) {
		DP_MST_DEBUG("DP%d mst not enabled. has_mst:%d, registered:%d\n",
				dp->cell_idx, dp->parser->has_mst, dp->mst.drm_registered);
		return;
	}

	DP_MST_DEBUG("DP%d mst_hpd_high work. mst_probe:%d\n",
			dp->cell_idx, mst_probe);

	if (!dp->mst.mst_active) {
		is_mst_receiver = dp->panel->read_mst_cap(dp->panel);

		if (!is_mst_receiver) {
			DP_MST_DEBUG("DP%d sink doesn't support mst\n", dp->cell_idx);
			return;
		}

		/* clear sink mst state */
		drm_dp_dpcd_readb(dp->aux->drm_aux, DP_MSTM_CTRL,
				&old_mstm_ctrl);
		drm_dp_dpcd_writeb(dp->aux->drm_aux, DP_MSTM_CTRL, 0);

		/* add extra delay if MST state is not cleared */
		if (old_mstm_ctrl) {
			DP_MST_DEBUG("DP%d MSTM_CTRL is not cleared, wait %dus\n",
					dp->cell_idx, clear_mstm_ctrl_timeout);
			usleep_range(clear_mstm_ctrl_timeout,
				clear_mstm_ctrl_timeout + 1000);
		}

		ret = drm_dp_dpcd_writeb(dp->aux->drm_aux, DP_MSTM_CTRL,
				 DP_MST_EN | DP_UP_REQ_EN | DP_UPSTREAM_IS_SRC);
		if (ret < 0) {
			pr_err("DP%d sink mst enablement failed\n", dp->cell_idx);
			return;
		}

		dp_display_update_mst_state(dp, true);
	} else if (dp->mst.mst_active && mst_probe) {
		if (dp->mst.cbs.hpd)
			dp->mst.cbs.hpd(&dp->dp_display, true);
	}

	DP_MST_DEBUG("DP%d mst_hpd_high. mst_active:%d\n",
			dp->cell_idx, dp->mst.mst_active);
}

static void dp_display_change_phy_bond_mode(struct dp_display_private *dp,
		enum dp_phy_bond_mode mode)
{
	if (dp->phy_bond_mode != mode)
		pr_info("DP%d  %d -> %d\n", dp->cell_idx,
				dp->phy_bond_mode, mode);

	dp->phy_bond_mode = mode;
	/* Propagate to dp_ctrl, dp_catalog, dp_power and dp_panel */
	dp->ctrl->set_phy_bond_mode(dp->ctrl, mode);
}

static void dp_display_host_init(struct dp_display_private *dp)
{
	bool flip = false;
	bool reset;

	if (dp->core_initialized)
		return;

	if (dp->hpd->orientation == ORIENTATION_CC2)
		flip = true;

	/* avoid phy reset when doing continuous splash */
	reset = ((dp->parser->is_cont_splash_enabled || dp->debug->sim_mode) ?
			false : (!dp->hpd->multi_func ||
			!dp->hpd->peer_usb_comm));

	dp->power->init(dp->power, flip);
	dp->hpd->host_init(dp->hpd, &dp->catalog->hpd);
	enable_irq(dp->irq);
	dp->ctrl->init(dp->ctrl, flip, reset);
	dp_display_abort_hdcp(dp, false);
	dp->aux->init(dp->aux, dp->parser->aux_cfg);
	dp->panel->init(dp->panel);
	dp->core_initialized = true;

	/* log this as it results from user action of cable connection */
	pr_info("DP%d [OK]\n", dp->cell_idx);
}

static void dp_display_host_deinit(struct dp_display_private *dp)
{
	if (!dp->core_initialized)
		return;

	if (dp->active_stream_cnt) {
		pr_debug("DP%d active stream present\n", dp->cell_idx);
		return;
	}

	dp->aux->deinit(dp->aux);
	dp_display_abort_hdcp(dp, true);
	dp->ctrl->deinit(dp->ctrl);
	dp->hpd->host_deinit(dp->hpd, &dp->catalog->hpd);
	dp->power->deinit(dp->power);
	disable_irq(dp->irq);
	dp->core_initialized = false;
	dp->aux->state = 0;

	/* log this as it results from user action of cable dis-connection */
	pr_info("DP%d [OK]\n", dp->cell_idx);
}

static int dp_display_process_hpd_high(struct dp_display_private *dp)
{
	int rc = -EINVAL;

	pr_debug("DP%d\n", dp->cell_idx);
	mutex_lock(&dp->session_lock);

	if (dp->is_connected) {
		pr_debug("DP%d already connected, skipping hpd high processing\n",
				dp->cell_idx);
		mutex_unlock(&dp->session_lock);
		return -EISCONN;
	}

	dp->is_connected = true;

	dp->dp_display.max_pclk_khz = min(dp->parser->max_pclk_khz,
					dp->debug->max_pclk_khz);
	dp->dp_display.force_bond_mode = dp->parser->force_bond_mode ||
					dp->debug->force_bond_mode;
	dp->dp_display.max_hdisplay = dp->parser->max_hdisplay;
	dp->dp_display.max_vdisplay = dp->parser->max_vdisplay;

	dp_display_host_init(dp);

	dp->link->psm_config(dp->link, &dp->panel->link_info, false);
	dp->debug->psm_enabled = false;

	if (!dp->dp_display.base_connector)
		goto end;

	rc = dp->panel->read_sink_caps(dp->panel,
			dp->dp_display.base_connector, dp->hpd->multi_func);
	/*
	 * ETIMEDOUT --> cable may have been removed
	 * ENOTCONN --> no downstream device connected
	 */
	if (rc == -ETIMEDOUT || rc == -ENOTCONN) {
		dp->is_connected = false;
		goto end;
	}

	dp->link->process_request(dp->link);
	dp->panel->handle_sink_request(dp->panel);

	dp_display_process_mst_hpd_high(dp, false);

	rc = dp->ctrl->on(dp->ctrl, dp->mst.mst_active,
			dp->panel->fec_en, dp->panel->dsc_en,
			dp->parser->force_connect_mode ?
			LINK_TRAINING_MODE_FORCE : LINK_TRAINING_MODE_NORMAL);
	if (rc) {
		dp->is_connected = false;
		goto end;
	}

	dp->process_hpd_connect = false;

	dp_display_process_mst_hpd_high(dp, true);
end:
	mutex_unlock(&dp->session_lock);

	if (!rc)
		dp_display_send_hpd_notification(dp);

	if (dp->parser->force_connect_mode)
		dp_display_send_force_connect_event(dp);

	return rc;
}

static void dp_display_process_mst_hpd_low(struct dp_display_private *dp)
{
	if (dp->mst.mst_active) {
		DP_MST_DEBUG("DP%d mst_hpd_low work\n", dp->cell_idx);

		if (dp->mst.cbs.hpd)
			dp->mst.cbs.hpd(&dp->dp_display, false);

		dp_display_update_mst_state(dp, false);
	}

	DP_MST_DEBUG("DP%d mst_hpd_low. mst_active:%d\n",
			dp->cell_idx, dp->mst.mst_active);
}

static void dp_display_process_hpd_low(struct dp_display_private *dp)
{
	struct dp_link_hdcp_status *status;

	mutex_lock(&dp->session_lock);

	status = &dp->link->hdcp_status;
	dp->is_connected = false;
	dp->process_hpd_connect = false;

	if (dp_display_is_hdcp_enabled(dp) &&
			status->hdcp_state != HDCP_STATE_INACTIVE) {
		cancel_delayed_work_sync(&dp->hdcp_cb_work);
		if (dp->hdcp.ops->off)
			dp->hdcp.ops->off(dp->hdcp.data);

		dp_display_update_hdcp_status(dp, true);
	}

	dp_display_audio_enable(dp, false);

	mutex_unlock(&dp->session_lock);

	dp_display_process_mst_hpd_low(dp);

	dp_display_send_hpd_notification(dp);

	dp->panel->video_test = false;
}

static int dp_display_usbpd_configure_cb(struct device *dev)
{
	int rc = 0;
	struct dp_display_private *dp;

	if (!dev) {
		pr_err("invalid dev\n");
		rc = -EINVAL;
		goto end;
	}

	dp = dev_get_drvdata(dev);
	if (!dp) {
		pr_err("no driver data found\n");
		rc = -ENODEV;
		goto end;
	}

	/*
	 * When dp is connected during boot, there is a chance that
	 * configure_cb is called before drm probe is finished and
	 * cause host_init failure. Here we poll the value of
	 * poll_enabled and wait until drm driver is ready.
	 */
	if (!dp->dp_display.drm_dev->mode_config.poll_enabled) {
		const int poll_timeout = 10000;
		int i;

		for (i = 0; !dp->dp_display.drm_dev->mode_config.poll_enabled &&
				i < poll_timeout; i++)
			usleep_range(1000, 1100);

		if (i == poll_timeout) {
			pr_err("DP%d driver is not loaded\n", dp->cell_idx);
			rc = -ENODEV;
			goto end;
		}
	}

	if (!dp->debug->sim_mode && !dp->parser->no_aux_switch
	    && !dp->parser->gpio_aux_switch) {
		rc = dp->aux->aux_switch(dp->aux, true, dp->hpd->orientation);
		if (rc)
			goto end;
	}

	mutex_lock(&dp->session_lock);
	dp_display_host_init(dp);

	/* check for hpd high */
	if (dp->hpd->hpd_high)
		queue_work(dp->wq, &dp->connect_work);
	else
		dp->process_hpd_connect = true;
	mutex_unlock(&dp->session_lock);
end:
	return rc;
}

static int dp_display_stream_pre_disable(struct dp_display_private *dp,
			struct dp_panel *dp_panel)
{
	dp->ctrl->stream_pre_off(dp->ctrl, dp_panel);

	return 0;
}

static void dp_display_stream_disable(struct dp_display_private *dp,
			struct dp_panel *dp_panel)
{
	if (!dp->active_stream_cnt) {
		pr_err("DP%d invalid active_stream_cnt (%d)\n", dp->cell_idx,
				dp->active_stream_cnt);
		return;
	}

	if (dp_panel->stream_id == DP_STREAM_MAX ||
			!dp->active_panels[dp_panel->stream_id]) {
		pr_err("DP%d panel is already disabled\n", dp->cell_idx);
		return;
	}

	pr_debug("DP%d stream_id=%d, active_stream_cnt=%d\n",
			dp->cell_idx, dp_panel->stream_id, dp->active_stream_cnt);

	dp->ctrl->stream_off(dp->ctrl, dp_panel);
	dp->active_panels[dp_panel->stream_id] = NULL;
	dp->active_stream_cnt--;
}

static void dp_display_clean(struct dp_display_private *dp)
{
	int idx;
	struct dp_panel *dp_panel;
	struct dp_link_hdcp_status *status = &dp->link->hdcp_status;

	pr_debug("DP%d\n", dp->cell_idx);
	if (dp_display_is_hdcp_enabled(dp) &&
			status->hdcp_state != HDCP_STATE_INACTIVE) {
		cancel_delayed_work_sync(&dp->hdcp_cb_work);
		if (dp->hdcp.ops->off)
			dp->hdcp.ops->off(dp->hdcp.data);

		dp_display_update_hdcp_status(dp, true);
	}

	for (idx = DP_STREAM_0; idx < DP_STREAM_MAX; idx++) {
		if (!dp->active_panels[idx])
			continue;

		dp_panel = dp->active_panels[idx];

		if (dp_panel->audio_supported)
			dp_panel->audio->off(dp_panel->audio);

		dp_display_stream_pre_disable(dp, dp_panel);
		dp_display_stream_disable(dp, dp_panel);
		dp_panel->deinit(dp_panel, 0);
	}

	dp->power_on = false;
	dp->ctrl->off(dp->ctrl);
}

static void dp_display_handle_disconnect(struct dp_display_private *dp)
{
	pr_debug("DP%d\n", dp->cell_idx);
	if (dp->parser->force_connect_mode) {
		/*
		 * switch from normal mode to simulation mode. update EDID
		 * and send hotplug to user. this gives user a chance to
		 * update the mode if simulation EDID is different than
		 * current EDID.
		 */
		mutex_lock(&dp->session_lock);
		dp_sim_set_sim_mode(dp->aux_bridge, DP_SIM_MODE_ALL);
		mutex_unlock(&dp->session_lock);

		/*
		 * Get out of abort status, so that link training and
		 * stream enabling can be performed for simulation mode.
		 */
		dp->aux->abort(dp->aux, true);
		dp->ctrl->abort(dp->ctrl, true);
		atomic_set(&dp->aborted, 0);

		dp_display_send_force_connect_event(dp);

		dp_display_process_hpd_high(dp);

		/* If stream isn't running, started here */
		if (!dp->power_on && dp->dp_display.base_connector)
			sde_connector_helper_mode_change_commit(
					dp->dp_display.base_connector);
		return;
	}

	dp_display_process_hpd_low(dp);

	/* cancel any pending request */
	dp->ctrl->abort(dp->ctrl, false);
	dp->aux->abort(dp->aux, false);

	mutex_lock(&dp->session_lock);
	if (!dp->active_stream_cnt && !IS_BOND_MODE(dp->phy_bond_mode)) {
		dp_display_clean(dp);
		dp_display_host_deinit(dp);
	}
	mutex_unlock(&dp->session_lock);
}

static void dp_display_disconnect_sync(struct dp_display_private *dp)
{
	/* cancel any pending request */
	pr_debug("DP%d\n", dp->cell_idx);
	atomic_set(&dp->aborted, 1);
	dp->ctrl->abort(dp->ctrl, false);
	dp->aux->abort(dp->aux, false);

	/* wait for idle state */
	cancel_work_sync(&dp->connect_work);
	cancel_work_sync(&dp->attention_work);
	flush_workqueue(dp->wq);

	dp_display_handle_disconnect(dp);

	/* Reset abort value to allow future connections */
	atomic_set(&dp->aborted, 0);
}

static int dp_display_usbpd_disconnect_cb(struct device *dev)
{
	int rc = 0;
	struct dp_display_private *dp;

	if (!dev) {
		pr_err("invalid dev\n");
		rc = -EINVAL;
		goto end;
	}

	dp = dev_get_drvdata(dev);
	if (!dp) {
		pr_err("no driver data found\n");
		rc = -ENODEV;
		goto end;
	}

	dp_display_disconnect_sync(dp);

	if (!dp->debug->sim_mode && !dp->parser->no_aux_switch
	    && !dp->parser->gpio_aux_switch)
		dp->aux->aux_switch(dp->aux, false, ORIENTATION_NONE);
end:
	return rc;
}

static int dp_display_stream_enable(struct dp_display_private *dp,
			struct dp_panel *dp_panel)
{
	int rc = 0;

	rc = dp->ctrl->stream_on(dp->ctrl, dp_panel);

	if (dp->debug->tpg_state)
		dp_panel->tpg_config(dp_panel, true);

	if (!rc) {
		dp->active_panels[dp_panel->stream_id] = dp_panel;
		dp->active_stream_cnt++;
	}

	pr_debug("DP%d active_stream_cnt:%d\n",
			dp->cell_idx, dp->active_stream_cnt);

	return rc;
}

static void dp_display_mst_attention(struct dp_display_private *dp)
{
	if (dp->mst.mst_active && dp->mst.cbs.hpd_irq)
		dp->mst.cbs.hpd_irq(&dp->dp_display);

	DP_MST_DEBUG("DP%d mst_attention_work. mst_active:%d\n",
			dp->cell_idx, dp->mst.mst_active);
}

static void dp_display_attention_work(struct work_struct *work)
{
	struct dp_display_private *dp = container_of(work,
			struct dp_display_private, attention_work);

	mutex_lock(&dp->session_lock);

	if (!dp->core_initialized) {
		mutex_unlock(&dp->session_lock);
		goto mst_attention;
	}

	if (dp->link->process_request(dp->link)) {
		mutex_unlock(&dp->session_lock);
		goto cp_irq;
	}

	mutex_unlock(&dp->session_lock);

	if (dp->link->sink_request & DS_PORT_STATUS_CHANGED) {
		if (dp_display_is_sink_count_zero(dp)) {
			dp_display_handle_disconnect(dp);
		} else {
			if (!dp->mst.mst_active) {
				dp_display_handle_disconnect(dp);
				queue_work(dp->wq, &dp->connect_work);
			}
		}

		goto mst_attention;
	}

	if (dp->link->sink_request & DP_TEST_LINK_VIDEO_PATTERN) {
		dp_display_handle_disconnect(dp);

		dp->panel->video_test = true;
		queue_work(dp->wq, &dp->connect_work);

		goto mst_attention;
	}

	/*
	 * This is for GPIO based HPD only, that if HPD low is detected
	 * as HPD_IRQ, we need to handle TEST_EDID_READ in this function.
	 */
	if ((dp->parser->no_aux_switch && !dp->parser->lphw_hpd) &&
			(dp->link->sink_request & DP_TEST_LINK_EDID_READ)) {
		dp_display_handle_disconnect(dp);
		queue_work(dp->wq, &dp->connect_work);
		goto mst_attention;
	}

	if ((dp->link->sink_request & DP_TEST_LINK_PHY_TEST_PATTERN) ||
			(dp->link->sink_request & DP_TEST_LINK_TRAINING) ||
			(dp->link->sink_request & DP_LINK_STATUS_UPDATED)) {
		mutex_lock(&dp->session_lock);
		dp_display_audio_enable(dp, false);
		mutex_unlock(&dp->session_lock);

		if (dp->link->sink_request & DP_TEST_LINK_PHY_TEST_PATTERN) {
			dp->ctrl->process_phy_test_request(dp->ctrl);
		} else if (dp->link->sink_request & DP_TEST_LINK_TRAINING) {
			dp->link->send_test_response(dp->link);
			dp->ctrl->link_maintenance(dp->ctrl);
		} else if (dp->link->sink_request & DP_LINK_STATUS_UPDATED) {
			/*
			 * This is for GPIO based HPD only, that if HPD low is
			 * detected as HPD_IRQ, we need to treat
			 * LINK_STATUS_UPDATED as HPD high.
			 */
			if (dp->parser->no_aux_switch &&
					!dp->parser->lphw_hpd) {
				dp_display_handle_disconnect(dp);
				queue_work(dp->wq, &dp->connect_work);
				goto mst_attention;
			} else {
				dp->ctrl->link_maintenance(dp->ctrl);
			}
		}

		mutex_lock(&dp->session_lock);
		dp_display_audio_enable(dp, true);
		mutex_unlock(&dp->session_lock);
		goto mst_attention;
	}
cp_irq:
	if (dp_display_is_hdcp_enabled(dp) && dp->hdcp.ops->cp_irq)
		dp->hdcp.ops->cp_irq(dp->hdcp.data);
mst_attention:
	dp_display_mst_attention(dp);
}

static int dp_display_usbpd_attention_cb(struct device *dev)
{
	struct dp_display_private *dp;

	if (!dev) {
		pr_err("invalid dev\n");
		return -EINVAL;
	}

	dp = dev_get_drvdata(dev);
	if (!dp) {
		pr_err("no driver data found\n");
		return -ENODEV;
	}

	pr_debug("DP%d hpd_irq:%d, hpd_high:%d, power_on:%d, is_connected:%d\n",
			dp->cell_idx, dp->hpd->hpd_irq, dp->hpd->hpd_high,
			dp->power_on, dp->is_connected);

	if (!dp->hpd->hpd_high)
		dp_display_disconnect_sync(dp);
	else if (dp->hpd->hpd_irq && dp->core_initialized)
		queue_work(dp->wq, &dp->attention_work);
	else if (dp->process_hpd_connect || !dp->is_connected)
		queue_work(dp->wq, &dp->connect_work);
	else
		pr_debug("DP%d ignored\n", dp->cell_idx);

	return 0;
}

static void dp_display_connect_work(struct work_struct *work)
{
	int rc = 0;
	struct dp_display_private *dp = container_of(work,
			struct dp_display_private, connect_work);
	struct drm_connector *reset_connector = NULL;

	pr_debug("DP%d\n", dp->cell_idx);

	if (atomic_read(&dp->aborted)) {
		pr_warn("HPD off requested\n");
		return;
	}

	if (!dp->hpd->hpd_high) {
		pr_warn("Sink disconnected\n");
		return;
	}

	mutex_lock(&dp->session_lock);

	/*
	 * Reset panel as link param may change during link training.
	 * MST panel or SST panel in video test mode will reset immediately.
	 * SST panel in normal mode will reset by the mode change commit.
	 */
	if (dp->active_stream_cnt) {
		if (IS_BOND_MODE(dp->phy_bond_mode)) {
			dp->aux->abort(dp->aux, true);
			dp->ctrl->abort(dp->ctrl, true);
			reset_connector = dp->bond_primary;
		} else if (dp->active_panels[DP_STREAM_0] == dp->panel &&
				!dp->panel->video_test) {
			dp->aux->abort(dp->aux, true);
			dp->ctrl->abort(dp->ctrl, true);
			reset_connector = dp->dp_display.base_connector;
		} else {
			dp_display_clean(dp);
			dp_display_host_deinit(dp);
		}
	}

	if (dp->parser->force_connect_mode) {
		if (!reset_connector) {
			dp_display_clean(dp);
			dp_display_host_deinit(dp);
		}
		dp->is_connected = false;
		dp_display_process_mst_hpd_low(dp);
		dp_sim_set_sim_mode(dp->aux_bridge, 0);
		dp->aux->state = 0;
		dp_display_send_force_connect_event(dp);
	}

	mutex_unlock(&dp->session_lock);

	rc = dp_display_process_hpd_high(dp);

	if (!rc && dp->panel->video_test)
		dp->link->send_test_response(dp->link);

	if (reset_connector)
		sde_connector_helper_mode_change_commit(reset_connector);
}

static int dp_display_usb_notifier(struct notifier_block *nb,
	unsigned long event, void *ptr)
{
	struct extcon_dev *edev = ptr;
	struct dp_display_private *dp = container_of(nb,
			struct dp_display_private, usb_nb);
	if (!edev)
		goto end;

	if (!event && dp->debug->sim_mode) {
		dp_display_disconnect_sync(dp);
		dp->debug->abort(dp->debug);
	}
end:
	return NOTIFY_DONE;
}

static int dp_display_get_usb_extcon(struct dp_display_private *dp)
{
	struct extcon_dev *edev;
	int rc;

	edev = extcon_get_edev_by_phandle(&dp->pdev->dev, 0);
	if (IS_ERR(edev))
		return PTR_ERR(edev);

	dp->usb_nb.notifier_call = dp_display_usb_notifier;
	dp->usb_nb.priority = 2;
	rc = extcon_register_notifier(edev, EXTCON_USB, &dp->usb_nb);
	if (rc)
		pr_err("DP%d failed to register for usb event: %d\n",
				dp->cell_idx, rc);

	return rc;
}

static void dp_display_deinit_sub_modules(struct dp_display_private *dp)
{
	dp_audio_put(dp->panel->audio);
	dp_ctrl_put(dp->ctrl);
	dp_link_put(dp->link);
	dp_panel_put(dp->panel);
	dp_aux_put(dp->aux);
	dp_power_put(dp->power);
	dp_catalog_put(dp->catalog);
	dp_parser_put(dp->parser);
	dp_hpd_put(dp->hpd);
	mutex_destroy(&dp->session_lock);
	dp_debug_put(dp->debug);
}

static int dp_init_sub_modules(struct dp_display_private *dp)
{
	int rc = 0;
	bool hdcp_disabled;
	struct device *dev = &dp->pdev->dev;
	struct dp_hpd_cb *cb = &dp->hpd_cb;
	struct dp_ctrl_in ctrl_in = {
		.dev = dev,
	};
	struct dp_panel_in panel_in = {
		.dev = dev,
	};
	struct dp_debug_in debug_in = {
		.dev = dev,
	};

	mutex_init(&dp->session_lock);

	dp->parser = dp_parser_get(dp->pdev);
	if (IS_ERR(dp->parser)) {
		rc = PTR_ERR(dp->parser);
		pr_err("DP%d failed to initialize parser, rc = %d\n",
				dp->cell_idx, rc);
		dp->parser = NULL;
		goto error;
	}

	rc = dp->parser->parse(dp->parser);
	if (rc) {
		pr_err("DP%d device tree parsing failed\n", dp->cell_idx);
		goto error_catalog;
	}

	dp->dp_display.is_mst_supported = dp->parser->has_mst;
	dp->dp_display.no_mst_encoder = dp->parser->no_mst_encoder;

	dp->catalog = dp_catalog_get(dev, dp->cell_idx, dp->parser);
	if (IS_ERR(dp->catalog)) {
		rc = PTR_ERR(dp->catalog);
		pr_err("DP%d failed to initialize catalog, rc = %d\n",
				dp->cell_idx, rc);
		dp->catalog = NULL;
		goto error_catalog;
	}

	dp->power = dp_power_get(dp->parser);
	if (IS_ERR(dp->power)) {
		rc = PTR_ERR(dp->power);
		pr_err("DP%d failed to initialize power, rc = %d\n",
				dp->cell_idx, rc);
		dp->power = NULL;
		goto error_power;
	}

	rc = dp->power->power_client_init(dp->power, &dp->priv->phandle,
		dp->dp_display.drm_dev);
	if (rc) {
		pr_err("DP%d Power client create failed\n", dp->cell_idx);
		goto error_aux;
	}

	dp->aux = dp_aux_get(dev, &dp->catalog->aux, dp->parser,
			dp->aux_switch_node, dp->aux_bridge);
	if (IS_ERR(dp->aux)) {
		rc = PTR_ERR(dp->aux);
		pr_err("DP%d failed to initialize aux, rc = %d\n", dp->cell_idx, rc);
		dp->aux = NULL;
		goto error_aux;
	}

	rc = dp->aux->drm_aux_register(dp->aux);
	if (rc) {
		pr_err("DP%d DRM DP AUX register failed\n", dp->cell_idx);
		goto error_link;
	}

	dp->link = dp_link_get(dev, dp->aux);
	if (IS_ERR(dp->link)) {
		rc = PTR_ERR(dp->link);
		pr_err("DP%d failed to initialize link, rc = %d\n", dp->cell_idx, rc);
		dp->link = NULL;
		goto error_link;
	}

	panel_in.aux = dp->aux;
	panel_in.catalog = &dp->catalog->panel;
	panel_in.link = dp->link;
	panel_in.connector = dp->dp_display.base_connector;
	panel_in.base_panel = NULL;
	panel_in.parser = dp->parser;

	dp->panel = dp_panel_get(&panel_in);
	if (IS_ERR(dp->panel)) {
		rc = PTR_ERR(dp->panel);
		pr_err("DP%d failed to initialize panel, rc = %d\n", dp->cell_idx, rc);
		dp->panel = NULL;
		goto error_panel;
	}

	ctrl_in.cell_idx = dp->cell_idx;
	ctrl_in.link = dp->link;
	ctrl_in.panel = dp->panel;
	ctrl_in.aux = dp->aux;
	ctrl_in.power = dp->power;
	ctrl_in.catalog = &dp->catalog->ctrl;
	ctrl_in.parser = dp->parser;

	dp->ctrl = dp_ctrl_get(&ctrl_in);
	if (IS_ERR(dp->ctrl)) {
		rc = PTR_ERR(dp->ctrl);
		pr_err("DP%d failed to initialize ctrl, rc = %d\n", dp->cell_idx, rc);
		dp->ctrl = NULL;
		goto error_ctrl;
	}

	dp->panel->audio = dp_audio_get(dp->pdev, dp->panel,
						&dp->catalog->audio);
	if (IS_ERR(dp->panel->audio)) {
		rc = PTR_ERR(dp->panel->audio);
		pr_err("DP%d failed to initialize audio, rc = %d\n", dp->cell_idx, rc);
		dp->panel->audio = NULL;
		goto error_audio;
	}

	memset(&dp->mst, 0, sizeof(dp->mst));
	dp->active_stream_cnt = 0;

	cb->configure  = dp_display_usbpd_configure_cb;
	cb->disconnect = dp_display_usbpd_disconnect_cb;
	cb->attention  = dp_display_usbpd_attention_cb;

	dp->hpd = dp_hpd_get(dev, dp->parser, &dp->catalog->hpd,
			dp->aux_bridge, cb);
	if (IS_ERR_OR_NULL(dp->hpd)) {
		rc = PTR_ERR(dp->hpd);
		pr_err("DP%d failed to initialize hpd, rc = %d\n", dp->cell_idx, rc);
		dp->hpd = NULL;
		goto error_hpd;
	}

	hdcp_disabled = !!dp_display_initialize_hdcp(dp);

	debug_in.panel = dp->panel;
	debug_in.hpd = dp->hpd;
	debug_in.link = dp->link;
	debug_in.aux = dp->aux;
	debug_in.connector = &dp->dp_display.base_connector;
	debug_in.catalog = dp->catalog;
	debug_in.parser = dp->parser;
	debug_in.ctrl = dp->ctrl;
	debug_in.power = dp->power;
	debug_in.index = dp->cell_idx;

	dp->debug = dp_debug_get(&debug_in);
	if (IS_ERR(dp->debug)) {
		rc = PTR_ERR(dp->debug);
		pr_err("DP%d failed to initialize debug, rc = %d\n", dp->cell_idx, rc);
		dp->debug = NULL;
		goto error_debug;
	}
	dp->debug->hdcp_wait_sink_sync =
		dp->parser->hdcp_wait_sink_sync_enabled;

	dp->tot_dsc_blks_in_use = 0;

	dp->debug->hdcp_disabled = hdcp_disabled;
	dp_display_update_hdcp_status(dp, true);

	dp_display_get_usb_extcon(dp);

	if (dp->hpd->register_hpd) {
		rc = dp->hpd->register_hpd(dp->hpd);
		if (rc) {
			pr_err("failed register hpd\n");
			goto error_hpd_reg;
		}
	}

	if (dp->parser->force_connect_mode) {
		/*
		 * always enter simulation first regardless of the actual
		 * connection state to make connector always connected.
		 * this will fix the corner case when user tries to read
		 * connector modes when link training is still running.
		 */
		dp_sim_set_sim_mode(dp->aux_bridge, DP_SIM_MODE_ALL);
		dp_display_process_hpd_high(dp);
	}

	return rc;
error_hpd_reg:
	dp_debug_put(dp->debug);
error_debug:
	dp_hpd_put(dp->hpd);
error_hpd:
	dp_audio_put(dp->panel->audio);
error_audio:
	dp_ctrl_put(dp->ctrl);
error_ctrl:
	dp_panel_put(dp->panel);
error_panel:
	dp_link_put(dp->link);
error_link:
	dp_aux_put(dp->aux);
error_aux:
	dp_power_put(dp->power);
error_power:
	dp_catalog_put(dp->catalog);
error_catalog:
	dp_parser_put(dp->parser);
error:
	mutex_destroy(&dp->session_lock);
	return rc;
}

static int dp_display_post_init(struct dp_display *dp_display)
{
	int rc = 0;
	struct dp_display_private *dp;

	if (!dp_display) {
		pr_err("invalid input\n");
		rc = -EINVAL;
		goto end;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	if (IS_ERR_OR_NULL(dp)) {
		pr_err("invalid params\n");
		rc = -EINVAL;
		goto end;
	}

	rc = dp_init_sub_modules(dp);
	if (rc)
		goto end;

	dp_display->post_init = NULL;
end:
	pr_debug("DP%d %s\n", dp->cell_idx, rc ? "failed" : "success");
	return rc;
}

static int dp_display_set_mode(struct dp_display *dp_display, void *panel,
		struct dp_display_mode *mode)
{
	const u32 num_components = 3, default_bpp = 24;
	struct dp_display_private *dp;
	struct dp_panel *dp_panel;

	if (!dp_display || !panel) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp_panel = panel;
	if (!dp_panel->connector) {
		pr_err("invalid connector input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	mutex_lock(&dp->session_lock);
	mode->timing.bpp =
		dp_panel->connector->display_info.bpc * num_components;
	if (!mode->timing.bpp)
		mode->timing.bpp = default_bpp;

	mode->timing.bpp = dp->panel->get_mode_bpp(dp->panel,
			mode->timing.bpp, mode->timing.pixel_clk_khz);

	dp_panel->pinfo = mode->timing;
	mutex_unlock(&dp->session_lock);

	return 0;
}

/**
 * dp_display_cont_splash_config() - initialize splash resources
 * @display:    Handle to display
 * Return:      Zero on Success
 */
int dp_display_cont_splash_config(void *display)
{
	int rc = 0;
	struct dp_display *dp_display = display;
	struct dp_display_private *dp;

	if (!dp_display) {
		pr_err("invalid input display param\n");
		rc = -EINVAL;
		return rc;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	if (IS_ERR_OR_NULL(dp)) {
		pr_err("invalid params\n");
		rc = -EINVAL;
		return rc;
	}

	mutex_lock(&dp->session_lock);

	rc = pm_runtime_get_sync(dp_display->drm_dev->dev);
	if (rc < 0) {
		pr_err("DP%d failed to vote gdsc for continuous splash, rc=%d\n",
				dp->cell_idx, rc);
		return rc;
	}

	dp->parser->is_cont_splash_enabled = true;

	/* vote for core, link and stream clocks */
	if (dp->power->clk_enable) {
		dp->power->clk_enable(dp->power, DP_CORE_PM, true);
		dp->power->clk_enable(dp->power, DP_LINK_PM, true);
		/* DP SST mode */
		if (dp->panel->stream_id == DP_STREAM_0)
			dp->power->clk_enable(dp->power, DP_STREAM0_PM, true);
	}

	mutex_unlock(&dp->session_lock);
	return rc;
}

/*
 * dp_display_splash_res_cleanup() - cleanup for continuous splash
 * @display:    Pointer to DP display
 * return:      Zero on success
 */

int dp_display_splash_res_cleanup(struct dp_display *dp_display)
{
	int rc = 0;
	struct dp_display_private *dp;

	if (!dp_display) {
		pr_err("invalid input display param\n");
		rc = -EINVAL;
		return rc;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	if (IS_ERR_OR_NULL(dp)) {
		pr_err("invalid params\n");
		rc = -EINVAL;
		return rc;
	}

	if (!dp->parser->is_cont_splash_enabled)
		return 0;

	pm_runtime_put_sync(dp_display->drm_dev->dev);

	/* unvote for core, link and stream clocks */
	if (dp->power->clk_enable) {
		dp->power->clk_enable(dp->power, DP_CORE_PM, false);
		dp->power->clk_enable(dp->power, DP_LINK_PM, false);
		/* DP SST mode */
		if (dp->panel->stream_id == DP_STREAM_0)
			dp->power->clk_enable(dp->power, DP_STREAM0_PM, false);
	}
	dp->parser->is_cont_splash_enabled = false;

	return rc;
}

static int dp_display_prepare(struct dp_display *dp_display, void *panel)
{
	struct dp_display_private *dp;
	struct dp_panel *dp_panel;
	int rc = 0;

	if (!dp_display || !panel) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp_panel = panel;
	if (!dp_panel->connector) {
		pr_err("invalid connector input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	pr_debug("DP%d\n", dp->cell_idx);

	mutex_lock(&dp->session_lock);

	if (atomic_read(&dp->aborted))
		goto end;

	if (dp->power_on)
		goto end;

	if (!dp_display_is_ready(dp) && !dp->parser->force_connect_mode)
		goto end;

	dp_display_host_init(dp);

	if (dp->debug->psm_enabled) {
		dp->link->psm_config(dp->link, &dp->panel->link_info, false);
		dp->debug->psm_enabled = false;
	}

	/*
	 * Execute the dp controller power on in shallow mode here.
	 * In normal cases, controller should have been powered on
	 * by now. In some cases like suspend/resume or framework
	 * reboot, we end up here without a powered on controller.
	 * Cable may have been removed in suspended state. In that
	 * case, link training is bound to fail on system resume.
	 * So, we execute in shallow mode here to do only minimal
	 * and required things.
	 */
	rc = dp->ctrl->on(dp->ctrl, dp->mst.mst_active, dp_panel->fec_en,
			dp_panel->dsc_en,
			dp->parser->force_connect_mode ?
			LINK_TRAINING_MODE_NORMAL : LINK_TRAINING_MODE_SHALLOW);
	if (rc)
		goto end;

end:
	mutex_unlock(&dp->session_lock);

	if (dp->parser->force_connect_mode)
		dp_display_send_force_connect_event(dp);

	return 0;
}

static int dp_display_set_stream_info(struct dp_display *dp_display,
			void *panel, u32 strm_id, u32 start_slot,
			u32 num_slots, u32 pbn, int vcpi)
{
	int rc = 0;
	struct dp_panel *dp_panel;
	struct dp_display_private *dp;
	const int max_slots = 64;

	if (!dp_display) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	if (strm_id >= DP_STREAM_MAX) {
		pr_err("invalid stream id:%d\n", strm_id);
		return -EINVAL;
	}

	if (start_slot + num_slots > max_slots) {
		pr_err("invalid channel info received. start:%d, slots:%d\n",
				start_slot, num_slots);
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	mutex_lock(&dp->session_lock);

	dp->ctrl->set_mst_channel_info(dp->ctrl, strm_id,
			start_slot, num_slots);

	if (panel) {
		dp_panel = panel;
		dp_panel->set_stream_info(dp_panel, strm_id, start_slot,
				num_slots, pbn, vcpi);
	}

	mutex_unlock(&dp->session_lock);

	return rc;
}

static void dp_display_update_dsc_resources(struct dp_display_private *dp,
		struct dp_panel *panel, bool enable)
{
	u32 dsc_blk_cnt = 0;

	if (panel->pinfo.comp_info.comp_type == MSM_DISPLAY_COMPRESSION_DSC &&
		panel->pinfo.comp_info.comp_ratio) {
		dsc_blk_cnt = panel->pinfo.h_active /
				dp->parser->max_dp_dsc_input_width_pixs;
		if (panel->pinfo.h_active %
				dp->parser->max_dp_dsc_input_width_pixs)
			dsc_blk_cnt++;
	}

	if (enable) {
		dp->tot_dsc_blks_in_use += dsc_blk_cnt;
		panel->tot_dsc_blks_in_use += dsc_blk_cnt;
	} else {
		dp->tot_dsc_blks_in_use -= dsc_blk_cnt;
		panel->tot_dsc_blks_in_use -= dsc_blk_cnt;
	}
}

static int dp_display_enable(struct dp_display *dp_display, void *panel)
{
	int rc = 0;
	struct dp_display_private *dp;

	if (!dp_display || !panel) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	pr_debug("DP%d\n", dp->cell_idx);

	mutex_lock(&dp->session_lock);

	if (!dp->core_initialized) {
		pr_err("DP%d host not initialized\n", dp->cell_idx);
		goto end;
	}

	rc = dp_display_stream_enable(dp, panel);
	if (rc)
		goto end;

	dp_display_update_dsc_resources(dp, panel, true);
	dp->power_on = true;
end:
	mutex_unlock(&dp->session_lock);
	return rc;
}

static void dp_display_stream_post_enable(struct dp_display_private *dp,
			struct dp_panel *dp_panel)
{
	dp_panel->spd_config(dp_panel);
	dp_panel->setup_hdr(dp_panel, NULL);
}

static int dp_display_post_enable(struct dp_display *dp_display, void *panel)
{
	struct dp_display_private *dp;
	struct dp_panel *dp_panel;

	if (!dp_display || !panel) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	dp_panel = panel;
	pr_debug("DP%d\n", dp->cell_idx);

	mutex_lock(&dp->session_lock);

	if (!dp->power_on) {
		pr_debug("DP%d stream not setup, return\n", dp->cell_idx);
		goto end;
	}

	if (atomic_read(&dp->aborted))
		goto end;

	if (!dp_display_is_ready(dp) || !dp->core_initialized) {
		pr_debug("DP%d display not ready\n", dp->cell_idx);
		goto end;
	}

	dp_display_stream_post_enable(dp, dp_panel);

	if (dp_panel->audio_supported) {
		dp_panel->audio->bw_code = dp->link->link_params.bw_code;
		dp_panel->audio->lane_count = dp->link->link_params.lane_count;
		dp_panel->audio->on(dp_panel->audio);
	}

	if (dp->msm_hdcp_dev) {
		cancel_delayed_work_sync(&dp->hdcp_cb_work);
		queue_delayed_work(dp->wq, &dp->hdcp_cb_work, HZ);
	}

end:
	dp->aux->state |= DP_STATE_CTRL_POWERED_ON;

	if (dp->parser->force_connect_mode)
		dp_display_send_force_connect_event(dp);

	mutex_unlock(&dp->session_lock);
	return 0;
}

static int dp_display_pre_disable(struct dp_display *dp_display, void *panel)
{
	struct dp_display_private *dp;
	struct dp_panel *dp_panel = panel;
	struct dp_link_hdcp_status *status;
	int i, rc = 0;

	if (!dp_display || !panel) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	pr_debug("DP%d\n", dp->cell_idx);

	mutex_lock(&dp->session_lock);

	status = &dp->link->hdcp_status;

	if (!dp->power_on) {
		pr_debug("DP%d stream already powered off, return\n", dp->cell_idx);
		goto end;
	}

	dp->hdcp_abort = true;
	cancel_delayed_work_sync(&dp->hdcp_cb_work);
	if (dp_display_is_hdcp_enabled(dp) &&
			status->hdcp_state != HDCP_STATE_INACTIVE) {
		bool off = true;

		if (dp->suspended) {
			pr_debug("DP%d Can't perform HDCP cleanup while suspended. Defer\n",
					dp->cell_idx);
			dp->hdcp_delayed_off = true;
			goto clean;
		}

		if (dp->mst.mst_active) {
			dp_display_hdcp_deregister_stream(dp,
				dp_panel->stream_id);
			for (i = DP_STREAM_0; i < DP_STREAM_MAX; i++) {
				if (i != dp_panel->stream_id &&
						dp->active_panels[i]) {
					pr_debug("DP%d Streams are still active. Skip disabling HDCP\n",
							dp->cell_idx);
					off = false;
				}
			}
		}

		if (off) {
			if (dp->hdcp.ops->off)
				dp->hdcp.ops->off(dp->hdcp.data);
			dp_display_update_hdcp_status(dp, true);
		}
	}

clean:
	if (dp_panel->audio_supported)
		dp_panel->audio->off(dp_panel->audio);

	rc = dp_display_stream_pre_disable(dp, dp_panel);

end:
	mutex_unlock(&dp->session_lock);
	return 0;
}

static int dp_display_disable(struct dp_display *dp_display, void *panel)
{
	int i;
	struct dp_display_private *dp = NULL;
	struct dp_panel *dp_panel = NULL;
	struct dp_link_hdcp_status *status;

	if (!dp_display || !panel) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	dp_panel = panel;
	status = &dp->link->hdcp_status;
	pr_debug("DP%d\n", dp->cell_idx);

	mutex_lock(&dp->session_lock);

	if (!dp->power_on || !dp->core_initialized) {
		pr_debug("DP%d Link already powered off, return\n", dp->cell_idx);
		goto end;
	}

	dp_display_stream_disable(dp, dp_panel);
	dp_display_update_dsc_resources(dp, dp_panel, false);

	dp->hdcp_abort = false;
	for (i = DP_STREAM_0; i < DP_STREAM_MAX; i++) {
		if (dp->active_panels[i]) {
			if (status->hdcp_state != HDCP_STATE_AUTHENTICATED)
				queue_delayed_work(dp->wq, &dp->hdcp_cb_work,
						HZ/4);
			break;
		}
	}
end:
	mutex_unlock(&dp->session_lock);
	return 0;
}

static int dp_request_irq(struct dp_display *dp_display)
{
	int rc = 0;
	struct dp_display_private *dp;

	if (!dp_display) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	dp->irq = irq_of_parse_and_map(dp->pdev->dev.of_node, 0);
	if (dp->irq < 0) {
		rc = dp->irq;
		pr_err("DP%d failed to get irq: %d\n", dp->cell_idx, rc);
		return rc;
	}

	rc = devm_request_irq(&dp->pdev->dev, dp->irq, dp_display_irq,
		IRQF_TRIGGER_HIGH, "dp_display_isr", dp);
	if (rc < 0) {
		pr_err("DP%d failed to request IRQ%u: %d\n", dp->cell_idx,
				dp->irq, rc);
		return rc;
	}
	disable_irq(dp->irq);

	return 0;
}

static struct dp_debug *dp_get_debug(struct dp_display *dp_display)
{
	struct dp_display_private *dp;

	if (!dp_display) {
		pr_err("invalid input\n");
		return ERR_PTR(-EINVAL);
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	return dp->debug;
}

static int dp_display_unprepare(struct dp_display *dp_display, void *panel)
{
	struct dp_display_private *dp;
	struct dp_panel *dp_panel = panel;
	u32 flags = 0;

	if (!dp_display || !panel) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	pr_debug("DP%d\n", dp->cell_idx);

	mutex_lock(&dp->session_lock);

	/*
	 * Check if the power off sequence was triggered
	 * by a source initialated action like framework
	 * reboot or suspend-resume but not from normal
	 * hot plug.
	 */
	if (dp_display_is_ready(dp) || dp->parser->force_connect_mode)
		flags |= DP_PANEL_SRC_INITIATED_POWER_DOWN;

	/*
	 * If connector is in MST mode and not in suspend state, skip
	 * powering down host as aux need keep alive
	 * to handle hot-plug sideband message.
	 */
	if (dp->active_stream_cnt || (dp->mst.mst_active && !dp->suspended))
		goto end;

	/*
	 * There are monitors that can't resume from D3 mode after reboot,
	 * and we need to skip psm_config for these monitors. This option
	 * should only be used for non-pluggable monitors.
	 */
	if (!dp->parser->no_power_down) {
		dp->link->psm_config(dp->link, &dp->panel->link_info, true);
		dp->debug->psm_enabled = true;
	}

	dp->ctrl->off(dp->ctrl);
	dp_display_host_deinit(dp);

	dp->power_on = false;
	dp->aux->state = DP_STATE_CTRL_POWERED_OFF;

	if (dp->parser->force_connect_mode)
		dp_display_send_force_connect_event(dp);

	/* log this as it results from user action of cable dis-connection */
	pr_info("DP%d [OK]", dp->cell_idx);
end:
	/*
	 * Once the DP driver is turned off, set to non-bond mode.
	 * If bond mode is required afterwards, call set_phy_bond_mode.
	 */
	dp_display_change_phy_bond_mode(dp, DP_PHY_BOND_MODE_NONE);

	dp_panel->deinit(dp_panel, flags);
	mutex_unlock(&dp->session_lock);

	return 0;
}

static enum drm_mode_status dp_display_validate_mode(
		struct dp_display *dp_display,
		void *panel, struct drm_display_mode *mode)
{
	struct dp_display_private *dp;
	struct drm_dp_link *link_info;
	u32 mode_rate_khz = 0, supported_rate_khz = 0, mode_bpp = 0;
	struct dp_panel *dp_panel;
	enum drm_mode_status mode_status = MODE_BAD;
	int rate;
	struct dp_display_mode dp_mode;
	bool dsc_en;
	u32 pclk_khz;
	struct msm_drm_private *priv;
	struct sde_kms *sde_kms;
	u32 num_lm = 0;
	int rc = 0;

	if (!dp_display || !mode || !panel) {
		pr_err("invalid params\n");
		return mode_status;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	mutex_lock(&dp->session_lock);

	dp_panel = panel;
	if (!dp_panel->connector) {
		pr_err("invalid connector\n");
		goto end;
	}

	link_info = &dp->panel->link_info;

	dp_display->convert_to_dp_mode(dp_display, panel, mode, &dp_mode);

	dsc_en = dp_mode.timing.comp_info.comp_ratio ? true : false;
	mode_bpp = dsc_en ? dp_mode.timing.comp_info.dsc_info.bpp :
			dp_mode.timing.bpp;

	mode_rate_khz = mode->clock * mode_bpp;
	rate = drm_dp_bw_code_to_link_rate(dp->link->link_params.bw_code);
	supported_rate_khz = link_info->num_lanes * rate * 8;

	if (mode_rate_khz > supported_rate_khz) {
		DP_MST_DEBUG("DP%d pclk:%d, supported_rate:%d\n", dp->cell_idx,
				mode->clock, supported_rate_khz);
		goto end;
	}

	pclk_khz = dp_mode.timing.widebus_en ?
		(dp_mode.timing.pixel_clk_khz >> 1) :
		(dp_mode.timing.pixel_clk_khz);

	if (pclk_khz > dp_display->max_pclk_khz) {
		DP_MST_DEBUG("DP%d clk:%d, max:%d\n", dp->cell_idx, pclk_khz,
				dp_display->max_pclk_khz);
		goto end;
	}

	priv = dp_display->drm_dev->dev_private;
	sde_kms = to_sde_kms(priv->kms);
	rc = msm_get_mixer_count(dp->priv, mode,
			sde_kms->catalog->max_mixer_width, &num_lm);
	if (rc) {
		DP_MST_DEBUG("DP%d error getting mixer count. rc:%d\n",
				dp->cell_idx, rc);
		goto end;
	}

	if (dp_display->max_hdisplay > 0 && dp_display->max_vdisplay > 0 &&
			((mode->hdisplay > dp_display->max_hdisplay) ||
			(mode->vdisplay > dp_display->max_vdisplay))) {
		DP_MST_DEBUG("DP%d hdisplay:%d, max-hdisplay:%d", dp->cell_idx,
			mode->hdisplay, dp_display->max_hdisplay);
		DP_MST_DEBUG(" vdisplay:%d, max-vdisplay:%d\n",
			mode->vdisplay, dp_display->max_vdisplay);
		goto end;
	}

	mode_status = MODE_OK;
end:
	mutex_unlock(&dp->session_lock);
	return mode_status;
}

static int dp_display_get_modes(struct dp_display *dp, void *panel,
	struct dp_display_mode *dp_mode)
{
	struct dp_display_private *dp_display;
	struct dp_panel *dp_panel;
	int ret = 0;

	if (!dp || !panel) {
		pr_err("invalid params\n");
		return 0;
	}

	dp_panel = panel;
	if (!dp_panel->connector) {
		pr_err("invalid connector\n");
		return 0;
	}

	dp_display = container_of(dp, struct dp_display_private, dp_display);

	ret = dp_panel->get_modes(dp_panel, dp_panel->connector, dp_mode);
	if (dp_mode->timing.pixel_clk_khz)
		dp->max_pclk_khz = dp_mode->timing.pixel_clk_khz;
	return ret;
}

static void dp_display_convert_to_dp_mode(struct dp_display *dp_display,
		void *panel,
		const struct drm_display_mode *drm_mode,
		struct dp_display_mode *dp_mode)
{
	struct dp_display_private *dp;
	struct dp_panel *dp_panel;
	u32 free_dsc_blks = 0, required_dsc_blks = 0;

	if (!dp_display || !drm_mode || !dp_mode || !panel) {
		pr_err("invalid input\n");
		return;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	dp_panel = panel;

	memset(dp_mode, 0, sizeof(*dp_mode));

	free_dsc_blks = dp->parser->max_dp_dsc_blks -
				dp->tot_dsc_blks_in_use +
				dp_panel->tot_dsc_blks_in_use;
	required_dsc_blks = drm_mode->hdisplay /
				dp->parser->max_dp_dsc_input_width_pixs;
	if (drm_mode->hdisplay % dp->parser->max_dp_dsc_input_width_pixs)
		required_dsc_blks++;

	if (free_dsc_blks >= required_dsc_blks)
		dp_mode->capabilities |= DP_PANEL_CAPS_DSC;

	pr_debug("in_use:%d, max:%d, free:%d, req:%d, caps:0x%x, width:%d",
			dp->tot_dsc_blks_in_use, dp->parser->max_dp_dsc_blks,
			free_dsc_blks, required_dsc_blks, dp_mode->capabilities,
			dp->parser->max_dp_dsc_input_width_pixs);

	dp_panel->convert_to_dp_mode(dp_panel, drm_mode, dp_mode);
}

static int dp_display_config_hdr(struct dp_display *dp_display, void *panel,
			struct drm_msm_ext_hdr_metadata *hdr)
{
	int rc = 0;
	struct dp_display_private *dp;
	struct dp_panel *dp_panel;

	if (!dp_display || !panel) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	dp_panel = panel;

	rc = dp_panel->setup_hdr(dp_panel, hdr);

	return rc;
}

static int dp_display_create_workqueue(struct dp_display_private *dp)
{
	dp->wq = create_singlethread_workqueue("drm_dp");
	if (IS_ERR_OR_NULL(dp->wq)) {
		pr_err("DP%d Error creating wq\n", dp->cell_idx);
		return -EPERM;
	}

	INIT_DELAYED_WORK(&dp->hdcp_cb_work, dp_display_hdcp_cb_work);
	INIT_WORK(&dp->connect_work, dp_display_connect_work);
	INIT_WORK(&dp->attention_work, dp_display_attention_work);

	return 0;
}

static int dp_display_fsa4480_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	return 0;
}

static int dp_display_init_aux_switch(struct dp_display_private *dp)
{
	int rc = 0;
	const char *phandle = "qcom,dp-aux-switch";
	struct notifier_block nb;

	if (!dp->pdev->dev.of_node) {
		pr_err("DP%d cannot find dev.of_node\n", dp->cell_idx);
		rc = -ENODEV;
		goto end;
	}

	dp->aux_switch_node = of_parse_phandle(dp->pdev->dev.of_node,
			phandle, 0);
	if (!dp->aux_switch_node) {
		pr_warn("cannot parse %s handle\n", phandle);
		goto end;
	}

	nb.notifier_call = dp_display_fsa4480_callback;
	nb.priority = 0;

	rc = fsa4480_reg_notifier(&nb, dp->aux_switch_node);
	if (rc) {
		pr_err("DP%d failed to register notifier (%d)\n", dp->cell_idx, rc);
		goto end;
	}

	fsa4480_unreg_notifier(&nb, dp->aux_switch_node);
end:
	return rc;
}

static int dp_parser_msm_hdcp_dev(struct dp_display_private *dp)
{
	struct device_node *node;
	struct platform_device *pdev;

	node = of_parse_phandle(dp->pdev->dev.of_node, "qcom,msm-hdcp", 0);
	if (!node) {
		// This is a non-fatal error, module initialization can proceed
		pr_warn("couldn't find msm-hdcp node\n");
		return 0;
	}

	pdev = of_find_device_by_node(node);
	if (!pdev) {
		// defer the  module initialization
		pr_err("DP%d couldn't find msm-hdcp pdev defer probe\n", dp->cell_idx);
		return -EPROBE_DEFER;
	}

	dp->msm_hdcp_dev = &pdev->dev;

	return 0;
}


static int dp_display_bridge_internal_hpd(void *dev, bool hpd, bool hpd_irq)
{
	struct dp_display_private *dp = dev;
	struct drm_device *drm_dev = dp->dp_display.drm_dev;

	if (!drm_dev || !drm_dev->mode_config.poll_enabled)
		return -EBUSY;

	if (hpd_irq)
		dp_display_mst_attention(dp);
	else
		dp->hpd->simulate_connect(dp->hpd, hpd);

	return 0;
}

static int dp_display_init_aux_bridge(struct dp_display_private *dp)
{
	int rc = 0;
	const char *phandle = "qcom,dp-aux-bridge";
	struct device_node *bridge_node;

	if (!dp->pdev->dev.of_node) {
		pr_err("DP%d cannot find dev.of_node\n", dp->cell_idx);
		rc = -ENODEV;
		goto end;
	}

	bridge_node = of_parse_phandle(dp->pdev->dev.of_node,
			phandle, 0);
	if (!bridge_node)
		goto end;

	dp->aux_bridge = of_msm_dp_aux_find_bridge(bridge_node);
	if (!dp->aux_bridge) {
		pr_err("DP%d failed to find dp aux bridge\n", dp->cell_idx);
		rc = -EPROBE_DEFER;
		goto end;
	}

	if (dp->aux_bridge->register_hpd &&
			!(dp->aux_bridge->flag & MSM_DP_AUX_BRIDGE_HPD))
		dp->aux_bridge->register_hpd(dp->aux_bridge,
				dp_display_bridge_internal_hpd, dp);

end:
	return rc;
}

static int dp_display_mst_install(struct dp_display *dp_display,
			struct dp_mst_drm_install_info *mst_install_info)
{
	struct dp_display_private *dp;

	if (!dp_display || !mst_install_info) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	if (!mst_install_info->cbs->hpd || !mst_install_info->cbs->hpd_irq) {
		pr_err("DP%d invalid mst cbs\n", dp->cell_idx);
		return -EINVAL;
	}

	dp_display->dp_mst_prv_info = mst_install_info->dp_mst_prv_info;

	if (!dp->parser->has_mst) {
		pr_debug("DP%d mst not enabled\n", dp->cell_idx);
		return -EPERM;
	}

	memcpy(&dp->mst.cbs, mst_install_info->cbs, sizeof(dp->mst.cbs));
	dp->mst.drm_registered = true;

	DP_MST_DEBUG("DP%d dp mst drm installed\n", dp->cell_idx);

	return 0;
}

static int dp_display_mst_uninstall(struct dp_display *dp_display)
{
	struct dp_display_private *dp;

	if (!dp_display) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	if (!dp->mst.drm_registered) {
		pr_debug("DP%d drm mst not registered\n", dp->cell_idx);
		return -EPERM;
	}

	dp = container_of(dp_display, struct dp_display_private,
				dp_display);
	memset(&dp->mst.cbs, 0, sizeof(dp->mst.cbs));
	dp->mst.drm_registered = false;

	DP_MST_DEBUG("DP%d dp mst drm uninstalled\n", dp->cell_idx);

	return 0;
}

static int dp_display_mst_connector_install(struct dp_display *dp_display,
		struct drm_connector *connector)
{
	int rc = 0;
	struct dp_panel_in panel_in;
	struct dp_panel *dp_panel;
	struct dp_display_private *dp;

	if (!dp_display || !connector) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	mutex_lock(&dp->session_lock);

	if (!dp->mst.drm_registered) {
		pr_debug("DP%d drm mst not registered\n", dp->cell_idx);
		mutex_unlock(&dp->session_lock);
		return -EPERM;
	}

	panel_in.dev = &dp->pdev->dev;
	panel_in.aux = dp->aux;
	panel_in.catalog = &dp->catalog->panel;
	panel_in.link = dp->link;
	panel_in.connector = connector;
	panel_in.base_panel = dp->panel;
	panel_in.parser = dp->parser;

	dp_panel = dp_panel_get(&panel_in);
	if (IS_ERR(dp_panel)) {
		rc = PTR_ERR(dp_panel);
		pr_err("DP%d failed to initialize panel, rc = %d\n", dp->cell_idx, rc);
		mutex_unlock(&dp->session_lock);
		return rc;
	}

	dp_panel->audio = dp_audio_get(dp->pdev, dp_panel, &dp->catalog->audio);
	if (IS_ERR(dp_panel->audio)) {
		rc = PTR_ERR(dp_panel->audio);
		pr_err("DP%d [mst] failed to initialize audio, rc = %d\n",
				dp->cell_idx, rc);
		dp_panel->audio = NULL;
		mutex_unlock(&dp->session_lock);
		return rc;
	}

	DP_MST_DEBUG("DP%d dp mst connector installed. conn:%d\n", dp->cell_idx,
			connector->base.id);

	mutex_unlock(&dp->session_lock);

	return 0;
}

static int dp_display_mst_connector_uninstall(struct dp_display *dp_display,
			struct drm_connector *connector)
{
	int rc = 0;
	struct sde_connector *sde_conn;
	struct dp_panel *dp_panel;
	struct dp_display_private *dp;

	if (!dp_display || !connector) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	mutex_lock(&dp->session_lock);

	if (!dp->mst.drm_registered) {
		pr_debug("DP%d drm mst not registered\n", dp->cell_idx);
		mutex_unlock(&dp->session_lock);
		return -EPERM;
	}

	sde_conn = to_sde_connector(connector);
	if (!sde_conn->drv_panel) {
		pr_err("DP%d invalid panel for connector:%d\n",
				dp->cell_idx, connector->base.id);
		mutex_unlock(&dp->session_lock);
		return -EINVAL;
	}

	dp_panel = sde_conn->drv_panel;
	dp_audio_put(dp_panel->audio);
	dp_panel_put(dp_panel);

	DP_MST_DEBUG("DP%d dp mst connector uninstalled. conn:%d\n", dp->cell_idx,
			connector->base.id);

	mutex_unlock(&dp->session_lock);

	return rc;
}

static int dp_display_mst_connector_update_edid(struct dp_display *dp_display,
			struct drm_connector *connector,
			struct edid *edid)
{
	int rc = 0;
	struct sde_connector *sde_conn;
	struct dp_panel *dp_panel;
	struct dp_display_private *dp;

	if (!dp_display || !connector || !edid) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	if (!dp->mst.drm_registered) {
		pr_debug("DP%d drm mst not registered\n", dp->cell_idx);
		return -EPERM;
	}

	sde_conn = to_sde_connector(connector);
	if (!sde_conn->drv_panel) {
		pr_err("DP%d invalid panel for connector:%d\n",
				dp->cell_idx, connector->base.id);
		return -EINVAL;
	}

	dp_panel = sde_conn->drv_panel;
	rc = dp_panel->update_edid(dp_panel, edid);

	DP_MST_DEBUG("DP%d dp mst connector:%d edid updated. mode_cnt:%d\n",
			dp->cell_idx, connector->base.id, rc);

	return rc;
}

static int dp_display_update_pps(struct dp_display *dp_display,
		struct drm_connector *connector, char *pps_cmd)
{
	struct sde_connector *sde_conn;
	struct dp_panel *dp_panel;
	struct dp_display_private *dp;

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	sde_conn = to_sde_connector(connector);
	if (!sde_conn->drv_panel) {
		pr_err("DP%d invalid panel for connector:%d\n",
				dp->cell_idx, connector->base.id);
		return -EINVAL;
	}

	dp_panel = sde_conn->drv_panel;
	dp_panel->update_pps(dp_panel, pps_cmd);
	return 0;
}

static int dp_display_mst_connector_update_link_info(
			struct dp_display *dp_display,
			struct drm_connector *connector)
{
	int rc = 0;
	struct sde_connector *sde_conn;
	struct dp_panel *dp_panel;
	struct dp_display_private *dp;

	if (!dp_display || !connector) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	if (!dp->mst.drm_registered) {
		pr_debug("DP%d drm mst not registered\n", dp->cell_idx);
		return -EPERM;
	}

	sde_conn = to_sde_connector(connector);
	if (!sde_conn->drv_panel) {
		pr_err("DP%d invalid panel for connector:%d\n",
				dp->cell_idx, connector->base.id);
		return -EINVAL;
	}

	dp_panel = sde_conn->drv_panel;

	memcpy(dp_panel->dpcd, dp->panel->dpcd,
			DP_RECEIVER_CAP_SIZE + 1);
	memcpy(dp_panel->dsc_dpcd, dp->panel->dsc_dpcd,
			DP_RECEIVER_DSC_CAP_SIZE + 1);
	memcpy(&dp_panel->link_info, &dp->panel->link_info,
			sizeof(dp_panel->link_info));
	dp_panel->mst_state = dp->panel->mst_state;
	dp_panel->widebus_en = dp->panel->widebus_en;
	dp_panel->fec_en = dp->panel->fec_en;
	dp_panel->dsc_en = dp->panel->dsc_en;
	dp_panel->fec_overhead_fp = dp->panel->fec_overhead_fp;

	DP_MST_DEBUG("DP%d dp mst connector: %d link info updated\n", dp->cell_idx,
			sde_conn->base.base.id);

	return rc;
}

static int dp_display_mst_get_fixed_topology_port(
			struct dp_display *dp_display,
			u32 strm_id, u32 *port_num)
{
	struct dp_display_private *dp;
	u32 port;

	if (!dp_display) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	if (strm_id >= DP_STREAM_MAX) {
		pr_err("invalid stream id:%d\n", strm_id);
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	port = dp->parser->mst_fixed_port[strm_id];

	if (!port || port > 255)
		return -ENOENT;

	if (port_num)
		*port_num = port;

	return 0;
}

static int dp_display_get_mst_caps(struct dp_display *dp_display,
			struct dp_mst_caps *mst_caps)
{
	int rc = 0;
	struct dp_display_private *dp;

	if (!dp_display || !mst_caps) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	mst_caps->has_mst = dp->parser->has_mst;
	mst_caps->max_streams_supported = (mst_caps->has_mst) ? 2 : 0;
	mst_caps->max_dpcd_transaction_bytes = (mst_caps->has_mst) ? 16 : 0;
	mst_caps->drm_aux = dp->aux->drm_aux;

	return rc;
}

static void dp_display_wakeup_phy_layer(struct dp_display *dp_display,
		bool wakeup)
{
	struct dp_display_private *dp;
	struct dp_hpd *hpd;

	if (!dp_display) {
		pr_err("invalid input\n");
		return;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	if (!dp->mst.drm_registered) {
		pr_debug("DP%d drm mst not registered\n", dp->cell_idx);
		return;
	}

	hpd = dp->hpd;
	if (hpd && hpd->wakeup_phy)
		hpd->wakeup_phy(hpd, wakeup);
}

static int dp_display_get_display_type(struct dp_display *dp_display,
		const char **display_type)
{
	struct dp_display_private *dp;

	if (!dp_display || !display_type) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	if (dp->parser)
		*display_type = dp->parser->display_type;

	return 0;
}

static int dp_display_mst_get_fixed_topology_display_type(
		struct dp_display *dp_display, u32 strm_id,
		const char **display_type)
{
	struct dp_display_private *dp;

	if (!dp_display || !display_type) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	if (strm_id >= DP_STREAM_MAX) {
		pr_err("invalid stream id:%d\n", strm_id);
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	*display_type = dp->parser->mst_fixed_display_type[strm_id];

	return 0;
}


static int dp_display_set_phy_bond_mode(struct dp_display *dp_display,
		enum dp_phy_bond_mode mode,
		struct drm_connector *primary_connector)
{
	struct dp_display_private *dp;

	if (!dp_display) {
		pr_err("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	mutex_lock(&dp->session_lock);

	if (dp->phy_bond_mode != mode) {
		/*
		 * The DP driver has been firstly inited in process_hpd_high.
		 * Then the upper layer will decide the display mode after
		 * receiving the HPD event.
		 * If the bond mode need to be changed afterwards, tear it
		 * down here and allow it to be re-init in dp_display_prepare,
		 * where the master/slave order is guaranteed by the bond
		 * bridge.
		 */
		dp_display_clean(dp);

		dp_display_host_deinit(dp);

		dp_display_change_phy_bond_mode(dp, mode);
	}

	dp->bond_primary = primary_connector;

	mutex_unlock(&dp->session_lock);

	return 0;
}

/**
 * dp_display_parse_boot_display_selection()- Parse DP boot display name
 *
 * Return:      returns error status
 */
static int dp_display_parse_boot_display_selection(void)
{
	char *pos = NULL;
	char disp_buf[MAX_CMDLINE_PARAM_LEN] = {'\0'};
	int i, j;

	for (i = 0; i < MAX_DP_BOOT_DISPLAY; i++) {
		strlcpy(disp_buf, boot_displays[i].boot_param,
				MAX_CMDLINE_PARAM_LEN);

		pos = strnstr(disp_buf, ":", MAX_CMDLINE_PARAM_LEN);

		/* Use ':' as a delimiter to retrieve the display name */
		if (!pos) {
			pr_err("display name[%s]is not valid\n", disp_buf);
			continue;
		}

		for (j = 0; (disp_buf + j) < pos; j++)
			boot_displays[i].name[j] = *(disp_buf + j);

		boot_displays[i].name[j] = '\0';

		boot_displays[i].boot_disp_en = true;
	}

	return 0;
}

static int dp_display_probe(struct platform_device *pdev)
{
	int rc = 0;
	struct dp_display_private *dp;
	struct dp_display *dp_display;
	struct dp_display_boot_param *boot_disp;
	int index;

	if (!pdev || !pdev->dev.of_node) {
		pr_err("pdev not found\n");
		rc = -ENODEV;
		goto bail;
	}

	boot_disp = &boot_displays[0];

	index = dp_display_get_num_of_displays();
	if (index >= MAX_DP_ACTIVE_DISPLAY) {
		pr_err("exceeds max dp count\n");
		rc = -EINVAL;
		goto bail;
	}

	dp = devm_kzalloc(&pdev->dev, sizeof(*dp), GFP_KERNEL);
	if (!dp) {
		rc = -ENOMEM;
		goto bail;
	}

	dp->pdev = pdev;
	snprintf(dp->name, MAX_DP_NAME_SIZE,
			"drm_dp%d", index);

	memset(&dp->mst, 0, sizeof(dp->mst));
	atomic_set(&dp->aborted, 0);

	rc = dp_display_init_aux_switch(dp);
	if (rc) {
		rc = -EPROBE_DEFER;
		goto error;
	}

	rc = dp_parser_msm_hdcp_dev(dp);
	if (rc)
		goto error;

	rc = dp_display_init_aux_bridge(dp);
	if (rc)
		goto error;

	rc = dp_display_create_workqueue(dp);
	if (rc) {
		pr_err("Failed to create workqueue\n");
		goto error;
	}

	if (boot_disp->boot_disp_en) {
		if (!strcmp(boot_disp->name, dp->name)) {
			boot_disp->node = pdev->dev.of_node;
			boot_disp->disp = dp;
		}
	}

	platform_set_drvdata(pdev, dp);

	dp_display = &dp->dp_display;
	g_dp_display[index] = dp_display;

	dp_display->enable        = dp_display_enable;
	dp_display->post_enable   = dp_display_post_enable;
	dp_display->pre_disable   = dp_display_pre_disable;
	dp_display->disable       = dp_display_disable;
	dp_display->set_mode      = dp_display_set_mode;
	dp_display->validate_mode = dp_display_validate_mode;
	dp_display->get_modes     = dp_display_get_modes;
	dp_display->prepare       = dp_display_prepare;
	dp_display->unprepare     = dp_display_unprepare;
	dp_display->request_irq   = dp_request_irq;
	dp_display->get_debug     = dp_get_debug;
	dp_display->post_open     = NULL;
	dp_display->post_init     = dp_display_post_init;
	dp_display->config_hdr    = dp_display_config_hdr;
	dp_display->mst_install   = dp_display_mst_install;
	dp_display->mst_uninstall = dp_display_mst_uninstall;
	dp_display->mst_connector_install = dp_display_mst_connector_install;
	dp_display->mst_connector_uninstall =
					dp_display_mst_connector_uninstall;
	dp_display->mst_connector_update_edid =
					dp_display_mst_connector_update_edid;
	dp_display->mst_connector_update_link_info =
				dp_display_mst_connector_update_link_info;
	dp_display->get_mst_caps = dp_display_get_mst_caps;
	dp_display->set_stream_info = dp_display_set_stream_info;
	dp_display->update_pps = dp_display_update_pps;
	dp_display->convert_to_dp_mode = dp_display_convert_to_dp_mode;
	dp_display->mst_get_fixed_topology_port =
					dp_display_mst_get_fixed_topology_port;
	dp_display->wakeup_phy_layer =
					dp_display_wakeup_phy_layer;
	dp_display->get_display_type = dp_display_get_display_type;
	dp_display->mst_get_fixed_topology_display_type =
				dp_display_mst_get_fixed_topology_display_type;
	dp_display->set_phy_bond_mode = dp_display_set_phy_bond_mode;

	rc = component_add(&pdev->dev, &dp_display_comp_ops);
	if (rc) {
		pr_err("component add failed, rc=%d\n", rc);
		goto error;
	}

	return 0;
error:
	g_dp_display[index] = NULL;
	devm_kfree(&pdev->dev, dp);
bail:
	return rc;
}

int dp_display_get_displays(void **displays, int count)
{
	int i;

	if (!displays) {
		pr_err("invalid data\n");
		return -EINVAL;
	}

	for (i = 0; i < MAX_DP_ACTIVE_DISPLAY && i < count; i++) {
		struct dp_display *display = g_dp_display[i];

		if (!display)
			break;

		displays[i] = g_dp_display[i];
	}

	return count;
}

int dp_display_get_num_of_boot_displays(void)
{
	int i, count = 0;

	for (i = 0; i < MAX_DP_BOOT_DISPLAY; i++) {
		if (boot_displays[i].disp && boot_displays[i].node)
			count++;
	}

	return count;
}

int dp_display_get_num_of_displays(void)
{
	int i;

	for (i = 0; i < MAX_DP_ACTIVE_DISPLAY; i++) {
		struct dp_display *display = g_dp_display[i];

		if (!display)
			break;
	}

	return i;
}

int dp_display_get_num_of_streams(void *dp_display)
{
	struct dp_display_private *dp;
	bool has_mst, no_mst_encoder;

	if (!dp_display) {
		pr_debug("dp display not initialized\n");
		return 0;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	if (!dp->parser) {
		has_mst = of_property_read_bool(dp->pdev->dev.of_node,
				"qcom,mst-enable");
		no_mst_encoder = of_property_read_bool(dp->pdev->dev.of_node,
				"qcom,no-mst-encoder");
	} else {
		has_mst = dp->parser->has_mst;
		no_mst_encoder = dp->parser->no_mst_encoder;
	}

	return (has_mst && !no_mst_encoder) ? DP_STREAM_MAX : 0;
}

int dp_display_get_num_of_bonds(void *dp_display)
{
	struct dp_display_private *dp;
	int i, cnt = 0;

	if (!dp_display) {
		pr_debug("dp display not initialized\n");
		return 0;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	if (!dp->parser) {
		if (of_property_count_u32_elems(dp->pdev->dev.of_node,
				"qcom,bond-dual-ctrl") > 0)
			cnt++;
		if (of_property_count_u32_elems(dp->pdev->dev.of_node,
				"qcom,bond-tri-ctrl") > 0)
			cnt++;
	} else {
		for (i = 0; i < DP_BOND_MAX; i++) {
			if (dp->parser->bond_cfg[i].enable)
				cnt++;
		}
	}

	return cnt;
}

int dp_display_get_info(void *dp_display, struct dp_display_info *dp_info)
{
	struct dp_display_private *dp;
	int i;

	if (!dp_display) {
		pr_debug("dp display not initialized\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	dp_info->cell_idx = dp->cell_idx;
	for (i = 0; i < DP_STREAM_MAX; i++)
		dp_info->intf_idx[i] = dp->intf_idx[i];
	dp_info->phy_idx = dp->phy_idx;

	return 0;
}

int dp_display_get_bond_displays(void *dp_display, enum dp_bond_type type,
		struct dp_display_bond_displays *dp_bond_info)
{
	struct dp_display_private *dp;
	int i, j;

	if (!dp_display) {
		pr_debug("dp display not initialized\n");
		return -EINVAL;
	}

	if (type < 0 || type >= DP_BOND_MAX) {
		pr_debug("invalid bond type\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	memset(dp_bond_info, 0, sizeof(*dp_bond_info));

	if (!dp->parser->bond_cfg[type].enable)
		return 0;

	dp_bond_info->dp_display_num = type + 2;

	for (i = 0; i < MAX_DP_ACTIVE_DISPLAY; i++) {
		struct dp_display *display = g_dp_display[i];
		struct dp_display_private *dp_disp;

		if (!display)
			break;

		dp_disp = container_of(display,
				struct dp_display_private, dp_display);

		for (j = 0; j < dp_bond_info->dp_display_num; j++) {
			if (dp->parser->bond_cfg[type].ctrl[j] ==
					dp_disp->cell_idx) {
				dp_bond_info->dp_display[j] = display;
				break;
			}
		}
	}

	return 0;
}

static void dp_display_set_mst_state(void *dp_display,
		enum dp_drv_state mst_state)
{
	struct dp_display_private *dp;

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	if (dp->mst.mst_active && dp->mst.cbs.set_drv_state)
		dp->mst.cbs.set_drv_state(dp_display, mst_state);
}

static int dp_display_remove(struct platform_device *pdev)
{
	struct dp_display_private *dp;

	if (!pdev)
		return -EINVAL;

	dp = platform_get_drvdata(pdev);

	dp_display_deinit_sub_modules(dp);

	if (dp->wq)
		destroy_workqueue(dp->wq);

	platform_set_drvdata(pdev, NULL);
	devm_kfree(&pdev->dev, dp);

	return 0;
}

static int dp_pm_prepare(struct device *dev)
{
	struct dp_display_private *dp;

	if (!dev)
		return -EINVAL;

	dp = dev_get_drvdata(dev);

	dp->suspended = true;

	if (!dp->dp_display.base_connector)
		return 0;

	dp_display_set_mst_state(&dp->dp_display, PM_SUSPEND);

	/*
	 * There are a few instances where the DP is hotplugged when the device
	 * is in PM suspend state. After hotplug, it is observed the device
	 * enters and exits the PM suspend multiple times while aux transactions
	 * are taking place. This may sometimes cause an unclocked register
	 * access error. So, abort aux transactions when such a situation
	 * arises i.e. when DP is connected but not powered on yet.
	 */
	if (dp->is_connected && !dp->power_on) {
		dp->aux->abort(dp->aux, false);
		dp->ctrl->abort(dp->ctrl, false);
	}

	/*
	 * If DP is not enabled but powered and suspend state
	 * is entered, we need to power off the host to disable all
	 * clocks. This is needed when link training failed.
	 */
	if (!dp->power_on && dp->aux->state != DP_STATE_CTRL_POWERED_OFF) {
		dp->ctrl->off(dp->ctrl);
		dp_display_host_deinit(dp);
		dp->aux->state = DP_STATE_CTRL_POWERED_OFF;

		if (dp->parser->force_connect_mode)
			dp_display_send_force_connect_event(dp);
	}

	return 0;
}

static void dp_pm_complete(struct device *dev)
{
	struct dp_display_private *dp;

	if (!dev)
		return;

	dp = dev_get_drvdata(dev);

	dp->suspended = false;

	if (!dp->dp_display.base_connector)
		return;

	dp_display_set_mst_state(&dp->dp_display, PM_DEFAULT);

	/*
	 * There are multiple PM suspend entry and exits observed before
	 * the connect uevent is issued to userspace. The aux transactions are
	 * aborted during PM suspend entry in dp_pm_prepare to prevent unclocked
	 * register access. On PM suspend exit, there will be no host_init call
	 * to reset the abort flags for ctrl and aux incase the DP is connected
	 * but not powered on. So, resetting the abort flags for aux and ctrl.
	 */
	if (dp->is_connected && !dp->power_on) {
		dp->aux->abort(dp->aux, true);
		dp->ctrl->abort(dp->ctrl, true);
	}
}

static int dp_pm_freeze(struct device *dev)
{
	struct dp_display_private *dp;

	if (!dev)
		return -EINVAL;

	dp = dev_get_drvdata(dev);

	if (!dp->dp_display.base_connector)
		return 0;

	dp_display_set_mst_state(&dp->dp_display, PM_FREEZE);

	return 0;
}

static const struct dev_pm_ops dp_pm_ops = {
	.prepare = dp_pm_prepare,
	.complete = dp_pm_complete,
	.freeze = dp_pm_freeze,
};

static struct platform_driver dp_display_driver = {
	.probe  = dp_display_probe,
	.remove = dp_display_remove,
	.driver = {
		.name = "msm-dp-display",
		.of_match_table = dp_dt_match,
		.suppress_bind_attrs = true,
		.pm = &dp_pm_ops,
	},
};

void __init dp_display_register(void)
{
	dp_display_parse_boot_display_selection();
	platform_driver_register(&dp_display_driver);
}

void __exit dp_display_unregister(void)
{
	platform_driver_unregister(&dp_display_driver);
}

module_param_string(dp_display0, dp_display_0, MAX_CMDLINE_PARAM_LEN, 0600);
MODULE_PARM_DESC(dp_display0,
	"msm_drm.dp_display0=<display node>:<configX> where <display node> is 'external dp display node name' and <configX> where x represents index in the topology list");



