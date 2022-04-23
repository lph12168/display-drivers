// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt)	"[drm-dp] %s: " fmt, __func__

#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/string.h>

#include "dp_parser.h"

#define REG_MASK(n)                     ((BIT(n)) - 1)

static void dp_parser_unmap_io_resources(struct dp_parser *parser)
{
	int i = 0;
	struct dp_io *io = &parser->io;

	for (i = 0; i < io->len; i++)
		msm_dss_iounmap(&io->data[i].io);
}

static int dp_parser_reg(struct dp_parser *parser)
{
	int rc = 0, i = 0;
	u32 reg_count;
	struct platform_device *pdev = parser->pdev;
	struct dp_io *io = &parser->io;
	struct device *dev = &pdev->dev;

	reg_count = of_property_count_strings(dev->of_node, "reg-names");
	if (reg_count <= 0) {
		pr_err("no reg defined\n");
		return -EINVAL;
	}

	io->len = reg_count;
	io->data = devm_kzalloc(dev, sizeof(struct dp_io_data) * reg_count,
			GFP_KERNEL);
	if (!io->data)
		return -ENOMEM;

	for (i = 0; i < reg_count; i++) {
		of_property_read_string_index(dev->of_node,
				"reg-names", i,	&io->data[i].name);
		rc = msm_dss_ioremap_byname(pdev, &io->data[i].io,
			io->data[i].name);
		if (rc) {
			pr_err("unable to remap %s resources\n",
				io->data[i].name);
			goto err;
		}
	}

	return 0;
err:
	dp_parser_unmap_io_resources(parser);
	return rc;
}

static const char *dp_get_phy_aux_config_property(u32 cfg_type)
{
	switch (cfg_type) {
	case PHY_AUX_CFG0:
		return "qcom,aux-cfg0-settings";
	case PHY_AUX_CFG1:
		return "qcom,aux-cfg1-settings";
	case PHY_AUX_CFG2:
		return "qcom,aux-cfg2-settings";
	case PHY_AUX_CFG3:
		return "qcom,aux-cfg3-settings";
	case PHY_AUX_CFG4:
		return "qcom,aux-cfg4-settings";
	case PHY_AUX_CFG5:
		return "qcom,aux-cfg5-settings";
	case PHY_AUX_CFG6:
		return "qcom,aux-cfg6-settings";
	case PHY_AUX_CFG7:
		return "qcom,aux-cfg7-settings";
	case PHY_AUX_CFG8:
		return "qcom,aux-cfg8-settings";
	case PHY_AUX_CFG9:
		return "qcom,aux-cfg9-settings";
	default:
		return "unknown";
	}
}

static void dp_parser_phy_aux_cfg_reset(struct dp_parser *parser)
{
	int i = 0;

	for (i = 0; i < PHY_AUX_CFG_MAX; i++)
		parser->aux_cfg[i] = (const struct dp_aux_cfg){ 0 };
}

static int dp_parser_aux(struct dp_parser *parser)
{
	struct device_node *of_node = parser->pdev->dev.of_node;
	int len = 0, i = 0, j = 0, config_count = 0;
	const char *data;
	int const minimum_config_count = 1;

	for (i = 0; i < PHY_AUX_CFG_MAX; i++) {
		const char *property = dp_get_phy_aux_config_property(i);

		data = of_get_property(of_node, property, &len);
		if (!data) {
			pr_err("Unable to read %s\n", property);
			goto error;
		}

		config_count = len - 1;
		if ((config_count < minimum_config_count) ||
			(config_count > DP_AUX_CFG_MAX_VALUE_CNT)) {
			pr_err("Invalid config count (%d) configs for %s\n",
					config_count, property);
			goto error;
		}

		parser->aux_cfg[i].offset = data[0];
		parser->aux_cfg[i].cfg_cnt = config_count;
		pr_debug("%s offset=0x%x, cfg_cnt=%d\n",
				property,
				parser->aux_cfg[i].offset,
				parser->aux_cfg[i].cfg_cnt);
		for (j = 1; j < len; j++) {
			parser->aux_cfg[i].lut[j - 1] = data[j];
			pr_debug("%s lut[%d]=0x%x\n",
					property,
					i,
					parser->aux_cfg[i].lut[j - 1]);
		}
	}
		return 0;

error:
	dp_parser_phy_aux_cfg_reset(parser);
	return -EINVAL;
}

static int dp_parser_misc(struct dp_parser *parser)
{
	int rc = 0, len = 0, i = 0;
	const char *data = NULL;

	struct device_node *of_node = parser->pdev->dev.of_node;

	data = of_get_property(of_node, "qcom,logical2physical-lane-map", &len);
	if (data && (len == DP_MAX_PHY_LN)) {
		for (i = 0; i < len; i++)
			parser->l_map[i] = data[i];
	} else {
		pr_debug("Incorrect mapping, configure default\n");
		parser->l_map[0] = DP_PHY_LN0;
		parser->l_map[1] = DP_PHY_LN1;
		parser->l_map[2] = DP_PHY_LN2;
		parser->l_map[3] = DP_PHY_LN3;
	}

	data = of_get_property(of_node, "qcom,pn-swap-lane-map", &len);
	if (data && (len == DP_MAX_PHY_LN)) {
		for (i = 0; i < len; i++)
			parser->l_pnswap |= (data[i] & 0x01) << i;
	}

	rc = of_property_read_u32(of_node,
		"qcom,max-lane-count", &parser->max_lane_count);
	if (rc) {
		pr_debug("No qcom,max-lane-count defined, fallback to default 4-lanes");
		parser->max_lane_count = 4;
	} else if (parser->max_lane_count < 1 || parser->max_lane_count > 4) {
		pr_warn("Invalid qcom,max-lane-count, fallback to default 4-lanes");
		parser->max_lane_count = 4;
	}

	rc = of_property_read_u32(of_node,
		"qcom,max-pclk-frequency-khz", &parser->max_pclk_khz);
	if (rc)
		parser->max_pclk_khz = DP_MAX_PIXEL_CLK_KHZ;

	rc = of_property_read_u32(of_node,
		"qcom,max-lclk-frequency-khz", &parser->max_lclk_khz);
	if (rc)
		parser->max_lclk_khz = DP_MAX_LINK_CLK_KHZ;

	rc = of_property_read_u32(of_node,
		"qcom,max-hdisplay", &parser->max_hdisplay);

	rc = of_property_read_u32(of_node,
		"qcom,max-vdisplay", &parser->max_vdisplay);

	parser->no_power_down = of_property_read_bool(of_node,
			"qcom,no-power-down");

	parser->display_type = of_get_property(of_node,
					"qcom,display-type", NULL);
	if (!parser->display_type)
		parser->display_type = "unknown";

	parser->force_bond_mode = of_property_read_bool(of_node,
			"qcom,dp-force-bond-mode");

	parser->force_connect_mode = of_property_read_bool(of_node,
			"qcom,dp-force-connect-mode");

	parser->no_link_rate_reduction = of_property_read_bool(of_node,
			"qcom,no-link-rate-reduction");

	parser->no_lane_count_reduction = of_property_read_bool(of_node,
			"qcom,no-lane-count-reduction");

	return 0;
}

static int dp_parser_pinctrl(struct dp_parser *parser)
{
	struct dp_pinctrl *pinctrl = &parser->pinctrl;

	pinctrl->pin = devm_pinctrl_get(&parser->pdev->dev);

	if (IS_ERR_OR_NULL(pinctrl->pin)) {
		pr_debug("failed to get pinctrl\n");
		return 0;
	}

	if (parser->no_aux_switch && parser->lphw_hpd) {
		pinctrl->state_hpd_tlmm = pinctrl->state_hpd_ctrl = NULL;

		pinctrl->state_hpd_tlmm = pinctrl_lookup_state(pinctrl->pin,
					"mdss_dp_hpd_tlmm");
		if (!IS_ERR_OR_NULL(pinctrl->state_hpd_tlmm)) {
			pinctrl->state_hpd_ctrl = pinctrl_lookup_state(
				pinctrl->pin, "mdss_dp_hpd_ctrl");
		}

		if (IS_ERR_OR_NULL(pinctrl->state_hpd_tlmm) ||
				IS_ERR_OR_NULL(pinctrl->state_hpd_ctrl)) {
			pinctrl->state_hpd_tlmm = NULL;
			pinctrl->state_hpd_ctrl = NULL;
			pr_debug("tlmm or ctrl pinctrl state does not exist\n");
		}
	}

	pinctrl->state_active = pinctrl_lookup_state(pinctrl->pin,
					"mdss_dp_active");
	if (IS_ERR_OR_NULL(pinctrl->state_active)) {
		pinctrl->state_active = NULL;
		pr_debug("failed to get pinctrl active state\n");
	}

	pinctrl->state_suspend = pinctrl_lookup_state(pinctrl->pin,
					"mdss_dp_sleep");
	if (IS_ERR_OR_NULL(pinctrl->state_suspend)) {
		pinctrl->state_suspend = NULL;
		pr_debug("failed to get pinctrl suspend state\n");
	}

	return 0;
}

static int dp_parser_gpio(struct dp_parser *parser)
{
	int i = 0;
	struct device *dev = &parser->pdev->dev;
	struct device_node *of_node = dev->of_node;
	struct dss_module_power *mp = &parser->mp[DP_CORE_PM];
	static const char * const dp_gpios[] = {
		"qcom,aux-en-gpio",
		"qcom,aux-sel-gpio",
		"qcom,usbplug-cc-gpio",
	};

	if (of_find_property(of_node, "qcom,dp-hpd-gpio", NULL)) {
		parser->no_aux_switch = true;
		parser->lphw_hpd = of_find_property(of_node,
				"qcom,dp-low-power-hw-hpd", NULL);
		return 0;
	}

	if (of_find_property(of_node, "qcom,dp-gpio-aux-switch", NULL))
		parser->gpio_aux_switch = true;
	mp->gpio_config = devm_kzalloc(dev,
		sizeof(struct dss_gpio) * ARRAY_SIZE(dp_gpios), GFP_KERNEL);
	if (!mp->gpio_config)
		return -ENOMEM;

	mp->num_gpio = ARRAY_SIZE(dp_gpios);

	for (i = 0; i < ARRAY_SIZE(dp_gpios); i++) {
		mp->gpio_config[i].gpio = of_get_named_gpio(of_node,
			dp_gpios[i], 0);

		if (!gpio_is_valid(mp->gpio_config[i].gpio)) {
			pr_debug("%s gpio not specified\n", dp_gpios[i]);
			/* In case any gpio was not specified, we think gpio
			 * aux switch also was not specified.
			 */
			parser->gpio_aux_switch = false;
			continue;
		}

		strlcpy(mp->gpio_config[i].gpio_name, dp_gpios[i],
			sizeof(mp->gpio_config[i].gpio_name));

		mp->gpio_config[i].value = 0;
	}

	return 0;
}

static const char *dp_parser_supply_node_name(enum dp_pm_type module)
{
	switch (module) {
	case DP_CORE_PM:	return "qcom,core-supply-entries";
	case DP_CTRL_PM:	return "qcom,ctrl-supply-entries";
	case DP_PHY_PM:		return "qcom,phy-supply-entries";
	default:		return "???";
	}
}

static int dp_parser_get_vreg(struct dp_parser *parser,
		enum dp_pm_type module)
{
	int i = 0, rc = 0;
	u32 tmp = 0;
	const char *pm_supply_name = NULL;
	struct device_node *supply_node = NULL;
	struct device_node *of_node = parser->pdev->dev.of_node;
	struct device_node *supply_root_node = NULL;
	struct dss_module_power *mp = &parser->mp[module];

	mp->num_vreg = 0;
	pm_supply_name = dp_parser_supply_node_name(module);
	supply_root_node = of_get_child_by_name(of_node, pm_supply_name);
	if (!supply_root_node) {
		pr_err("no supply entry present: %s\n", pm_supply_name);
		goto novreg;
	}

	mp->num_vreg = of_get_available_child_count(supply_root_node);

	if (mp->num_vreg == 0) {
		pr_debug("no vreg\n");
		goto novreg;
	} else {
		pr_debug("vreg found. count=%d\n", mp->num_vreg);
	}

	mp->vreg_config = devm_kzalloc(&parser->pdev->dev,
		sizeof(struct dss_vreg) * mp->num_vreg, GFP_KERNEL);
	if (!mp->vreg_config) {
		rc = -ENOMEM;
		goto error;
	}

	for_each_child_of_node(supply_root_node, supply_node) {
		const char *st = NULL;
		/* vreg-name */
		rc = of_property_read_string(supply_node,
			"qcom,supply-name", &st);
		if (rc) {
			pr_err("error reading name. rc=%d\n",
				 rc);
			goto error;
		}
		snprintf(mp->vreg_config[i].vreg_name,
			ARRAY_SIZE((mp->vreg_config[i].vreg_name)), "%s", st);
		/* vreg-min-voltage */
		rc = of_property_read_u32(supply_node,
			"qcom,supply-min-voltage", &tmp);
		if (rc) {
			pr_err("error reading min volt. rc=%d\n",
				rc);
			goto error;
		}
		mp->vreg_config[i].min_voltage = tmp;

		/* vreg-max-voltage */
		rc = of_property_read_u32(supply_node,
			"qcom,supply-max-voltage", &tmp);
		if (rc) {
			pr_err("error reading max volt. rc=%d\n",
				rc);
			goto error;
		}
		mp->vreg_config[i].max_voltage = tmp;

		/* enable-load */
		rc = of_property_read_u32(supply_node,
			"qcom,supply-enable-load", &tmp);
		if (rc) {
			pr_err("error reading enable load. rc=%d\n",
				rc);
			goto error;
		}
		mp->vreg_config[i].enable_load = tmp;

		/* disable-load */
		rc = of_property_read_u32(supply_node,
			"qcom,supply-disable-load", &tmp);
		if (rc) {
			pr_err("error reading disable load. rc=%d\n",
				rc);
			goto error;
		}
		mp->vreg_config[i].disable_load = tmp;

		pr_debug("%s min=%d, max=%d, enable=%d, disable=%d\n",
			mp->vreg_config[i].vreg_name,
			mp->vreg_config[i].min_voltage,
			mp->vreg_config[i].max_voltage,
			mp->vreg_config[i].enable_load,
			mp->vreg_config[i].disable_load
			);
		++i;
	}

	return rc;

error:
	if (mp->vreg_config) {
		devm_kfree(&parser->pdev->dev, mp->vreg_config);
		mp->vreg_config = NULL;
	}
novreg:
	mp->num_vreg = 0;

	return rc;
}

static void dp_parser_put_vreg_data(struct device *dev,
	struct dss_module_power *mp)
{
	if (!mp) {
		DEV_ERR("invalid input\n");
		return;
	}

	if (mp->vreg_config) {
		devm_kfree(dev, mp->vreg_config);
		mp->vreg_config = NULL;
	}
	mp->num_vreg = 0;
}

static int dp_parser_regulator(struct dp_parser *parser)
{
	int i, rc = 0;
	struct platform_device *pdev = parser->pdev;

	/* Parse the regulator information */
	for (i = DP_CORE_PM; i <= DP_PHY_PM; i++) {
		rc = dp_parser_get_vreg(parser, i);
		if (rc) {
			pr_err("get_dt_vreg_data failed for %s. rc=%d\n",
				dp_parser_pm_name(i), rc);
			i--;
			for (; i >= DP_CORE_PM; i--)
				dp_parser_put_vreg_data(&pdev->dev,
					&parser->mp[i]);
			break;
		}
	}

	return rc;
}

static bool dp_parser_check_prefix(const char *clk_prefix, const char *clk_name)
{
	return !!strnstr(clk_name, clk_prefix, strlen(clk_name));
}

static void dp_parser_put_clk_data(struct device *dev,
	struct dss_module_power *mp)
{
	if (!mp) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	if (mp->clk_config) {
		devm_kfree(dev, mp->clk_config);
		mp->clk_config = NULL;
	}

	mp->num_clk = 0;
}

static void dp_parser_put_gpio_data(struct device *dev,
	struct dss_module_power *mp)
{
	if (!mp) {
		DEV_ERR("%s: invalid input\n", __func__);
		return;
	}

	if (mp->gpio_config) {
		devm_kfree(dev, mp->gpio_config);
		mp->gpio_config = NULL;
	}

	mp->num_gpio = 0;
}

static int dp_parser_init_clk_data(struct dp_parser *parser)
{
	int num_clk = 0, i = 0, rc = 0;
	int core_clk_count = 0, link_clk_count = 0;
	int strm0_clk_count = 0, strm1_clk_count = 0;
	const char *core_clk = "core";
	const char *strm0_clk = "strm0";
	const char *strm1_clk = "strm1";
	const char *link_clk = "link";
	const char *clk_name;
	struct device *dev = &parser->pdev->dev;
	struct dss_module_power *core_power = &parser->mp[DP_CORE_PM];
	struct dss_module_power *strm0_power = &parser->mp[DP_STREAM0_PM];
	struct dss_module_power *strm1_power = &parser->mp[DP_STREAM1_PM];
	struct dss_module_power *link_power = &parser->mp[DP_LINK_PM];

	num_clk = of_property_count_strings(dev->of_node, "clock-names");
	if (num_clk <= 0) {
		pr_err("no clocks are defined\n");
		rc = -EINVAL;
		goto exit;
	}

	for (i = 0; i < num_clk; i++) {
		of_property_read_string_index(dev->of_node,
				"clock-names", i, &clk_name);

		if (dp_parser_check_prefix(core_clk, clk_name))
			core_clk_count++;

		if (dp_parser_check_prefix(strm0_clk, clk_name))
			strm0_clk_count++;

		if (dp_parser_check_prefix(strm1_clk, clk_name))
			strm1_clk_count++;

		if (dp_parser_check_prefix(link_clk, clk_name))
			link_clk_count++;
	}

	/* Initialize the CORE power module */
	if (core_clk_count <= 0) {
		pr_err("no core clocks are defined\n");
		rc = -EINVAL;
		goto exit;
	}

	core_power->num_clk = core_clk_count;
	core_power->clk_config = devm_kzalloc(dev,
			sizeof(struct dss_clk) * core_power->num_clk,
			GFP_KERNEL);
	if (!core_power->clk_config) {
		rc = -EINVAL;
		goto exit;
	}

	/* Initialize the STREAM0 power module */
	if (strm0_clk_count <= 0) {
		pr_debug("no strm0 clocks are defined\n");
	} else {
		strm0_power->num_clk = strm0_clk_count;
		strm0_power->clk_config = devm_kzalloc(dev,
			sizeof(struct dss_clk) * strm0_power->num_clk,
			GFP_KERNEL);
		if (!strm0_power->clk_config) {
			strm0_power->num_clk = 0;
			rc = -EINVAL;
			goto strm0_clock_error;
		}
	}

	/* Initialize the STREAM1 power module */
	if (strm1_clk_count <= 0) {
		pr_debug("no strm1 clocks are defined\n");
	} else {
		strm1_power->num_clk = strm1_clk_count;
		strm1_power->clk_config = devm_kzalloc(dev,
			sizeof(struct dss_clk) * strm1_power->num_clk,
			GFP_KERNEL);
		if (!strm1_power->clk_config) {
			strm1_power->num_clk = 0;
			rc = -EINVAL;
			goto strm1_clock_error;
		}
	}

	/* Initialize the link power module */
	if (link_clk_count <= 0) {
		pr_err("no link clocks are defined\n");
		rc = -EINVAL;
		goto link_clock_error;
	}

	link_power->num_clk = link_clk_count;
	link_power->clk_config = devm_kzalloc(dev,
			sizeof(struct dss_clk) * link_power->num_clk,
			GFP_KERNEL);
	if (!link_power->clk_config) {
		link_power->num_clk = 0;
		rc = -EINVAL;
		goto link_clock_error;
	}

	return rc;

link_clock_error:
	dp_parser_put_clk_data(dev, strm1_power);
strm1_clock_error:
	dp_parser_put_clk_data(dev, strm0_power);
strm0_clock_error:
	dp_parser_put_clk_data(dev, core_power);
exit:
	return rc;
}

static int dp_parser_clock(struct dp_parser *parser)
{
	int rc = 0, i = 0;
	int num_clk = 0;
	int core_clk_index = 0, link_clk_index = 0;
	int core_clk_count = 0, link_clk_count = 0;
	int strm0_clk_index = 0, strm1_clk_index = 0;
	int strm0_clk_count = 0, strm1_clk_count = 0;
	const char *clk_name;
	const char *core_clk = "core";
	const char *strm0_clk = "strm0";
	const char *strm1_clk = "strm1";
	const char *link_clk = "link";
	struct device *dev = &parser->pdev->dev;
	struct dss_module_power *core_power;
	struct dss_module_power *strm0_power;
	struct dss_module_power *strm1_power;
	struct dss_module_power *link_power;

	core_power = &parser->mp[DP_CORE_PM];
	strm0_power = &parser->mp[DP_STREAM0_PM];
	strm1_power = &parser->mp[DP_STREAM1_PM];
	link_power = &parser->mp[DP_LINK_PM];

	rc =  dp_parser_init_clk_data(parser);
	if (rc) {
		pr_err("failed to initialize power data\n");
		rc = -EINVAL;
		goto exit;
	}

	core_clk_count = core_power->num_clk;
	link_clk_count = link_power->num_clk;
	strm0_clk_count = strm0_power->num_clk;
	strm1_clk_count = strm1_power->num_clk;

	num_clk = of_property_count_strings(dev->of_node, "clock-names");

	for (i = 0; i < num_clk; i++) {
		of_property_read_string_index(dev->of_node, "clock-names",
				i, &clk_name);

		if (dp_parser_check_prefix(core_clk, clk_name) &&
				core_clk_index < core_clk_count) {
			struct dss_clk *clk =
				&core_power->clk_config[core_clk_index];
			strlcpy(clk->clk_name, clk_name, sizeof(clk->clk_name));
			clk->type = DSS_CLK_AHB;
			core_clk_index++;
		} else if (dp_parser_check_prefix(link_clk, clk_name) &&
			   link_clk_index < link_clk_count) {
			struct dss_clk *clk =
				&link_power->clk_config[link_clk_index];
			strlcpy(clk->clk_name, clk_name, sizeof(clk->clk_name));
			link_clk_index++;

			if (!strcmp(clk_name, "link_clk"))
				clk->type = DSS_CLK_PCLK;
			else
				clk->type = DSS_CLK_AHB;
		} else if (dp_parser_check_prefix(strm0_clk, clk_name) &&
			   strm0_clk_index < strm0_clk_count) {
			struct dss_clk *clk =
				&strm0_power->clk_config[strm0_clk_index];
			strlcpy(clk->clk_name, clk_name, sizeof(clk->clk_name));
			strm0_clk_index++;

			clk->type = DSS_CLK_PCLK;
		} else if (dp_parser_check_prefix(strm1_clk, clk_name) &&
			   strm1_clk_index < strm1_clk_count) {
			struct dss_clk *clk =
				&strm1_power->clk_config[strm1_clk_index];
			strlcpy(clk->clk_name, clk_name, sizeof(clk->clk_name));
			strm1_clk_index++;

			clk->type = DSS_CLK_PCLK;
		}
	}

	pr_debug("clock parsing successful\n");

exit:
	return rc;
}

static int dp_parser_catalog(struct dp_parser *parser)
{
	int rc;
	u32 version;
	const char *st = NULL;
	struct device *dev = &parser->pdev->dev;

	rc = of_property_read_u32(dev->of_node, "qcom,phy-version", &version);

	if (!rc)
		parser->hw_cfg.phy_version = version;

	/* phy-mode */
	rc = of_property_read_string(dev->of_node, "qcom,phy-mode", &st);

	if (!rc) {
		if (!strcmp(st, "dp"))
			parser->hw_cfg.phy_mode = DP_PHY_MODE_DP;
		else if (!strcmp(st, "minidp"))
			parser->hw_cfg.phy_mode = DP_PHY_MODE_MINIDP;
		else if (!strcmp(st, "edp"))
			parser->hw_cfg.phy_mode = DP_PHY_MODE_EDP;
		else if (!strcmp(st, "edp-highswing"))
			parser->hw_cfg.phy_mode = DP_PHY_MODE_EDP_HIGH_SWING;
		else {
			parser->hw_cfg.phy_mode = DP_PHY_MODE_UNKNOWN;
			pr_warn("unknown phy-mode %s\n", st);
		}
	} else {
		parser->hw_cfg.phy_mode = DP_PHY_MODE_UNKNOWN;
	}

	return 0;
}

static int dp_parser_mst(struct dp_parser *parser)
{
	struct device *dev = &parser->pdev->dev;
	int i;

	parser->has_mst = of_property_read_bool(dev->of_node,
			"qcom,mst-enable");
	parser->no_mst_encoder = of_property_read_bool(dev->of_node,
			"qcom,no-mst-encoder");
	parser->has_mst_sideband = parser->has_mst;

	pr_debug("mst parsing successful. mst:%d\n", parser->has_mst);

	for (i = 0; i < MAX_DP_MST_STREAMS; i++) {
		of_property_read_u32_index(dev->of_node,
				"qcom,mst-fixed-topology-ports", i,
				&parser->mst_fixed_port[i]);
		of_property_read_string_index(
				dev->of_node,
				"qcom,mst-fixed-topology-display-types", i,
				&parser->mst_fixed_display_type[i]);
		if (!parser->mst_fixed_display_type[i])
			parser->mst_fixed_display_type[i] = "unknown";
	}

	return 0;
}

static void dp_parser_dsc(struct dp_parser *parser)
{
	int rc;
	struct device *dev = &parser->pdev->dev;

	parser->dsc_feature_enable = of_property_read_bool(dev->of_node,
			"qcom,dsc-feature-enable");

	rc = of_property_read_u32(dev->of_node,
		"qcom,max-dp-dsc-blks", &parser->max_dp_dsc_blks);
	if (rc || !parser->max_dp_dsc_blks)
		parser->dsc_feature_enable = false;

	rc = of_property_read_u32(dev->of_node,
		"qcom,max-dp-dsc-input-width-pixs",
		&parser->max_dp_dsc_input_width_pixs);
	if (rc || !parser->max_dp_dsc_input_width_pixs)
		parser->dsc_feature_enable = false;

	pr_debug("dsc parsing successful. dsc:%d, blks:%d, width:%d\n",
			parser->dsc_feature_enable,
			parser->max_dp_dsc_blks,
			parser->max_dp_dsc_input_width_pixs);
}

static void dp_parser_msa(struct dp_parser *parser)
{
	int rc = 0;
	u32 tmp;
	struct device *dev = &parser->pdev->dev;
	struct device_node *msa_node = NULL;

	msa_node = of_get_child_by_name(dev->of_node, "qcom,mdss_dp_ovr_msa");

	if (!msa_node) {
		pr_err("msa values not defined\n");
		goto error;
	}

	rc = of_property_read_u32(msa_node, "qcom,ovr_visible_width_in_px", &tmp);
	if (rc) {
		pr_err("error reading ovr_visible_width_in_px. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_visible_width_in_px = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_visible_height_in_px", &tmp);
	if (rc) {
		pr_err("error reading ovr_visible_height_in_px. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_visible_height_in_px = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_h_back_porch_px", &tmp);
	if (rc) {
		pr_err("error reading ovr_h_back_porch_px. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_h_back_porch_px = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_h_front_porch_px", &tmp);
	if (rc) {
		pr_err("error reading ovr_h_front_porch_px. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_h_front_porch_px = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_h_sync_pulse_px", &tmp);
	if (rc) {
		pr_err("error reading ovr_h_sync_pulse_px. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_h_sync_pulse_px = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_h_sync_skew_px", &tmp);
	if (rc) {
		pr_err("error reading ovr_h_sync_skew_px. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_h_sync_skew_px = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_v_back_porch_ln", &tmp);
	if (rc) {
		pr_err("error reading ovr_v_back_porch_ln. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_v_back_porch_ln = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_v_front_porch_ln", &tmp);
	if (rc) {
		pr_err("error reading ovr_v_front_porch_ln. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_v_front_porch_ln = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_v_sync_pulse_ln", &tmp);
	if (rc) {
		pr_err("error reading ovr_v_sync_pulse_ln. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_v_sync_pulse_ln = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_h_left_border_px", &tmp);
	if (rc) {
		pr_err("error reading ovr_h_left_border_px. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_h_left_border_px = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_h_right_border_px", &tmp);
	if (rc) {
		pr_err("error reading ovr_h_right_border_px. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_h_right_border_px = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_v_top_border_ln", &tmp);
	if (rc) {
		pr_err("error reading ovr_v_top_border_ln. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_v_top_border_ln = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_v_bottom_border_ln", &tmp);
	if (rc) {
		pr_err("error reading ovr_v_bottom_border_ln. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_v_bottom_border_ln = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_h_sync_active_low", &tmp);
	if (rc) {
		pr_err("error reading ovr_h_sync_active_low. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_h_sync_active_low = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_v_sync_active_low", &tmp);
	if (rc) {
		pr_err("error reading ovr_v_sync_active_low. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_v_sync_active_low = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_sw_mvid", &tmp);
	if (rc) {
		pr_err("error reading ovr_sw_mvid. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_sw_mvid = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_sw_nvid", &tmp);
	if (rc) {
		pr_err("error reading ovr_sw_nvid. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_sw_nvid = tmp;

	rc = of_property_read_u32(msa_node, "qcom,ovr_v_refresh_rate", &tmp);
	if (rc) {
		pr_err("error reading ovr_v_refresh_rate. rc=%d\n", rc);
		goto error;
	}
	parser->msa.ovr_v_refresh_rate = tmp;

	parser->msa_config = true;

	pr_debug("w=%d, h=%d, hbp=%d, hfp=%d, hsp=%d, hss=%d, vbp=%d, vfp=%d, vsp=%d, lb=%d, rb=%d, tb=%d, bb=%d, hsa=%d, vsa=%d, mvid=%d, nvid=%d, rate=%d\n",
		parser->msa.ovr_visible_width_in_px,
		parser->msa.ovr_visible_height_in_px, parser->msa.ovr_h_back_porch_px,
		parser->msa.ovr_h_front_porch_px, parser->msa.ovr_h_sync_pulse_px,
		parser->msa.ovr_h_sync_skew_px, parser->msa.ovr_v_back_porch_ln,
		parser->msa.ovr_v_front_porch_ln, parser->msa.ovr_v_sync_pulse_ln,
		parser->msa.ovr_h_left_border_px, parser->msa.ovr_h_right_border_px,
		parser->msa.ovr_v_top_border_ln, parser->msa.ovr_v_bottom_border_ln,
		parser->msa.ovr_h_sync_active_low, parser->msa.ovr_v_sync_active_low,
		parser->msa.ovr_sw_mvid, parser->msa.ovr_sw_nvid,
		parser->msa.ovr_v_refresh_rate);

error:
	if (!parser->msa_config) {
		parser->msa_config = false;
	}
}
static void dp_parser_fec(struct dp_parser *parser)
{
	struct device *dev = &parser->pdev->dev;

	parser->fec_feature_enable = of_property_read_bool(dev->of_node,
			"qcom,fec-feature-enable");

	pr_debug("fec parsing successful. fec:%d\n",
			parser->fec_feature_enable);
}

static void dp_parser_widebus(struct dp_parser *parser)
{
	struct device *dev = &parser->pdev->dev;

	parser->has_widebus = of_property_read_bool(dev->of_node,
			"qcom,widebus-enable");

	pr_debug("widebus parsing successful. widebus:%d\n",
			parser->has_widebus);
}

static void dp_parser_force_encryption(struct dp_parser *parser)
{
	struct device *dev = &parser->pdev->dev;

	parser->has_force_encryption = of_property_read_bool(dev->of_node,
			"qcom,hdcp-force-encryption");

	pr_debug("hdcp-force-encryption parsing successful:%d\n",
			parser->has_force_encryption);
}

static int dp_parser_bond(struct dp_parser *parser)
{
	struct device *dev = &parser->pdev->dev;
	int count, i;
	int rc;

	count = of_property_count_u32_elems(dev->of_node,
			"qcom,bond-dual-ctrl");
	if (count > 0) {
		if (count != 2) {
			pr_warn("dual bond ctrl num doesn't match\n");
			goto next;
		}
		for (i = 0; i < 2; i++) {
			rc = of_property_read_u32_index(dev->of_node,
				"qcom,bond-dual-ctrl", i,
				&parser->bond_cfg[DP_BOND_DUAL].ctrl[i]);
			if (rc) {
				pr_warn("failed to read bond index %d", i);
				goto next;
			}
		}
		parser->bond_cfg[DP_BOND_DUAL].enable = true;
	}

next:
	count = of_property_count_u32_elems(dev->of_node,
			"qcom,bond-tri-ctrl");
	if (count > 0) {
		if (count != 3) {
			pr_warn("tri bond ctrl num doesn't match\n");
			goto out;
		}
		for (i = 0; i < 3; i++) {
			rc = of_property_read_u32_index(dev->of_node,
				"qcom,bond-tri-ctrl", i,
				&parser->bond_cfg[DP_BOND_TRIPLE].ctrl[i]);
			if (rc) {
				pr_warn("failed to read bond index %d", i);
				goto out;
			}
		}
		parser->bond_cfg[DP_BOND_TRIPLE].enable = true;
	}

out:
	pr_debug("dual-bond:%d tri-bond:%d\n",
			parser->bond_cfg[DP_BOND_DUAL].enable,
			parser->bond_cfg[DP_BOND_TRIPLE].enable);

	return 0;
}

static u16 swap_u16_endianness(u16 in)
{
	return ((*(((char *)&in)) << 8) | (*(((char *)&in)+1)));
}

static u16 read_u16_from_byte_stream(const char *data, size_t *offset)
{
	return swap_u16_endianness((*offset = *offset + 2,
			*((u16 *)(data + *offset-2))));
}

static char read_char_from_byte_stream(const char *data, size_t *offset)
{
	return ((*offset = *offset + 1), *(data + *offset-1));
}

static u8 read_n_bits_from_byte_stream(const char *data, size_t *offset,
		u8 bit_offset, u8 num_bits)
{
	return (((bit_offset - (num_bits-1)) == 0) ?
			((*offset = *offset+1), *(data + *offset-1)
			& (REG_MASK(num_bits) << (bit_offset - (num_bits-1)))) :
			*(data + *offset)
			& (REG_MASK(num_bits) << (bit_offset - (num_bits-1)))
		) >> (bit_offset - (num_bits-1));
}

static void skip_n_bits_from_byte_stream(size_t *offset, u8 bit_offset,
		u8 num_bits)
{
	*offset = ((bit_offset - (num_bits-1)) == 0) ? (*offset+1) : *offset;
}

static void skip_n_bytes_from_byte_stream(size_t *offset, size_t skip_bytes)
{
	*offset = *offset + skip_bytes;
}

static void dp_parser_dsc_passthrough(struct dp_parser *parser)
{
	int len = 0;
	int i = 0;
	size_t parsed = 0;
	const char *data = NULL;

	struct device *dev = &parser->pdev->dev;
	struct device_node *dsc_passthrough_root_node = NULL;
	struct device_node *child_node = NULL;
	struct msm_display_dsc_info *dsc_info =
		&(parser->dsc_passthrough.comp_info.dsc_info);

	memset(&parser->dsc_passthrough, 0x0, sizeof(parser->dsc_passthrough));

	dsc_passthrough_root_node = of_get_child_by_name(dev->of_node,
			"qcom,dsc-passthrough");

	if (dsc_passthrough_root_node == NULL) {
		pr_debug("DSC Passthrough not found.\n");
		goto error;
	}

	parser->dsc_passthrough.dsc_passthrough_enable =
			of_property_read_bool(dsc_passthrough_root_node,
					"qcom,dsc-passthrough-enable");

	if (parser->dsc_passthrough.dsc_passthrough_enable) {
		data = of_get_property(dsc_passthrough_root_node,
				"qcom,dsc-out-byte-order",
				&len);
		if (!data) {
			pr_err("Error parsing dsc-out-byte-order\n");
			goto error;
		} else {
			dsc_info->out_byte_order_size = len;
			dsc_info->out_byte_order =
					devm_kzalloc(&parser->pdev->dev, (sizeof(char) * len),
						GFP_KERNEL);
			if (!dsc_info->out_byte_order) {
				pr_err("Failed to allocate buffer space for out_byte_order\n");
				goto error;
			} else {
				for (i = 0; i < len; i++)
					dsc_info->out_byte_order[i] = data[i];
				print_hex_dump(KERN_DEBUG,
						"[dp-parser] dsc passthrough:out_byte_order = ",
						DUMP_PREFIX_NONE, len, 1,
						dsc_info->out_byte_order, len, false);
			}
		}
	}

	for_each_child_of_node(dsc_passthrough_root_node, child_node) {

		data = of_get_property(child_node, "qcom,pps-values", &len);
		if (!data) {
			pr_err("Error parsing pps-values\n");
			goto error;
		}

		for (i = 0 ; i < len ; i++)
			pr_debug("PPS%d : %02x\n", i, data[i]);

		/* Byte [0] [7:4]dsc_version_major & [3:0]dsc_version_minor */
		dsc_info->version =
				read_char_from_byte_stream(data, &parsed);

		/* Byte [1] pps_identifier SKIPPED */
		skip_n_bytes_from_byte_stream(&parsed, 1);

		/* Byte [2] RESERVED SKIPPED */
		skip_n_bytes_from_byte_stream(&parsed, 1);

		/* Byte [3] [7:4]bits_per_component */
		dsc_info->bpc =
				read_n_bits_from_byte_stream(data, &parsed, 7, 4);

		/* Byte [3] [3:0]linebuf_depth */
		dsc_info->line_buf_depth =
				read_n_bits_from_byte_stream(data, &parsed, 3, 4);

		/* Byte [4] [7:6]RESERVED SKIPPED */
		skip_n_bits_from_byte_stream(&parsed, 7, 2);

		/* Byte [4] [5]block_pred_enable */
		dsc_info->block_pred_enable =
				read_n_bits_from_byte_stream(data, &parsed, 5, 1);

		/* Byte [4] [4]convert_rgb */
		dsc_info->convert_rgb =
				read_n_bits_from_byte_stream(data, &parsed, 4, 1);

		/* Byte [4] [3]enable_422 */
		dsc_info->enable_422 =
				read_n_bits_from_byte_stream(data, &parsed, 3, 1);

		/* Byte [4] [2]vbr_enable */
		dsc_info->vbr_enable =
				read_n_bits_from_byte_stream(data, &parsed, 2, 1);

		/* Bytes [4] [1:0]bits_per_pixel [5] [7:0]bits_per_pixel */
		dsc_info->bpp =
				(read_n_bits_from_byte_stream(data, &parsed, 1, 2) << 8) |
				(read_char_from_byte_stream(data, &parsed) >> 4);
				// 4 Fractional bits

		/* Bytes [6][7:0]pic_height[1] [7][7:0]pic_height[0] */
		dsc_info->pic_height =
				read_u16_from_byte_stream(data, &parsed);

		/* Bytes [8][7:0]pic_height[1] [9][7:0]pic_height[0] */
		dsc_info->pic_width =
				read_u16_from_byte_stream(data, &parsed);

		/* Bytes [10][7:0]slice_height[1] [11][7:0]slice_height[0] */
		dsc_info->slice_height =
				read_u16_from_byte_stream(data, &parsed);

		/* Bytes [12][7:0]slice_width[1] [13][7:0]slice_width[0] */
		dsc_info->slice_width =
				read_u16_from_byte_stream(data, &parsed);

		/* Bytes [14][7:0]chunk_size[1] [15][7:0]chunk_size[0] */
		dsc_info->chunk_size =
				read_u16_from_byte_stream(data, &parsed);

		/* Byte [16] [7:2]RESERVED SKIPPED */
		skip_n_bits_from_byte_stream(&parsed, 7, 6);

		/* Bytes [16] [1:0]initial_xmit_delay [17] [7:0]initial_xmit_delay */
		dsc_info->initial_xmit_delay =
				(read_n_bits_from_byte_stream(data, &parsed, 1, 2) << 8) |
				read_char_from_byte_stream(data, &parsed);

		/* Bytes [18][7:0]initial_dec_delay[1] [19][7:0]initial_dec_delay[0] */
		dsc_info->initial_dec_delay =
				read_u16_from_byte_stream(data, &parsed);

		/* Byte [20] [7:0]RESERVED SKIPPED */
		skip_n_bits_from_byte_stream(&parsed, 7, 8);

		/* Byte [21] [7:6]RESERVED SKIPPED */
		skip_n_bits_from_byte_stream(&parsed, 7, 2);

		/* Byte [21] [5:0]initial_scale_value */
		dsc_info->initial_scale_value =
				read_n_bits_from_byte_stream(data, &parsed, 5, 6);

		/*
		 * Bytes [22][7:0]scale_increment_interval[1]
		 * [23][7:0]scale_increment_interval[0]
		 */
		dsc_info->scale_increment_interval =
				read_u16_from_byte_stream(data, &parsed);

		/* Byte [24] [7:4]RESERVED SKIPPED */
		skip_n_bits_from_byte_stream(&parsed, 7, 4);

		/*
		 * Bytes [24] [3:0]scale_decrement_interval
		 *	[25] [7:0]scale_decrement_interval
		 */
		dsc_info->scale_decrement_interval =
				(read_n_bits_from_byte_stream(data, &parsed, 3, 4) << 8) |
				read_char_from_byte_stream(data, &parsed);

		/* Byte [26] [7:0]RESERVED SKIPPED */
		skip_n_bits_from_byte_stream(&parsed, 7, 8);

		/* Byte [27] [7:5]RESERVED SKIPPED */
		skip_n_bits_from_byte_stream(&parsed, 7, 3);

		/* Byte [27][4:0]first_line_bpg_offset */
		dsc_info->first_line_bpg_offset =
				read_n_bits_from_byte_stream(data, &parsed, 4, 5);

		/* Bytes [28][7:0]nfl_bpg_offset[1] [29][7:0]nfl_bpg_offset[0] */
		dsc_info->nfl_bpg_offset =
				read_u16_from_byte_stream(data, &parsed);

		/* Bytes [30][7:0]slice_bpg_offset[1] [31][7:0]slice_bpg_offset[0] */
		dsc_info->slice_bpg_offset =
				read_u16_from_byte_stream(data, &parsed);

		/* Bytes [32][7:0]initial_offset[1] [33][7:0]initial_offset[0] */
		dsc_info->initial_offset =
				read_u16_from_byte_stream(data, &parsed);

		/* Bytes [34][7:0]final_offset[1] [35][7:0]final_offset[0] */
		dsc_info->final_offset =
				read_u16_from_byte_stream(data, &parsed);

		/* Byte [36][7:5]RESERVED SKIPPED */
		skip_n_bits_from_byte_stream(&parsed, 7, 3);

		/* Byte [36][4:0]min_qp_flatness */
		dsc_info->min_qp_flatness =
				read_n_bits_from_byte_stream(data, &parsed, 4, 5);

		/* Byte [37][7:5]RESERVED SKIPPED */
		skip_n_bits_from_byte_stream(&parsed, 7, 3);

		/* Byte [37][4:0]max_qp_flatness */
		dsc_info->max_qp_flatness =
				read_n_bits_from_byte_stream(data, &parsed, 4, 5);

		/* Bytes [38][7:0]rc_model_size[1] [39][7:0]rc_model_size[0] */
		dsc_info->rc_model_size =
				read_u16_from_byte_stream(data, &parsed);

		/* Byte [40] [7:4]RESERVED SKIPPED */
		skip_n_bits_from_byte_stream(&parsed, 7, 4);

		/* Byte [40][3:0]edge_factor */
		dsc_info->edge_factor =
				read_n_bits_from_byte_stream(data, &parsed, 3, 4);

		/* Byte [41][7:5]RESERVED SKIPPED */
		skip_n_bits_from_byte_stream(&parsed, 7, 3);

		/* Byte [41][4:0]quant_incr_limit0 */
		dsc_info->quant_incr_limit0 =
				read_n_bits_from_byte_stream(data, &parsed, 4, 5);

		/* Byte [42][7:5]RESERVED SKIPPED */
		skip_n_bits_from_byte_stream(&parsed, 7, 3);

		/* Byte [42][4:0]quant_incr_limit1 */
		dsc_info->quant_incr_limit1 =
				read_n_bits_from_byte_stream(data, &parsed, 4, 5);

		/* Byte [43][7:4]tgt_offset_hi */
		dsc_info->tgt_offset_hi =
				read_n_bits_from_byte_stream(data, &parsed, 7, 4);

		/* Byte [43][3:0]tgt_offset_lo */
		dsc_info->tgt_offset_lo =
				read_n_bits_from_byte_stream(data, &parsed, 3, 4);

		/* Bytes [44 to 57] are buf_thresh[0] ... [13] */
		dsc_info->buf_thresh =
			devm_kzalloc(&parser->pdev->dev, (sizeof(u32) * 14), GFP_KERNEL);
		for (i = 0 ; i < 14 ; i++)
			dsc_info->buf_thresh[i] =
					read_char_from_byte_stream(data, &parsed);

		/*
		 * Bytes [58 to 87] are [Bytei][7:3]range_min_qp[i] [2:0]range_max_qp[i]
		 *		[Bytei+1][7:6]range_max_qp[0] [5:0]range_bpg_offset[0]
		 */
		dsc_info->range_min_qp =
			devm_kzalloc(&parser->pdev->dev, (sizeof(char) * 15), GFP_KERNEL);
		dsc_info->range_max_qp =
			devm_kzalloc(&parser->pdev->dev, (sizeof(char) * 15), GFP_KERNEL);
		dsc_info->range_bpg_offset =
			devm_kzalloc(&parser->pdev->dev, (sizeof(char) * 15), GFP_KERNEL);
		for (i = 0 ; i < 15 ; i++) {
			dsc_info->range_min_qp[i] =
					read_n_bits_from_byte_stream(data, &parsed, 7, 5);
			dsc_info->range_max_qp[i] =
					(read_n_bits_from_byte_stream(data, &parsed, 2, 3) << 2) |
					read_n_bits_from_byte_stream(data, &parsed, 7, 2);
			dsc_info->range_bpg_offset[i] =
					read_n_bits_from_byte_stream(data, &parsed, 5, 6);
		}

		/* Bytes [88 to 127] RESERVED SKIPPED */
		skip_n_bytes_from_byte_stream(&parsed, 40);
	}

	pr_debug("dsc passthrough parsing successful. Parsed = %d bytes enable:%d\n",
			parsed,
			parser->dsc_passthrough.dsc_passthrough_enable);
	pr_debug("out-byte-order-size:%d, dsc-version:%d, scr_rev:%d, pps-bits-per-component:%d\n",
			dsc_info->out_byte_order_size,
			dsc_info->version,
			dsc_info->scr_rev,
			dsc_info->bpc);
	pr_debug("pps-line-buf-depth:%d, pps-block-pred-enable:%d, convert_rgb:%d, enable-422:%d\n",
			dsc_info->line_buf_depth,
			dsc_info->block_pred_enable,
			dsc_info->convert_rgb,
			dsc_info->enable_422);
	pr_debug("vbr-enable:%d, bits-per-pixel:%d, pic-height:%d, pic-width:%d, slice-height:%d\n",
			dsc_info->vbr_enable,
			dsc_info->bpp,
			dsc_info->pic_height,
			dsc_info->pic_width,
			dsc_info->slice_height);
	pr_debug("slice-width:%d, chunk-size:%d, initial-xmit-delay:%d, initial-dec-delay:%d\n",
			dsc_info->slice_width,
			dsc_info->chunk_size,
			dsc_info->initial_xmit_delay,
			dsc_info->initial_dec_delay);
	pr_debug("initial-scale-value:%d, scale-inc-interval:%d, scale-dec-interval:%d\n",
			dsc_info->initial_scale_value,
			dsc_info->scale_increment_interval,
			dsc_info->scale_decrement_interval);
	pr_debug("first-line-bpg-offset:%d, nfl-bpg-offset:%d, slice-bpg-offset:%d\n",
			dsc_info->first_line_bpg_offset,
			dsc_info->nfl_bpg_offset,
			dsc_info->slice_bpg_offset);
	pr_debug("initial-offset:%d, final-offset=%d, flatness-min-qp:%d, flatness-max-qp:%d\n",
			dsc_info->initial_offset,
			dsc_info->final_offset,
			dsc_info->min_qp_flatness,
			dsc_info->max_qp_flatness);
	pr_debug("rc-model-size:%d, rc-edge-factor:%d, rc-quant-incr-limit0:%d\n",
			dsc_info->rc_model_size,
			dsc_info->edge_factor,
			dsc_info->quant_incr_limit0);
	pr_debug("rc-quant-incr-limit1:%d, tgt-offset-hi:%d, tgt-offset-lo:%d\n",
			dsc_info->quant_incr_limit1,
			dsc_info->tgt_offset_hi,
			dsc_info->tgt_offset_lo
			);
	print_hex_dump(KERN_DEBUG,
			"[dp-parser] dsc passthrough:pps-rc-buf-thresh = ",
			DUMP_PREFIX_NONE, 14, sizeof(u32),
			dsc_info->buf_thresh, 14*sizeof(u32), false);
	print_hex_dump(KERN_DEBUG, "[dp-parser] dsc passthrough:range_min_qp = ",
			DUMP_PREFIX_NONE, 15, 1,
			dsc_info->range_min_qp, 15, false);
	print_hex_dump(KERN_DEBUG, "[dp-parser] dsc passthrough:range_max_qp = ",
			DUMP_PREFIX_NONE, 15, 1,
			dsc_info->range_max_qp, 15, false);
	print_hex_dump(KERN_DEBUG,
			"[dp-parser] dsc passthrough:range_bpg_offset = ",
			DUMP_PREFIX_NONE, 15, 1,
			dsc_info->range_bpg_offset, 15, false);

	parser->dsc_passthrough.comp_info.comp_type = MSM_DISPLAY_COMPRESSION_DSC;
	parser->dsc_passthrough.comp_info.comp_ratio =
		MSM_DISPLAY_COMPRESSION_RATIO_3_TO_1;
	return;

error:
	parser->dsc_passthrough.dsc_passthrough_enable = false;
}

static int dp_parser_parse(struct dp_parser *parser)
{
	int rc = 0;

	if (!parser) {
		pr_err("invalid input\n");
		rc = -EINVAL;
		goto err;
	}

	rc = dp_parser_reg(parser);
	if (rc)
		goto err;

	rc = dp_parser_aux(parser);
	if (rc)
		goto err;

	rc = dp_parser_misc(parser);
	if (rc)
		goto err;

	rc = dp_parser_clock(parser);
	if (rc)
		goto err;

	rc = dp_parser_regulator(parser);
	if (rc)
		goto err;

	rc = dp_parser_gpio(parser);
	if (rc)
		goto err;

	rc = dp_parser_catalog(parser);
	if (rc)
		goto err;

	rc = dp_parser_pinctrl(parser);
	if (rc)
		goto err;

	rc = dp_parser_mst(parser);
	if (rc)
		goto err;

	rc = dp_parser_bond(parser);
	if (rc)
		goto err;

	dp_parser_dsc(parser);
	dp_parser_fec(parser);
	dp_parser_widebus(parser);
	dp_parser_force_encryption(parser);
	dp_parser_dsc_passthrough(parser);
	//parse dsc passthrough before parsing msa.
	if (parser->dsc_passthrough.dsc_passthrough_enable)
		dp_parser_msa(parser);
err:
	return rc;
}

static struct dp_io_data *dp_parser_get_io(struct dp_parser *dp_parser,
				char *name)
{
	int i = 0;
	struct dp_io *io;

	if (!dp_parser) {
		pr_err("invalid input\n");
		goto err;
	}

	io = &dp_parser->io;

	for (i = 0; i < io->len; i++) {
		struct dp_io_data *data = &io->data[i];

		if (!strcmp(data->name, name))
			return data;
	}
err:
	return NULL;
}

static void dp_parser_get_io_buf(struct dp_parser *dp_parser, char *name)
{
	int i = 0;
	struct dp_io *io;

	if (!dp_parser) {
		pr_err("invalid input\n");
		return;
	}

	io = &dp_parser->io;

	for (i = 0; i < io->len; i++) {
		struct dp_io_data *data = &io->data[i];

		if (!strcmp(data->name, name)) {
			if (!data->buf)
				data->buf = devm_kzalloc(&dp_parser->pdev->dev,
					data->io.len, GFP_KERNEL);
		}
	}
}

static void dp_parser_clear_io_buf(struct dp_parser *dp_parser)
{
	int i = 0;
	struct dp_io *io;

	if (!dp_parser) {
		pr_err("invalid input\n");
		return;
	}

	io = &dp_parser->io;

	for (i = 0; i < io->len; i++) {
		struct dp_io_data *data = &io->data[i];

		if (data->buf)
			devm_kfree(&dp_parser->pdev->dev, data->buf);

		data->buf = NULL;
	}
}

struct dp_parser *dp_parser_get(struct platform_device *pdev)
{
	struct dp_parser *parser;

	parser = devm_kzalloc(&pdev->dev, sizeof(*parser), GFP_KERNEL);
	if (!parser)
		return ERR_PTR(-ENOMEM);

	parser->parse = dp_parser_parse;
	parser->get_io = dp_parser_get_io;
	parser->get_io_buf = dp_parser_get_io_buf;
	parser->clear_io_buf = dp_parser_clear_io_buf;
	parser->pdev = pdev;

	return parser;
}

void dp_parser_put(struct dp_parser *parser)
{
	int i = 0;
	struct dss_module_power *power = NULL;

	if (!parser) {
		pr_err("invalid parser module\n");
		return;
	}

	power = parser->mp;

	for (i = 0; i < DP_MAX_PM; i++) {
		dp_parser_put_clk_data(&parser->pdev->dev, &power[i]);
		dp_parser_put_vreg_data(&parser->pdev->dev, &power[i]);
		dp_parser_put_gpio_data(&parser->pdev->dev, &power[i]);
	}

	dp_parser_clear_io_buf(parser);
	devm_kfree(&parser->pdev->dev, parser->io.data);
	devm_kfree(&parser->pdev->dev, parser);
}
