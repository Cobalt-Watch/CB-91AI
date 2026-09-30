/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The cell, read on the VDD input of the SAADC (lot D6, first piece of D5): no
 * divider since the V1 rework of 2026-09-23 (Q1, R2 and R3 gone, the cell on
 * the rail), and none on the V2. Read at rest: the rail sags some 60 mV under
 * the radio. Nothing depends on the value: the
 * watch behaves the same over the whole range of the CR2016
 * (2026-09-22); it only goes to the phone (HELLO, Battery Service, through
 * cell.h since lot D5).
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "power.h"

LOG_MODULE_REGISTER(watch_power, LOG_LEVEL_INF);

/* Channel 1 of zephyr,user: the VDD input (the board devicetree) */
static const struct adc_dt_spec vdd_adc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 1);

int power_init(void)
{
	if (!adc_is_ready_dt(&vdd_adc)) {
		return -ENODEV;
	}
	return adc_channel_setup_dt(&vdd_adc);
}

int power_vdd_mv(int32_t *mv)
{
	int16_t sample;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};
	int32_t value;
	int ret = adc_sequence_init_dt(&vdd_adc, &seq);

	if (ret == 0) {
		ret = adc_read_dt(&vdd_adc, &seq);
	}
	if (ret == 0) {
		value = sample;
		ret = adc_raw_to_millivolts_dt(&vdd_adc, &value);
	}
	if (ret == 0) {
		*mv = value;
	}
	return ret;
}
