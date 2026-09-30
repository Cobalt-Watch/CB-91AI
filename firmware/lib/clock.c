/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wall clock of the watch (EF-01 to EF-03).
 *
 * Time base: the Zephyr uptime, that is RTC1 on the 32.768 kHz crystal (X4). On
 * the V1 board that crystal runs fast: 12.5 pF crystal with 10 pF load
 * capacitors (V2-02). Nothing can trim it in hardware, so the error is taken
 * out in software: the clock is an anchor (UTC time, uptime) plus the uptime
 * elapsed since, scaled by (1 - ppb / 1e9).
 *
 * The correction comes from three places, each one overriding the previous:
 * - CONFIG_CB91AI_CLOCK_DEFAULT_PPB, measured on a V1 board with
 *   firmware/tools/lfxo_drift.py: every V1 board shares the same capacitors,
 *   the crystals differ by +/-20 ppm;
 * - the value stored in the settings;
 * - self-calibration: each time a host sets the time (EI-03), the uptime
 *   elapsed since the previous setting is compared with the time elapsed on
 *   the clock of the host. A phone follows network time, so a few settings
 *   spread over days give the error of this very crystal within 1 ppm,
 *   temperature and ageing included, with no instrument.
 *
 * This correction is for the displayed time only. The Bluetooth controller
 * times its events on the raw crystal and cannot be trimmed: it is told a
 * wide enough sleep clock accuracy instead (250 ppm, EF-39).
 *
 * The time survives a soft reset or a firmware update in a few bytes of
 * retained RAM at a fixed address (board devicetree), marked approximate until
 * the next setting: the length of the reset is not known (a second, or a
 * minute for an MCUboot swap). A watchdog bite or a pin reset may corrupt that
 * RAM (nRF52840 PS v1.11, reset behavior, p. 90): its prefix and CRC then
 * refuse it, and the time is lost as with a new cell.
 */

#include <stdio.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/retention/retention.h>
#include <zephyr/settings/settings.h>

#include "clock.h"

LOG_MODULE_REGISTER(cb91ai_clock, LOG_LEVEL_INF);

#define PPB_LIMIT         1000000             /* 1000 ppm: beyond that, not a crystal error */
#define CAL_MIN_MS        (3600LL * 1000)     /* shortest interval worth a measurement */
#define CAL_FULL_MS       (12 * 3600LL * 1000) /* interval trusted entirely */
#define SETTING_PPB       "clock/ppb"
#define SETTING_24H       "clock/24h"

static const struct device *const retained = DEVICE_DT_GET(DT_NODELABEL(clock_retention));

struct retained_time {
	int64_t unix_ms;
	int16_t tz_minutes;
	uint8_t flags;
	uint8_t reserved;
} __packed;

static struct k_spinlock lock;
static int64_t anchor_unix_ms;
static int64_t anchor_uptime_ms;
static int16_t tz_minutes;
static uint8_t flags;
static int32_t ppb = CONFIG_CB91AI_CLOCK_DEFAULT_PPB;

/* Previous time setting by a host, for the self-calibration */
static int64_t sync_unix_ms;
static int64_t sync_uptime_ms;
static bool sync_valid;

static atomic_t ppb_dirty;

static bool hour24 = IS_ENABLED(CONFIG_CB91AI_CLOCK_24H);
static atomic_t format_dirty;

static int64_t corrected(int64_t raw_ms)
{
	return raw_ms - (raw_ms * ppb) / 1000000000LL;
}

static int64_t now_locked(int64_t uptime_ms)
{
	return anchor_unix_ms + corrected(uptime_ms - anchor_uptime_ms);
}

bool clock_is_set(void)
{
	return (flags & CLOCK_FLAG_SET) != 0;
}

uint8_t clock_flags(void)
{
	return flags;
}

int64_t clock_now_ms(void)
{
	int64_t uptime = k_uptime_get();
	k_spinlock_key_t key = k_spin_lock(&lock);
	int64_t now = (flags & CLOCK_FLAG_SET) ? now_locked(uptime) : 0;

	k_spin_unlock(&lock, key);
	return now;
}

int16_t clock_tz_minutes(void)
{
	return tz_minutes;
}

bool clock_local_tm(struct tm *out)
{
	int64_t now = clock_now_ms();
	time_t seconds;

	if (now == 0) {
		return false;
	}
	seconds = (time_t)(now / 1000) + (time_t)tz_minutes * 60;
	return gmtime_r(&seconds, out) != NULL;
}

bool clock_24h(void)
{
	return hour24;
}

void clock_set_24h(bool enable)
{
	hour24 = enable;
	atomic_set(&format_dirty, 1);
}

bool clock_display(char digits[7], bool *pm, struct tm *tm)
{
	int hour;

	if (!clock_local_tm(tm)) {
		return false;
	}
	hour = tm->tm_hour;
	*pm = !hour24 && hour >= 12;
	if (!hour24) {
		hour %= 12;
		if (hour == 0) {
			hour = 12;
		}
	}
	/* No leading zero on the hours, as the F-91W; on the V1 board the hour
	 * tens can show nothing but "1" anyway (see CONFIG_CB91AI_CLOCK_24H). */
	snprintf(digits, 7, "%2u%02u%02u", (unsigned int)hour % 100U,
		 (unsigned int)tm->tm_min % 60U, (unsigned int)tm->tm_sec % 60U);
	return true;
}

int32_t clock_ppb(void)
{
	return ppb;
}

int clock_set_ppb(int32_t new_ppb)
{
	int64_t uptime = k_uptime_get();
	k_spinlock_key_t key;

	if (abs(new_ppb) > PPB_LIMIT) {
		return -EINVAL;
	}
	key = k_spin_lock(&lock);
	/* New rate from now on: the time already counted does not move */
	if (flags & CLOCK_FLAG_SET) {
		anchor_unix_ms = now_locked(uptime);
		anchor_uptime_ms = uptime;
	}
	ppb = new_ppb;
	k_spin_unlock(&lock, key);
	atomic_set(&ppb_dirty, 1);
	return 0;
}

void clock_set(int64_t unix_ms, int16_t tz, bool calibrate)
{
	int64_t uptime = k_uptime_get();
	int64_t measured = 0, true_ms = 0;
	int32_t old_ppb, new_ppb;
	bool measured_ok = false;
	k_spinlock_key_t key = k_spin_lock(&lock);

	old_ppb = ppb;
	new_ppb = ppb;
	if (calibrate && sync_valid) {
		true_ms = unix_ms - sync_unix_ms;
		if (true_ms >= CAL_MIN_MS) {
			int64_t raw_ms = uptime - sync_uptime_ms;

			measured = (raw_ms - true_ms) * 1000000000LL / true_ms;
			if (llabs(measured) <= PPB_LIMIT) {
				/* The host writes with some tens of ms of latency: a short
				 * interval only nudges the correction, 12 h replace it. */
				int64_t weight_ms = MIN(true_ms, CAL_FULL_MS);

				new_ppb = (int32_t)(ppb + (measured - ppb) * weight_ms / CAL_FULL_MS);
				measured_ok = true;
			}
		}
	}
	if (!calibrate) {
		sync_valid = false; /* a time typed by hand is no reference */
	} else if (!sync_valid || true_ms >= CAL_MIN_MS || true_ms < 0) {
		/* Shorter intervals keep the earlier reference, so that frequent
		 * connections still add up to a long baseline. */
		sync_unix_ms = unix_ms;
		sync_uptime_ms = uptime;
		sync_valid = true;
	}
	ppb = new_ppb;
	anchor_unix_ms = unix_ms;
	anchor_uptime_ms = uptime;
	tz_minutes = tz;
	flags |= CLOCK_FLAG_SET;
	flags &= ~CLOCK_FLAG_APPROXIMATE;
	if (measured_ok) {
		flags |= CLOCK_FLAG_CALIBRATED;
	}
	k_spin_unlock(&lock, key);

	if (measured_ok) {
		atomic_set(&ppb_dirty, 1);
		LOG_INF("crystal measured at %d ppb over %d min: correction %d -> %d ppb",
			(int)measured, (int)(true_ms / 60000), old_ppb, new_ppb);
	}
	LOG_INF("time set, UTC%+d min", tz);
}

void clock_retain(void)
{
	if (flags & CLOCK_FLAG_SET) {
		struct retained_time keep = {
			.unix_ms = clock_now_ms(),
			.tz_minutes = tz_minutes,
			.flags = flags,
		};

		(void)retention_write(retained, 0, (const uint8_t *)&keep, sizeof(keep));
	}
}

void clock_process(void)
{
	clock_retain();
	if (atomic_cas(&ppb_dirty, 1, 0)) {
		int32_t value = ppb;
		int ret = settings_save_one(SETTING_PPB, &value, sizeof(value));

		LOG_INF("clock correction %d ppb %s (%d)", value, ret ? "not stored" : "stored", ret);
	}
	if (atomic_cas(&format_dirty, 1, 0)) {
		uint8_t value = hour24;
		int ret = settings_save_one(SETTING_24H, &value, sizeof(value));

		LOG_INF("%s-hour display %s (%d)", value ? "24" : "12", ret ? "not stored" : "stored", ret);
	}
}

static int clock_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	int32_t value;
	uint8_t format;

	if (settings_name_steq(name, "ppb", NULL) && len == sizeof(value) &&
	    read_cb(cb_arg, &value, sizeof(value)) == sizeof(value) && abs(value) <= PPB_LIMIT) {
		ppb = value;
		flags |= CLOCK_FLAG_CALIBRATED;
	} else if (settings_name_steq(name, "24h", NULL) && len == sizeof(format) &&
		   read_cb(cb_arg, &format, sizeof(format)) == sizeof(format)) {
		hour24 = format != 0;
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(cb91ai_clock, "clock", NULL, clock_settings_set, NULL, NULL);

int clock_init(void)
{
	struct retained_time keep;
	int ret = settings_subsys_init();

	if (ret == 0) {
		ret = settings_load_subtree("clock");
	}
	if (ret) {
		LOG_WRN("settings not available (%d): default correction", ret);
	}

	if (device_is_ready(retained) && retention_is_valid(retained) == 1 &&
	    retention_read(retained, 0, (uint8_t *)&keep, sizeof(keep)) == 0 && keep.unix_ms > 0) {
		anchor_unix_ms = keep.unix_ms;
		anchor_uptime_ms = k_uptime_get();
		tz_minutes = keep.tz_minutes;
		flags |= CLOCK_FLAG_SET | CLOCK_FLAG_APPROXIMATE;
		LOG_INF("time restored from retained RAM (late by the length of the reset)");
	}
	LOG_INF("clock correction %d ppb (%s), time %s, %s-hour display", ppb,
		(flags & CLOCK_FLAG_CALIBRATED) ? "stored" : "board default",
		(flags & CLOCK_FLAG_SET) ? "restored" : "not set", hour24 ? "24" : "12");
	return ret;
}
