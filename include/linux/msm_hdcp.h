/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __MSM_HDCP_H
#define __MSM_HDCP_H
#include <linux/types.h>
#include "hdcp/msm_hdmi_hdcp_mgr.h"

struct msm_hdcp_status {
	int state;
	int version;
	u8 min_enc_level;
};

#if IS_ENABLED(CONFIG_HDCP_QSEECOM)
void msm_hdcp_notify_topology(struct device *dev);
void msm_hdcp_cache_repeater_topology(struct device *dev,
			struct HDCP_V2V1_MSG_TOPOLOGY *tp);
void msm_hdcp_register_cb(struct device *dev, void *ctx,
	void (*cb)(void *ctx, u8 data));
void msm_hdcp_notify_status(struct device *dev,
		struct msm_hdcp_status *status);

#else
static inline void msm_hdcp_notify_topology(struct device *dev)
{
}

static inline void msm_hdcp_cache_repeater_topology(struct device *dev,
			struct HDCP_V2V1_MSG_TOPOLOGY *tp)
{
}

static inline void msm_hdcp_register_cb(struct device *dev, void *ctx,
	void (*cb)(void *ctx, u8 data))
{
}

static inline void msm_hdcp_notify_status(struct device *dev,
		struct msm_hdcp_status *status)
{
}
#endif /* CONFIG_HDCP_QSEECOM*/

#endif /* __MSM_HDCP_H */
