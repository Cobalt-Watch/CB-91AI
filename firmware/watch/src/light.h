/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * What the RGB LED shows, and when (lot D6): the light of the LIGHT button in
 * the colour and the animation chosen in the app (EF-15, PO-03, Cobalt Link
 * key 0x05), the red blink of a take (EF-24), the green or red verdict of the
 * app (EF-45). Pure logic: led.c drives the PWM from what it computes, and the
 * host tests check it (firmware/tests/host/test_light.c).
 *
 * Layers, the first one active wins: the verdict pulse, the light, the blink
 * of a take. The light lasts 1.5 s after a press, until the release when LIGHT
 * is held, and 10 s at most (the cell). The blink of a take is the
 * one of the self-test's `rec`, 100 ms every 500 ms, which leaves no trace in
 * the recording.
 */

#ifndef CB91AI_WATCH_LIGHT_H
#define CB91AI_WATCH_LIGHT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIGHT_MIN_MS     1500U  /* a press lights this long at least (EF-15) */
#define LIGHT_MAX_MS     10000U /* held, this long at most */
#define LIGHT_STEP_MS    20U    /* an animation moves on this often */
#define LIGHT_BREATHE_MS 2000U  /* one breath */
#define LIGHT_BLINK_MS   250U   /* on, then off */
#define LIGHT_RAINBOW_MS 1500U  /* one turn of the colour wheel, as at boot */
#define LIGHT_REC_PERIOD_MS 500U
#define LIGHT_REC_ON_MS     100U
#define LIGHT_NO_CHANGE  UINT32_MAX

struct light_rgb {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

/* The light chosen in the app: an effect (LINK_LIGHT_*) and a colour */
struct light_style {
	uint8_t effect;
	struct light_rgb color;
};

#define LIGHT_STYLE_DEFAULT { .effect = 0, .color = { 255, 255, 255 } } /* steady white */

struct light_state {
	struct light_style style;
	bool light;             /* the light is on until light_until */
	uint32_t light_since;
	uint32_t light_until;
	bool recording;         /* a take runs: the red blink */
	uint32_t rec_since;
	bool pulse;             /* a verdict until pulse_until */
	struct light_rgb pulse_color;
	uint32_t pulse_until;
};

void light_init(struct light_state *s, const struct light_style *style);

/* The value of Cobalt Link key 0x05: effect, red, green, blue. False when it
 * is short, its effect unknown, or its colour black where one is used. */
bool light_style_parse(const uint8_t *value, size_t len, struct light_style *out);
size_t light_style_put(const struct light_style *style, uint8_t *out, size_t room);

void light_press(struct light_state *s, uint32_t now_ms);   /* LIGHT pressed */
void light_hold(struct light_state *s, uint32_t now_ms);    /* LIGHT held past a long press */
void light_release(struct light_state *s, uint32_t now_ms); /* LIGHT released after it */
void light_recording(struct light_state *s, bool on, uint32_t now_ms);
void light_pulse(struct light_state *s, struct light_rgb color, uint32_t ms, uint32_t now_ms);

/* The colour now, and in how many ms it may change (LIGHT_NO_CHANGE: not
 * before the next call of the functions above) */
struct light_rgb light_frame(struct light_state *s, uint32_t now_ms, uint32_t *next_ms);

#endif /* CB91AI_WATCH_LIGHT_H */
