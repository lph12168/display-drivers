// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 */

/*
 * Display Port PLL driver block diagram for branch clocks
 *
 *		+------------------------------+
 *		|         eDP_VCO_CLK          |
 *		|                              |
 *		|    +-------------------+     |
 *		|    |   (eDP PLL/VCO)   |     |
 *		|    +---------+---------+     |
 *		|              v               |
 *		|   +----------+-----------+   |
 *		|   | hsclk_divsel_clk_src |   |
 *		|   +----------+-----------+   |
 *		+------------------------------+
 *				|
 *	 +------------<---------v------------>----------+
 *	 |                                              |
 * +-----v------------+                                 |
 * | edp_link_clk_src |                                 |
 * |    divsel_ten    |                                 |
 * +---------+--------+                                 |
 *	|                                               |
 *	|                                               |
 *	v                                               v
 * Input to DISPCC block                                |
 * for link clk, crypto clk                             |
 * and interface clock                                  |
 *							|
 *							|
 *	+--------<------------+-----------------+---<---+
 *	|                     |                 |
 * +-------v------+  +--------v-----+  +--------v------+
 * | vco_divided  |  | vco_divided  |  | vco_divided   |
 * |    _clk_src  |  |    _clk_src  |  |    _clk_src   |
 * |              |  |              |  |               |
 * |divsel_six    |  |  divsel_two  |  |  divsel_four  |
 * +-------+------+  +-----+--------+  +--------+------+
 *         |	           |		        |
 *	v------->----------v-------------<------v
 *                         |
 *		+----------+---------+
 *		|   vco_divided_clk  |
 *		|       _src_mux     |
 *		+---------+----------+
 *                        |
 *                        v
 *              Input to DISPCC block
 *              for eDP pixel clock
 *
 */

#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <dt-bindings/clock/mdss-10nm-pll-clk.h>

#include "pll_drv.h"
#include "dp_pll.h"
#include "edp_pll_7nm.h"

enum {
	DP_PHY_INDEX_DP_ONLY	= 0,
	DP_PHY_INDEX_EDP,
	DP_PHY_INDEX_MAX,
};

static struct edp_pll_db_7nm edp_pdb_7nm[DP_PHY_INDEX_MAX];
static struct clk_ops mux_clk_ops;

static struct regmap_config edp_pll_7nm_cfg = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register = 0x910,
};

static struct regmap_bus edp_pixel_mux_regmap_ops = {
	.reg_write = edp_mux_set_parent_7nm,
	.reg_read = edp_mux_get_parent_7nm,
};

/* Op structures */
static const struct clk_ops edp_7nm_vco_clk_ops = {
	.recalc_rate = edp_vco_recalc_rate_7nm,
	.set_rate = edp_vco_set_rate_7nm,
	.round_rate = edp_vco_round_rate_7nm,
	.prepare = edp_vco_prepare_7nm,
	.unprepare = edp_vco_unprepare_7nm,
};

static struct dp_pll_vco_clk edp_vco_clk = {
	.min_rate = EDP_VCO_HSCLK_RATE_1620MHZDIV1000,
	.max_rate = EDP_VCO_HSCLK_RATE_8100MHZDIV1000,
	.hw.init = &(struct clk_init_data){
		.name = "edp_vco_clk",
		.parent_names = (const char *[]){ "xo_board" },
		.num_parents = 1,
		.ops = &edp_7nm_vco_clk_ops,
	},
};

static struct clk_fixed_factor edp_phy_pll_link_clk = {
	.div = 10,
	.mult = 1,

	.hw.init = &(struct clk_init_data){
		.name = "edp_phy_pll_link_clk",
		.parent_names =
			(const char *[]){ "edp_vco_clk" },
		.num_parents = 1,
		.flags = (CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT),
		.ops = &clk_fixed_factor_ops,
	},
};

static struct clk_fixed_factor edp_vco_divsel_two_clk_src = {
	.div = 2,
	.mult = 1,

	.hw.init = &(struct clk_init_data){
		.name = "edp_vco_divsel_two_clk_src",
		.parent_names =
			(const char *[]){ "edp_vco_clk" },
		.num_parents = 1,
		.flags = (CLK_GET_RATE_NOCACHE),
		.ops = &clk_fixed_factor_ops,
	},
};

static struct clk_fixed_factor edp_vco_divsel_four_clk_src = {
	.div = 4,
	.mult = 1,

	.hw.init = &(struct clk_init_data){
		.name = "edp_vco_divsel_four_clk_src",
		.parent_names =
			(const char *[]){ "edp_vco_clk" },
		.num_parents = 1,
		.flags = (CLK_GET_RATE_NOCACHE),
		.ops = &clk_fixed_factor_ops,
	},
};

static struct clk_fixed_factor edp_vco_divsel_six_clk_src = {
	.div = 6,
	.mult = 1,

	.hw.init = &(struct clk_init_data){
		.name = "edp_vco_divsel_six_clk_src",
		.parent_names =
			(const char *[]){ "edp_vco_clk" },
		.num_parents = 1,
		.flags = (CLK_GET_RATE_NOCACHE),
		.ops = &clk_fixed_factor_ops,
	},
};

static struct dp_pll_vco_clk dp2_vco_clk = {
	.min_rate = EDP_VCO_HSCLK_RATE_1620MHZDIV1000,
	.max_rate = EDP_VCO_HSCLK_RATE_8100MHZDIV1000,
	.hw.init = &(struct clk_init_data){
		.name = "dp2_vco_clk",
		.parent_names = (const char *[]){ "xo_board" },
		.num_parents = 1,
		.ops = &edp_7nm_vco_clk_ops,
	},
};

static struct clk_fixed_factor dptx2_phy_pll_link_clk = {
	.div = 10,
	.mult = 1,

	.hw.init = &(struct clk_init_data){
		.name = "dptx2_phy_pll_link_clk",
		.parent_names =
			(const char *[]){ "dp2_vco_clk" },
		.num_parents = 1,
		.flags = (CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT),
		.ops = &clk_fixed_factor_ops,
	},
};

static struct clk_fixed_factor dp2_vco_divsel_two_clk_src = {
	.div = 2,
	.mult = 1,

	.hw.init = &(struct clk_init_data){
		.name = "dp2_vco_divsel_two_clk_src",
		.parent_names =
			(const char *[]){ "dp2_vco_clk" },
		.num_parents = 1,
		.flags = (CLK_GET_RATE_NOCACHE),
		.ops = &clk_fixed_factor_ops,
	},
};

static struct clk_fixed_factor dp2_vco_divsel_four_clk_src = {
	.div = 4,
	.mult = 1,

	.hw.init = &(struct clk_init_data){
		.name = "dp2_vco_divsel_four_clk_src",
		.parent_names =
			(const char *[]){ "dp2_vco_clk" },
		.num_parents = 1,
		.flags = (CLK_GET_RATE_NOCACHE),
		.ops = &clk_fixed_factor_ops,
	},
};

static struct clk_fixed_factor dp2_vco_divsel_six_clk_src = {
	.div = 6,
	.mult = 1,

	.hw.init = &(struct clk_init_data){
		.name = "dp2_vco_divsel_six_clk_src",
		.parent_names =
			(const char *[]){ "dp2_vco_clk" },
		.num_parents = 1,
		.flags = (CLK_GET_RATE_NOCACHE),
		.ops = &clk_fixed_factor_ops,
	},
};


static int clk_mux_determine_rate(struct clk_hw *hw,
				  struct clk_rate_request *req)
{
	int ret = 0;

	if (!hw || !req) {
		pr_err("Invalid input parameters\n");
		return -EINVAL;
	}

	ret = __clk_mux_determine_rate_closest(hw, req);
	if (ret)
		return ret;

	/* Set the new parent of mux if there is a new valid parent */
	if (hw->clk && req->best_parent_hw->clk)
		clk_set_parent(hw->clk, req->best_parent_hw->clk);

	return 0;
}

static unsigned long mux_recalc_rate(struct clk_hw *hw,
					unsigned long parent_rate)
{
	struct clk *div_clk = NULL, *vco_clk = NULL;
	struct dp_pll_vco_clk *vco = NULL;

	if (!hw) {
		pr_err("Invalid input parameter\n");
		return 0;
	}

	div_clk = clk_get_parent(hw->clk);
	if (!div_clk)
		return 0;

	vco_clk = clk_get_parent(div_clk);
	if (!vco_clk)
		return 0;

	vco = to_dp_vco_hw(__clk_get_hw(vco_clk));
	if (!vco)
		return 0;

	if (vco->rate == EDP_VCO_HSCLK_RATE_8100MHZDIV1000)
		return (vco->rate / 6);
	else if (vco->rate == EDP_VCO_HSCLK_RATE_5400MHZDIV1000)
		return (vco->rate / 4);
	else
		return (vco->rate / 2);
}

static struct clk_regmap_mux edp_phy_pll_vco_div_clk = {
	.reg = 0x64,
	.shift = 0,
	.width = 2,

	.clkr = {
		.hw.init = &(struct clk_init_data){
			.name = "edp_phy_pll_vco_div_clk",
			.parent_names =
				(const char *[]){"edp_vco_divsel_two_clk_src",
					"edp_vco_divsel_four_clk_src",
					"edp_vco_divsel_six_clk_src"},
			.num_parents = 3,
			.ops = &mux_clk_ops,
			.flags = (CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT),
		},
	},
};

static struct clk_regmap_mux dptx2_phy_pll_vco_div_clk = {
	.reg = 0x64,
	.shift = 0,
	.width = 2,

	.clkr = {
		.hw.init = &(struct clk_init_data){
			.name = "dptx2_phy_pll_vco_div_clk",
			.parent_names =
				(const char *[]){"dp2_vco_divsel_two_clk_src",
					"dp2_vco_divsel_four_clk_src",
					"dp2_vco_divsel_six_clk_src"},
			.num_parents = 3,
			.ops = &mux_clk_ops,
			.flags = (CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT),
		},
	},
};

static struct clk_hw *mdss_dp_pllcc_7nm
	[DP_PHY_INDEX_MAX][DP_VCO_DIVIDED_CLK_SRC_MUX + 1] = {
	{
		[DP_VCO_CLK] = &dp2_vco_clk.hw,
		[DP_LINK_CLK_DIVSEL_TEN] = &dptx2_phy_pll_link_clk.hw,
		[DP_VCO_DIVIDED_TWO_CLK_SRC] = &dp2_vco_divsel_two_clk_src.hw,
		[DP_VCO_DIVIDED_FOUR_CLK_SRC] = &dp2_vco_divsel_four_clk_src.hw,
		[DP_VCO_DIVIDED_SIX_CLK_SRC] = &dp2_vco_divsel_six_clk_src.hw,
		[DP_VCO_DIVIDED_CLK_SRC_MUX] =
				&dptx2_phy_pll_vco_div_clk.clkr.hw,
	},
	{
		[DP_VCO_CLK] = &edp_vco_clk.hw,
		[DP_LINK_CLK_DIVSEL_TEN] = &edp_phy_pll_link_clk.hw,
		[DP_VCO_DIVIDED_TWO_CLK_SRC] = &edp_vco_divsel_two_clk_src.hw,
		[DP_VCO_DIVIDED_FOUR_CLK_SRC] = &edp_vco_divsel_four_clk_src.hw,
		[DP_VCO_DIVIDED_SIX_CLK_SRC] = &edp_vco_divsel_six_clk_src.hw,
		[DP_VCO_DIVIDED_CLK_SRC_MUX] =
				&edp_phy_pll_vco_div_clk.clkr.hw,
	},
};

int edp_pll_clock_register_7nm(struct platform_device *pdev,
				 struct mdss_pll_resources *pll_res)
{
	int rc = -ENOTSUPP, i = 0;
	struct clk_onecell_data *clk_data;
	struct clk *clk = NULL;
	struct regmap *regmap;
	int num_clks = ARRAY_SIZE(mdss_dp_pllcc_7nm[0]);

	if (!pdev || !pdev->dev.of_node) {
		pr_err("Invalid input parameters\n");
		return -EINVAL;
	}

	if (!pll_res || !pll_res->pll_base || !pll_res->phy_base ||
		!pll_res->ln_tx0_base || !pll_res->ln_tx1_base) {
		pr_err("Invalid input parameters\n");
		return -EINVAL;
	}

	if (pll_res->index < 0 || pll_res->index >= DP_PHY_INDEX_MAX) {
		pr_err("Invalid cell_index parameter\n");
		return -EINVAL;
	}

	clk_data = devm_kzalloc(&pdev->dev, sizeof(struct clk_onecell_data),
					GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;

	clk_data->clks = devm_kzalloc(&pdev->dev, (num_clks *
				sizeof(struct clk *)), GFP_KERNEL);
	if (!clk_data->clks) {
		devm_kfree(&pdev->dev, clk_data);
		return -ENOMEM;
	}
	clk_data->clk_num = num_clks;

	pll_res->priv = &edp_pdb_7nm[pll_res->index];
	edp_pdb_7nm[pll_res->index].pll = pll_res;

	/* Set client data for vco, mux and div clocks */
	regmap = devm_regmap_init(&pdev->dev, &edp_pixel_mux_regmap_ops,
			pll_res, &edp_pll_7nm_cfg);
	switch (pll_res->index) {
	case DP_PHY_INDEX_DP_ONLY:
		dptx2_phy_pll_vco_div_clk.clkr.regmap = regmap;
		dp2_vco_clk.priv = pll_res;
		break;
	case DP_PHY_INDEX_EDP:
		edp_phy_pll_vco_div_clk.clkr.regmap = regmap;
		edp_vco_clk.priv = pll_res;
		break;
	}
	mux_clk_ops = clk_regmap_mux_closest_ops;
	mux_clk_ops.determine_rate = clk_mux_determine_rate;
	mux_clk_ops.recalc_rate = mux_recalc_rate;

	for (i = DP_VCO_CLK; i <= DP_VCO_DIVIDED_CLK_SRC_MUX; i++) {
		pr_debug("reg clk: %d index: %d\n", i, pll_res->index);
		clk = devm_clk_register(&pdev->dev,
			mdss_dp_pllcc_7nm[pll_res->index][i]);
		if (IS_ERR(clk)) {
			pr_err("clk registration failed for eDP: %d\n",
					pll_res->index);
			rc = -EINVAL;
			goto clk_reg_fail;
		}
		clk_data->clks[i] = clk;
	}

	rc = of_clk_add_provider(pdev->dev.of_node,
			of_clk_src_onecell_get, clk_data);
	if (rc) {
		pr_err("Clock register failed rc=%d\n", rc);
		rc = -EPROBE_DEFER;
		goto clk_reg_fail;
	} else {
		pr_err("SUCCESS\n");
	}
	return rc;
clk_reg_fail:
	devm_kfree(&pdev->dev, clk_data->clks);
	devm_kfree(&pdev->dev, clk_data);
	return rc;
}
