/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * LCD segment walk: lights one pixel of the glass (COM, SEG) at a time, or
 * one SEG plot with its three COM lines, and moves on when a case button is
 * pressed (MODE next, LIGHT previous, ALARM next COM line) or from the shell
 * (cb91ai walk ...). Each step is logged with the element the Sensor Watch
 * map expects there, so that one photo per step maps the glass. The blue LED
 * flashes on every accepted press.
 *
 * The polarity of the case buttons is not known (V2-22): every 20 ms each
 * input is read with a pull-up then with a pull-down; a level that follows
 * neither pull is a pressed button, and the log says which rail it went to.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "lcd.h"
#include "lcdwalk.h"

LOG_MODULE_REGISTER(cb91ai_walk, LOG_LEVEL_INF);

void selftest_clock_enable(bool enable);
void selftest_display_hold(bool hold);
void selftest_buttons_irq(bool enable);

#define GLASS_COMS       3
#define GLASS_SEGS       24
#define POLL_MS          20
#define DEBOUNCE_SAMPLES 3
#define FLASH_MS         80

enum { BTN_LIGHT, BTN_MODE, BTN_ALARM, BTN_COUNT };

static const struct gpio_dt_spec buttons[BTN_COUNT] = {
	GPIO_DT_SPEC_GET(DT_NODELABEL(button_light), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(button_mode), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(button_alarm), gpios),
};
static const char *const button_names[BTN_COUNT] = { "LIGHT", "MODE", "ALARM" };
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_NODELABEL(led_blue), gpios);

static struct {
	bool active;
	enum lcdwalk_unit unit;
	int step;
	int count;
	uint8_t held[BTN_COUNT]; /* consecutive samples seen pressed */
	bool pressed[BTN_COUNT];
} walk;

static struct k_work_delayable poll_work;
static struct k_work_delayable led_work;
static bool works_ready;

static void led_off(struct k_work *work)
{
	gpio_pin_set_dt(&led, 0);
}

static void flash(void)
{
	gpio_pin_set_dt(&led, 1);
	k_work_reschedule(&led_work, K_MSEC(FLASH_MS));
}

static void show_step(void)
{
	char what[80];
	int com, seg;

	lcd_all_pixels(false);
	lcd_clear();
	if (walk.unit == LCDWALK_PIXEL) {
		com = walk.step / GLASS_SEGS;
		seg = walk.step % GLASS_SEGS;
		lcd_set_pixel(com, seg, true);
		lcd_describe_pixel(com, seg, what, sizeof(what));
		LOG_INF("walk %d/%d: COM%d SEG%d = %s", walk.step + 1, walk.count, com, seg, what);
	} else {
		seg = walk.step;
		for (com = 0; com < GLASS_COMS; com++) {
			lcd_set_pixel(com, seg, true);
		}
		LOG_INF("walk %d/%d: SEG%d on COM0 to COM2", walk.step + 1, walk.count, seg);
		for (com = 0; com < GLASS_COMS; com++) {
			lcd_describe_pixel(com, seg, what, sizeof(what));
			LOG_INF("  COM%d = %s", com, what);
		}
	}
	lcd_flush();
}

static void act(int button)
{
	flash();
	switch (button) {
	case BTN_MODE:
		lcdwalk_move(1);
		break;
	case BTN_LIGHT:
		lcdwalk_move(-1);
		break;
	case BTN_ALARM:
		if (walk.unit == LCDWALK_PIXEL) {
			lcdwalk_goto(((walk.step / GLASS_SEGS + 1) % GLASS_COMS) * GLASS_SEGS);
		} else {
			lcdwalk_move(8);
		}
		break;
	default:
		break;
	}
}

static void poll(struct k_work *work)
{
	for (int i = 0; i < BTN_COUNT; i++) {
		int up, down;

		/* Pull-up first, pull-down last: the pin rests in its devicetree state */
		gpio_pin_configure(buttons[i].port, buttons[i].pin, GPIO_INPUT | GPIO_PULL_UP);
		k_busy_wait(50);
		up = gpio_pin_get_raw(buttons[i].port, buttons[i].pin);
		gpio_pin_configure(buttons[i].port, buttons[i].pin, GPIO_INPUT | GPIO_PULL_DOWN);
		k_busy_wait(50);
		down = gpio_pin_get_raw(buttons[i].port, buttons[i].pin);

		bool now = (down == 1) || (up == 0);

		walk.held[i] = now ? MIN(walk.held[i] + 1, DEBOUNCE_SAMPLES) : 0;
		if (!walk.pressed[i] && walk.held[i] == DEBOUNCE_SAMPLES) {
			walk.pressed[i] = true;
			LOG_INF("%s pressed (contact to %s)", button_names[i],
				down == 1 ? "VDD" : "GND");
			act(i);
		} else if (walk.pressed[i] && walk.held[i] == 0) {
			walk.pressed[i] = false;
		}
	}
	if (walk.active) {
		k_work_reschedule(&poll_work, K_MSEC(POLL_MS));
	}
}

int lcdwalk_start(enum lcdwalk_unit unit, int step)
{
	int ret;

	for (int i = 0; i < BTN_COUNT; i++) {
		if (!gpio_is_ready_dt(&buttons[i])) {
			return -ENODEV;
		}
	}
	if (!works_ready) {
		k_work_init_delayable(&poll_work, poll);
		k_work_init_delayable(&led_work, led_off);
		works_ready = true;
	}
	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		return ret;
	}
	selftest_clock_enable(false);
	/* Steady display for the photos, and no wake-up interrupts while the
	 * inputs are probed with both pulls
	 */
	selftest_display_hold(true);
	selftest_buttons_irq(false);
	walk.unit = unit;
	walk.count = unit == LCDWALK_PIXEL ? GLASS_COMS * GLASS_SEGS : GLASS_SEGS;
	walk.step = CLAMP(step, 0, walk.count - 1);
	memset(walk.held, 0, sizeof(walk.held));
	memset(walk.pressed, 0, sizeof(walk.pressed));
	walk.active = true;
	LOG_INF("walk started, %s mode, %d steps: MODE next, LIGHT previous, ALARM next COM",
		unit == LCDWALK_PIXEL ? "pixel" : "seg", walk.count);
	show_step();
	k_work_reschedule(&poll_work, K_MSEC(POLL_MS));
	return 0;
}

int lcdwalk_move(int delta)
{
	int next;

	if (!walk.active) {
		return lcdwalk_start(LCDWALK_PIXEL, 0);
	}
	next = ((walk.step + delta) % walk.count + walk.count) % walk.count;
	if ((delta > 0 && next < walk.step) || (delta < 0 && next > walk.step)) {
		LOG_INF("walk: wrapped around");
	}
	walk.step = next;
	show_step();
	return 0;
}

int lcdwalk_goto(int step)
{
	if (!walk.active) {
		return lcdwalk_start(LCDWALK_PIXEL, step);
	}
	walk.step = CLAMP(step, 0, walk.count - 1);
	show_step();
	return 0;
}

void lcdwalk_stop(void)
{
	if (!walk.active) {
		return;
	}
	walk.active = false;
	k_work_cancel_delayable(&poll_work);
	for (int i = 0; i < BTN_COUNT; i++) {
		gpio_pin_configure_dt(&buttons[i], GPIO_INPUT);
	}
	gpio_pin_set_dt(&led, 0);
	lcd_all_pixels(false);
	selftest_buttons_irq(true);
	selftest_display_hold(false);
	selftest_clock_enable(true);
	LOG_INF("walk stopped, clock back");
}

bool lcdwalk_active(void)
{
	return walk.active;
}

int lcdwalk_step(void)
{
	return walk.step;
}

int lcdwalk_count(void)
{
	return walk.count;
}
