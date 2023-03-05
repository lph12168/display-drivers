// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <asm/neon.h>
#include "wfd_kms_float.h"

uint32_t wfd_kms_convert_float_paramter_handler(uint32_t temp)
{
	/*
	 * Patameter 'temp' save a float value, copy the float value to 'a' and multiply
	 * it by 1000000, then cast 'a' to 'b'. Add NENO API to fix float register not
	 * be preserved during context switching.
	 *
	 * Use 1000000 because float defaults to 6 decimal places.
	 */
	float a = 0;
	uint32_t b = 0;

	kernel_neon_begin();
	preempt_disable();

	memcpy(&a, &temp, sizeof(float));
	a = a * 1000000;
	b = (uint32_t)a;

	preempt_enable();
	kernel_neon_end();

	return b;
}
