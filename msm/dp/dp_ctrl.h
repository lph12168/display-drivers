/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2012-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _DP_CTRL_H_
#define _DP_CTRL_H_

#include "dp_aux.h"
#include "dp_panel.h"
#include "dp_link.h"
#include "dp_parser.h"
#include "dp_power.h"
#include "dp_catalog.h"


enum {
	LINK_TRAINING_MODE_NORMAL	= 0,
	LINK_TRAINING_MODE_FORCE,
	LINK_TRAINING_MODE_SHALLOW,
};

struct dp_ctrl {
	int (*init)(struct dp_ctrl *dp_ctrl, bool flip, bool reset);
	void (*deinit)(struct dp_ctrl *dp_ctrl);
	int (*on)(struct dp_ctrl *dp_ctrl, bool mst_mode, bool fec_en,
			bool dsc_en, int training_mode);
	void (*off)(struct dp_ctrl *dp_ctrl);
	void (*abort)(struct dp_ctrl *dp_ctrl, bool reset);
	void (*isr)(struct dp_ctrl *dp_ctrl);
	bool (*handle_sink_request)(struct dp_ctrl *dp_ctrl);
	void (*process_phy_test_request)(struct dp_ctrl *dp_ctrl);
	int (*link_maintenance)(struct dp_ctrl *dp_ctrl);
	int (*stream_on)(struct dp_ctrl *dp_ctrl, struct dp_panel *panel);
	void (*stream_off)(struct dp_ctrl *dp_ctrl, struct dp_panel *panel);
	void (*stream_pre_off)(struct dp_ctrl *dp_ctrl, struct dp_panel *panel);
	void (*set_mst_channel_info)(struct dp_ctrl *dp_ctrl,
			enum dp_stream_id strm,
			u32 ch_start_slot, u32 ch_tot_slots);
	void (*set_phy_bond_mode)(struct dp_ctrl *dp_ctrl,
			enum dp_phy_bond_mode mode);
	void (*setup_misr)(struct dp_ctrl *dp_ctrl,
			bool enable, u32 frame_count);
	int (*collect_misr)(struct dp_ctrl *dp_ctrl, u32 *misr);
	int (*collect_crc)(struct dp_ctrl *dp_ctrl,
			u32 *r, u32 *g, u32 *b, struct dp_panel *panel);
};

struct dp_ctrl_in {
	u32 cell_idx;
	struct device *dev;
	struct dp_panel *panel;
	struct dp_aux *aux;
	struct dp_link *link;
	struct dp_parser *parser;
	struct dp_power *power;
	struct dp_catalog_ctrl *catalog;
	enum dp_phy_bond_mode phy_bond_mode;
};

struct dp_ctrl *dp_ctrl_get(struct dp_ctrl_in *in);
void dp_ctrl_put(struct dp_ctrl *dp_ctrl);

#endif /* _DP_CTRL_H_ */
