/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2017-2020, The Linux Foundation. All rights reserved.
 */

#ifndef _DP_DRM_H_
#define _DP_DRM_H_

#include <linux/types.h>
#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>

#include "msm_drv.h"
#include "dp_display.h"

struct dp_bridge {
	struct drm_bridge base;
	u32 id;

	struct drm_connector *connector;
	struct dp_display *display;
	struct dp_display_mode dp_mode;
	void *dp_panel;
};

/**
 * dp_connector_config_hdr - callback to configure HDR
 * @connector: Pointer to drm connector structure
 * @display: Pointer to private display handle
 * @c_state: connect state data
 * Returns: Zero on success
 */
int dp_connector_config_hdr(struct drm_connector *connector,
		void *display,
		struct sde_connector_state *c_state);

/**
 * dp_connector_post_init - callback to perform additional initialization steps
 * @connector: Pointer to drm connector structure
 * @display: Pointer to private display handle
 * Returns: Zero on success
 */
int dp_connector_post_init(struct drm_connector *connector, void *display);

/**
 * dp_connector_detect - callback to determine if connector is connected
 * @connector: Pointer to drm connector structure
 * @force: Force detect setting from drm framework
 * @display: Pointer to private display handle
 * Returns: Connector 'is connected' status
 */
enum drm_connector_status dp_connector_detect(struct drm_connector *conn,
		bool force,
		void *display);

/**
 * dp_connector_get_modes - callback to add drm modes via drm_mode_probed_add()
 * @connector: Pointer to drm connector structure
 * @display: Pointer to private display handle
 * Returns: Number of modes added
 */
int dp_connector_get_modes(struct drm_connector *connector,
		void *display);

/**
 * dp_connector_mode_valid - callback to determine if specified mode is valid
 * @connector: Pointer to drm connector structure
 * @mode: Pointer to drm mode structure
 * @display: Pointer to private display handle
 * Returns: Validity status for specified mode
 */
enum drm_mode_status dp_connector_mode_valid(struct drm_connector *connector,
		struct drm_display_mode *mode,
		void *display);

/**
 * dp_connector_get_mode_info - retrieve information of the mode selected
 * @connector: Pointer to drm connector structure
 * @drm_mode: Display mode set for the display
 * @mode_info: Out parameter. Information of the mode
 * @max_mixer_width: max width supported by HW layer mixer
 * @display: Pointer to private display structure
 * Returns: zero on success
 */
int dp_connector_get_mode_info(struct drm_connector *connector,
		const struct drm_display_mode *drm_mode,
		struct msm_mode_info *mode_info,
		u32 max_mixer_width, void *display);

/**
 * dp_connector_get_info - retrieve connector display info
 * @connector: Pointer to drm connector structure
 * @info: Out parameter. Information of the connected display
 * @display: Pointer to private display structure
 * Returns: zero on success
 */
int dp_connector_get_info(struct drm_connector *connector,
		struct msm_display_info *info, void *display);

/**
 * dp_connector_post_open - handle the post open functionalites
 * @connector: Pointer to drm connector structure
 * @display: Pointer to private display structure
 */
void dp_connector_post_open(struct drm_connector *connector, void *display);

/**
 * dp_conn_set_info_blob - callback to perform info blob initialization
 * @connector: Pointer to drm connector structure
 * @info: Pointer to sde connector info structure
 * @display: Pointer to private display handle
 * @mode_info: Pointer to mode info structure
 * Returns: Zero on success
 */
int dp_connnector_set_info_blob(struct drm_connector *connector,
		void *info, void *display, struct msm_mode_info *mode_info);

/**
 * dp_connector_atomic_best_encoder - callback to select best encoder
 * @connector: Pointer to drm connector structure
 * @display: Pointer to private display handle
 * @state: Pointer to connector state
 * Returns: Pointer of encoder, NULL if failed
 */
struct drm_encoder *dp_connector_atomic_best_encoder(
		struct drm_connector *connector, void *display,
		struct drm_connector_state *state);

/**
 * dp_connector_atomic_check - callback to do atomic check
 * @connector: Pointer to drm connector structure
 * @display: Pointer to private display handle
 * @new_conn_state: Pointer to connector state
 * Returns: Zero on success
 */
int dp_connector_atomic_check(
		struct drm_connector *connector, void *display,
		struct drm_connector_state *new_conn_state);

/**
 * dp_connector_get_tile_map - callback to get tile map
 * @connector: Pointer to drm connector structure
 * @display: Pointer to private display handle
 * @num_tile: Number of tiles
 * @tile_map: Pointer to tile indices
 * Returns: Zero on success
 */
int dp_connector_get_tile_map(struct drm_connector *connector,
		void *display, int num_tile, int *tile_map);

int dp_drm_bridge_init(void *display,
	struct drm_encoder *encoder);

void dp_drm_bridge_deinit(void *display);

int dp_drm_bond_bridge_init(void *display,
	struct drm_encoder *encoder,
	enum dp_bond_type type,
	struct dp_display_bond_displays *bond_displays);

/**
 * convert_to_drm_mode - convert dp mode to drm mode
 * @dp_mode: Point to dp mode
 * @drm_mode: Pointer to drm mode
 */
void convert_to_drm_mode(const struct dp_display_mode *dp_mode,
				struct drm_display_mode *drm_mode);

/**
 * dp_connector_update_pps - update pps for given connector
 * @dp_mode: Point to dp mode
 * @pps_cmd: PPS packet
 * @display: Pointer to private display structure
 */
int dp_connector_update_pps(struct drm_connector *connector,
		char *pps_cmd, void *display);

/**
 * dp_mst_drm_bridge_init - initialize mst bridge
 * @display: Pointer to private display structure
 * @encoder: Pointer to encoder for mst bridge mapping
 */
int dp_mst_drm_bridge_init(void *display,
	struct drm_encoder *encoder);

/**
 * dp_mst_drm_super_bridge_init - initialize mst super bridge
 * @display: Pointer to private display structure
 * @encoder: Pointer to encoder for mst bridge mapping
 */
int dp_mst_drm_super_bridge_init(void *display,
	struct drm_encoder *encoder);

/**
 * dp_mst_drm_bridge_deinit - de-initialize mst bridges
 * @display: Pointer to private display structure
 */
void dp_mst_drm_bridge_deinit(void *display);

/**
 * dp_mst_init - initialize mst objects for the given display
 * @display: Pointer to private display structure
 */
int dp_mst_init(struct dp_display *dp_display);

/**
 * dp_mst_deinit - de-initialize mst objects for the given display
 * @display: Pointer to private display structure
 */
void dp_mst_deinit(struct dp_display *dp_display);

/**
 * dp_mst_dump_topology - dump out current mst topology
 * @display: Pointer to private display structure
 * @m: Pointer to sequential file
 */
void dp_mst_dump_topology(struct dp_display *dp_display, struct seq_file *m);

#endif /* _DP_DRM_H_ */

