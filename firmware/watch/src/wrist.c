/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The wrist, see wrist.h.
 */

#include <errno.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "events.h"
#include "wrist.h"

LOG_MODULE_REGISTER(watch_wrist, LOG_LEVEL_INF);

static const struct gpio_dt_spec int1 = GPIO_DT_SPEC_GET(DT_NODELABEL(bma400), int1_gpios);
static const struct gpio_dt_spec int2 = GPIO_DT_SPEC_GET(DT_NODELABEL(bma400), int2_gpios);

static struct gpio_callback line_cb;
static struct wrist_tuning tuning = WRIST_TUNING_DEFAULT;
static bool started;
static uint32_t changes;
static uint32_t raises;
static uint32_t taps;
static int32_t last_z_mg;
/* What the status registers said, for the bench (key 0x7B) */
static uint32_t wakes;
static uint32_t overruns;
static uint32_t empty; /* a line up, and nothing in the status */
static uint8_t last_stat0, last_stat1, seen_stat0, seen_stat1;

static void on_line(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	(void)evt_post(EVT_MOTION, 0, 0, 0);
}

static int start(void)
{
	const int err = accel_wrist_start(&tuning.sensor);

	started = err == 0;
	if (err) {
		/* A start cut short may leave the sensor in normal mode (3.5 uA
		 * instead of 0.85): asleep, as at boot (main.c) */
		LOG_WRN("accelerometer not started (%d): the buttons only", err);
		(void)accel_sleep();
	}
	return err;
}

BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR(DT_NODELABEL(bma400), int1_gpios),
			  DT_GPIO_CTLR(DT_NODELABEL(bma400), int2_gpios)),
	     "one callback serves both lines");

int wrist_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&int1) || !gpio_is_ready_dt(&int2)) {
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(&int1, GPIO_INPUT);
	err = err ? err : gpio_pin_configure_dt(&int2, GPIO_INPUT);
	if (err) {
		return err;
	}
	gpio_init_callback(&line_cb, on_line, BIT(int1.pin) | BIT(int2.pin));
	err = gpio_add_callback(int1.port, &line_cb);
	err = err ? err : gpio_pin_interrupt_configure_dt(&int1, GPIO_INT_EDGE_TO_ACTIVE);
	err = err ? err : gpio_pin_interrupt_configure_dt(&int2, GPIO_INT_EDGE_TO_ACTIVE);
	return err ? err : start();
}

enum wrist_event wrist_event(void)
{
	uint8_t stat0 = 0;
	uint8_t stat1 = 0;
	int32_t mg[3];

	if (!started || accel_wrist_status(&stat0, &stat1) != 0) {
		return WRIST_NONE;
	}
	last_stat0 = stat0;
	last_stat1 = stat1;
	seen_stat0 |= stat0;
	seen_stat1 |= stat1;
	wakes += (stat0 & ACCEL_STAT0_WAKEUP) ? 1 : 0;
	overruns += (stat0 & ACCEL_STAT0_OVERRUN) ? 1 : 0;
	empty += (stat0 == 0 && stat1 == 0) ? 1 : 0;
	if ((stat0 & ACCEL_STAT0_WAKEUP) && tuning.sensor.orient_ref == ACCEL_ORIENT_REF_HOST) {
		/* Awake again: the orientation the watch holds for the last one
		 * written back; before the engine judges a change only when the
		 * wake-up is on INT1 (ACCEL_WRIST_WAKE_INT1), seen with it otherwise */
		(void)accel_orient_ref_again();
	}
	/* A double tap lights the glass whatever its orientation; an orientation
	 * change read in the same status is still taken, or the reference the
	 * watch holds would stay behind and the same change come again */
	const enum wrist_event tap = (stat1 & ACCEL_STAT1_D_TAP) ? WRIST_TAP : WRIST_NONE;

	taps += tap == WRIST_TAP ? 1 : 0;
	if ((stat0 & ACCEL_STAT0_ORIENTCH) == 0) {
		return tap;
	}
	/* The orientation before the change, which the watch holds (look.h);
	 * none when the sensor moves its own reference */
	const bool host = tuning.sensor.orient_ref == ACCEL_ORIENT_REF_HOST;
	int32_t before[3];

	if (host) {
		accel_orient_ref_mg(before);
	}
	if ((host ? accel_orient_ref_now(mg) : accel_now(mg)) != 0) {
		return tap;
	}
	/* A new orientation, held: a look at the watch if the glass is up, turned
	 * toward the eyes; any other (the arm let down, turned away, back to the
	 * keyboard) is nothing to show */
	changes++;
	last_z_mg = mg[2];
	if (!look_raised(host ? before : NULL, mg, &tuning.look)) {
		return tap;
	}
	raises++;
	return WRIST_RAISE;
}

bool wrist_line_up(void)
{
	return started && (gpio_pin_get_dt(&int1) > 0 || gpio_pin_get_dt(&int2) > 0);
}

void wrist_counts(uint32_t *c, uint32_t *r, uint32_t *t, int32_t *z)
{
	*c = changes;
	*r = raises;
	*t = taps;
	*z = last_z_mg;
}

size_t wrist_tuning_get(uint8_t *out, size_t room)
{
	if (room < WRIST_TUNING_SIZE) {
		return 0;
	}
	out[0] = tuning.sensor.wake_thres;
	out[1] = tuning.sensor.wake_samples;
	out[2] = tuning.sensor.orient_thres;
	out[3] = tuning.sensor.orient_dur;
	out[4] = tuning.sensor.tap_sensitivity;
	sys_put_le16(tuning.sensor.lowpower_ms, &out[5]);
	sys_put_le16((uint16_t)tuning.look.face_up_mg, &out[7]);
	out[9] = tuning.sensor.wake_ref;
	out[10] = tuning.sensor.orient_ref;
	out[11] = tuning.sensor.osr_lp;
	out[12] = tuning.sensor.flags;
	sys_put_le16((uint16_t)tuning.look.toward_mg, &out[13]);
	sys_put_le16((uint16_t)tuning.look.tip_mg, &out[15]);
	sys_put_le16((uint16_t)tuning.look.twelve_up_mg, &out[17]);
	return WRIST_TUNING_SIZE;
}

bool wrist_tuning_set(const uint8_t *v, size_t len)
{
	struct wrist_tuning t = tuning;

	if ((len != WRIST_TUNING_SIZE_OLD && len != WRIST_TUNING_SIZE_17 &&
	     len != WRIST_TUNING_SIZE_28 && len < WRIST_TUNING_SIZE) ||
	    v[0] == 0 || v[1] < 1 || v[1] > 8 || v[2] == 0 || v[4] > 7 ||
	    sys_get_le16(&v[5]) == 0 || sys_get_le16(&v[5]) > 10000) {
		return false;
	}
	if (len >= WRIST_TUNING_SIZE_17 &&
	    ((v[9] != ACCEL_WAKE_REF_ONCE && v[9] != ACCEL_WAKE_REF_EVERY) ||
	     v[10] > ACCEL_ORIENT_REF_LP || v[11] > 3 ||
	     (v[12] & ~(ACCEL_WRIST_ORIENT_LP | ACCEL_WRIST_WAKE_INT1 | ACCEL_WRIST_ORIENT_Z_ONLY)) !=
		     0)) {
		return false;
	}
	if ((len >= WRIST_TUNING_SIZE_28 && (int16_t)sys_get_le16(&v[13]) < 0) ||
	    (len >= WRIST_TUNING_SIZE && (int16_t)sys_get_le16(&v[15]) < 0)) {
		return false; /* a turn or a tip away from the eyes is no look */
	}
	t.sensor.wake_thres = v[0];
	t.sensor.wake_samples = v[1];
	t.sensor.orient_thres = v[2];
	t.sensor.orient_dur = v[3];
	t.sensor.tap_sensitivity = v[4];
	t.sensor.lowpower_ms = sys_get_le16(&v[5]);
	t.look.face_up_mg = (int16_t)sys_get_le16(&v[7]);
	if (len >= WRIST_TUNING_SIZE_17) {
		t.sensor.wake_ref = v[9];
		t.sensor.orient_ref = v[10];
		t.sensor.osr_lp = v[11];
		t.sensor.flags = v[12];
	}
	if (len >= WRIST_TUNING_SIZE_28) {
		t.look.toward_mg = (int16_t)sys_get_le16(&v[13]);
	}
	if (len >= WRIST_TUNING_SIZE) {
		t.look.tip_mg = (int16_t)sys_get_le16(&v[15]);
		t.look.twelve_up_mg = (int16_t)sys_get_le16(&v[17]);
	}
	tuning = t;
	LOG_INF("wrist: wake %u x %u (ref %u, osr %u), orientation %u x %u (ref %u), tap %u, "
		"low power %u ms, face up %d mg, turn %d mg to %d, tip %d mg, flags 0x%02x",
		t.sensor.wake_thres, t.sensor.wake_samples, t.sensor.wake_ref, t.sensor.osr_lp,
		t.sensor.orient_thres, t.sensor.orient_dur, t.sensor.orient_ref,
		t.sensor.tap_sensitivity, t.sensor.lowpower_ms, t.look.face_up_mg, t.look.toward_mg,
		t.look.twelve_up_mg, t.look.tip_mg, t.sensor.flags);
	return start() == 0;
}

size_t wrist_diag(uint8_t *out, size_t room)
{
	if (room < WRIST_DIAG_SIZE || accel_dump(&out[12]) != 0) {
		return 0;
	}
	out[0] = 1;
	out[1] = last_stat0;
	out[2] = last_stat1;
	out[3] = seen_stat0;
	out[4] = seen_stat1;
	sys_put_le16((uint16_t)MIN(wakes, UINT16_MAX), &out[5]);
	sys_put_le16((uint16_t)MIN(overruns, UINT16_MAX), &out[7]);
	sys_put_le16((uint16_t)MIN(empty, UINT16_MAX), &out[9]);
	out[11] = 0;
	return WRIST_DIAG_SIZE;
}
