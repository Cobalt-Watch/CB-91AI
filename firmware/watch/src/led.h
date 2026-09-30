/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The RGB LED of the watch (lot D6): what light.c decides, on the three PWM
 * channels of the board (pwm_led_red, green, blue). From the event loop only;
 * the colours move on in a work item of the system work queue, so the loop
 * never waits for an animation.
 */

#ifndef CB91AI_WATCH_LED_H
#define CB91AI_WATCH_LED_H

#include <stdbool.h>
#include <stdint.h>

#include "light.h"

int led_init(const struct light_style *style);

/* The light chosen in the app (Cobalt Link key 0x05) */
void led_set_style(const struct light_style *style);

/* The LIGHT button: pressed, held past a long press, released after it */
void led_light_press(void);
void led_light_hold(void);
void led_light_release(void);

/* The red blink while a take runs */
void led_recording(bool on);

/* The verdict of the app (EF-45): a short pulse, green or red */
void led_pulse(bool green, uint32_t ms);

/* Some colour on the LED now: its current would weigh on a reading of the
 * cell (main.c reads it when nothing draws) */
bool led_lit(void);

#endif /* CB91AI_WATCH_LED_H */
