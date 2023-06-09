// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2020, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt)	"[drm:%s:%d] " fmt, __func__, __LINE__
#include "sde_reg_dma.h"
#include "sde_hw_reg_dma_v1.h"
#include "sde_dbg.h"

#define REG_DMA_VER_1_0 0x00010000
#define REG_DMA_VER_1_1 0x00010001
#define REG_DMA_VER_1_2 0x00010002

static int default_check_support(enum sde_reg_dma_features feature,
		     enum sde_reg_dma_blk blk,
		     bool *is_supported)
{

	if (!is_supported)
		return -EINVAL;

	*is_supported = false;
	return 0;
}

static int default_setup_payload(struct sde_reg_dma_setup_ops_cfg *cfg)
{
	DRM_ERROR("not implemented\n");
	return -EINVAL;
}

static int default_kick_off(struct sde_reg_dma_kickoff_cfg *cfg)
{
	DRM_ERROR("not implemented\n");
	return -EINVAL;

}

static int default_reset(struct sde_hw_ctl *ctl)
{
	DRM_ERROR("not implemented\n");
	return -EINVAL;
}

struct sde_reg_dma_buffer *default_alloc_reg_dma_buf(u32 size)
{
	DRM_ERROR("not implemented\n");
	return ERR_PTR(-EINVAL);
}

int default_dealloc_reg_dma(struct sde_reg_dma_buffer *lut_buf)
{
	DRM_ERROR("not implemented\n");
	return -EINVAL;
}

static int default_buf_reset_reg_dma(struct sde_reg_dma_buffer *lut_buf)
{
	DRM_ERROR("not implemented\n");
	return -EINVAL;
}

static int default_last_command(struct sde_hw_ctl *ctl,
		enum sde_reg_dma_queue q, enum sde_reg_dma_last_cmd_mode mode)
{
	return 0;
}

static void default_dump_reg(void)
{
}

static struct sde_hw_reg_dma reg_dma = {
	.ops = {default_check_support, default_setup_payload,
		default_kick_off, default_reset, default_alloc_reg_dma_buf,
		default_dealloc_reg_dma, default_buf_reset_reg_dma,
		default_last_command, default_dump_reg},
};

int sde_reg_dma_init(void __iomem *addr, struct sde_mdss_cfg *m,
		struct drm_device *dev)
{
	int rc = 0;

	if (!addr || !m || !dev) {
		DRM_DEBUG("invalid addr %pK catalog %pK dev %pK\n", addr, m,
				dev);
		return 0;
	}

	reg_dma.drm_dev = dev;
	reg_dma.caps = &m->dma_cfg;
	reg_dma.addr = addr;

	if (!m->reg_dma_count)
		return 0;

	switch (reg_dma.caps->version) {
	case REG_DMA_VER_1_0:
		rc = init_v1(&reg_dma);
		if (rc)
			DRM_DEBUG("init v1 dma ops failed\n");
		break;
	case REG_DMA_VER_1_1:
	case REG_DMA_VER_1_2:
		rc = init_v11(&reg_dma);
		if (rc)
			DRM_DEBUG("init v11 dma ops failed\n");
		break;
	default:
		break;
	}

	return 0;
}

struct sde_hw_reg_dma_ops *sde_reg_dma_get_ops(void)
{
	return &reg_dma.ops;
}

void sde_reg_dma_deinit(void)
{
	struct sde_hw_reg_dma op = {
	.ops = {default_check_support, default_setup_payload,
		default_kick_off, default_reset, default_alloc_reg_dma_buf,
		default_dealloc_reg_dma, default_buf_reset_reg_dma,
		default_last_command, default_dump_reg},
	};

	if (!reg_dma.drm_dev || !reg_dma.caps)
		return;

	switch (reg_dma.caps->version) {
	case REG_DMA_VER_1_0:
		deinit_v1();
		break;
	case REG_DMA_VER_1_1:
	case REG_DMA_VER_1_2:
		deinit_v1();
		break;
	default:
		break;
	}
	memset(&reg_dma, 0, sizeof(reg_dma));
	memcpy(&reg_dma.ops, &op.ops, sizeof(op.ops));
}
