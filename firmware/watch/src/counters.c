/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The boots and the watchdog bites, see counters.h.
 */

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/retention/retention.h>
#include <zephyr/settings/settings.h>

#include "counters.h"
#include "counts.h"

LOG_MODULE_REGISTER(watch_counters, LOG_LEVEL_INF);

#define SETTING_COUNTS "stats/counts"
/* Nothing written to the flash before the watch has run this long */
#define COUNTERS_QUIET_MS 60000

static const struct device *const retained = DEVICE_DT_GET(DT_NODELABEL(counters_retention));

static struct counts stored;
static bool stored_ok;
static struct counts now; /* the loop's own */
static bool dirty;

static int stats_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "counts", NULL) && len == sizeof(stored)) {
		stored_ok = read_cb(cb_arg, &stored, sizeof(stored)) == (ssize_t)sizeof(stored);
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(cb91ai_stats, "stats", NULL, stats_settings_set, NULL, NULL);

void counters_boot(uint32_t reset_cause)
{
	struct counts kept = { 0 };
	const bool kept_ok = device_is_ready(retained) && retention_is_valid(retained) == 1 &&
			     retention_read(retained, 0, (uint8_t *)&kept, sizeof(kept)) == 0;

	(void)settings_load_subtree("stats");
	now = counts_boot(&kept, kept_ok, &stored, stored_ok, (reset_cause & RESET_WATCHDOG) != 0);
	if (device_is_ready(retained)) {
		(void)retention_write(retained, 0, (const uint8_t *)&now, sizeof(now));
	}
	dirty = true;
	LOG_INF("boot %u, %u watchdog bite(s)%s", (unsigned int)now.boots, (unsigned int)now.bites,
		(reset_cause & RESET_WATCHDOG) ? ", this one among them" : "");
}

void counters_keep(void)
{
	if (!dirty || k_uptime_get() < COUNTERS_QUIET_MS) {
		return;
	}
	if (settings_save_one(SETTING_COUNTS, &now, sizeof(now)) == 0) {
		dirty = false;
	}
}

uint16_t counters_boots(void)
{
	return now.boots;
}

uint16_t counters_bites(void)
{
	return now.bites;
}
