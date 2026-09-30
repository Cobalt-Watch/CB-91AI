/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The RGB LED of the watch, see led.h.
 *
 * light.c decides the colour and when it next changes; a delayable work item
 * of the system work queue applies it and comes back then, every 20 ms while
 * an animation runs, never once the LED is dark. The state is shared with the
 * loop under a spin lock, and each call of the loop brings the work forward.
 *
 * PWM at 500 Hz, shorter than the 20 ms of the devicetree, which would flicker
 * in a breath. A dark LED sets every channel to 0, and the driver then stops
 * the PWM.
 *
 * The pins are GPIO outputs first, in standard drive (the green and blue LEDs
 * have no series resistor), as in the self-test. The PWM driver
 * leaves them in their sleep state, inputs, while the runtime power management
 * keeps pwm0 suspended, and it only writes the OUT register for 0 and 100 %: on
 * an input a steady colour stayed dark in 0.2.1 as built first (review of
 * 2026-09-24, no pure colour, no red blink, no verdict).
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

#include "led.h"

LOG_MODULE_REGISTER(watch_led, LOG_LEVEL_INF);

#define LED_PERIOD_NS PWM_USEC(2000)

static const struct pwm_dt_spec channels[3] = {
	PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led_red)),
	PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led_green)),
	PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led_blue)),
};

/* The same pins, active low: common anode on the 3V rail */
static const struct gpio_dt_spec pins[3] = {
	GPIO_DT_SPEC_GET(DT_NODELABEL(led_red), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(led_green), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(led_blue), gpios),
};

static struct k_spinlock lock;
static struct light_state state; /* under the lock */
static uint8_t shown[3];         /* the work queue's own */
static atomic_t lit;             /* some colour shown now, for led_lit() */
static bool ready;

static void render(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(led_work, render);

static void show(struct light_rgb c)
{
	const uint8_t want[3] = { c.r, c.g, c.b };

	for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
		if (want[i] != shown[i]) {
			const uint32_t pulse = (uint32_t)((uint64_t)LED_PERIOD_NS * want[i] / 255U);

			if (pwm_set_dt(&channels[i], LED_PERIOD_NS, pulse) == 0) {
				shown[i] = want[i];
			}
		}
	}
	atomic_set(&lit, (shown[0] | shown[1] | shown[2]) != 0U);
}

bool led_lit(void)
{
	return atomic_get(&lit) != 0;
}

/* System work queue */
static void render(struct k_work *work)
{
	uint32_t next;
	struct light_rgb c;
	k_spinlock_key_t key = k_spin_lock(&lock);

	ARG_UNUSED(work);
	c = light_frame(&state, k_uptime_get_32(), &next);
	k_spin_unlock(&lock, key);
	show(c);
	if (next != LIGHT_NO_CHANGE) {
		(void)k_work_reschedule(&led_work, K_MSEC(MAX(next, 1U)));
	}
}

/* The loop changed something: the work applies it at once */
static void refresh(void)
{
	if (ready) {
		(void)k_work_reschedule(&led_work, K_NO_WAIT);
	}
}

int led_init(const struct light_style *style)
{
	for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
		int err;

		if (!pwm_is_ready_dt(&channels[i]) || !gpio_is_ready_dt(&pins[i])) {
			LOG_ERR("LED PWM not ready");
			return -ENODEV;
		}
		/* Outputs, dark, before the PWM uses them */
		err = gpio_pin_configure_dt(&pins[i], GPIO_OUTPUT_INACTIVE);
		if (err) {
			LOG_ERR("LED pin %u: %d", (unsigned int)i, err);
			return err;
		}
		(void)pwm_set_dt(&channels[i], LED_PERIOD_NS, 0);
	}
	light_init(&state, style);
	ready = true;
	return 0;
}

void led_set_style(const struct light_style *style)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	state.style = *style;
	k_spin_unlock(&lock, key);
	refresh();
}

void led_light_press(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	light_press(&state, k_uptime_get_32());
	k_spin_unlock(&lock, key);
	refresh();
}

void led_light_hold(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	light_hold(&state, k_uptime_get_32());
	k_spin_unlock(&lock, key);
	refresh();
}

void led_light_release(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	light_release(&state, k_uptime_get_32());
	k_spin_unlock(&lock, key);
	refresh();
}

void led_recording(bool on)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	light_recording(&state, on, k_uptime_get_32());
	k_spin_unlock(&lock, key);
	refresh();
}

void led_pulse(bool green, uint32_t ms)
{
	const struct light_rgb c = { green ? 0U : 255U, green ? 255U : 0U, 0 };
	k_spinlock_key_t key = k_spin_lock(&lock);

	light_pulse(&state, c, ms, k_uptime_get_32());
	k_spin_unlock(&lock, key);
	refresh();
}
