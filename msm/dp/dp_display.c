// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021-2023, Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/component.h>
#include <linux/of_irq.h>
#include <linux/delay.h>
#include <linux/usb/phy.h>
#include <linux/jiffies.h>
#include <linux/pm_qos.h>

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
#include "dp_pll.h"
#include "sde_dbg.h"

#define DP_MST_DEBUG(fmt, ...) DP_DEBUG(fmt, ##__VA_ARGS__)

#define dp_display_state_show(x) { \
	DP_ERR("DP%d %s: state (0x%x): %s\n", dp->cell_idx, x, dp->state, \
		dp_display_state_name(dp->state)); \
	SDE_EVT32_EXTERNAL(dp->state); }

#define dp_display_state_warn(x) { \
	DP_WARN("%s: state (0x%x): %s\n", x, dp->state, \
		dp_display_state_name(dp->state)); \
	SDE_EVT32_EXTERNAL(dp->state); }

#define dp_display_state_log(x) { \
	DP_DEBUG("DP%d %s: state (0x%x): %s\n", dp->cell_idx, x, dp->state, \
		dp_display_state_name(dp->state)); \
	SDE_EVT32_EXTERNAL(dp->state); }

#define dp_display_state_is(x) (dp->state & (x))
#define dp_display_state_add(x) { \
	(dp->state |= (x)); \
	dp_display_state_log("add "#x); }
#define dp_display_state_remove(x) { \
	(dp->state &= ~(x)); \
	dp_display_state_log("remove "#x); }

enum dp_display_states {
	DP_STATE_DISCONNECTED           = 0,
	DP_STATE_CONFIGURED             = BIT(0),
	DP_STATE_INITIALIZED            = BIT(1),
	DP_STATE_READY                  = BIT(2),
	DP_STATE_CONNECTED              = BIT(3),
	DP_STATE_CONNECT_NOTIFIED       = BIT(4),
	DP_STATE_DISCONNECT_NOTIFIED    = BIT(5),
	DP_STATE_ENABLED                = BIT(6),
	DP_STATE_SUSPENDED              = BIT(7),
	DP_STATE_ABORTED                = BIT(8),
	DP_STATE_HDCP_ABORTED           = BIT(9),
	DP_STATE_SRC_PWRDN              = BIT(10),
	DP_STATE_TUI_ACTIVE             = BIT(11),
};

static char *dp_display_state_name(enum dp_display_states state)
{
	static char buf[SZ_1K];
	u32 len = 0;

	memset(buf, 0, SZ_1K);

	if (state & DP_STATE_CONFIGURED)
		len += scnprintf(buf + len, sizeof(buf) - len, "|%s|",
			"CONFIGURED");

	if (state & DP_STATE_INITIALIZED)
		len += scnprintf(buf + len, sizeof(buf) - len, "|%s|",
			"INITIALIZED");

	if (state & DP_STATE_READY)
		len += scnprintf(buf + len, sizeof(buf) - len, "|%s|",
			"READY");

	if (state & DP_STATE_CONNECTED)
		len += scnprintf(buf + len, sizeof(buf) - len, "|%s|",
			"CONNECTED");

	if (state & DP_STATE_CONNECT_NOTIFIED)
		len += scnprintf(buf + len, sizeof(buf) - len, "|%s|",
			"CONNECT_NOTIFIED");

	if (state & DP_STATE_DISCONNECT_NOTIFIED)
		len += scnprintf(buf + len, sizeof(buf) - len, "|%s|",
			"DISCONNECT_NOTIFIED");

	if (state & DP_STATE_ENABLED)
		len += scnprintf(buf + len, sizeof(buf) - len, "|%s|",
			"ENABLED");

	if (state & DP_STATE_SUSPENDED)
		len += scnprintf(buf + len, sizeof(buf) - len, "|%s|",
			"SUSPENDED");

	if (state & DP_STATE_ABORTED)
		len += scnprintf(buf + len, sizeof(buf) - len, "|%s|",
			"ABORTED");

	if (state & DP_STATE_HDCP_ABORTED)
		len += scnprintf(buf + len, sizeof(buf) - len, "|%s|",
			"HDCP_ABORTED");

	if (state & DP_STATE_SRC_PWRDN)
		len += scnprintf(buf + len, sizeof(buf) - len, "|%s|",
			"SRC_PWRDN");

	if (state & DP_STATE_TUI_ACTIVE)
		len += scnprintf(buf + len, sizeof(buf) - len, "|%s|",
			"TUI_ACTIVE");

	if (!strlen(buf))
		return "DISCONNECTED";

	return buf;
}

static struct dp_display *g_dp_display[MAX_DP_ACTIVE_DISPLAY];
#define HPD_STRING_SIZE 30

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
	const char *name;
	int irq;

	enum drm_connector_status cached_connector_status;
	enum dp_display_states state;

	struct platform_device *pdev;
	struct device_node *aux_switch_node;
	bool aux_switch_ready;
	struct dp_aux_bridge *aux_bridge;
	struct dentry *root;
	struct completion attention_comp;

	struct dp_hpd     *hpd;
	struct dp_parser  *parser;
	struct dp_power   *power;
	struct dp_catalog *catalog;
	struct dp_aux     *aux;
	struct dp_link    *link;
	struct dp_panel   *panel;
	struct dp_ctrl    *ctrl;
	struct dp_debug   *debug;
	struct dp_pll     *pll;

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
	bool hdcp_delayed_off;

	u32 active_stream_cnt;
	struct dp_mst mst;

	u32 tot_dsc_blks_in_use;

	bool process_hpd_connect;
	struct dev_pm_qos_request pm_qos_req[NR_CPUS];
	bool pm_qos_requested;

	struct notifier_block usb_nb;

	u32 cell_idx;
	u32 intf_idx[DP_STREAM_MAX];
	u32 phy_idx;
	u32 stream_cnt;
	enum dp_phy_bond_mode phy_bond_mode;
	struct drm_connector *bond_primary;

	struct device *msm_hdcp_dev;
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
		DP_ERR("invalid data\n");
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
			DP_ERR("DP%d dp_hdcp_isr failed\n", dp->cell_idx);
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
	return dp->hpd->hpd_high && dp_display_state_is(DP_STATE_CONNECTED) &&
		!dp_display_is_sink_count_zero(dp) &&
		dp->hpd->alt_mode_cfg_done;
}

static void dp_audio_enable(struct dp_display_private *dp, bool enable)
{
	struct dp_panel *dp_panel;
	int idx;

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

static void dp_display_qos_request(struct dp_display_private *dp, bool add_vote)
{
	struct device *cpu_dev;
	int cpu = 0;
	struct cpumask *cpu_mask;
	u32 latency = dp->parser->qos_cpu_latency;
	unsigned long mask = dp->parser->qos_cpu_mask;

	if (!dp->parser->qos_cpu_mask || (dp->pm_qos_requested == add_vote))
		return;

	cpu_mask = to_cpumask(&mask);
	for_each_cpu(cpu, cpu_mask) {
		cpu_dev = get_cpu_device(cpu);
		if (!cpu_dev) {
			SDE_DEBUG("%s: failed to get cpu%d device\n", __func__, cpu);
			continue;
		}

		if (add_vote)
			dev_pm_qos_add_request(cpu_dev, &dp->pm_qos_req[cpu],
				DEV_PM_QOS_RESUME_LATENCY, latency);
		else
			dev_pm_qos_remove_request(&dp->pm_qos_req[cpu]);
	}

	SDE_EVT32_EXTERNAL(add_vote, mask, latency);
	dp->pm_qos_requested = add_vote;
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

	DP_DEBUG("DP%d HDCP version supported: %s\n", dp->cell_idx,
		sde_hdcp_version(dp->link->hdcp_status.hdcp_version));
}

static void dp_display_check_source_hdcp_caps(struct dp_display_private *dp)
{
	int i;
	struct dp_hdcp_dev *hdcp_dev = dp->hdcp.dev;

	if (dp->debug->hdcp_disabled) {
		DP_DEBUG("DP%d hdcp disabled\n", dp->cell_idx);
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

		DP_DEBUG("DP%d Registering all active panel streams with HDCP\n",
				dp->cell_idx);
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
				DP_ERR("DP%d failed to register streams. rc = %d\n",
						dp->cell_idx, rc);
		}
	}
}

static void dp_display_hdcp_deregister_stream(struct dp_display_private *dp,
		enum dp_stream_id stream_id)
{
	if (dp->hdcp.ops->deregister_streams && dp->active_panels[stream_id]) {
		struct stream_info stream = {stream_id,
				dp->active_panels[stream_id]->vcpi};

		DP_DEBUG("DP%d Deregistering stream within HDCP library\n",
				dp->cell_idx);
		dp->hdcp.ops->deregister_streams(dp->hdcp.data, 1, &stream);
	}
}

static void dp_display_hdcp_process_delayed_off(struct dp_display_private *dp)
{
	if (dp->hdcp_delayed_off) {
		if (dp->hdcp.ops && dp->hdcp.ops->off)
			dp->hdcp.ops->off(dp->hdcp.data);
		dp_display_update_hdcp_status(dp, true);
		dp->hdcp_delayed_off = false;
	}
}

static int dp_display_hdcp_process_sink_sync(struct dp_display_private *dp)
{
	u8 sink_status = 0;
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY);

	if (dp->debug->hdcp_wait_sink_sync) {
		drm_dp_dpcd_readb(dp->aux->drm_aux, DP_SINK_STATUS,
				&sink_status);
		sink_status &= (DP_RECEIVE_PORT_0_STATUS |
				DP_RECEIVE_PORT_1_STATUS);
		if (sink_status < 1) {
			DP_DEBUG("DP%d Sink not synchronized. Queuing again then exiting\n",
					dp->cell_idx);
			queue_delayed_work(dp->wq, &dp->hdcp_cb_work, HZ);
			return -EAGAIN;
		}
	}
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT);

	return 0;
}

static int dp_display_hdcp_start(struct dp_display_private *dp)
{
	if (dp->link->hdcp_status.hdcp_state != HDCP_STATE_INACTIVE)
		return -EINVAL;

	dp_display_check_source_hdcp_caps(dp);
	dp_display_update_hdcp_info(dp);

	if (dp_display_is_hdcp_enabled(dp)) {
		if (dp->hdcp.ops && dp->hdcp.ops->on &&
				dp->hdcp.ops->on(dp->hdcp.data)) {
			dp_display_update_hdcp_status(dp, true);
			return 0;
		}
	} else {
		dp_display_update_hdcp_status(dp, true);
		return 0;
	}

	return -EINVAL;
}

static void dp_display_hdcp_print_auth_state(struct dp_display_private *dp)
{
	u32 hdcp_auth_state;
	int rc;

	rc = dp->catalog->ctrl.read_hdcp_status(&dp->catalog->ctrl);
	if (rc >= 0) {
		hdcp_auth_state = (rc >> 20) & 0x3;
		DP_DEBUG("DP%d hdcp auth state %d\n", dp->cell_idx, hdcp_auth_state);
	}
}

static void dp_display_hdcp_process_state(struct dp_display_private *dp)
{
	struct dp_link_hdcp_status *status;
	struct sde_hdcp_ops *ops;
	void *data;
	int rc = 0;

	status = &dp->link->hdcp_status;

	ops = dp->hdcp.ops;
	data = dp->hdcp.data;

	if (status->hdcp_state != HDCP_STATE_AUTHENTICATED &&
		dp->debug->force_encryption && ops && ops->force_encryption)
		ops->force_encryption(data, dp->debug->force_encryption);

	if (status->hdcp_state == HDCP_STATE_AUTHENTICATED)
		dp_display_qos_request(dp, false);
	else
		dp_display_qos_request(dp, true);

	switch (status->hdcp_state) {
	case HDCP_STATE_INACTIVE:
		dp_display_hdcp_register_streams(dp);
		if (dp->hdcp.ops && dp->hdcp.ops->authenticate)
			rc = dp->hdcp.ops->authenticate(data);
		if (!rc)
			status->hdcp_state = HDCP_STATE_AUTHENTICATING;
		break;
	case HDCP_STATE_AUTH_FAIL:
		if (dp_display_is_ready(dp) &&
		    dp_display_state_is(DP_STATE_ENABLED)) {
			if (ops && ops->on && ops->on(data)) {
				dp_display_update_hdcp_status(dp, true);
				return;
			}
			dp_display_hdcp_register_streams(dp);
			if (ops && ops->reauthenticate) {
				rc = ops->reauthenticate(data);
				if (rc)
					DP_ERR("DP%d failed rc=%d\n", dp->cell_idx, rc);
			}
			status->hdcp_state = HDCP_STATE_AUTHENTICATING;
		} else {
			DP_DEBUG("DP%d not reauthenticating, cable disconnected\n",
					dp->cell_idx);
		}
		break;
	default:
		dp_display_hdcp_register_streams(dp);
		break;
	}
}

static void dp_display_abort_hdcp(struct dp_display_private *dp,
		bool abort)
{
	u8 i = HDCP_VERSION_2P2;
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
	struct dp_link_hdcp_status *status;
	int rc = 0;

	dp = container_of(dw, struct dp_display_private, hdcp_cb_work);

	if (!dp_display_state_is(DP_STATE_ENABLED | DP_STATE_CONNECTED) ||
	     dp_display_state_is(DP_STATE_ABORTED | DP_STATE_HDCP_ABORTED))
		return;

	if (dp_display_state_is(DP_STATE_SUSPENDED)) {
		DP_DEBUG("DP%d System suspending. Delay HDCP operations\n",
				dp->cell_idx);
		queue_delayed_work(dp->wq, &dp->hdcp_cb_work, HZ);
		return;
	}

	dp_display_hdcp_process_delayed_off(dp);

	rc = dp_display_hdcp_process_sink_sync(dp);
	if (rc)
		return;

	rc = dp_display_hdcp_start(dp);
	if (!rc)
		return;

	dp_display_hdcp_print_auth_state(dp);

	status = &dp->link->hdcp_status;
	DP_DEBUG("DP%d %s: %s\n", dp->cell_idx,
			sde_hdcp_version(status->hdcp_version),
			sde_hdcp_state_name(status->hdcp_state));

	dp_display_update_hdcp_status(dp, false);

	dp_display_hdcp_process_state(dp);
}

static void dp_display_notify_hdcp_status_cb(void *ptr,
		enum sde_hdcp_state state)
{
	struct dp_display_private *dp = ptr;

	if (!dp) {
		DP_ERR("invalid input\n");
		return;
	}

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY,
					dp->link->hdcp_status.hdcp_state);

	dp->link->hdcp_status.hdcp_state = state;

	queue_delayed_work(dp->wq, &dp->hdcp_cb_work, HZ/4);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT,
					dp->link->hdcp_status.hdcp_state);
}

static void dp_display_deinitialize_hdcp(struct dp_display_private *dp)
{
	if (!dp) {
		DP_ERR("invalid input\n");
		return;
	}

	sde_hdcp_1x_deinit(dp->hdcp.dev[HDCP_VERSION_1X].fd);
	sde_dp_hdcp2p2_deinit(dp->hdcp.dev[HDCP_VERSION_2P2].fd);
}

static int dp_display_initialize_hdcp(struct dp_display_private *dp)
{
	struct sde_hdcp_init_data hdcp_init_data;
	struct dp_parser *parser;
	void *fd;
	int rc = 0;

	if (!dp) {
		DP_ERR("invalid input\n");
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
	hdcp_init_data.hdcp_io       = &parser->get_io(parser,
						"hdcp_physical")->io;
	hdcp_init_data.revision      = &dp->panel->link_info.revision;
	hdcp_init_data.msm_hdcp_dev  = dp->msm_hdcp_dev;
	hdcp_init_data.forced_encryption = parser->has_force_encryption;

	fd = sde_hdcp_1x_init(&hdcp_init_data);
	if (IS_ERR_OR_NULL(fd)) {
		DP_DEBUG("DP%d Error initializing HDCP 1.x\n", dp->cell_idx);
		return -EINVAL;
	}

	dp->hdcp.dev[HDCP_VERSION_1X].fd = fd;
	dp->hdcp.dev[HDCP_VERSION_1X].ops = sde_hdcp_1x_get(fd);
	dp->hdcp.dev[HDCP_VERSION_1X].ver = HDCP_VERSION_1X;
	DP_INFO("DP%d HDCP 1.3 initialized\n", dp->cell_idx);

	fd = sde_dp_hdcp2p2_init(&hdcp_init_data);
	if (IS_ERR_OR_NULL(fd)) {
		DP_DEBUG("DP%d Error initializing HDCP 2.x\n", dp->cell_idx);
		rc = -EINVAL;
		goto error;
	}

	dp->hdcp.dev[HDCP_VERSION_2P2].fd = fd;
	dp->hdcp.dev[HDCP_VERSION_2P2].ops = sde_dp_hdcp2p2_get(fd);
	dp->hdcp.dev[HDCP_VERSION_2P2].ver = HDCP_VERSION_2P2;
	DP_INFO("DP%d HDCP 2.2 initialized\n", dp->cell_idx);

	return 0;
error:
	sde_hdcp_1x_deinit(dp->hdcp.dev[HDCP_VERSION_1X].fd);

	return rc;
}

static void dp_display_pause_audio(struct dp_display_private *dp, bool pause)
{
	struct dp_panel *dp_panel;
	int idx;

	for (idx = DP_STREAM_0; idx < DP_STREAM_MAX; idx++) {
		if (!dp->active_panels[idx])
			continue;
		dp_panel = dp->active_panels[idx];

		if (dp_panel->audio_supported)
			dp_panel->audio->tui_active = pause;
	}
}

static int dp_display_pre_hw_release(void *data)
{
	struct dp_display_private *dp;
	struct dp_display *dp_display = data;

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY);

	if (!dp_display)
		return -EINVAL;

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	mutex_lock(&dp->session_lock);

	dp_display_state_add(DP_STATE_TUI_ACTIVE);
	cancel_work_sync(&dp->connect_work);
	cancel_work_sync(&dp->attention_work);
	flush_workqueue(dp->wq);

	dp_display_pause_audio(dp, true);
	disable_irq(dp->irq);

	mutex_unlock(&dp->session_lock);

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT);
	return 0;
}

static int dp_display_post_hw_acquire(void *data)
{
	struct dp_display_private *dp;
	struct dp_display *dp_display = data;

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY);

	if (!dp_display)
		return -EINVAL;

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	mutex_lock(&dp->session_lock);

	dp_display_state_remove(DP_STATE_TUI_ACTIVE);
	dp_display_pause_audio(dp, false);
	enable_irq(dp->irq);

	mutex_unlock(&dp->session_lock);

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT);
	return 0;
}

static int dp_display_get_cell_info(struct dp_display_private *dp)
{
	struct device_node *of_node = dp->pdev->dev.of_node;
	int i, rc;

	of_property_read_u32(of_node,
			"cell-index", &dp->cell_idx);

	if (of_property_read_bool(of_node, "qcom,mst-enable"))
		dp->stream_cnt = DP_STREAM_MAX;

	of_property_read_u32_index(of_node,
			"qcom,intf-index", 0, &dp->intf_idx[0]);

	for (i = 1; i < dp->stream_cnt; i++) {
		rc = of_property_read_u32_index(of_node,
				"qcom,intf-index", i, &dp->intf_idx[i]);
		if (rc)
			dp->intf_idx[i] = dp->intf_idx[0] + i;
	}

	of_property_read_u32(of_node,
			"qcom,phy-index", &dp->phy_idx);

	return 0;
}

static ssize_t status_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct dp_display_private *dp = NULL;
	struct drm_connector *connector;

	char *p = buf;

	if (!dev) {
		pr_err("invalid device pointer\n");
		return -ENODEV;
	}

	dp = dev_get_drvdata(dev);
	if (!dp) {
		pr_err("invalid driver pointer\n");
		return -ENODEV;
	}

	connector = dp->dp_display.base_connector;

	if (!connector) {
		pr_err("DP%d connector not set\n", dp->cell_idx);
		p += scnprintf(p, PAGE_SIZE, "connector not set");
		goto out;
	}

	p += scnprintf(p, PAGE_SIZE + buf - p, "name=%s", connector->name);

	p += scnprintf(p, PAGE_SIZE + buf - p, " status=%s",
			dp->hpd->hpd_high ? "connected" : "disconnected");

	if ((dp->aux->state & DP_STATE_TRAIN_1_SUCCEEDED) &&
			(dp->aux->state & DP_STATE_TRAIN_2_SUCCEEDED))
		p += scnprintf(p, PAGE_SIZE + buf - p, " link=ready");
	else if ((dp->aux->state & DP_STATE_TRAIN_1_FAILED) ||
			(dp->aux->state & DP_STATE_TRAIN_2_FAILED))
		p += scnprintf(p, PAGE_SIZE + buf - p, " link=failed");
	else if ((dp->aux->state & DP_STATE_TRAIN_1_STARTED) ||
			(dp->aux->state & DP_STATE_TRAIN_2_STARTED))
		p += scnprintf(p, PAGE_SIZE + buf - p, " link=training");
	else if (dp->aux->state & DP_STATE_LINK_MAINTENANCE_STARTED)
		p += scnprintf(p, PAGE_SIZE + buf - p, " link=maintaining");
	else
		p += scnprintf(p, PAGE_SIZE + buf - p, " link=not_ready");
	p += scnprintf(p, PAGE_SIZE + buf - p, " stream=%s",
			(dp->aux->state & DP_STATE_CTRL_POWERED_ON) ? "ON" : "OFF");
	p += scnprintf(p, PAGE_SIZE + buf - p, " state=0x%X", dp->aux->state);

out:
	return p - buf;
}

static DEVICE_ATTR_RO(status);

static struct attribute *dp_fs_attrs[] = {
	&dev_attr_status.attr,
	NULL
};

static struct attribute_group dp_fs_attr_group = {
	.attrs = dp_fs_attrs
};

static int dp_display_sysfs_init(struct dp_display_private *dp)
{
	int ret;

	ret = sysfs_create_group(&dp->pdev->dev.kobj, &dp_fs_attr_group);
	if (ret)
		pr_err("DP%d unable to register dp_display sysfs nodes\n", dp->cell_idx);

	return 0;
}

static int dp_display_sysfs_deinit(struct dp_display_private *dp)
{
	sysfs_remove_group(&dp->pdev->dev.kobj, &dp_fs_attr_group);
	return 0;
}

static int dp_display_bind(struct device *dev, struct device *master,
		void *data)
{
	int rc = 0;
	struct dp_display_private *dp;
	struct drm_device *drm;
	struct platform_device *pdev = to_platform_device(dev);
	struct msm_vm_ops vm_event_ops = {
		.vm_pre_hw_release = dp_display_pre_hw_release,
		.vm_post_hw_acquire = dp_display_post_hw_acquire,
	};

	if (!dev || !pdev || !master) {
		DP_ERR("invalid param(s), dev %pK, pdev %pK, master %pK\n",
				dev, pdev, master);
		rc = -EINVAL;
		goto end;
	}

	drm = dev_get_drvdata(master);
	dp = platform_get_drvdata(pdev);
	if (!drm || !dp) {
		DP_ERR("invalid param(s), drm %pK, dp %pK\n",
				drm, dp);
		rc = -EINVAL;
		goto end;
	}

	dp->dp_display.drm_dev = drm;
	dp->priv = drm->dev_private;
	msm_register_vm_event(master, dev, &vm_event_ops,
			(void *)&dp->dp_display);
end:
	return rc;
}

static void dp_display_unbind(struct device *dev, struct device *master,
		void *data)
{
	struct dp_display_private *dp;
	struct platform_device *pdev = to_platform_device(dev);

	if (!dev || !pdev) {
		DP_ERR("invalid param(s)\n");
		return;
	}

	dp = platform_get_drvdata(pdev);
	if (!dp) {
		DP_ERR("Invalid params\n");
		return;
	}

	if (dp->power)
		(void)dp->power->power_client_deinit(dp->power);
	if (dp->aux)
		(void)dp->aux->drm_aux_deregister(dp->aux);
	dp_display_deinitialize_hdcp(dp);
	dp_display_sysfs_deinit(dp);
}

static const struct component_ops dp_display_comp_ops = {
	.bind = dp_display_bind,
	.unbind = dp_display_unbind,
};

static bool dp_display_send_hpd_event(struct dp_display_private *dp)
{
	struct drm_device *dev = NULL;
	struct drm_connector *connector;
	char name[HPD_STRING_SIZE], status[HPD_STRING_SIZE],
		bpp[HPD_STRING_SIZE], pattern[HPD_STRING_SIZE];
	char *envp[6];
	struct dp_display *display;
	int rc = 0;

	connector = dp->dp_display.base_connector;
	display = &dp->dp_display;

	if (!connector) {
		DP_ERR("DP%d connector not set\n", dp->cell_idx);
		return false;
	}

	connector->status = display->is_sst_connected ? connector_status_connected :
			connector_status_disconnected;

	if (dp->cached_connector_status == connector->status) {
		DP_DEBUG("DP%d connector status (%d) unchanged, skipping uevent\n",
				dp->cell_idx, dp->cached_connector_status);
		return false;
	}

	dp->cached_connector_status = connector->status;

	dev = connector->dev;

	if (dp->debug->skip_uevent) {
		DP_INFO("DP%d skipping uevent\n", dp->cell_idx);
		return false;
	}

	snprintf(name, HPD_STRING_SIZE, "name=%s", connector->name);
	snprintf(status, HPD_STRING_SIZE, "status=%s",
		drm_get_connector_status_name(connector->status));
	snprintf(bpp, HPD_STRING_SIZE, "bpp=%d",
		dp_link_bit_depth_to_bpp(
		dp->link->test_video.test_bit_depth));
	snprintf(pattern, HPD_STRING_SIZE, "pattern=%d",
		dp->link->test_video.test_video_pattern);

	DP_INFO("DP%d [%s]:[%s] [%s] [%s]\n", dp->cell_idx,
			name, status, bpp, pattern);
	envp[0] = name;
	envp[1] = status;
	envp[2] = bpp;
	envp[3] = pattern;
	envp[4] = "HOTPLUG=1";
	envp[5] = NULL;

	rc = kobject_uevent_env(&dev->primary->kdev->kobj, KOBJ_CHANGE, envp);
	DP_INFO("DP%d uevent %s: %d\n", dp->cell_idx,
			rc ? "failure" : "success", rc);

	return true;
}

static int dp_display_send_hpd_notification(struct dp_display_private *dp)
{
	int ret = 0;
	bool hpd = !!dp_display_state_is(DP_STATE_CONNECTED);

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state, hpd);

	/*
	 * Send the notification only if there is any change. This check is
	 * necessary since it is possible that the connect_work may or may not
	 * skip sending the notification in order to respond to a pending
	 * attention message. Attention work thread will always attempt to
	 * send the notification after successfully handling the attention
	 * message. This check here will avoid any unintended duplicate
	 * notifications.
	 */
	if (dp_display_state_is(DP_STATE_CONNECT_NOTIFIED) && hpd) {
		DP_DEBUG("DP%d connection notified already, skip notification\n",
				dp->cell_idx);
		goto skip;
	} else if (dp_display_state_is(DP_STATE_DISCONNECT_NOTIFIED) && !hpd) {
		DP_DEBUG("DP%d disonnect notified already, skip notification\n",
				dp->cell_idx);
		goto skip;
	}

	dp->aux->state |= DP_STATE_NOTIFICATION_SENT;

	if (!dp->mst.mst_active) {
		dp->dp_display.is_sst_connected = hpd;

		if (!dp_display_send_hpd_event(dp))
			goto skip;
	} else {
		dp->dp_display.is_sst_connected = false;

		if (!dp->mst.cbs.hpd)
			goto skip;

		dp->mst.cbs.hpd(&dp->dp_display, hpd);
	}

	if (hpd) {
		dp_display_state_add(DP_STATE_CONNECT_NOTIFIED);
		dp_display_state_remove(DP_STATE_DISCONNECT_NOTIFIED);
	} else {
		dp_display_state_add(DP_STATE_DISCONNECT_NOTIFIED);
		dp_display_state_remove(DP_STATE_CONNECT_NOTIFIED);
	}

skip:
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state, hpd, ret);
	return ret;
}

static void dp_display_send_force_connect_event(struct dp_display_private *dp)
{
	struct drm_device *dev = NULL;
	struct drm_connector *connector;
	char name[HPD_STRING_SIZE];
	char *envp[5];

	connector = dp->dp_display.base_connector;

	if (!connector) {
		DP_ERR("DP%d connector not set\n", dp->cell_idx);
		return;
	}

	dev = connector->dev;

	snprintf(name, HPD_STRING_SIZE, "name=%s", connector->name);

	envp[0] = name;
	envp[1] = dp->hpd->hpd_high ? "sink=connected" : "sink=disconnected";
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
	DP_INFO("[%s]:[%s] [%s] [%s]\n", name, envp[1], envp[2], envp[3]);

	kobject_uevent_env(&dev->primary->kdev->kobj, KOBJ_CHANGE, envp);
}

static void dp_display_update_mst_state(struct dp_display_private *dp,
					bool state)
{
	dp->mst.mst_active = state;
	dp->panel->mst_state = state;
}

static void dp_display_mst_init(struct dp_display_private *dp)
{
	bool is_mst_receiver;
	const unsigned long clear_mstm_ctrl_timeout_us = 100000;
	u8 old_mstm_ctrl;
	int ret;

	if (!dp->parser->has_mst || !dp->mst.drm_registered) {
		DP_MST_DEBUG("mst not enabled. has_mst:%d, registered:%d\n",
				dp->parser->has_mst, dp->mst.drm_registered);
		return;
	}

	is_mst_receiver = dp->panel->read_mst_cap(dp->panel);

	if (!is_mst_receiver) {
		DP_MST_DEBUG("sink doesn't support mst\n");
		return;
	}

	/* clear sink mst state */
	drm_dp_dpcd_readb(dp->aux->drm_aux, DP_MSTM_CTRL, &old_mstm_ctrl);
	drm_dp_dpcd_writeb(dp->aux->drm_aux, DP_MSTM_CTRL, 0);

	/* add extra delay if MST state is not cleared */
	if (old_mstm_ctrl) {
		DP_MST_DEBUG("MSTM_CTRL is not cleared, wait %luus\n",
				clear_mstm_ctrl_timeout_us);
		usleep_range(clear_mstm_ctrl_timeout_us,
			clear_mstm_ctrl_timeout_us + 1000);
	}

	ret = drm_dp_dpcd_writeb(dp->aux->drm_aux, DP_MSTM_CTRL,
				DP_MST_EN | DP_UP_REQ_EN | DP_UPSTREAM_IS_SRC);
	if (ret < 0) {
		DP_ERR("DP%d sink mst enablement failed\n", dp->cell_idx);
		return;
	}

	dp_display_update_mst_state(dp, true);
}

static void dp_display_set_mst_mgr_state(struct dp_display_private *dp,
					bool state)
{
	if (!dp->mst.mst_active)
		return;

	if (dp->mst.cbs.set_mgr_state)
		dp->mst.cbs.set_mgr_state(&dp->dp_display, state);

	DP_MST_DEBUG("mst_mgr_state: %d\n", state);
}

static void dp_display_change_phy_bond_mode(struct dp_display_private *dp,
		enum dp_phy_bond_mode mode)
{
	if (dp->phy_bond_mode != mode)
		DP_INFO("DP%d  %d -> %d\n", dp->cell_idx,
				dp->phy_bond_mode, mode);

	dp->phy_bond_mode = mode;
	/* Propagate to dp_ctrl, dp_catalog, dp_power and dp_panel */
	dp->ctrl->set_phy_bond_mode(dp->ctrl, mode);
}

static int dp_display_host_init(struct dp_display_private *dp)
{
	bool flip = false;
	bool reset;
	int rc = 0;

	if (dp_display_state_is(DP_STATE_INITIALIZED)) {
		dp_display_state_log("[already initialized]");
		return rc;
	}

	if (dp->hpd->orientation == ORIENTATION_CC2)
		flip = true;

	reset = dp->debug->sim_mode ? false : !dp->hpd->multi_func;

	rc = dp->power->init(dp->power, flip);
	if (rc) {
		DP_WARN("DP%d Power init failed.\n", dp->cell_idx);
		SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_CASE1, dp->state);
		return rc;
	}

	dp->hpd->host_init(dp->hpd, &dp->catalog->hpd);
	rc = dp->ctrl->init(dp->ctrl, flip, reset);
	if (rc) {
		DP_WARN("DP%d Ctrl init Failed.\n", dp->cell_idx);
		SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_CASE2, dp->state);
		goto error_ctrl;
	}

	enable_irq(dp->irq);
	dp_display_abort_hdcp(dp, false);

	dp_display_state_remove(DP_STATE_ABORTED);
	dp_display_state_add(DP_STATE_INITIALIZED);

	/* log this as it results from user action of cable connection */
	DP_INFO("DP%d [OK]\n", dp->cell_idx);
	return rc;

error_ctrl:
	dp->hpd->host_deinit(dp->hpd, &dp->catalog->hpd);
	dp->power->deinit(dp->power);
	return rc;
}

static int dp_display_host_ready(struct dp_display_private *dp)
{
	int rc = 0;

	if (!dp_display_state_is(DP_STATE_INITIALIZED)) {
		rc = dp_display_host_init(dp);
		if (rc) {
			dp_display_state_show("[not initialized]");
			return rc;
		}
	}

	if (dp_display_state_is(DP_STATE_READY)) {
		dp_display_state_log("[already ready]");
		return rc;
	}

	/*
	 * Reset the aborted state for AUX and CTRL modules. This will
	 * allow these modules to execute normally in response to the
	 * cable connection event.
	 *
	 * One corner case still exists. While the execution flow ensures
	 * that cable disconnection flushes all pending work items on the DP
	 * workqueue, and waits for the user module to clean up the DP
	 * connection session, it is possible that the system delays can
	 * lead to timeouts in the connect path. As a result, the actual
	 * connection callback from user modules can come in late and can
	 * race against a subsequent connection event here which would have
	 * reset the aborted flags. There is no clear solution for this since
	 * the connect/disconnect notifications do not currently have any
	 * sessions IDs.
	 */
	dp->aux->abort(dp->aux, false);
	dp->ctrl->abort(dp->ctrl, false);

	dp->aux->init(dp->aux, dp->parser->aux_cfg);
	dp->panel->init(dp->panel);

	dp_display_state_add(DP_STATE_READY);
	/* log this as it results from user action of cable connection */
	DP_INFO("DP%d [OK]\n", dp->cell_idx);
	return rc;
}

static void dp_display_host_unready(struct dp_display_private *dp)
{
	if (!dp_display_state_is(DP_STATE_INITIALIZED)) {
		dp_display_state_warn("[not initialized]");
		return;
	}

	if (!dp_display_state_is(DP_STATE_READY)) {
		dp_display_state_show("[not ready]");
		return;
	}

	dp_display_state_remove(DP_STATE_READY);
	dp->aux->deinit(dp->aux);
	/* log this as it results from user action of cable disconnection */
	DP_INFO("DP%d [OK]\n", dp->cell_idx);
}

static void dp_display_host_deinit(struct dp_display_private *dp)
{
	if (dp->active_stream_cnt) {
		SDE_EVT32_EXTERNAL(dp->state, dp->active_stream_cnt);
		DP_DEBUG("DP%d active stream present\n", dp->cell_idx);
		return;
	}

	if (!dp_display_state_is(DP_STATE_INITIALIZED)) {
		dp_display_state_show("[not initialized]");
		return;
	}

	dp_display_abort_hdcp(dp, true);
	dp->ctrl->deinit(dp->ctrl);
	dp->hpd->host_deinit(dp->hpd, &dp->catalog->hpd);
	dp->power->deinit(dp->power);
	disable_irq(dp->irq);
	dp->aux->state = 0;

	dp_display_state_remove(DP_STATE_INITIALIZED);

	/* log this as it results from user action of cable dis-connection */
	DP_INFO("DP%d [OK]\n", dp->cell_idx);
}

static int dp_display_process_hpd_high(struct dp_display_private *dp,
		bool force)
{
	int rc = -EINVAL;
	unsigned long wait_timeout_ms;
	unsigned long t;

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state);
	mutex_lock(&dp->session_lock);

	if (dp_display_state_is(DP_STATE_CONNECTED) && !force) {
		DP_DEBUG("DP%d dp already connected, skipping hpd high\n",
				dp->cell_idx);
		mutex_unlock(&dp->session_lock);
		return -EISCONN;
	}

	dp_display_state_add(DP_STATE_CONNECTED);

	dp->dp_display.max_pclk_khz = min(dp->parser->max_pclk_khz,
					dp->debug->max_pclk_khz);
	dp->dp_display.force_bond_mode = dp->parser->force_bond_mode ||
					dp->debug->force_bond_mode;

	/*
	 * If dp video session is not restored from a previous session teardown
	 * by userspace, ensure the host_init is executed, in such a scenario,
	 * so that all the required DP resources are enabled.
	 *
	 * Below is one of the sequences of events which describe the above
	 * scenario:
	 *  a. Source initiated power down resulting in host_deinit.
	 *  b. Sink issues hpd low attention without physical cable disconnect.
	 *  c. Source initiated power up sequence returns early because hpd is
	 *     not high.
	 *  d. Sink issues a hpd high attention event.
	 */
	if (dp_display_state_is(DP_STATE_SRC_PWRDN) &&
			dp_display_state_is(DP_STATE_CONFIGURED)) {
		rc = dp_display_host_init(dp);
		if (rc) {
			DP_WARN("DP%d Host init Failed", dp->cell_idx);
			if (!dp_display_state_is(DP_STATE_SUSPENDED)) {
				/*
				 * If not suspended no point of going forward if
				 * resource is not enabled.
				 */
				dp_display_state_remove(DP_STATE_CONNECTED);
			}
			goto end;
		}

		/*
		 * If device is suspended and host_init fails, there is
		 * one more chance for host init to happen in prepare which
		 * is why DP_STATE_SRC_PWRDN is removed only at success.
		 */
		dp_display_state_remove(DP_STATE_SRC_PWRDN);
	}

	rc = dp_display_host_ready(dp);
	if (rc) {
		dp_display_state_show("[ready failed]");
		goto end;
	}

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
		if (!dp->parser->force_connect_mode)
			dp_display_state_remove(DP_STATE_CONNECTED);
		goto end;
	}

	dp->link->process_request(dp->link);
	dp->panel->handle_sink_request(dp->panel);

	dp_display_mst_init(dp);

	rc = dp->ctrl->on(dp->ctrl, dp->mst.mst_active,
			dp->panel->fec_en, dp->panel->dsc_en,
			dp->parser->force_connect_mode ?
			LINK_TRAINING_MODE_FORCE : LINK_TRAINING_MODE_NORMAL);
	if (rc) {
		if (!dp->parser->force_connect_mode)
			dp_display_state_remove(DP_STATE_CONNECTED);
		goto end;
	}

	dp->process_hpd_connect = false;

	dp_display_set_mst_mgr_state(dp, true);
end:
	mutex_unlock(&dp->session_lock);

	/*
	 * Delay the HPD connect notification to see if sink generates any
	 * IRQ HPDs immediately after the HPD high.
	 */
	reinit_completion(&dp->attention_comp);
	wait_timeout_ms = min_t(unsigned long,
			dp->debug->connect_notification_delay_ms,
			(unsigned long) MAX_CONNECT_NOTIFICATION_DELAY_MS);
	t = wait_for_completion_timeout(&dp->attention_comp,
		msecs_to_jiffies(wait_timeout_ms));
	DP_DEBUG("DP%d wait_timeout=%lu ms, time_waited=%u ms\n", dp->cell_idx,
			wait_timeout_ms, jiffies_to_msecs(t));

	/*
	 * If an IRQ HPD is pending, then do not send a connect notification.
	 * Once this work returns, the IRQ HPD would be processed and any
	 * required actions (such as link maintenance) would be done which
	 * will subsequently send the HPD notification. To keep things simple,
	 * do this only for SST use-cases. MST use cases require additional
	 * care in order to handle the side-band communications as well.
	 *
	 * One of the main motivations for this is DP LL 1.4 CTS use case
	 * where it is possible that we could get a test request right after
	 * a connection, and the strict timing requriements of the test can
	 * only be met if we do not wait for the e2e connection to be set up.
	 */
	if (!dp->mst.mst_active &&
		(work_busy(&dp->attention_work) == WORK_BUSY_PENDING)) {
		SDE_EVT32_EXTERNAL(dp->state, 99, jiffies_to_msecs(t));
		DP_DEBUG("DP%d Attention pending, skip HPD notification\n",
				dp->cell_idx);
		goto skip_notify;
	}

	if (dp->parser->force_connect_mode)
		dp_display_send_force_connect_event(dp);
	else if (!rc && !dp_display_state_is(DP_STATE_ABORTED))
		dp_display_send_hpd_notification(dp);

skip_notify:

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state,
		wait_timeout_ms, rc);
	return rc;
}

static void dp_display_process_mst_hpd_low(struct dp_display_private *dp)
{
	int rc = 0;

	if (dp->mst.mst_active) {
		DP_MST_DEBUG("mst_hpd_low work\n");

		/*
		 * HPD unplug callflow:
		 * 1. send hpd unplug on base connector so usermode can disable
                 * all external displays.
		 * 2. unset mst state in the topology mgr so the branch device
		 *  can be cleaned up.
		 */

		if ((dp_display_state_is(DP_STATE_CONNECT_NOTIFIED) ||
				dp_display_state_is(DP_STATE_ENABLED)))
			rc = dp_display_send_hpd_notification(dp);

		dp_display_set_mst_mgr_state(dp, false);
		dp_display_update_mst_state(dp, false);
	}

	DP_MST_DEBUG("mst_hpd_low. mst_active:%d\n", dp->mst.mst_active);
}

static int dp_display_process_hpd_low(struct dp_display_private *dp)
{
	int rc = 0;

	dp_display_state_remove(DP_STATE_CONNECTED);
	dp->process_hpd_connect = false;
	dp_audio_enable(dp, false);

	if (dp->mst.mst_active) {
		dp_display_process_mst_hpd_low(dp);
	} else {
		if ((dp_display_state_is(DP_STATE_CONNECT_NOTIFIED) ||
				dp_display_state_is(DP_STATE_ENABLED)))
			rc = dp_display_send_hpd_notification(dp);
	}

	dp->panel->video_test = false;

	return rc;
}

static int dp_display_aux_switch_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	return 0;
}

static int dp_display_init_aux_switch(struct dp_display_private *dp)
{
	int rc = 0;
	struct notifier_block nb;
	const u32 max_retries = 50;
	u32 retry;

	if (dp->aux_switch_ready)
	       return rc;

	if (!dp->aux->switch_register_notifier)
		return rc;

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY);

	nb.notifier_call = dp_display_aux_switch_callback;
	nb.priority = 0;

	/*
	 * Iteratively wait for reg notifier which confirms that fsa driver is probed.
	 * Bootup DP with cable connected usecase can hit this scenario.
	 */
	for (retry = 0; retry < max_retries; retry++) {
		rc = dp->aux->switch_register_notifier(&nb, dp->aux_switch_node);
		if (rc == 0) {
			DP_DEBUG("registered notifier successfully\n");
			dp->aux_switch_ready = true;
			break;
		} else {
			DP_DEBUG("failed to register notifier retry=%d rc=%d\n", retry, rc);
			msleep(100);
		}
	}

	if (retry == max_retries) {
		DP_WARN("Failed to register fsa notifier\n");
		dp->aux_switch_ready = false;
		return rc;
	}

	if (dp->aux->switch_unregister_notifier)
		dp->aux->switch_unregister_notifier(&nb, dp->aux_switch_node);

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, rc);
	return rc;
}

static int dp_display_usbpd_configure_cb(struct device *dev)
{
	int rc = 0;
	struct dp_display_private *dp;

	if (!dev) {
		DP_ERR("invalid dev\n");
		return -EINVAL;
	}

	dp = dev_get_drvdata(dev);
	if (!dp) {
		DP_ERR("no driver data found\n");
		return -ENODEV;
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
			DP_ERR("DP%d driver is not loaded\n", dp->cell_idx);
			return -ENODEV;
		}
	}

	if (!dp->debug->sim_mode && !dp->parser->no_aux_switch
	    && !dp->parser->gpio_aux_switch && dp->aux_switch_node && dp->aux->switch_configure) {
		rc = dp_display_init_aux_switch(dp);
		if (rc)
			return rc;

		rc = dp->aux->switch_configure(dp->aux, true, dp->hpd->orientation);
		if (rc)
			return rc;
	}

	mutex_lock(&dp->session_lock);

	if (dp_display_state_is(DP_STATE_TUI_ACTIVE)) {
		dp_display_state_log("[TUI is active]");
		mutex_unlock(&dp->session_lock);
		return 0;
	}

	dp->aux->abort(dp->aux, false);
	dp->ctrl->abort(dp->ctrl, false);
	dp_display_state_remove(DP_STATE_ABORTED);
	dp_display_state_add(DP_STATE_CONFIGURED);

	rc = dp_display_host_init(dp);
	if (rc) {
		DP_ERR("DP%d Host init Failed", dp->cell_idx);
		mutex_unlock(&dp->session_lock);
		return rc;
	}

	/* check for hpd high */
	if (dp->hpd->hpd_high)
		queue_work(dp->wq, &dp->connect_work);
	else
		dp->process_hpd_connect = true;
	mutex_unlock(&dp->session_lock);

	return 0;
}

static void dp_display_clear_dsc_resources(struct dp_display_private *dp,
		struct dp_panel *panel)
{
	dp->tot_dsc_blks_in_use -= panel->dsc_blks_in_use;
	panel->dsc_blks_in_use = 0;
}

static int dp_display_get_mst_pbn_div(struct dp_display *dp_display)
{
	struct dp_display_private *dp;
	u32 link_rate, lane_count;

	if (!dp_display) {
		DP_ERR("invalid params\n");
		return 0;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	link_rate = drm_dp_bw_code_to_link_rate(dp->link->link_params.bw_code);
	lane_count = dp->link->link_params.lane_count;

	return link_rate * lane_count / 54000;
}

static int dp_display_stream_pre_disable(struct dp_display_private *dp,
			struct dp_panel *dp_panel)
{
	if (!dp->active_stream_cnt) {
		DP_WARN("DP%d streams already disabled cnt=%d\n", dp->cell_idx,
				dp->active_stream_cnt);
		return 0;
	}

	dp->ctrl->stream_pre_off(dp->ctrl, dp_panel);

	return 0;
}

static void dp_display_stream_disable(struct dp_display_private *dp,
			struct dp_panel *dp_panel)
{
	if (!dp->active_stream_cnt) {
		DP_WARN("DP%d streams already disabled cnt=%d\n", dp->cell_idx,
				dp->active_stream_cnt);
		return;
	}

	if (dp_panel->stream_id == DP_STREAM_MAX ||
			!dp->active_panels[dp_panel->stream_id]) {
		DP_ERR("DP%d panel is already disabled\n", dp->cell_idx);
		return;
	}

	dp_display_clear_dsc_resources(dp, dp_panel);

	DP_DEBUG("DP%d stream_id=%d, active_stream_cnt=%d, tot_dsc_blks_in_use=%d\n",
			dp->cell_idx, dp_panel->stream_id, dp->active_stream_cnt,
			dp->tot_dsc_blks_in_use);

	dp->ctrl->stream_off(dp->ctrl, dp_panel);
	dp->active_panels[dp_panel->stream_id] = NULL;
	dp->active_stream_cnt--;
}

static void dp_display_clean(struct dp_display_private *dp)
{
	int idx;
	struct dp_panel *dp_panel;
	struct dp_link_hdcp_status *status = &dp->link->hdcp_status;

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state);

	if (dp_display_state_is(DP_STATE_TUI_ACTIVE)) {
		DP_WARN("DP%d TUI is active\n", dp->cell_idx);
		return;
	}

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

	if (dp->parser->force_connect_mode)
		dp_display_state_remove(DP_STATE_ENABLED)
	else
		dp_display_state_remove(DP_STATE_ENABLED | DP_STATE_CONNECTED);

	dp->ctrl->off(dp->ctrl);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state);
}

static int dp_display_handle_disconnect(struct dp_display_private *dp)
{
	int rc;

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state);

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
		dp->aux->abort(dp->aux, false);
		dp->ctrl->abort(dp->ctrl, false);

		dp_display_send_force_connect_event(dp);

		dp_display_process_hpd_high(dp, false);

		/* If stream isn't running, started here */
		if (!dp_display_state_is(DP_STATE_ENABLED) && dp->dp_display.base_connector)
			sde_connector_helper_mode_change_commit(
					dp->dp_display.base_connector);

		SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state);
		return 0;
	}

	rc = dp_display_process_hpd_low(dp);

	/* cancel any pending request */
	dp->ctrl->abort(dp->ctrl, true);
	dp->aux->abort(dp->aux, true);

	mutex_lock(&dp->session_lock);
	/**
	 * Can't disable the clock for the bond PLL here.
	 * The clock tear down sequence need to be guaranteed.
	 * Will be handled in dp_display_unprepare via bond bridge.
	 */
	if (!dp->active_stream_cnt && !IS_BOND_MODE(dp->phy_bond_mode)) {
		dp_display_clean(dp);
		dp_display_host_unready(dp);
	}
	mutex_unlock(&dp->session_lock);

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state);

	return rc;
}

static void dp_display_disconnect_sync(struct dp_display_private *dp)
{
	int disconnect_delay_ms;

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state);
	/* cancel any pending request */
	dp_display_state_add(DP_STATE_ABORTED);

	dp->ctrl->abort(dp->ctrl, true);
	dp->aux->abort(dp->aux, true);

	/* wait for idle state */
	cancel_work_sync(&dp->connect_work);
	cancel_work_sync(&dp->attention_work);
	flush_workqueue(dp->wq);

	/*
	 * Delay the teardown of the mainlink for better interop experience.
	 * It is possible that certain sinks can issue an HPD high immediately
	 * following an HPD low as soon as they detect the mainlink being
	 * turned off. This can sometimes result in the HPD low pulse getting
	 * lost with certain cable. This issue is commonly seen when running
	 * DP LL CTS test 4.2.1.3.
	 */
	disconnect_delay_ms = min_t(u32, dp->debug->disconnect_delay_ms,
			(u32) MAX_DISCONNECT_DELAY_MS);
	DP_DEBUG("DP%d disconnect delay = %d ms\n",
			dp->cell_idx, disconnect_delay_ms);
	msleep(disconnect_delay_ms);

	dp_display_handle_disconnect(dp);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state,
		disconnect_delay_ms);
}

static int dp_display_usbpd_disconnect_cb(struct device *dev)
{
	int rc = 0;
	struct dp_display_private *dp;

	if (!dev) {
		DP_ERR("invalid dev\n");
		rc = -EINVAL;
		goto end;
	}

	dp = dev_get_drvdata(dev);
	if (!dp) {
		DP_ERR("no driver data found\n");
		rc = -ENODEV;
		goto end;
	}

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state,
			dp->debug->psm_enabled);

	/* skip if a disconnect is already in progress */
	if (dp_display_state_is(DP_STATE_ABORTED) &&
	    dp_display_state_is(DP_STATE_READY)) {
		DP_DEBUG("DP%d disconnect already in progress\n", dp->cell_idx);
		SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_CASE1, dp->state);
		return 0;
	}

	if (dp->debug->psm_enabled && dp_display_state_is(DP_STATE_READY))
		dp->link->psm_config(dp->link, &dp->panel->link_info, true);

	dp_display_disconnect_sync(dp);

	if (!dp->parser->force_connect_mode) {
		mutex_lock(&dp->session_lock);
		dp_display_host_deinit(dp);
		dp_display_state_remove(DP_STATE_CONFIGURED);
		mutex_unlock(&dp->session_lock);
	}

	if (!dp->debug->sim_mode && !dp->parser->no_aux_switch
	    && !dp->parser->gpio_aux_switch && dp->aux->switch_configure)
		dp->aux->switch_configure(dp->aux, false, ORIENTATION_NONE);

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state);
end:
	return rc;
}

static int dp_display_stream_enable(struct dp_display_private *dp,
			struct dp_panel *dp_panel)
{
	int rc = 0;

	rc = dp->ctrl->stream_on(dp->ctrl, dp_panel);

	if (dp->debug->tpg_pattern)
		dp_panel->tpg_config(dp_panel, dp->debug->tpg_pattern);

	if (!rc) {
		dp->active_panels[dp_panel->stream_id] = dp_panel;
		dp->active_stream_cnt++;
	}


	DP_DEBUG("DP%d dp active_stream_cnt:%d, tot_dsc_blks_in_use=%d\n",
			dp->cell_idx, dp->active_stream_cnt, dp->tot_dsc_blks_in_use);

	return rc;
}

static void dp_display_mst_attention(struct dp_display_private *dp)
{
	if (dp->mst.mst_active && dp->mst.cbs.hpd_irq)
		dp->mst.cbs.hpd_irq(&dp->dp_display);

	DP_MST_DEBUG("mst_attention_work. mst_active:%d\n", dp->mst.mst_active);
}

static void dp_display_attention_work(struct work_struct *work)
{
	struct dp_display_private *dp = container_of(work,
			struct dp_display_private, attention_work);
	int rc = 0;

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state);
	mutex_lock(&dp->session_lock);
	SDE_EVT32_EXTERNAL(dp->state);

	if (dp_display_state_is(DP_STATE_ABORTED)) {
		DP_INFO("DP%d Hpd off, not handling any attention\n", dp->cell_idx);
		mutex_unlock(&dp->session_lock);
		goto exit;
	}

	if (!dp_display_state_is(DP_STATE_READY)) {
		mutex_unlock(&dp->session_lock);
		goto mst_attention;
	}

	if (dp->link->process_request(dp->link)) {
		mutex_unlock(&dp->session_lock);
		goto cp_irq;
	}

	mutex_unlock(&dp->session_lock);
	SDE_EVT32_EXTERNAL(dp->state, dp->link->sink_request);

	if (dp->link->sink_request & DS_PORT_STATUS_CHANGED) {
		SDE_EVT32_EXTERNAL(dp->state, DS_PORT_STATUS_CHANGED);
		if (dp_display_is_sink_count_zero(dp)) {
			dp_display_handle_disconnect(dp);
		} else {
			/*
			 * connect work should take care of sending
			 * the HPD notification.
			 */
			if (!dp->mst.mst_active)
				queue_work(dp->wq, &dp->connect_work);
		}

		goto mst_attention;
	}

	if (dp->link->sink_request & DP_TEST_LINK_VIDEO_PATTERN) {
		SDE_EVT32_EXTERNAL(dp->state, DP_TEST_LINK_VIDEO_PATTERN);
		dp_display_handle_disconnect(dp);

		dp->panel->video_test = true;
		/*
		 * connect work should take care of sending
		 * the HPD notification.
		 */
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

	if (dp->link->sink_request & (DP_TEST_LINK_PHY_TEST_PATTERN |
		DP_TEST_LINK_TRAINING | DP_LINK_STATUS_UPDATED)) {

		mutex_lock(&dp->session_lock);
		dp_audio_enable(dp, false);

		if (dp->link->sink_request & DP_TEST_LINK_PHY_TEST_PATTERN) {
			SDE_EVT32_EXTERNAL(dp->state,
					DP_TEST_LINK_PHY_TEST_PATTERN);
			dp->ctrl->process_phy_test_request(dp->ctrl);
		}

		if (dp->link->sink_request & DP_TEST_LINK_TRAINING) {
			SDE_EVT32_EXTERNAL(dp->state, DP_TEST_LINK_TRAINING);
			dp->link->send_test_response(dp->link);
			rc = dp->ctrl->link_maintenance(dp->ctrl);
		}

		if (dp->link->sink_request & DP_LINK_STATUS_UPDATED) {
			/*
			 * This is for GPIO based HPD only, that if HPD low is
			 * detected as HPD_IRQ, we need to treat
			 * LINK_STATUS_UPDATED as HPD high.
			 */
			if (dp->parser->no_aux_switch &&
					!dp->parser->lphw_hpd) {
				mutex_unlock(&dp->session_lock);
				dp_display_handle_disconnect(dp);
				queue_work(dp->wq, &dp->connect_work);
				goto mst_attention;
			} else {
				SDE_EVT32_EXTERNAL(dp->state, DP_LINK_STATUS_UPDATED);
				rc = dp->ctrl->link_maintenance(dp->ctrl);
			}
		}

		if (!rc)
			dp_audio_enable(dp, true);

		mutex_unlock(&dp->session_lock);
		if (rc)
			goto exit;

		if (dp->link->sink_request & (DP_TEST_LINK_PHY_TEST_PATTERN |
			DP_TEST_LINK_TRAINING))
			goto mst_attention;
	}

cp_irq:
	if (dp_display_is_hdcp_enabled(dp) && dp->hdcp.ops->cp_irq)
		dp->hdcp.ops->cp_irq(dp->hdcp.data);

	if (!dp->mst.mst_active) {
		/*
		 * It is possible that the connect_work skipped sending
		 * the HPD notification if the attention message was
		 * already pending. Send the notification here to
		 * account for that. This is not needed if this
		 * attention work was handling a test request
		 */
		dp_display_send_hpd_notification(dp);
	}

mst_attention:
	dp_display_mst_attention(dp);
exit:
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state);
}

static int dp_display_usbpd_attention_cb(struct device *dev)
{
	struct dp_display_private *dp;

	if (!dev) {
		DP_ERR("invalid dev\n");
		return -EINVAL;
	}

	dp = dev_get_drvdata(dev);
	if (!dp) {
		DP_ERR("no driver data found\n");
		return -ENODEV;
	}

	DP_DEBUG("DP%d hpd_irq:%d, hpd_high:%d, power_on:%d, is_connected:%d\n",
			dp->cell_idx, dp->hpd->hpd_irq, dp->hpd->hpd_high,
			!!dp_display_state_is(DP_STATE_ENABLED),
			!!dp_display_state_is(DP_STATE_CONNECTED));
	SDE_EVT32_EXTERNAL(dp->state, dp->hpd->hpd_irq, dp->hpd->hpd_high,
			!!dp_display_state_is(DP_STATE_ENABLED),
			!!dp_display_state_is(DP_STATE_CONNECTED));

	if (!dp->hpd->hpd_high) {
		dp_display_disconnect_sync(dp);
		return 0;
	}

	/*
	 * Ignore all the attention messages except HPD LOW when TUI is
	 * active, so user mode can be notified of the disconnect event. This
	 * allows user mode to tear down the control path after the TUI
	 * session is over. Ideally this should never happen, but on the off
	 * chance that there is a race condition in which there is a IRQ HPD
	 * during tear down of DP at TUI start then this check might help avoid
	 * a potential issue accessing registers in attention processing.
	 */
	if (dp_display_state_is(DP_STATE_TUI_ACTIVE)) {
		DP_WARN("DP%d TUI is active\n", dp->cell_idx);
		return 0;
	}

	if (dp->hpd->hpd_irq && dp_display_state_is(DP_STATE_READY)) {
		queue_work(dp->wq, &dp->attention_work);
		complete_all(&dp->attention_comp);
	} else if (dp->process_hpd_connect ||
			 !dp_display_state_is(DP_STATE_CONNECTED)) {
		dp_display_state_remove(DP_STATE_ABORTED);
		queue_work(dp->wq, &dp->connect_work);
	} else {
		DP_DEBUG("DP%d ignored\n", dp->cell_idx);
	}

	return 0;
}

static void dp_display_connect_work(struct work_struct *work)
{
	int rc = 0;
	struct dp_display_private *dp = container_of(work,
			struct dp_display_private, connect_work);
	struct drm_connector *reset_connector = NULL;

	if (dp_display_state_is(DP_STATE_TUI_ACTIVE)) {
		dp_display_state_log("[TUI is active]");
		return;
	}

	if (dp_display_state_is(DP_STATE_ABORTED)) {
		DP_WARN("DP%d HPD off requested\n", dp->cell_idx);
		return;
	}

	if (!dp->hpd->hpd_high) {
		DP_WARN("DP%d Sink disconnected\n", dp->cell_idx);
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
			dp->aux->abort(dp->aux, false);
			dp->ctrl->abort(dp->ctrl, false);
			reset_connector = dp->bond_primary;
		} else if (dp->active_panels[DP_STREAM_0] == dp->panel &&
				!dp->panel->video_test) {
			dp->aux->abort(dp->aux, false);
			dp->ctrl->abort(dp->ctrl, false);
			reset_connector = dp->dp_display.base_connector;
		} else {
			dp_display_clean(dp);
			dp_display_host_unready(dp);
			dp_display_host_deinit(dp);
			dp_display_state_add(DP_STATE_SRC_PWRDN);
		}
	}

	if (dp->parser->force_connect_mode) {
		if (!reset_connector) {
			dp_display_clean(dp);
			dp_display_host_unready(dp);
			dp_display_host_deinit(dp);
			dp_display_state_add(DP_STATE_SRC_PWRDN);
		}
		dp_display_process_mst_hpd_low(dp);
		dp_sim_set_sim_mode(dp->aux_bridge, 0);
		dp->aux->state = 0;
		dp_display_send_force_connect_event(dp);
	}

	mutex_unlock(&dp->session_lock);

	rc = dp_display_process_hpd_high(dp, dp->parser->force_connect_mode);

	if (!rc && dp->panel->video_test)
		dp->link->send_test_response(dp->link);

	if (reset_connector)
		sde_connector_helper_mode_change_commit(reset_connector);
}

static int dp_display_usb_notifier(struct notifier_block *nb,
	unsigned long action, void *data)
{
	struct dp_display_private *dp = container_of(nb,
			struct dp_display_private, usb_nb);

	SDE_EVT32_EXTERNAL(dp->state, dp->debug->sim_mode, action);
	if (!action && dp->debug->sim_mode) {
		DP_WARN("DP%d usb disconnected during simulation\n", dp->cell_idx);
		dp_display_disconnect_sync(dp);
		dp->debug->abort(dp->debug);
	}

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state, NOTIFY_DONE);
	return NOTIFY_DONE;
}

static void dp_display_register_usb_notifier(struct dp_display_private *dp)
{
	int rc = 0;
	const char *phandle = "usb-phy";
	struct usb_phy *usbphy;

	usbphy = devm_usb_get_phy_by_phandle(&dp->pdev->dev, phandle, 0);
	if (IS_ERR_OR_NULL(usbphy)) {
		DP_DEBUG("DP%d unable to get usbphy\n", dp->cell_idx);
		return;
	}

	dp->usb_nb.notifier_call = dp_display_usb_notifier;
	dp->usb_nb.priority = 2;
	rc = usb_register_notifier(usbphy, &dp->usb_nb);
	if (rc)
		DP_DEBUG("DP%d failed to register for usb event: %d\n", dp->cell_idx, rc);
}

int dp_display_mmrm_callback(struct mmrm_client_notifier_data *notifier_data)
{
	struct dss_clk_mmrm_cb *mmrm_cb_data = (struct dss_clk_mmrm_cb *)notifier_data->pvt_data;
	struct dp_display *dp_display = (struct dp_display *)mmrm_cb_data->phandle;
	struct dp_display_private *dp =
		container_of(dp_display, struct dp_display_private, dp_display);
	int ret = 0;

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state, notifier_data->cb_type);
	if (notifier_data->cb_type == MMRM_CLIENT_RESOURCE_VALUE_CHANGE
				&& dp_display_state_is(DP_STATE_ENABLED)
				&& !dp_display_state_is(DP_STATE_ABORTED)) {
		ret = dp_display_handle_disconnect(dp);
		if (ret)
			DP_ERR("DP%d mmrm callback error reducing clk, ret:%d\n",
					dp->cell_idx, ret);
	}

	DP_DEBUG("DP%d mmrm callback handled, state: 0x%x rc:%d\n",
			dp->cell_idx, dp->state, ret);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state, notifier_data->cb_type);
	return ret;
}

static void dp_display_deinit_sub_modules(struct dp_display_private *dp)
{
	dp_debug_put(dp->debug);
	dp_hpd_put(dp->hpd);
	if (dp->panel)
		dp_audio_put(dp->panel->audio);
	dp_ctrl_put(dp->ctrl);
	dp_panel_put(dp->panel);
	dp_link_put(dp->link);
	dp_power_put(dp->power);
	dp_pll_put(dp->pll);
	dp_aux_put(dp->aux);
	dp_catalog_put(dp->catalog);
	dp_parser_put(dp->parser);
	mutex_destroy(&dp->session_lock);
}

static int dp_init_sub_modules(struct dp_display_private *dp)
{
	int rc = 0;
	u32 dp_core_revision = 0;
	bool hdcp_disabled;
	const char *phandle = "qcom,dp-aux-switch";
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
	struct dp_pll_in pll_in = {
		.pdev = dp->pdev,
	};

	mutex_init(&dp->session_lock);

	dp->parser = dp_parser_get(dp->pdev, dp->cell_idx);
	if (IS_ERR(dp->parser)) {
		rc = PTR_ERR(dp->parser);
		DP_ERR("DP%d failed to initialize parser, rc = %d\n",
				dp->cell_idx, rc);
		dp->parser = NULL;
		goto error;
	}

	rc = dp->parser->parse(dp->parser);
	if (rc) {
		DP_ERR("DP%d device tree parsing failed\n", dp->cell_idx);
		goto error_catalog;
	}

	dp->dp_display.is_mst_supported = dp->parser->has_mst;
	dp->dp_display.dsc_cont_pps = dp->parser->dsc_continuous_pps;

	dp->catalog = dp_catalog_get(dev, dp->parser);
	if (IS_ERR(dp->catalog)) {
		rc = PTR_ERR(dp->catalog);
		DP_ERR("DP%d failed to initialize catalog, rc = %d\n",
				dp->cell_idx, rc);
		dp->catalog = NULL;
		goto error_catalog;
	}

	dp_core_revision = dp_catalog_get_dp_core_version(dp->catalog);

	dp->aux_switch_node = of_parse_phandle(dp->pdev->dev.of_node, phandle, 0);
	if (!dp->aux_switch_node) {
		DP_DEBUG("cannot parse %s handle\n", phandle);
		dp->parser->no_aux_switch = true;
	}

	dp->aux = dp_aux_get(dev, &dp->catalog->aux, dp->parser,
			dp->aux_switch_node, dp->aux_bridge, dp->cell_idx);
	if (IS_ERR(dp->aux)) {
		rc = PTR_ERR(dp->aux);
		DP_ERR("DP%d failed to initialize aux, rc = %d\n", dp->cell_idx, rc);
		dp->aux = NULL;
		goto error_aux;
	}

	rc = dp->aux->drm_aux_register(dp->aux, dp->dp_display.drm_dev);
	if (rc) {
		DP_ERR("DP%d DRM DP AUX register failed\n", dp->cell_idx);
		goto error_pll;
	}

	pll_in.aux = dp->aux;
	pll_in.parser = dp->parser;
	pll_in.dp_core_revision = dp_core_revision;

	dp->pll = dp_pll_get(&pll_in);
	if (IS_ERR(dp->pll)) {
		rc = PTR_ERR(dp->pll);
		DP_ERR("DP%d failed to initialize pll, rc = %d\n", dp->cell_idx, rc);
		dp->pll = NULL;
		goto error_pll;
	}

	dp->power = dp_power_get(dp->parser, dp->pll);
	if (IS_ERR(dp->power)) {
		rc = PTR_ERR(dp->power);
		DP_ERR("DP%d failed to initialize power, rc = %d\n", dp->cell_idx, rc);
		dp->power = NULL;
		goto error_power;
	}

	rc = dp->power->power_client_init(dp->power, &dp->priv->phandle,
		dp->dp_display.drm_dev);
	if (rc) {
		DP_ERR("DP%d Power client create failed\n", dp->cell_idx);
		goto error_link;
	}

	rc = dp->power->power_mmrm_init(dp->power, &dp->priv->phandle,
		(void *)&dp->dp_display, dp_display_mmrm_callback);
	if (rc) {
		DP_ERR("DP%d failed to initialize mmrm, rc = %d\n", dp->cell_idx, rc);
		goto error_link;
	}

	dp->link = dp_link_get(dev, dp->aux, dp_core_revision);
	if (IS_ERR(dp->link)) {
		rc = PTR_ERR(dp->link);
		DP_ERR("DP%d failed to initialize link, rc = %d\n", dp->cell_idx, rc);
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
		DP_ERR("DP%d failed to initialize panel, rc = %d\n", dp->cell_idx, rc);
		dp->panel = NULL;
		goto error_panel;
	}

	ctrl_in.link = dp->link;
	ctrl_in.panel = dp->panel;
	ctrl_in.aux = dp->aux;
	ctrl_in.power = dp->power;
	ctrl_in.catalog = &dp->catalog->ctrl;
	ctrl_in.parser = dp->parser;
	ctrl_in.pll = dp->pll;

	dp->ctrl = dp_ctrl_get(&ctrl_in);
	if (IS_ERR(dp->ctrl)) {
		rc = PTR_ERR(dp->ctrl);
		DP_ERR("DP%d failed to initialize ctrl, rc = %d\n", dp->cell_idx, rc);
		dp->ctrl = NULL;
		goto error_ctrl;
	}

	dp->panel->audio = dp_audio_get(dp->pdev, dp->panel,
						&dp->catalog->audio);
	if (IS_ERR(dp->panel->audio)) {
		rc = PTR_ERR(dp->panel->audio);
		DP_ERR("DP%d failed to initialize audio, rc = %d\n", dp->cell_idx, rc);
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
	if (IS_ERR(dp->hpd)) {
		rc = PTR_ERR(dp->hpd);
		DP_ERR("DP%d failed to initialize hpd, rc = %d\n", dp->cell_idx, rc);
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
	debug_in.pll = dp->pll;
	debug_in.display = &dp->dp_display;

	dp->debug = dp_debug_get(&debug_in);
	if (IS_ERR(dp->debug)) {
		rc = PTR_ERR(dp->debug);
		DP_ERR("DP%d failed to initialize debug, rc = %d\n", dp->cell_idx, rc);
		dp->debug = NULL;
		goto error_debug;
	}

	dp->cached_connector_status = connector_status_disconnected;

	dp->debug->hdcp_wait_sink_sync =
		dp->parser->hdcp_wait_sink_sync_enabled;

	dp->tot_dsc_blks_in_use = 0;

	dp->debug->hdcp_disabled = hdcp_disabled;
	dp_display_update_hdcp_status(dp, true);

	dp_display_register_usb_notifier(dp);

	if (dp->hpd->register_hpd) {
		rc = dp->hpd->register_hpd(dp->hpd);
		if (rc) {
			DP_ERR("DP%d failed register hpd\n", dp->cell_idx);
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
		dp_display_state_remove(DP_STATE_ABORTED);
		dp_display_state_add(DP_STATE_CONFIGURED);
		rc = dp_display_host_init(dp);
		if (rc)
			DP_ERR("DP%d Host init Failed", dp->cell_idx);
		dp_display_process_hpd_high(dp, true);
		dp_display_send_hpd_notification(dp);
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
	dp_power_put(dp->power);
error_power:
	dp_pll_put(dp->pll);
error_pll:
	dp_aux_put(dp->aux);
error_aux:
	dp_catalog_put(dp->catalog);
error_catalog:
	dp_parser_put(dp->parser);
error:
	mutex_destroy(&dp->session_lock);
	return rc;
}

static void dp_display_dbg_reister(struct dp_display_private *dp)
{
	struct dp_parser *parser = dp->parser;
	struct dss_io_data *io;
	char name[SZ_32];
	const char *label;

	label = of_get_property(dp->pdev->dev.of_node, "label", NULL);
	if (!label)
		label = "dp";

	io = &parser->get_io(parser, "dp_ahb")->io;
	if (io) {
		snprintf(name, sizeof(name), "%s_ahb", label);
		sde_dbg_reg_register_base(name, io->base, io->len,
				msm_get_phys_addr(dp->pdev, "dp_ahb"), SDE_DBG_DP);
	}

	io = &parser->get_io(parser, "dp_aux")->io;
	if (io) {
		snprintf(name, sizeof(name), "%s_aux", label);
		sde_dbg_reg_register_base(name, io->base, io->len,
				msm_get_phys_addr(dp->pdev, "dp_aux"), SDE_DBG_DP);
	}

	io = &parser->get_io(parser, "dp_link")->io;
	if (io) {
		snprintf(name, sizeof(name), "%s_link", label);
		sde_dbg_reg_register_base(name, io->base, io->len,
				msm_get_phys_addr(dp->pdev, "dp_link"), SDE_DBG_DP);
	}

	io = &parser->get_io(parser, "dp_p0")->io;
	if (io) {
		snprintf(name, sizeof(name), "%s_p0", label);
		sde_dbg_reg_register_base(name, io->base, io->len,
				msm_get_phys_addr(dp->pdev, "dp_p0"), SDE_DBG_DP);
	}

	io = &parser->get_io(parser, "hdcp_physical")->io;
	if (io) {
		snprintf(name, sizeof(name), "%s_hdcp_physical", label);
		sde_dbg_reg_register_base(name, io->base, io->len,
				msm_get_phys_addr(dp->pdev, "hdcp_physical"), SDE_DBG_DP);
	}
}

static int dp_display_post_init(struct dp_display *dp_display)
{
	int rc = 0;
	struct dp_display_private *dp;

	if (!dp_display) {
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	if (IS_ERR_OR_NULL(dp)) {
		DP_ERR("invalid params\n");
		return -EINVAL;
	}

	rc = dp_init_sub_modules(dp);
	if (rc)
		goto end;

	dp_display_dbg_reister(dp);

	dp_display_sysfs_init(dp);

	dp_display->post_init = NULL;
end:
	DP_DEBUG("DP%d %s\n", dp->cell_idx, rc ? "failed" : "success");
	return rc;
}

static int dp_display_set_mode(struct dp_display *dp_display, void *panel,
		struct dp_display_mode *mode)
{
	const u32 num_components = 3, default_bpp = 24;
	struct dp_display_private *dp;
	struct dp_panel *dp_panel;

	if (!dp_display || !panel) {
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp_panel = panel;
	if (!dp_panel->connector) {
		DP_ERR("invalid connector input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state,
			mode->timing.h_active, mode->timing.v_active,
			mode->timing.refresh_rate);

	mutex_lock(&dp->session_lock);
	mode->timing.bpp =
		dp_panel->connector->display_info.bpc * num_components;
	if (!mode->timing.bpp)
		mode->timing.bpp = default_bpp;

	mode->timing.bpp = dp->panel->get_mode_bpp(dp->panel,
			mode->timing.bpp, mode->timing.pixel_clk_khz);

	dp_panel->pinfo = mode->timing;
	mutex_unlock(&dp->session_lock);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state);

	return 0;
}

static int dp_display_prepare(struct dp_display *dp_display, void *panel)
{
	struct dp_display_private *dp;
	struct dp_panel *dp_panel;
	int rc = 0;

	if (!dp_display || !panel) {
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp_panel = panel;
	if (!dp_panel->connector) {
		DP_ERR("invalid connector input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state);
	mutex_lock(&dp->session_lock);

	/*
	 * If DP video session is restored by the userspace after display
	 * disconnect notification from dongle i.e. typeC cable connected to
	 * source but disconnected at the display side, the DP controller is
	 * not restored to the desired configured state. So, ensure host_init
	 * is executed in such a scenario so that all the DP controller
	 * resources are enabled for the next connection event.
	 */
	if (dp_display_state_is(DP_STATE_SRC_PWRDN) &&
			dp_display_state_is(DP_STATE_CONFIGURED)) {
		rc = dp_display_host_init(dp);
		if (rc) {
			/*
			 * Skip all the events that are similar to abort case, just that
			 * the stream clks should be enabled so that no commit failure can
			 * be seen.
			 */
			DP_ERR("DP%d Host init failed.\n", dp->cell_idx);
			goto end;
		}

		/*
		 * Remove DP_STATE_SRC_PWRDN flag on successful host_init to
		 * prevent cases such as below.
		 * 1. MST stream 1 failed to do host init then stream 2 can retry again.
		 * 2. Resume path fails, now sink sends hpd_high=0 and hpd_high=1.
		 */
		dp_display_state_remove(DP_STATE_SRC_PWRDN);
	}

	/*
	 * If the physical connection to the sink is already lost by the time
	 * we try to set up the connection, we can just skip all the steps
	 * here safely.
	 */
	if (dp_display_state_is(DP_STATE_ABORTED)) {
		dp_display_state_log("[aborted]");
		goto end;
	}

	/*
	 * If DP_STATE_ENABLED, there is nothing left to do.
	 * This would happen during MST flow. So, log this.
	 */
	if (dp_display_state_is(DP_STATE_ENABLED)) {
		dp_display_state_warn("[already enabled]");
		goto end;
	}

	if (!dp_display_is_ready(dp) && !dp->parser->force_connect_mode) {
		dp_display_state_show("[not ready]");
		goto end;
	}

	/* For supporting DP_PANEL_SRC_INITIATED_POWER_DOWN case */
	rc = dp_display_host_ready(dp);
	if (rc) {
		dp_display_state_show("[ready failed]");
		goto end;
	}

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

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state, rc);
	return rc;
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
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	if (strm_id >= DP_STREAM_MAX) {
		DP_ERR("invalid stream id:%d\n", strm_id);
		return -EINVAL;
	}

	if (start_slot + num_slots > max_slots) {
		DP_ERR("invalid channel info received. start:%d, slots:%d\n",
				start_slot, num_slots);
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state, strm_id,
			start_slot, num_slots);

	mutex_lock(&dp->session_lock);

	dp->ctrl->set_mst_channel_info(dp->ctrl, strm_id,
			start_slot, num_slots);

	if (panel) {
		dp_panel = panel;
		dp_panel->set_stream_info(dp_panel, strm_id, start_slot,
				num_slots, pbn, vcpi);
	}

	mutex_unlock(&dp->session_lock);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state, rc);

	return rc;
}

static int dp_display_enable(struct dp_display *dp_display, void *panel)
{
	int rc = 0;
	struct dp_display_private *dp;

	if (!dp_display || !panel) {
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state);
	mutex_lock(&dp->session_lock);

	/*
	 * If DP_STATE_READY is not set, we should not do any HW
	 * programming.
	 */
	if (!dp_display_state_is(DP_STATE_READY)) {
		dp_display_state_show("[host not ready]");
		goto end;
	}

	/*
	 * It is possible that by the time we get call back to establish
	 * the DP pipeline e2e, the physical DP connection to the sink is
	 * already lost. In such cases, the DP_STATE_ABORTED would be set.
	 * However, it is necessary to NOT abort the display setup here so as
	 * to ensure that the rest of the system is in a stable state prior to
	 * handling the disconnect notification.
	 */
	if (dp_display_state_is(DP_STATE_ABORTED))
		dp_display_state_log("[aborted, but continue on]");

	rc = dp_display_stream_enable(dp, panel);
	if (rc)
		goto end;

	dp_display_state_add(DP_STATE_ENABLED);
end:
	mutex_unlock(&dp->session_lock);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state, rc);
	return rc;
}

static void dp_display_stream_post_enable(struct dp_display_private *dp,
			struct dp_panel *dp_panel)
{
	dp_panel->spd_config(dp_panel);
	dp_panel->setup_hdr(dp_panel, NULL, false, 0, true);
}

static int dp_display_post_enable(struct dp_display *dp_display, void *panel)
{
	struct dp_display_private *dp;
	struct dp_panel *dp_panel;

	if (!dp_display || !panel) {
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	dp_panel = panel;

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state);
	mutex_lock(&dp->session_lock);

	/*
	 * If DP_STATE_READY is not set, we should not do any HW
	 * programming.
	 */
	if (!dp_display_state_is(DP_STATE_ENABLED)) {
		dp_display_state_show("[not enabled]");
		goto end;
	}

	/*
	 * If the physical connection to the sink is already lost by the time
	 * we try to set up the connection, we can just skip all the steps
	 * here safely.
	 */
	if (dp_display_state_is(DP_STATE_ABORTED)) {
		dp_display_state_log("[aborted]");
		goto end;
	}

	if (!dp_display_is_ready(dp) || !dp_display_state_is(DP_STATE_READY)) {
		dp_display_state_show("[not ready]");
		goto end;
	}

	dp_display_stream_post_enable(dp, dp_panel);

	cancel_delayed_work_sync(&dp->hdcp_cb_work);
	queue_delayed_work(dp->wq, &dp->hdcp_cb_work, HZ);

	if (dp_panel->audio_supported) {
		dp_panel->audio->bw_code = dp->link->link_params.bw_code;
		dp_panel->audio->lane_count = dp->link->link_params.lane_count;
		dp_panel->audio->on(dp_panel->audio);
	}
end:
	dp->aux->state |= DP_STATE_CTRL_POWERED_ON;

	if (dp->parser->force_connect_mode)
		dp_display_send_force_connect_event(dp);

	mutex_unlock(&dp->session_lock);
	DP_DEBUG("DP%d display post enable complete. state: 0x%x\n",
			dp->cell_idx, dp->state);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state);
	return 0;
}

static void dp_display_clear_colorspaces(struct dp_display *dp_display)
{
	struct drm_connector *connector;
	struct sde_connector *sde_conn;

	connector = dp_display->base_connector;
	sde_conn = to_sde_connector(connector);
	sde_conn->color_enc_fmt = 0;
}

static int dp_display_pre_disable(struct dp_display *dp_display, void *panel)
{
	struct dp_display_private *dp;
	struct dp_panel *dp_panel = panel;
	struct dp_link_hdcp_status *status;
	int rc = 0;
	size_t i;

	if (!dp_display || !panel) {
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state);
	mutex_lock(&dp->session_lock);

	status = &dp->link->hdcp_status;

	if (!dp_display_state_is(DP_STATE_ENABLED)) {
		dp_display_state_show("[not enabled]");
		goto end;
	}

	dp_display_state_add(DP_STATE_HDCP_ABORTED);
	cancel_delayed_work_sync(&dp->hdcp_cb_work);
	if (dp_display_is_hdcp_enabled(dp) &&
			status->hdcp_state != HDCP_STATE_INACTIVE) {
		bool off = true;

		if (dp_display_state_is(DP_STATE_SUSPENDED)) {
			DP_DEBUG("DP%d Can't perform HDCP cleanup while suspended. Defer\n",
					dp->cell_idx);
			dp->hdcp_delayed_off = true;
			goto clean;
		}

		flush_delayed_work(&dp->hdcp_cb_work);
		if (dp->mst.mst_active) {
			dp_display_hdcp_deregister_stream(dp,
				dp_panel->stream_id);
			for (i = DP_STREAM_0; i < DP_STREAM_MAX; i++) {
				if (i != dp_panel->stream_id &&
						dp->active_panels[i]) {
					DP_DEBUG("DP%d Streams are still active. Skip disabling HDCP\n",
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

	dp_display_clear_colorspaces(dp_display);

clean:
	if (dp_panel->audio_supported)
		dp_panel->audio->off(dp_panel->audio);

	rc = dp_display_stream_pre_disable(dp, dp_panel);

end:
	mutex_unlock(&dp->session_lock);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state);
	return 0;
}

static int dp_display_disable(struct dp_display *dp_display, void *panel)
{
	int i;
	struct dp_display_private *dp = NULL;
	struct dp_panel *dp_panel = NULL;
	struct dp_link_hdcp_status *status;

	if (!dp_display || !panel) {
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	dp_panel = panel;
	status = &dp->link->hdcp_status;

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state);
	mutex_lock(&dp->session_lock);

	if (!dp_display_state_is(DP_STATE_ENABLED)) {
		dp_display_state_show("[not enabled]");
		goto end;
	}

	if (!dp_display_state_is(DP_STATE_READY)) {
		dp_display_state_show("[not ready]");
		goto end;
	}

	dp_display_stream_disable(dp, dp_panel);

	dp_display_state_remove(DP_STATE_HDCP_ABORTED);
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
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state);
	return 0;
}

static int dp_request_irq(struct dp_display *dp_display)
{
	int rc = 0;
	struct dp_display_private *dp;

	if (!dp_display) {
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	dp->irq = irq_of_parse_and_map(dp->pdev->dev.of_node, 0);
	if (dp->irq < 0) {
		rc = dp->irq;
		DP_ERR("DP%d failed to get irq: %d\n", dp->cell_idx, rc);
		return rc;
	}

	rc = devm_request_irq(&dp->pdev->dev, dp->irq, dp_display_irq,
		IRQF_TRIGGER_HIGH, "dp_display_isr", dp);
	if (rc < 0) {
		DP_ERR("DP%d failed to request IRQ%u: %d\n",
				dp->cell_idx, dp->irq, rc);
		return rc;
	}
	disable_irq(dp->irq);

	return 0;
}

static struct dp_debug *dp_get_debug(struct dp_display *dp_display)
{
	struct dp_display_private *dp;

	if (!dp_display) {
		DP_ERR("invalid input\n");
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
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state);
	mutex_lock(&dp->session_lock);

	/*
	 * Check if the power off sequence was triggered
	 * by a source initialated action like framework
	 * reboot or suspend-resume but not from normal
	 * hot plug. If connector is in MST mode, skip
	 * powering down host as aux need keep alive
	 * to handle hot-plug sideband message.
	 */
	if ((dp_display_is_ready(dp) && !dp->mst.mst_active)
			|| dp->parser->force_connect_mode
			|| IS_BOND_MODE(dp->phy_bond_mode))
		 flags |= DP_PANEL_SRC_INITIATED_POWER_DOWN;

	/*
	 * If connector is in MST mode, skip
	 * powering down host as aux needs to be kept
	 * alive to handle hot-plug sideband message.
	 */
	if (dp->active_stream_cnt || dp->mst.mst_active)
		goto end;

	dp->link->psm_config(dp->link, &dp->panel->link_info, true);
	dp->debug->psm_enabled = true;

	dp->ctrl->off(dp->ctrl);
	dp_display_host_unready(dp);
	dp_display_host_deinit(dp);
	dp_display_state_add(DP_STATE_SRC_PWRDN);

	dp_display_state_remove(DP_STATE_ENABLED);
	dp->aux->state = DP_STATE_CTRL_POWERED_OFF;

	if (dp->parser->force_connect_mode)
		dp_display_send_force_connect_event(dp);

	/* log this as it results from user action of cable dis-connection */
	DP_INFO("DP%d [OK]\n", dp->cell_idx);
end:
	/**
	 * Once the DP driver is turned off, set to non-bond mode.
	 * If bond mode is required afterwards, call set_phy_bond_mode.
	 */
	dp_display_change_phy_bond_mode(dp, DP_PHY_BOND_MODE_NONE);

	dp_panel->deinit(dp_panel, flags);
	mutex_unlock(&dp->session_lock);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state);

	return 0;
}

static int dp_display_validate_link_clock(struct dp_display_private *dp,
		struct drm_display_mode *mode, struct dp_display_mode dp_mode)
{
	u32 mode_rate_khz = 0, supported_rate_khz = 0, mode_bpp = 0;
	bool dsc_en;
	int rate;

	dsc_en = dp_mode.timing.comp_info.enabled;
	mode_bpp = dsc_en ?
		DSC_BPP(dp_mode.timing.comp_info.dsc_info.config)
		: dp_mode.timing.bpp;

	mode_rate_khz = mode->clock * mode_bpp;
	rate = drm_dp_bw_code_to_link_rate(dp->link->link_params.bw_code);
	supported_rate_khz = dp->link->link_params.lane_count * rate * 8;

	if (mode_rate_khz > supported_rate_khz) {
		DP_DEBUG("DP%d mode_rate: %d kHz, supported_rate: %d kHz\n",
				dp->cell_idx, mode_rate_khz, supported_rate_khz);
		return -EPERM;
	}

	return 0;
}

static int dp_display_validate_pixel_clock(struct dp_display_mode dp_mode,
		u32 max_pclk_khz)
{
	u32 pclk_khz = dp_mode.timing.widebus_en ?
		(dp_mode.timing.pixel_clk_khz >> 1) :
		dp_mode.timing.pixel_clk_khz;

	if (pclk_khz > max_pclk_khz) {
		DP_DEBUG("clk: %d kHz, max: %d kHz\n", pclk_khz, max_pclk_khz);
		return -EPERM;
	}

	return 0;
}

static int dp_display_validate_topology(struct dp_display_private *dp,
		struct dp_panel *dp_panel, struct drm_display_mode *mode,
		struct dp_display_mode *dp_mode,
		const struct msm_resource_caps_info *avail_res)
{
	int rc;
	struct msm_drm_private *priv = dp->priv;
	const u32 dual = 2, quad = 4;
	u32 num_lm = 0, num_dsc = 0, num_3dmux = 0;
	bool dsc_capable = dp_mode->capabilities & DP_PANEL_CAPS_DSC;
	u32 fps = dp_mode->timing.refresh_rate;

	rc = msm_get_mixer_count(priv, mode, avail_res, &num_lm);
	if (rc) {
		DP_ERR("DP%d error getting mixer count. rc:%d\n", dp->cell_idx, rc);
		return rc;
	}

	/* Merge using DSC, if enabled */
	if (dp_panel->dsc_en && dsc_capable) {
		rc = msm_get_dsc_count(priv, mode->hdisplay, &num_dsc);
		if (rc) {
			DP_ERR("DP%d error getting dsc count. rc:%d\n", dp->cell_idx, rc);
			return rc;
		}

		num_dsc = max(num_lm, num_dsc);
		if ((num_dsc > avail_res->num_lm) ||  (num_dsc > avail_res->num_dsc)) {
			DP_DEBUG("mode %sx%d: not enough resources for dsc %d dsc_a:%d lm_a:%d\n",
					mode->name, fps, num_dsc, avail_res->num_dsc,
					avail_res->num_lm);
			/* Clear DSC caps and retry */
			dp_mode->capabilities &= ~DP_PANEL_CAPS_DSC;
			return -EAGAIN;
		} else {
			/* Only DSCMERGE is supported on DP */
			num_lm = num_dsc;
		}
	}

	if (!num_dsc && (num_lm == 2) && avail_res->num_3dmux) {
		num_3dmux = 1;
	}

	if (num_lm > avail_res->num_lm) {
		DP_DEBUG("DP%d mode %sx%d is invalid, not enough lm %d %d\n",
				dp->cell_idx, mode->name, fps,
				num_lm, avail_res->num_lm);
		return -EPERM;
	} else if (!num_dsc && (num_lm == dual && !num_3dmux)) {
		DP_DEBUG("DP%d mode %sx%d is invalid, not enough 3dmux %d %d\n",
				dp->cell_idx, mode->name, fps,
				num_3dmux, avail_res->num_3dmux);
		return -EPERM;
	} else if (num_lm == quad && num_dsc != quad)  {
		DP_DEBUG("DP%d mode %sx%d is invalid, unsupported DP topology lm:%d dsc:%d\n",
				dp->cell_idx, mode->name, fps, num_lm, num_dsc);
		return -EPERM;
	}

	DP_DEBUG("DP%d mode %sx%d is valid, supported DP topology lm:%d dsc:%d 3dmux:%d\n",
				dp->cell_idx, mode->name, fps, num_lm, num_dsc, num_3dmux);

	return 0;
}

static enum drm_mode_status dp_display_validate_mode(
		struct dp_display *dp_display,
		void *panel, struct drm_display_mode *mode,
		const struct msm_resource_caps_info *avail_res)
{
	struct dp_display_private *dp;
	struct dp_panel *dp_panel;
	struct dp_debug *debug;
	enum drm_mode_status mode_status = MODE_BAD;
	struct dp_display_mode dp_mode;
	int rc = 0;

	if (!dp_display || !mode || !panel ||
			!avail_res || !avail_res->max_mixer_width) {
		DP_ERR("invalid params\n");
		return mode_status;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	mutex_lock(&dp->session_lock);

	dp_panel = panel;
	if (!dp_panel->connector) {
		DP_ERR("DP%d invalid connector\n", dp->cell_idx);
		goto end;
	}

	debug = dp->debug;
	if (!debug)
		goto end;

	dp_display->convert_to_dp_mode(dp_display, panel, mode, &dp_mode);

	rc = dp_display_validate_topology(dp, dp_panel, mode, &dp_mode, avail_res);
	if (rc == -EAGAIN) {
		dp_panel->convert_to_dp_mode(dp_panel, mode, &dp_mode);
		rc = dp_display_validate_topology(dp, dp_panel, mode, &dp_mode, avail_res);
	}

	if (rc)
		goto end;

	rc = dp_display_validate_link_clock(dp, mode, dp_mode);
	if (rc)
		goto end;

	rc = dp_display_validate_pixel_clock(dp_mode, dp_display->max_pclk_khz);
	if (rc)
		goto end;

	mode_status = MODE_OK;
end:
	mutex_unlock(&dp->session_lock);
	DP_DEBUG("DP%d [%s] mode is %s\n", dp->cell_idx, mode->name,
			(mode_status == MODE_OK) ? "valid" : "invalid");

	return mode_status;
}

static int dp_display_get_available_dp_resources(struct dp_display *dp_display,
		const struct msm_resource_caps_info *avail_res,
		struct msm_resource_caps_info *max_dp_avail_res)
{
	struct dp_display_private *dp;

	if (!dp_display || !avail_res || !max_dp_avail_res) {
		DP_ERR("invalid arguments\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	memcpy(max_dp_avail_res, avail_res,
			sizeof(struct msm_resource_caps_info));

	max_dp_avail_res->num_lm = min(avail_res->num_lm,
			dp_display->max_mixer_count);
	max_dp_avail_res->num_dsc = min(avail_res->num_dsc,
			dp_display->max_dsc_count);

	DP_DEBUG("DP%d max_lm:%d, avail_lm:%d, dp_avail_lm:%d\n",
			dp->cell_idx, dp_display->max_mixer_count, avail_res->num_lm,
			max_dp_avail_res->num_lm);

	DP_DEBUG("DP%d max_dsc:%d, avail_dsc:%d, dp_avail_dsc:%d\n",
			dp->cell_idx, dp_display->max_dsc_count, avail_res->num_dsc,
			max_dp_avail_res->num_dsc);

	return 0;
}

static int dp_display_get_modes(struct dp_display *dp, void *panel,
	struct dp_display_mode *dp_mode)
{
	struct dp_display_private *dp_display;
	struct dp_panel *dp_panel;
	int ret = 0;

	if (!dp || !panel) {
		DP_ERR("invalid params\n");
		return 0;
	}

	dp_panel = panel;
	if (!dp_panel->connector) {
		DP_ERR("invalid connector\n");
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
	int rc;
	struct dp_display_private *dp;
	struct dp_panel *dp_panel;
	u32 free_dsc_blks = 0, required_dsc_blks = 0, curr_dsc = 0, new_dsc = 0;

	if (!dp_display || !drm_mode || !dp_mode || !panel) {
		DP_ERR("invalid input\n");
		return;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	dp_panel = panel;

	memset(dp_mode, 0, sizeof(*dp_mode));

	free_dsc_blks = dp_display->max_dsc_count -
				dp->tot_dsc_blks_in_use +
				dp_panel->dsc_blks_in_use;
	DP_DEBUG("Before: in_use:%d, max:%d, free:%d\n",
				dp->tot_dsc_blks_in_use,
				dp_display->max_dsc_count, free_dsc_blks);

	rc = msm_get_dsc_count(dp->priv, drm_mode->hdisplay,
			&required_dsc_blks);
	if (rc) {
		DP_ERR("DP%d error getting dsc count. rc:%d\n", dp->cell_idx, rc);
		return;
	}

	curr_dsc = dp_panel->dsc_blks_in_use;
	dp->tot_dsc_blks_in_use -= dp_panel->dsc_blks_in_use;
	dp_panel->dsc_blks_in_use = 0;

	if (free_dsc_blks >= required_dsc_blks) {
		dp_mode->capabilities |= DP_PANEL_CAPS_DSC;
		new_dsc = max(curr_dsc, required_dsc_blks);
		dp_panel->dsc_blks_in_use = new_dsc;
		dp->tot_dsc_blks_in_use += new_dsc;
	}

	if (dp_mode->capabilities & DP_PANEL_CAPS_DSC)
		DP_DEBUG("DP%d After: in_use:%d, max:%d, free:%d, req:%d, caps:0x%x\n",
				dp->cell_idx, dp->tot_dsc_blks_in_use,
				dp_display->max_dsc_count,
				free_dsc_blks, required_dsc_blks,
				dp_mode->capabilities);

	dp_panel->convert_to_dp_mode(dp_panel, drm_mode, dp_mode);
}

static int dp_display_config_hdr(struct dp_display *dp_display, void *panel,
			struct drm_msm_ext_hdr_metadata *hdr, bool dhdr_update)
{
	struct dp_panel *dp_panel;
	struct sde_connector *sde_conn;
	struct dp_display_private *dp;
	u64 core_clk_rate;
	bool flush_hdr;

	if (!dp_display || !panel) {
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp_panel = panel;
	dp = container_of(dp_display, struct dp_display_private, dp_display);
	sde_conn =  to_sde_connector(dp_panel->connector);

	core_clk_rate = dp->power->clk_get_rate(dp->power, "core_clk");
	if (!core_clk_rate) {
		DP_ERR("DP%d invalid rate for core_clk\n", dp->cell_idx);
		return -EINVAL;
	}

	if (!dp_display_state_is(DP_STATE_ENABLED)) {
		dp_display_state_show("[not enabled]");
		return 0;
	}

	/*
	 * In rare cases where HDR metadata is updated independently
	 * flush the HDR metadata immediately instead of relying on
	 * the colorspace
	 */
	flush_hdr = !sde_conn->colorspace_updated;

	if (flush_hdr)
		DP_DEBUG("DP%d flushing the HDR metadata\n", dp->cell_idx);
	else
		DP_DEBUG("DP%d piggy-backing with colorspace\n", dp->cell_idx);

	return dp_panel->setup_hdr(dp_panel, hdr, dhdr_update,
		core_clk_rate, flush_hdr);
}

static int dp_display_setup_colospace(struct dp_display *dp_display,
		void *panel,
		u32 colorspace)
{
	struct dp_panel *dp_panel;
	struct dp_display_private *dp;

	if (!dp_display || !panel) {
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	if (!dp_display_state_is(DP_STATE_ENABLED)) {
		dp_display_state_show("[not enabled]");
		return 0;
	}

	dp_panel = panel;

	return dp_panel->set_colorspace(dp_panel, colorspace);
}

static int dp_display_create_workqueue(struct dp_display_private *dp)
{
	dp->wq = create_singlethread_workqueue("drm_dp");
	if (IS_ERR_OR_NULL(dp->wq)) {
		DP_ERR("DP%d Error creating wq\n", dp->cell_idx);
		return -EPERM;
	}

	INIT_DELAYED_WORK(&dp->hdcp_cb_work, dp_display_hdcp_cb_work);
	INIT_WORK(&dp->connect_work, dp_display_connect_work);
	INIT_WORK(&dp->attention_work, dp_display_attention_work);

	return 0;
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
		pr_err("couldn't find msm-hdcp pdev defer probe\n");
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
		DP_ERR("cannot find dev.of_node\n");
		rc = -ENODEV;
		goto end;
	}

	bridge_node = of_parse_phandle(dp->pdev->dev.of_node,
			phandle, 0);
	if (!bridge_node)
		goto end;

	dp->aux_bridge = of_dp_aux_find_bridge(bridge_node);
	if (!dp->aux_bridge) {
		DP_ERR("failed to find dp aux bridge\n");
		rc = -EPROBE_DEFER;
		goto end;
	}

	if (dp->aux_bridge->register_hpd &&
			!(dp->aux_bridge->flag & DP_AUX_BRIDGE_HPD))
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
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state);

	if (!mst_install_info->cbs->hpd || !mst_install_info->cbs->hpd_irq) {
		DP_ERR("DP%d invalid mst cbs\n", dp->cell_idx);
		return -EINVAL;
	}

	dp_display->dp_mst_prv_info = mst_install_info->dp_mst_prv_info;

	if (!dp->parser->has_mst) {
		DP_DEBUG("DP%d mst not enabled\n", dp->cell_idx);
		return -EPERM;
	}

	memcpy(&dp->mst.cbs, mst_install_info->cbs, sizeof(dp->mst.cbs));
	dp->mst.drm_registered = true;

	DP_MST_DEBUG("dp mst drm installed\n");
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state);

	return 0;
}

static int dp_display_mst_uninstall(struct dp_display *dp_display)
{
	struct dp_display_private *dp;

	if (!dp_display) {
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state);

	if (!dp->mst.drm_registered) {
		DP_DEBUG("DP%d drm mst not registered\n", dp->cell_idx);
		return -EPERM;
	}

	dp = container_of(dp_display, struct dp_display_private,
				dp_display);
	memset(&dp->mst.cbs, 0, sizeof(dp->mst.cbs));
	dp->mst.drm_registered = false;

	DP_MST_DEBUG("dp mst drm uninstalled\n");
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state);

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
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state);
	mutex_lock(&dp->session_lock);

	if (!dp->mst.drm_registered) {
		DP_DEBUG("DP%d drm mst not registered\n", dp->cell_idx);
		rc = -EPERM;
		goto end;
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
		DP_ERR("DP%d failed to initialize panel, rc = %d\n", dp->cell_idx, rc);
		goto end;
	}

	dp_panel->audio = dp_audio_get(dp->pdev, dp_panel, &dp->catalog->audio);
	if (IS_ERR(dp_panel->audio)) {
		rc = PTR_ERR(dp_panel->audio);
		DP_ERR("DP%d [mst] failed to initialize audio, rc = %d\n",
				dp->cell_idx, rc);
		dp_panel->audio = NULL;
		goto end;
	}

	DP_MST_DEBUG("dp mst connector installed. conn:%d\n",
			connector->base.id);

end:
	mutex_unlock(&dp->session_lock);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state, rc);

	return rc;
}

static int dp_display_mst_connector_uninstall(struct dp_display *dp_display,
			struct drm_connector *connector)
{
	int rc = 0;
	struct sde_connector *sde_conn;
	struct dp_panel *dp_panel;
	struct dp_display_private *dp;

	if (!dp_display || !connector) {
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY, dp->state);
	mutex_lock(&dp->session_lock);

	if (!dp->mst.drm_registered) {
		DP_DEBUG("DP%d drm mst not registered\n", dp->cell_idx);
		mutex_unlock(&dp->session_lock);
		return -EPERM;
	}

	sde_conn = to_sde_connector(connector);
	if (!sde_conn->drv_panel) {
		DP_ERR("DP%d invalid panel for connector:%d\n",
				dp->cell_idx, connector->base.id);
		mutex_unlock(&dp->session_lock);
		return -EINVAL;
	}

	dp_panel = sde_conn->drv_panel;
	dp_audio_put(dp_panel->audio);
	dp_panel_put(dp_panel);

	DP_MST_DEBUG("dp mst connector uninstalled. conn:%d\n",
			connector->base.id);

	mutex_unlock(&dp->session_lock);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state);

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
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	if (!dp->mst.drm_registered) {
		DP_DEBUG("DP%d drm mst not registered\n", dp->cell_idx);
		return -EPERM;
	}

	sde_conn = to_sde_connector(connector);
	if (!sde_conn->drv_panel) {
		DP_ERR("DP%d invalid panel for connector:%d\n",
				dp->cell_idx, connector->base.id);
		return -EINVAL;
	}

	dp_panel = sde_conn->drv_panel;
	rc = dp_panel->update_edid(dp_panel, edid);

	DP_MST_DEBUG("dp mst connector:%d edid updated. mode_cnt:%d\n",
			connector->base.id, rc);

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
		DP_ERR("DP%d invalid panel for connector:%d\n",
				dp->cell_idx, connector->base.id);
		return -EINVAL;
	}

	if (!dp_display_state_is(DP_STATE_ENABLED)) {
		dp_display_state_show("[not enabled]");
		return 0;
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
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	if (!dp->mst.drm_registered) {
		DP_DEBUG("DP%d drm mst not registered\n", dp->cell_idx);
		return -EPERM;
	}

	sde_conn = to_sde_connector(connector);
	if (!sde_conn->drv_panel) {
		DP_ERR("DP%d invalid panel for connector:%d\n",
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

	DP_MST_DEBUG("dp mst connector:%d link info updated\n",
		connector->base.id);

	return rc;
}

static int dp_display_mst_get_fixed_topology_port(
			struct dp_display *dp_display,
			u32 strm_id, u32 *port_num)
{
	struct dp_display_private *dp;
	u32 port;

	if (!dp_display) {
		DP_ERR("invalid input\n");
		return -EINVAL;
	}

	if (strm_id >= DP_STREAM_MAX) {
		DP_ERR("invalid stream id:%d\n", strm_id);
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
		DP_ERR("invalid input\n");
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
		DP_ERR("invalid input\n");
		return;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	if (!dp->mst.drm_registered) {
		DP_DEBUG("DP%d drm mst not registered\n", dp->cell_idx);
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
		DP_ERR("invalid input\n");
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
		dp_display_host_unready(dp);
		dp_display_change_phy_bond_mode(dp, mode);
	}

	dp->bond_primary = primary_connector;

	mutex_unlock(&dp->session_lock);

	return 0;
}

static int dp_display_probe(struct platform_device *pdev)
{
	int rc = 0;
	struct dp_display_private *dp;
	struct dp_display *dp_display;
	int index;

	if (!pdev || !pdev->dev.of_node) {
		DP_ERR("pdev not found\n");
		rc = -ENODEV;
		goto bail;
	}

	index = dp_display_get_num_of_displays(NULL);
	if (index >= MAX_DP_ACTIVE_DISPLAY) {
		DP_ERR("exceeds max dp count\n");
		rc = -EINVAL;
		goto bail;
	}

	dp = devm_kzalloc(&pdev->dev, sizeof(*dp), GFP_KERNEL);
	if (!dp) {
		rc = -ENOMEM;
		goto bail;
	}

	init_completion(&dp->attention_comp);

	dp->pdev = pdev;
	dp->name = of_get_property(dp->pdev->dev.of_node, "label", NULL);
	if (!dp->name)
		dp->name = "drm_dp";

	dp->cell_idx = -1;
	dp->phy_idx = -1;
	memset(&dp->mst, 0, sizeof(dp->mst));

	rc = dp_display_get_cell_info(dp);
	if (rc)
		goto error;

	rc = dp_parser_msm_hdcp_dev(dp);
	if (rc)
		goto error;

	rc = dp_display_init_aux_bridge(dp);
	if (rc)
		goto error;

	rc = dp_display_create_workqueue(dp);
	if (rc) {
		DP_ERR("DP%d Failed to create workqueue\n", dp->cell_idx);
		goto error;
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
	dp_display->set_colorspace = dp_display_setup_colospace;
	dp_display->get_available_dp_resources =
				dp_display_get_available_dp_resources;
	dp_display->get_display_type = dp_display_get_display_type;
	dp_display->mst_get_fixed_topology_display_type =
				dp_display_mst_get_fixed_topology_display_type;
	dp_display->set_phy_bond_mode = dp_display_set_phy_bond_mode;
	dp_display->get_mst_pbn_div = dp_display_get_mst_pbn_div;

	rc = component_add(&pdev->dev, &dp_display_comp_ops);
	if (rc) {
		DP_ERR("DP%d component add failed, rc=%d\n", dp->cell_idx, rc);
		goto error;
	}

	return 0;
error:
	g_dp_display[index] = NULL;
	devm_kfree(&pdev->dev, dp);
bail:
	return rc;
}

int dp_display_get_displays(struct drm_device *dev, void **displays, int count)
{
	int i, j;

	if (!displays) {
		DP_ERR("invalid data\n");
		return -EINVAL;
	}

	for (i = 0, j = 0; i < MAX_DP_ACTIVE_DISPLAY && j < count; i++) {
		if (!g_dp_display[i])
			break;

		if (g_dp_display[i]->drm_dev == dev) {
			displays[j] = g_dp_display[i];
			j++;
		}
	}

	return j;
}

int dp_display_get_num_of_displays(struct drm_device *dev)
{
	int i, j;

	for (i = 0, j = 0; i < MAX_DP_ACTIVE_DISPLAY; i++) {
		if (!g_dp_display[i])
			break;

		if (!dev || g_dp_display[i]->drm_dev == dev)
			j++;
	}

	return j;
}

int dp_display_get_num_of_streams(struct drm_device *dev)
{
	struct dp_display_private *dp;
	int i, count = 0;

	for (i = 0; i < MAX_DP_ACTIVE_DISPLAY; i++) {
		if (!g_dp_display[i])
			break;

		if (g_dp_display[i]->drm_dev != dev)
			continue;

		dp = container_of(g_dp_display[i], struct dp_display_private, dp_display);

		count += dp->stream_cnt;
	}

	return count;
}

int dp_display_get_num_of_bonds(void *dp_display)
{
	struct dp_display_private *dp;
	int i, cnt = 0;
	struct {
		const char *name;
		enum dp_bond_type type;
	} static const bond_types[] =
	{
		{ "qcom,bond-dual-ctrl-phy", DP_BOND_DUAL_PHY },
		{ "qcom,bond-dual-ctrl-pclk", DP_BOND_DUAL_PCLK },
		{ "qcom,bond-tri-ctrl-phy", DP_BOND_TRIPLE_PHY },
		{ "qcom,bond-tri-ctrl-pclk", DP_BOND_TRIPLE_PCLK },
		/* for backward compatiblity */
		{ "qcom,bond-dual-ctrl", DP_BOND_DUAL_PHY },
		{ "qcom,bond-tri-ctrl", DP_BOND_TRIPLE_PCLK },
	};

	if (!dp_display) {
		DP_DEBUG("dp display not initialized\n");
		return 0;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	if (!dp->parser) {
		for (i = 0; i < ARRAY_SIZE(bond_types); i++) {
			if (of_property_count_u32_elems(dp->pdev->dev.of_node,
					bond_types[i].name) == num_bond_dp[bond_types[i].type])
				cnt++;
		}
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
		DP_DEBUG("dp display not initialized\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	dp_info->cell_idx = dp->cell_idx;
	dp_info->intf_idx[0] = dp->intf_idx[0];
	for (i = 1; i < dp->stream_cnt; i++)
		dp_info->intf_idx[i] = dp->intf_idx[i];
	dp_info->phy_idx = dp->phy_idx;
	dp_info->stream_cnt = dp->stream_cnt;

	return 0;
 }

int dp_display_get_bond_displays(void *dp_display, enum dp_bond_type type,
		struct dp_display_bond_displays *dp_bond_info)
{
	struct dp_display_private *dp;
	int i, j, n = 0;

	if (!dp_display) {
		DP_DEBUG("dp display not initialized\n");
		return -EINVAL;
	}

	if (type < 0 || type >= DP_BOND_MAX) {
		DP_DEBUG("invalid bond type\n");
		return -EINVAL;
	}

	dp = container_of(dp_display, struct dp_display_private, dp_display);

	memset(dp_bond_info, 0, sizeof(*dp_bond_info));

	if (!dp->parser->bond_cfg[type].enable)
		return 0;

	dp_bond_info->dp_display_num = num_bond_dp[type];

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
				n++;
				break;
			}
		}
		if (n == dp_bond_info->dp_display_num)
			break;
	}

	if (n < dp_bond_info->dp_display_num) {
		DP_WARN("no enough dp displays (%d:%d) for bond type %d, disabled\n",
				n, dp_bond_info->dp_display_num, type);
		dp_bond_info->dp_display_num = 0;
	}

	return 0;
}

static void dp_display_set_mst_state(void *dp_display,
		enum dp_drv_state mst_state)
{
	struct dp_display_private *dp;

	dp = container_of(dp_display, struct dp_display_private, dp_display);
	SDE_EVT32_EXTERNAL(mst_state, dp->mst.mst_active);

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

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY);
	mutex_lock(&dp->session_lock);
	dp_display_set_mst_state(&dp->dp_display, PM_SUSPEND);

	/*
	 * There are a few instances where the DP is hotplugged when the device
	 * is in PM suspend state. After hotplug, it is observed the device
	 * enters and exits the PM suspend multiple times while aux transactions
	 * are taking place. This may sometimes cause an unclocked register
	 * access error. So, abort aux transactions when such a situation
	 * arises i.e. when DP is connected but display not enabled yet.
	 */
	if (dp_display_state_is(DP_STATE_CONNECTED) &&
			!dp_display_state_is(DP_STATE_ENABLED)) {
		dp->aux->abort(dp->aux, true);
		dp->ctrl->abort(dp->ctrl, true);
	}

	dp_display_state_add(DP_STATE_SUSPENDED);
	mutex_unlock(&dp->session_lock);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state);

	return 0;
}

static void dp_pm_complete(struct device *dev)
{
	struct dp_display_private *dp;

	if (!dev)
		return;

	dp = dev_get_drvdata(dev);

	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_ENTRY);
	mutex_lock(&dp->session_lock);
	dp_display_set_mst_state(&dp->dp_display, PM_DEFAULT);

	/*
	 * There are multiple PM suspend entry and exits observed before
	 * the connect uevent is issued to userspace. The aux transactions are
	 * aborted during PM suspend entry in dp_pm_prepare to prevent unclocked
	 * register access. On PM suspend exit, there will be no host_init call
	 * to reset the abort flags for ctrl and aux incase DP is connected
	 * but display not enabled. So, resetting abort flags for aux and ctrl.
	 */
	if (dp_display_state_is(DP_STATE_CONNECTED) &&
			!dp_display_state_is(DP_STATE_ENABLED)) {
		dp->aux->abort(dp->aux, false);
		dp->ctrl->abort(dp->ctrl, false);
	}

	dp_display_state_remove(DP_STATE_SUSPENDED);
	mutex_unlock(&dp->session_lock);
	SDE_EVT32_EXTERNAL(SDE_EVTLOG_FUNC_EXIT, dp->state);
}

static const struct dev_pm_ops dp_pm_ops = {
	.prepare = dp_pm_prepare,
	.complete = dp_pm_complete,
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

	dp_pll_drv_register();
	platform_driver_register(&dp_display_driver);
}

void __exit dp_display_unregister(void)
{
	platform_driver_unregister(&dp_display_driver);
	dp_pll_drv_unregister();
}
