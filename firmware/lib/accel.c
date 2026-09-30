/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bosch BMA400 accelerometer (U118) over I2C, raw registers: Zephyr 4.4 has
 * no in-tree driver for this part. Only what the bring-up needs: reset,
 * normal power mode, one range and rate, data and temperature readout.
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include "accel.h"

LOG_MODULE_REGISTER(cb91ai_accel, LOG_LEVEL_INF);

static const struct i2c_dt_spec bma400 = I2C_DT_SPEC_GET(DT_NODELABEL(bma400));

#define REG_CHIPID      0x00
#define REG_STATUS      0x03
#define REG_ACC_X_LSB   0x04 /* X, Y, Z: LSB then MSB (4 bits), 12-bit two's complement */
#define REG_TEMP_DATA   0x11 /* signed, 0.5 degC per LSB, 0 = 23 degC */
#define REG_ACC_CONFIG0 0x19 /* bits 1:0 power mode: 0 sleep, 1 low power, 2 normal */
#define REG_ACC_CONFIG1 0x1a /* bits 7:6 range (0: 2 g, 1: 4 g), 3:0 ODR (0x08: 100 Hz) */
#define REG_CMD         0x7e
#define CMD_SOFTRESET   0xb6
#define CHIPID          0x90

#define RANGE_4G        0x40
#define ODR_100HZ       0x08
#define POWER_SLEEP     0x00
#define POWER_NORMAL    0x02
#define LSB_PER_G_4G    512 /* 12 bits over +/-4 g */
#define WAKE_SETTLE_MS  40  /* sleep to normal, then a few 100 Hz samples */

int accel_init(void)
{
	uint8_t id;
	int ret;

	if (!i2c_is_ready_dt(&bma400)) {
		return -ENODEV;
	}
	ret = i2c_reg_write_byte_dt(&bma400, REG_CMD, CMD_SOFTRESET);
	if (ret) {
		return ret;
	}
	k_msleep(5);
	ret = i2c_reg_read_byte_dt(&bma400, REG_CHIPID, &id);
	if (ret) {
		return ret;
	}
	if (id != CHIPID) {
		LOG_WRN("chip id 0x%02x, expected 0x%02x", id, CHIPID);
	}
	/* The sensor stays in sleep mode (0.16 uA typ.) between readings:
	 * accel_read() wakes it for the time of one measurement. Normal mode
	 * left running costs 3.5 uA and more for nothing.
	 */
	return i2c_reg_write_byte_dt(&bma400, REG_ACC_CONFIG1, RANGE_4G | ODR_100HZ);
}

int accel_sleep(void)
{
	if (!i2c_is_ready_dt(&bma400)) {
		return -ENODEV;
	}
	return i2c_reg_write_byte_dt(&bma400, REG_ACC_CONFIG0, POWER_SLEEP);
}

static int32_t to_mg(uint8_t lsb, uint8_t msb)
{
	int32_t raw = ((msb & 0x0f) << 8) | lsb;

	if (raw & 0x800) {
		raw -= 0x1000;
	}
	return raw * 1000 / LSB_PER_G_4G;
}

int accel_read(int32_t mg[3], int32_t *temp_dc)
{
	uint8_t data[6];
	uint8_t temp;
	int ret;

	/* Wake the sensor for this reading only, then back to sleep */
	ret = i2c_reg_write_byte_dt(&bma400, REG_ACC_CONFIG0, POWER_NORMAL);
	if (ret) {
		return ret;
	}
	k_msleep(WAKE_SETTLE_MS);
	ret = i2c_burst_read_dt(&bma400, REG_ACC_X_LSB, data, sizeof(data));
	ret = ret ? ret : i2c_reg_read_byte_dt(&bma400, REG_TEMP_DATA, &temp);
	(void)accel_sleep();
	if (ret) {
		return ret;
	}
	for (int axis = 0; axis < 3; axis++) {
		mg[axis] = to_mg(data[2 * axis], data[2 * axis + 1]);
	}
	*temp_dc = 230 + (int32_t)(int8_t)temp * 5;
	return 0;
}

int accel_test(void)
{
	int32_t mg[3], temp_dc;
	int ret = accel_init();

	if (ret) {
		LOG_ERR("init failed (%d)", ret);
		return ret;
	}
	for (int i = 0; i < 3; i++) {
		ret = accel_read(mg, &temp_dc);
		if (ret) {
			LOG_ERR("read failed (%d)", ret);
			return ret;
		}
		int32_t norm_sq = mg[0] * mg[0] + mg[1] * mg[1] + mg[2] * mg[2];

		LOG_INF("accel: x %d y %d z %d mg, norm^2 %d (1 g = 1000000), temp %d.%d degC",
			mg[0], mg[1], mg[2], norm_sq, temp_dc / 10, abs(temp_dc % 10));
		k_msleep(100);
	}
	return 0;
}

/* ---- The wrist (lot D4): registers from the datasheet, rev. 2.3 ----------- */

#define REG_INT_STAT0        0x0e /* p. 57: bit 1 orientation changed */
#define REG_ACC_CONFIG2      0x1b /* p. 65: bits 3:2 source of the data registers */
#define REG_INT_CONFIG0      0x1f /* p. 65: bit 1 orientation change enabled */
#define REG_INT_CONFIG1      0x20 /* p. 66: bit 7 latched, bit 3 double tap enabled */
#define REG_INT1_MAP         0x21 /* p. 66: bit 1 orientation change on INT1 */
#define REG_INT2_MAP         0x22
#define REG_INT12_MAP        0x23 /* p. 67: bit 6 tap on INT2 */
#define REG_INT12_IO_CTRL    0x24 /* p. 68: 0x22, both push-pull and high-active */
#define REG_AUTOLOWPOW_0     0x2a /* p. 71: timeout bits 11:4, 2.5 ms steps */
#define REG_AUTOLOWPOW_1     0x2b /* p. 72: timeout bits 3:0 in 7:4; 3:2 = 01, timeout on */
#define REG_AUTOWAKEUP_1     0x2d /* p. 73: bit 1 wake-up interrupt wakes the sensor */
#define REG_WKUP_INT_CONFIG0 0x2f /* p. 74: 7:5 z y x on, 4:2 samples - 1, 1:0 reference update */
#define REG_WKUP_INT_CONFIG1 0x30 /* p. 75: threshold, 2^(2 + range) / 256 g per LSB */
#define REG_ORIENTCH_CONFIG0 0x35 /* p. 77: 7 z on, 4 data source, 3:2 reference update */
#define REG_ORIENTCH_CONFIG1 0x36 /* p. 78: threshold, 8 mg per LSB */
#define REG_ORIENTCH_CONFIG3 0x38 /* p. 78: duration, samples at 100 Hz */
#define REG_ORIENTCH_REF     0x39 /* p. 79: x 7:0, x 11:8, y 7:0, y 11:8, z 7:0, z 11:8 */
#define REG_TAP_CONFIG       0x57 /* p. 95: 4:3 axis (00 Z), 2:0 sensitivity */
#define REG_TAP_CONFIG1      0x58 /* p. 96: 0x06, its reset value: quiet 80, tics 12 */

#define ODR_200HZ            0x09 /* the tap needs 200 Hz on acc_filt1 (p. 38, p. 43) */
#define POWER_LOW            0x01 /* bits 6:5 osr_lp: 0.85 to 1.35 uA (p. 19, 63) */
#define NORMAL_SETTLE_MS     50   /* sleep to normal (2/ODR, 10 ms at 200 Hz, p. 9), then
                                   * the filters */

/* The last orientation reference written, as the data registers give it */
static uint8_t orient_ref[6];

static int write_orient_ref(const uint8_t data[6])
{
	/* The data registers and the reference share their layout: 7:0, then
	 * 11:8 in the low nibble, for x, y and z (p. 79) */
	for (int i = 0; i < 6; i++) {
		const uint8_t v = (i & 1) ? (uint8_t)(data[i] & 0x0f) : data[i];
		const int ret = i2c_reg_write_byte_dt(&bma400, REG_ORIENTCH_REF + i, v);

		if (ret) {
			return ret;
		}
	}
	memcpy(orient_ref, data, sizeof(orient_ref));
	return 0;
}

static int write_regs(const uint8_t (*regs)[2], size_t n)
{
	for (size_t i = 0; i < n; i++) {
		const int ret = i2c_reg_write_byte_dt(&bma400, regs[i][0], regs[i][1]);

		if (ret) {
			return ret;
		}
	}
	return 0;
}

int accel_wrist_start(const struct accel_wrist *w)
{
	const uint32_t lp = MIN(MAX((uint32_t)w->lowpower_ms * 2U / 5U, 1U), 4095U);
	const uint8_t samples = (uint8_t)(MIN(MAX(w->wake_samples, 1U), 8U) - 1U);
	const uint8_t wake_ref = w->wake_ref == ACCEL_WAKE_REF_ONCE ? 0x01 : 0x02;
	const uint8_t orient_ref_mode = (uint8_t)MIN(w->orient_ref, 2U);
	const uint8_t osr_lp = (uint8_t)MIN(w->osr_lp, 3U);
	/* p. 77: 7 z on, 6 y on, 4 data source (1: acc_filt_lp), 3:2 reference update */
	const uint8_t orientch = (uint8_t)(((w->flags & ACCEL_WRIST_ORIENT_Z_ONLY) ? 0x80 : 0xc0) |
					   ((w->flags & ACCEL_WRIST_ORIENT_LP) ? 0x10 : 0) |
					   (orient_ref_mode << 2));
	/* p. 66: bit 1 orientation change, bit 0 wake-up */
	const uint8_t int1 = (uint8_t)(0x02 | ((w->flags & ACCEL_WRIST_WAKE_INT1) ? 0x01 : 0));
	uint8_t data[6];
	uint8_t id;
	int ret;

	if (!i2c_is_ready_dt(&bma400)) {
		return -ENODEV;
	}
	/* From its reset state: every interrupt off while it is configured */
	ret = i2c_reg_write_byte_dt(&bma400, REG_CMD, CMD_SOFTRESET);
	if (ret) {
		return ret;
	}
	k_msleep(5);
	ret = i2c_reg_read_byte_dt(&bma400, REG_CHIPID, &id);
	if (ret) {
		return ret;
	}
	if (id != CHIPID) {
		LOG_WRN("chip id 0x%02x, expected 0x%02x", id, CHIPID);
	}
	{
		const uint8_t setup[][2] = {
			{ REG_ACC_CONFIG1, RANGE_4G | ODR_200HZ }, /* osr 0: 3.5 uA in normal mode */
			{ REG_ACC_CONFIG2, 0x00 },                 /* the data registers: acc_filt1 */
			{ REG_INT12_IO_CTRL, 0x22 },
			{ REG_ACC_CONFIG0, POWER_NORMAL },         /* for the reference below */
		};

		ret = write_regs(setup, ARRAY_SIZE(setup));
	}
	if (ret) {
		return ret;
	}
	{
		const uint8_t setup[][2] = {
			{ REG_TAP_CONFIG, (uint8_t)(MIN(w->tap_sensitivity, 7U)) }, /* Z: taps on the glass */
			{ REG_TAP_CONFIG1, 0x06 },
			{ REG_ORIENTCH_CONFIG0, orientch },
			{ REG_ORIENTCH_CONFIG1, w->orient_thres },
			{ REG_ORIENTCH_CONFIG3, w->orient_dur },
			/* p. 74: 7:5 z y x on, 4:2 samples - 1, 1:0 reference update */
			{ REG_WKUP_INT_CONFIG0, (uint8_t)(0xe0 | (samples << 2) | wake_ref) },
			{ REG_WKUP_INT_CONFIG1, w->wake_thres },
			{ REG_AUTOWAKEUP_1, 0x02 },
			{ REG_AUTOLOWPOW_0, (uint8_t)(lp >> 4) },
			{ REG_AUTOLOWPOW_1, (uint8_t)(((lp & 0x0f) << 4) | 0x04) },
			{ REG_INT1_MAP, int1 },
			{ REG_INT2_MAP, 0x00 },
			{ REG_INT12_MAP, 0x40 },
			{ REG_INT_CONFIG0, 0x02 },
			{ REG_INT_CONFIG1, 0x88 },
		};

		ret = write_regs(setup, ARRAY_SIZE(setup));
	}
	if (ret) {
		return ret;
	}
	/* The orientation now is the reference, so that the next change is a
	 * gesture. Written after ORIENTCH_CONFIG0, as Bosch's own driver does in
	 * one burst: written before it, the reference read 0 (26/09) */
	k_msleep(NORMAL_SETTLE_MS);
	ret = i2c_burst_read_dt(&bma400, REG_ACC_X_LSB, data, sizeof(data));
	ret = ret ? ret : write_orient_ref(data);
	/* and it wakes itself on motion */
	return ret ? ret
		   : i2c_reg_write_byte_dt(&bma400, REG_ACC_CONFIG0,
					   (uint8_t)((osr_lp << 5) | POWER_LOW));
}

int accel_orient_ref_now(int32_t mg[3])
{
	uint8_t data[6];
	int ret = i2c_burst_read_dt(&bma400, REG_ACC_X_LSB, data, sizeof(data));

	ret = ret ? ret : write_orient_ref(data);
	if (ret) {
		return ret;
	}
	for (int axis = 0; axis < 3; axis++) {
		mg[axis] = to_mg(data[2 * axis], data[2 * axis + 1]);
	}
	return 0;
}

int accel_orient_ref_again(void)
{
	uint8_t data[6];

	memcpy(data, orient_ref, sizeof(data));
	return write_orient_ref(data);
}

void accel_orient_ref_mg(int32_t mg[3])
{
	for (int axis = 0; axis < 3; axis++) {
		mg[axis] = to_mg(orient_ref[2 * axis], orient_ref[2 * axis + 1]);
	}
}

int accel_dump(uint8_t out[ACCEL_DUMP_SIZE])
{
	/* Clear of the status registers 0x0E to 0x10, which a read releases,
	 * and of the FIFO data, which a read consumes */
	int ret = i2c_burst_read_dt(&bma400, 0x00, &out[0], 13);

	ret = ret ? ret : i2c_burst_read_dt(&bma400, REG_ACC_CONFIG0, &out[13], 38);
	return ret ? ret : i2c_burst_read_dt(&bma400, REG_TAP_CONFIG, &out[51], 2);
}

int accel_wrist_status(uint8_t *stat0, uint8_t *stat1)
{
	uint8_t stat[2];
	const int ret = i2c_burst_read_dt(&bma400, REG_INT_STAT0, stat, sizeof(stat));

	if (ret == 0) {
		*stat0 = stat[0];
		*stat1 = stat[1];
	}
	return ret;
}

int accel_now(int32_t mg[3])
{
	uint8_t data[6];
	const int ret = i2c_burst_read_dt(&bma400, REG_ACC_X_LSB, data, sizeof(data));

	if (ret) {
		return ret;
	}
	for (int axis = 0; axis < 3; axis++) {
		mg[axis] = to_mg(data[2 * axis], data[2 * axis + 1]);
	}
	return 0;
}
