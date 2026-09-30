/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The case buttons of the watch (lot D3), see buttons.h.
 *
 * Every edge of every button, through the SENSE mechanism of the GPIO port
 * (no clock runs for it), posts EVT_BUTTON_EDGE, once until the loop has taken
 * it: a bouncing contact makes a burst of edges, and the loop reads the levels
 * anyway. The loop feeds them to the gesture machine, which gives the
 * gestures and says when it wants to look again; a one-shot timer posts the
 * same event then. Nothing polls, and the core sleeps between two presses
 * (ENF-03).
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "buttons.h"
#include "events.h"
#include "stuck.h"

LOG_MODULE_REGISTER(watch_buttons, LOG_LEVEL_INF);

/* A probe of a stuck button: its input connected again, the pull-down against
 * the pin's few pF settles in well under a microsecond */
#define PROBE_SETTLE_US 10

/* The frame of the module carries the + of the cell: a press pulls the input
 * up against its pull-down (V2-22, the devicetree flags say so) */
static const struct gpio_dt_spec keys[BUTTON_COUNT] = {
	[BUTTON_LIGHT] = GPIO_DT_SPEC_GET(DT_NODELABEL(button_light), gpios),
	[BUTTON_MODE] = GPIO_DT_SPEC_GET(DT_NODELABEL(button_mode), gpios),
	[BUTTON_ALARM] = GPIO_DT_SPEC_GET(DT_NODELABEL(button_alarm), gpios),
};

static struct gpio_callback callbacks[BUTTON_COUNT];
static struct gesture_state machine;
static struct stuck stuck_state; /* the loop's own */
/* A take held down its whole length must never be taken for stuck: ALARM
 * hidden mid-take would leave its release and the stop press unseen */
BUILD_ASSERT(STUCK_HELD_MS >= CONFIG_CB91AI_WATCH_NOTE_MAX_S * 1000U + 60000U,
	     "a take this long would be taken for stuck: raise STUCK_HELD_MS");
static atomic_t pending;
/* When each button last moved, stamped in the interrupt: the loop may read
 * the levels late (a flash write), a click falls where it came (clicks.h) */
static atomic_t moved_at[BUTTON_COUNT];

/* False when the queue was full and the event dropped */
static bool wake_loop(void)
{
	/* Dropped: let go, the next edge posts again */
	if (atomic_cas(&pending, 0, 1) && !evt_post(EVT_BUTTON_EDGE, 0, 0, 0)) {
		atomic_set(&pending, 0);
		return false;
	}
	return true;
}

static void edge_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	const ptrdiff_t i = cb - callbacks;

	ARG_UNUSED(port);
	ARG_UNUSED(pins);
	if (i >= 0 && i < BUTTON_COUNT) {
		atomic_set(&moved_at[i], (atomic_val_t)k_uptime_get_32());
	}
	(void)wake_loop();
}

static void deadline_expired(struct k_timer *timer)
{
	/* No edge comes after a deadline (a button taken for stuck, or all
	 * three, makes none): dropped, it comes again a little later */
	if (!wake_loop()) {
		k_timer_start(timer, K_MSEC(100), K_NO_WAIT);
	}
}

static K_TIMER_DEFINE(deadline_timer, deadline_expired, NULL);

int buttons_init(void)
{
	static const struct gesture_cfg cfg = GESTURE_CFG_V1;
	int ret;

	gesture_init(&machine, &cfg, BUTTON_COUNT);
	stuck_init(&stuck_state, BUTTON_COUNT);
	for (size_t i = 0; i < BUTTON_COUNT; i++) {
		if (!gpio_is_ready_dt(&keys[i])) {
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&keys[i], GPIO_INPUT);
		if (ret == 0) {
			gpio_init_callback(&callbacks[i], edge_isr, BIT(keys[i].pin));
			ret = gpio_add_callback(keys[i].port, &callbacks[i]);
		}
		if (ret == 0) {
			ret = gpio_pin_interrupt_configure_dt(&keys[i], GPIO_INT_EDGE_BOTH);
		}
		if (ret) {
			LOG_ERR("button %u: setup failed (%d)", (unsigned int)i, ret);
			return ret;
		}
	}
	/* A button already held counts from now */
	(void)wake_loop();
	return 0;
}

/* Taken for stuck (stuck.h): its input disconnected, pull-down included (the
 * driver gives the pin back its reset state), so that nothing runs through it */
static void let_go(size_t i)
{
	(void)gpio_pin_interrupt_configure_dt(&keys[i], GPIO_INT_DISABLE);
	(void)gpio_pin_configure(keys[i].port, keys[i].pin, GPIO_DISCONNECTED);
	LOG_WRN("button %u held %u s: taken for stuck, its pull-down released", (unsigned int)i,
		STUCK_HELD_MS / 1000U);
}

/* Connected again for a moment: still down, it is let go again; up, it is a
 * button like the others */
static void probe_due(uint32_t now)
{
	const uint8_t due = stuck_due(&stuck_state, now);

	for (size_t i = 0; i < BUTTON_COUNT; i++) {
		bool down;

		if ((due & BIT(i)) == 0) {
			continue;
		}
		(void)gpio_pin_configure_dt(&keys[i], GPIO_INPUT);
		k_busy_wait(PROBE_SETTLE_US);
		down = gpio_pin_get_dt(&keys[i]) > 0;
		stuck_probed(&stuck_state, (unsigned int)i, down, now);
		if (down) {
			(void)gpio_pin_configure(keys[i].port, keys[i].pin, GPIO_DISCONNECTED);
		} else {
			(void)gpio_pin_interrupt_configure_dt(&keys[i], GPIO_INT_EDGE_BOTH);
			LOG_INF("button %u let go", (unsigned int)i);
		}
	}
}

size_t buttons_process(struct gesture *out)
{
	const uint32_t now = k_uptime_get_32();
	uint32_t levels = 0;
	uint32_t wait = 0;
	uint32_t stuck_wait;
	uint8_t now_stuck;
	bool timed;
	size_t n;

	/* First: an edge from here on posts again, and is read below or next time */
	atomic_set(&pending, 0);
	probe_due(now);
	for (size_t i = 0; i < BUTTON_COUNT; i++) {
		if ((stuck_state.stuck & BIT(i)) == 0 && gpio_pin_get_dt(&keys[i]) > 0) {
			levels |= BIT(i);
		}
	}
	now_stuck = stuck_update(&stuck_state, (uint8_t)levels, now);
	for (size_t i = 0; i < BUTTON_COUNT; i++) {
		if (now_stuck & BIT(i)) {
			let_go(i);
		}
	}
	/* A stuck button is up for the gestures: its long press ends there,
	 * which does nothing that matters (ui.c), and the others work as ever */
	levels &= ~(uint32_t)stuck_state.stuck;
	n = gesture_update(&machine, levels, now, out);
	timed = gesture_next(&machine, now, &wait);
	if (stuck_next(&stuck_state, now, &stuck_wait) && (!timed || stuck_wait < wait)) {
		wait = stuck_wait;
		timed = true;
	}
	if (timed) {
		k_timer_start(&deadline_timer, K_MSEC(wait), K_NO_WAIT);
	} else {
		k_timer_stop(&deadline_timer);
	}
	return n;
}

uint32_t buttons_changed_at(enum button b)
{
	return (uint32_t)atomic_get(&moved_at[b]);
}

bool buttons_down(enum button b)
{
	return machine.b[b].raw;
}

uint8_t buttons_stuck(void)
{
	return stuck_state.stuck;
}
