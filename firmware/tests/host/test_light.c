/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of what the RGB LED shows (firmware/watch/src/light.c, lot D6):
 * the light of LIGHT and its effects, the red blink of a take, the verdict
 * pulse, and which one wins.
 */

#include "harness.h"
#include "light.h"
#include "link_proto.h"

static const struct light_style white = LIGHT_STYLE_DEFAULT;

static int same(struct light_rgb c, uint8_t r, uint8_t g, uint8_t b)
{
	return c.r == r && c.g == g && c.b == b;
}

static void styles_from_the_app(void)
{
	struct light_style s;
	uint8_t out[4];
	const uint8_t blue_breath[] = { LINK_LIGHT_BREATHE, 0, 0, 255 };
	const uint8_t black[] = { LINK_LIGHT_STEADY, 0, 0, 0 };
	const uint8_t rainbow[] = { LINK_LIGHT_RAINBOW, 0, 0, 0 };
	const uint8_t unknown[] = { 4, 255, 255, 255 };

	CHECK(light_style_parse(blue_breath, sizeof(blue_breath), &s));
	CHECK_EQ(s.effect, LINK_LIGHT_BREATHE);
	CHECK(same(s.color, 0, 0, 255));
	CHECK_EQ(light_style_put(&s, out, sizeof(out)), 4);
	CHECK_EQ(out[3], 255);
	CHECK_EQ(light_style_put(&s, out, 3), 0);
	/* Refused: too short, a light that does not light, an unknown effect */
	CHECK(!light_style_parse(blue_breath, 3, &s));
	CHECK(!light_style_parse(black, sizeof(black), &s));
	CHECK(!light_style_parse(unknown, sizeof(unknown), &s));
	/* The colour wheel does not use the colour */
	CHECK(light_style_parse(rainbow, sizeof(rainbow), &s));
}

static void a_press_lights_one_and_a_half_seconds(void)
{
	struct light_state s;
	uint32_t next;

	light_init(&s, &white);
	CHECK(same(light_frame(&s, 1000, &next), 0, 0, 0));
	CHECK_EQ(next, LIGHT_NO_CHANGE);
	light_press(&s, 1000);
	CHECK(same(light_frame(&s, 1000, &next), 255, 255, 255));
	CHECK_EQ(next, LIGHT_MIN_MS); /* steady: nothing to do until its end */
	CHECK(same(light_frame(&s, 2499, &next), 255, 255, 255));
	CHECK(same(light_frame(&s, 2500, &next), 0, 0, 0));
	CHECK_EQ(next, LIGHT_NO_CHANGE);
	CHECK(!s.light);
}

static void held_until_the_release(void)
{
	struct light_state s;
	uint32_t next;

	light_init(&s, &white);
	/* Held, released at 4 s: off then */
	light_press(&s, 0);
	light_hold(&s, 500);
	CHECK(same(light_frame(&s, 3999, &next), 255, 255, 255));
	light_release(&s, 4000);
	CHECK(same(light_frame(&s, 4000, &next), 0, 0, 0));
	/* Released at 0.7 s: the 1.5 s of a press all the same */
	light_press(&s, 10000);
	light_hold(&s, 10500);
	light_release(&s, 10700);
	CHECK(same(light_frame(&s, 11499, &next), 255, 255, 255));
	CHECK_EQ(next, 1);
	CHECK(same(light_frame(&s, 11500, &next), 0, 0, 0));
	/* Held for good: 10 s at most */
	light_press(&s, 20000);
	light_hold(&s, 20500);
	CHECK(same(light_frame(&s, 29999, &next), 255, 255, 255));
	CHECK(same(light_frame(&s, 30000, &next), 0, 0, 0));
	light_release(&s, 31000); /* too late: nothing comes back */
	CHECK(same(light_frame(&s, 31000, &next), 0, 0, 0));
}

static void effects(void)
{
	struct light_state s;
	struct light_rgb c;
	uint32_t next;
	struct light_style style = { .effect = LINK_LIGHT_BREATHE, .color = { 255, 255, 255 } };

	/* A breath: full at the press, a fifth squared at its middle */
	light_init(&s, &style);
	light_press(&s, 0);
	light_hold(&s, 500);
	CHECK(same(light_frame(&s, 0, &next), 255, 255, 255));
	CHECK_EQ(next, LIGHT_STEP_MS);
	c = light_frame(&s, 1000, &next);
	CHECK_EQ(c.r, 51 * 51 / 255);
	CHECK(same(light_frame(&s, 2000, &next), 255, 255, 255));
	/* Blinks: 250 ms on, 250 off */
	style.effect = LINK_LIGHT_BLINK;
	style.color = (struct light_rgb){ 0, 255, 0 };
	light_init(&s, &style);
	light_press(&s, 0);
	CHECK(same(light_frame(&s, 100, &next), 0, 255, 0));
	CHECK_EQ(next, 150);
	CHECK(same(light_frame(&s, 250, &next), 0, 0, 0));
	CHECK(same(light_frame(&s, 500, &next), 0, 255, 0));
	/* The colour wheel: red, yellow at a sixth, cyan at the half */
	style.effect = LINK_LIGHT_RAINBOW;
	light_init(&s, &style);
	light_press(&s, 0);
	CHECK(same(light_frame(&s, 0, &next), 255, 0, 0));
	CHECK(same(light_frame(&s, 250, &next), 255, 255, 0));
	CHECK(same(light_frame(&s, 750, &next), 0, 255, 255));
}

static void a_take_blinks_red(void)
{
	struct light_state s;
	uint32_t next;

	light_init(&s, &white);
	light_recording(&s, true, 1000);
	CHECK(same(light_frame(&s, 1000, &next), 255, 0, 0));
	CHECK_EQ(next, LIGHT_REC_ON_MS);
	CHECK(same(light_frame(&s, 1100, &next), 0, 0, 0));
	CHECK_EQ(next, LIGHT_REC_PERIOD_MS - LIGHT_REC_ON_MS);
	CHECK(same(light_frame(&s, 1500, &next), 255, 0, 0));
	/* A second start keeps the rhythm */
	light_recording(&s, true, 1700);
	CHECK(same(light_frame(&s, 2000, &next), 255, 0, 0));
	/* LIGHT over it while it lights, then the blink again */
	light_press(&s, 2100);
	CHECK(same(light_frame(&s, 2150, &next), 255, 255, 255));
	CHECK(same(light_frame(&s, 3600, &next), 0, 0, 0)); /* 3600: off time of the blink */
	CHECK(same(light_frame(&s, 4000, &next), 255, 0, 0));
	light_recording(&s, false, 4050);
	CHECK(same(light_frame(&s, 4050, &next), 0, 0, 0));
	CHECK_EQ(next, LIGHT_NO_CHANGE);
}

static void the_verdict_goes_first(void)
{
	struct light_state s;
	uint32_t next;
	const struct light_rgb green = { 0, 255, 0 };

	light_init(&s, &white);
	light_press(&s, 0);
	light_pulse(&s, green, 30, 100);
	CHECK(same(light_frame(&s, 100, &next), 0, 255, 0));
	CHECK_EQ(next, 30);
	CHECK(same(light_frame(&s, 130, &next), 255, 255, 255));
	CHECK(!s.pulse);
}

static void across_the_wrap_of_the_uptime(void)
{
	struct light_state s;
	uint32_t next;
	const uint32_t t0 = 0xffffff00U;

	light_init(&s, &white);
	light_press(&s, t0);
	CHECK(same(light_frame(&s, t0 + 1000U, &next), 255, 255, 255));
	CHECK(same(light_frame(&s, t0 + 1500U, &next), 0, 0, 0));
	light_recording(&s, true, t0);
	CHECK(same(light_frame(&s, t0 + 500U, &next), 255, 0, 0));
}

int main(void)
{
	RUN(styles_from_the_app);
	RUN(a_press_lights_one_and_a_half_seconds);
	RUN(held_until_the_release);
	RUN(effects);
	RUN(a_take_blinks_red);
	RUN(the_verdict_goes_first);
	RUN(across_the_wrap_of_the_uptime);
	return harness_report("light");
}
