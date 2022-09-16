/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _DP_LPHW_HPD_H_
#define _DP_LPHW_HPD_H_

#include "dp_hpd.h"

#define DP_HPD_PLUG_INT_STATUS		BIT(0)
#define DP_IRQ_HPD_INT_STATUS		BIT(1)
#define DP_HPD_REPLUG_INT_STATUS	BIT(2)
#define DP_HPD_UNPLUG_INT_STATUS	BIT(3)

enum {
	DP_HPD_STATUS_DISCONNECTED = 0,
	DP_HPD_STATUS_CONNECT_PENDING = 1,
	DP_HPD_STATUS_CONNECTED = 2,
	DP_HPD_STATUS_HPD_IO_GLITCH_COUNT = 3,
	DP_HPD_STATUS_IRQ_HPD_PULSE_COUNT = 4,
	DP_HPD_STATUS_HPD_REPLUG_COUNT = 5,
	DP_HPD_STATUS_UNKNOWN_1 = 6,
	DP_HPD_STATUS_UNKNOWN_2 = 7,
};

/**
 * dp_lphw_hpd_get() - configure and get the DisplayPlot HPD module data
 *
 * @dev: device instance of the caller
 * return: pointer to allocated gpio hpd module data
 *
 * This function sets up the lphw hpd module
 */
struct dp_hpd *dp_lphw_hpd_get(struct device *dev, struct dp_parser *parser,
	struct dp_catalog_hpd *catalog, struct dp_hpd_cb *cb);

/**
 * dp_lphw_hpd_put()
 *
 * Cleans up dp_hpd instance
 *
 * @hpd: instance of lphw_hpd
 */
void dp_lphw_hpd_put(struct dp_hpd *hpd);

#endif /* _DP_LPHW_HPD_H_ */
