/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the screens and buttons of the watch (firmware/watch/src/ui.c,
 * lots D3 and D6): the gestures go in as the gesture machine reports them, the
 * effects and the frame of the glass come out.
 */

#include <string.h>

#include "calendar.h"
#include "harness.h"
#include "ui.h"

#define L UI_LIGHT
#define M UI_MODE
#define A UI_ALARM

/* Wednesday 23 September 2026, 20:15:07 local */
static int64_t base_ms(void)
{
	const struct cal_time t = { .year = 2026, .month = 9, .day = 23, .hour = 20, .minute = 15,
				    .second = 7 };
	int64_t ms = 0;

	CHECK(cal_to_ms(&t, &ms));
	return ms;
}

static struct ui_context ctx_at(uint32_t uptime_ms, bool display_on)
{
	struct ui_context c = {
		.uptime_ms = uptime_ms,
		.time_set = true,
		.local_ms = base_ms() + uptime_ms,
		.h24 = false,
		.notes = 3,
		.link = UI_LINK_OFF,
		.battery_mv = 2817,
		.display_on = display_on,
	};
	return c;
}

static uint32_t act(struct ui *ui, uint8_t type, uint8_t button, uint8_t count, uint32_t ms,
		    uint32_t at, bool display_on)
{
	const struct gesture g = { .type = type, .button = button, .count = count, .ms = ms };
	const struct ui_context c = ctx_at(at, display_on);

	return ui_gesture(ui, &g, &c);
}

static const char *glass(const struct ui *ui, uint32_t at)
{
	static struct ui_frame f;
	const struct ui_context c = ctx_at(at, true);

	ui_render(ui, &c, &f);
	f.text[10] = '\0';
	return f.text;
}

static void a_dark_glass_only_lights(void)
{
	struct ui ui;

	ui_init(&ui);
	CHECK_EQ(act(&ui, GESTURE_PRESS, M, 0, 0, 1000, false), UI_FX_DISPLAY);
	CHECK_EQ(ui.screen, UI_SCREEN_TIME);
	CHECK_EQ(act(&ui, GESTURE_CLICK, M, 1, 0, 1400, true), 0);
	CHECK_EQ(ui.screen, UI_SCREEN_TIME);
}

static void mode_walks_the_screens(void)
{
	struct ui ui;

	ui_init(&ui);
	/* Wednesday the 23rd, 8:15:07 PM */
	CHECK(strcmp(glass(&ui, 0), "WE23 81507") == 0);
	CHECK_EQ(act(&ui, GESTURE_PRESS, M, 0, 0, 1000, true), UI_FX_DISPLAY);
	/* No date screen since 2026-09-25: the notes and the radio */
	CHECK(strcmp(glass(&ui, 1000), "bL E 3 OFF") == 0); /* three notes, radio off */
	act(&ui, GESTURE_PRESS, M, 1, 0, 1200, true);
	CHECK(strcmp(glass(&ui, 1200), "bA t  2817") == 0);
	act(&ui, GESTURE_PRESS, M, 2, 0, 1400, true);
	/* The temperature since 2026-09-25: nothing read yet here */
	CHECK_EQ(ui.screen, UI_SCREEN_TEMP);
	CHECK(strcmp(glass(&ui, 1400), "tE P  --#C") == 0);
	act(&ui, GESTURE_PRESS, M, 0, 0, 3000, true);
	CHECK_EQ(ui.screen, UI_SCREEN_TIME);
	CHECK(strcmp(glass(&ui, 3000), "WE23 81510") == 0);
	/* The clicks that follow do nothing more */
	CHECK_EQ(act(&ui, GESTURE_CLICK, M, 3, 0, 3500, true), 0);
	/* The glass goes dark: the time again next time */
	act(&ui, GESTURE_PRESS, M, 0, 0, 5000, true);
	ui_display_off(&ui);
	CHECK_EQ(ui.screen, UI_SCREEN_TIME);
}

static void nothing_is_set_on_the_watch(void)
{
	struct ui ui;

	ui_init(&ui);
	/* MODE held: no settings any more, the phone sets time and format */
	act(&ui, GESTURE_PRESS, M, 0, 0, 1000, true);
	CHECK_EQ(act(&ui, GESTURE_LONG, M, 0, 0, 1450, true), 0);
	CHECK_EQ(act(&ui, GESTURE_LONG_END, M, 0, 900, 1900, true), 0);
	CHECK_EQ(ui.screen, UI_SCREEN_LINK); /* only the press acted */
	CHECK_EQ(ui_display_ms(&ui, 10000), 10000);
	CHECK_EQ(ui_tick_ms(&ui), 1000);
}

static void light_lights_the_led(void)
{
	struct ui ui;

	ui_init(&ui);
	/* At once, even on a dark glass, and on no screen of its own */
	CHECK_EQ(act(&ui, GESTURE_PRESS, L, 0, 0, 1000, false), UI_FX_LIGHT_ON | UI_FX_DISPLAY);
	CHECK_EQ(ui.screen, UI_SCREEN_TIME);
	CHECK_EQ(act(&ui, GESTURE_CLICK, L, 1, 0, 1400, true), 0);
	/* Held: on until the release; never a sync any more */
	CHECK_EQ(act(&ui, GESTURE_PRESS, L, 0, 0, 3000, true), UI_FX_LIGHT_ON | UI_FX_DISPLAY);
	CHECK_EQ(act(&ui, GESTURE_LONG, L, 0, 0, 3500, true), UI_FX_LIGHT_HOLD);
	CHECK_EQ(act(&ui, GESTURE_LONG_END, L, 0, 4000, 7000, true), UI_FX_LIGHT_OFF);
	CHECK_EQ(ui.screen, UI_SCREEN_TIME);
}

static void alarm_click_syncs_and_drops_the_take(void)
{
	struct ui ui;

	ui_init(&ui);
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 0, 0, 1000, true), UI_FX_TAKE_START | UI_FX_DISPLAY);
	CHECK(ui.taking);
	CHECK_EQ(act(&ui, GESTURE_CLICK, A, 1, 0, 1400, true), UI_FX_TAKE_DROP | UI_FX_SYNC);
	CHECK(!ui.taking);
	/* A double click: one take, that of the first press, dropped; no sync
	 * (the events of the app, lot D6) */
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 0, 0, 3000, true), UI_FX_TAKE_START | UI_FX_DISPLAY);
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 1, 0, 3200, true), UI_FX_DISPLAY);
	CHECK_EQ(ui.take_at, 3000);
	CHECK_EQ(act(&ui, GESTURE_CLICK, A, 2, 0, 3600, true), UI_FX_TAKE_DROP);
	CHECK(!ui.taking);
	/* Pressed again and held: the take of the first press is kept */
	act(&ui, GESTURE_PRESS, A, 0, 0, 5000, true);
	act(&ui, GESTURE_PRESS, A, 1, 0, 5200, true);
	CHECK_EQ(act(&ui, GESTURE_LONG, A, 1, 0, 5700, true), UI_FX_TAKE_KEEP | UI_FX_DISPLAY);
	CHECK_EQ(ui.take_at, 5000);
}

static void alarm_click_on_a_dark_glass_only_lights(void)
{
	struct ui ui;

	ui_init(&ui);
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 0, 0, 1000, false), UI_FX_TAKE_START | UI_FX_DISPLAY);
	CHECK_EQ(act(&ui, GESTURE_CLICK, A, 1, 0, 1400, true), UI_FX_TAKE_DROP);
}

static void alarm_held_records_until_a_press(void)
{
	struct ui ui;
	struct ui_frame f;
	const struct ui_context c = ctx_at(3000, true);

	ui_init(&ui);
	/* From a dark glass too: the take starts on the press */
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 0, 0, 1000, false), UI_FX_TAKE_START | UI_FX_DISPLAY);
	CHECK_EQ(act(&ui, GESTURE_LONG, A, 0, 0, 1450, true), UI_FX_TAKE_KEEP | UI_FX_DISPLAY);
	CHECK(ui.recording);
	CHECK_EQ(ui_display_ms(&ui, 10000), 0); /* lit while it records */
	/* "rE C" and the time since the press, 0:02 */
	ui_render(&ui, &c, &f);
	f.text[10] = '\0';
	CHECK(strcmp(f.text, "rE C 002  ") == 0);
	CHECK(f.colon);
	/* Released: it runs on, and the voice counts from now */
	CHECK_EQ(act(&ui, GESTURE_LONG_END, A, 0, 6000, 7000, true), UI_FX_TAKE_LISTEN);
	CHECK(ui.recording);
	CHECK(strcmp(glass(&ui, 65000), "rE C 104  ") == 0); /* 1:04 */
	/* MODE meanwhile changes nothing to see */
	CHECK_EQ(act(&ui, GESTURE_PRESS, M, 0, 0, 66000, true), UI_FX_DISPLAY);
	CHECK_EQ(ui.screen, UI_SCREEN_TIME);
	/* A press of ALARM ends it: the note is kept */
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 0, 0, 70000, true), UI_FX_TAKE_STOP | UI_FX_DISPLAY);
	CHECK(!ui.recording);
	CHECK(!ui.taking);
	/* That press is spent: no sync on its click, no new take on its long press */
	CHECK_EQ(act(&ui, GESTURE_CLICK, A, 1, 0, 70400, true), 0);
	act(&ui, GESTURE_PRESS, A, 0, 0, 80000, true);
	act(&ui, GESTURE_LONG, A, 0, 0, 80500, true);
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 0, 0, 90000, true), UI_FX_TAKE_STOP | UI_FX_DISPLAY);
	CHECK_EQ(act(&ui, GESTURE_LONG, A, 0, 0, 90500, true), 0);
	CHECK(!ui.recording);
	CHECK_EQ(act(&ui, GESTURE_LONG_END, A, 0, 700, 90700, true), 0);
}

static void quick_presses_after_a_stop(void)
{
	struct ui ui;

	/* A take runs; ALARM ends it, and a new press follows within 300 ms:
	 * the gesture machine then reports a series of two for them */
	ui_init(&ui);
	act(&ui, GESTURE_PRESS, A, 0, 0, 1000, true);
	act(&ui, GESTURE_LONG, A, 0, 0, 1500, true);
	act(&ui, GESTURE_LONG_END, A, 0, 600, 1600, true);
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 0, 0, 5000, true), UI_FX_TAKE_STOP | UI_FX_DISPLAY);
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 1, 0, 5200, true), UI_FX_TAKE_START | UI_FX_DISPLAY);
	/* ... short: dropped, no sync for a double click */
	CHECK_EQ(act(&ui, GESTURE_CLICK, A, 2, 0, 5600, true), UI_FX_TAKE_DROP);
	CHECK(!ui.taking);
	/* ... or long: it records */
	act(&ui, GESTURE_PRESS, A, 0, 0, 8000, true);
	act(&ui, GESTURE_LONG, A, 0, 0, 8500, true);
	act(&ui, GESTURE_LONG_END, A, 0, 600, 8600, true);
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 0, 0, 9000, true), UI_FX_TAKE_STOP | UI_FX_DISPLAY);
	act(&ui, GESTURE_PRESS, A, 1, 0, 9200, true);
	CHECK_EQ(act(&ui, GESTURE_LONG, A, 1, 0, 9700, true), UI_FX_TAKE_KEEP | UI_FX_DISPLAY);
	CHECK(ui.recording);
}

static void a_press_after_the_longest_take(void)
{
	struct ui ui;

	ui_init(&ui);
	act(&ui, GESTURE_PRESS, A, 0, 0, 1000, true);
	act(&ui, GESTURE_LONG, A, 0, 0, 1500, true);
	act(&ui, GESTURE_LONG_END, A, 0, 600, 1600, true);
	ui_take_ended(&ui);
	/* The wearer presses to end it: nothing starts, no sync */
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 0, 0, 122000, true), UI_FX_DISPLAY);
	CHECK(!ui.taking);
	CHECK_EQ(act(&ui, GESTURE_CLICK, A, 1, 0, 122400, true), 0);
	/* Afterwards ALARM works again; and once the glass went dark as well */
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 0, 0, 130000, true), UI_FX_TAKE_START | UI_FX_DISPLAY);
	act(&ui, GESTURE_CLICK, A, 1, 0, 130400, true);
	ui_take_ended(&ui);
	ui_display_off(&ui);
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 0, 0, 140000, false),
		 UI_FX_TAKE_START | UI_FX_DISPLAY);
}

/* Lot S1: the code a phone must type, "PA" and five digits; the glass lit
 * until the pairing ends, whatever the screen or a take */
static void the_code_of_a_pairing(void)
{
	struct ui ui;

	ui_init(&ui);
	ui_pairing(&ui, true, 4071);
	CHECK(strcmp(glass(&ui, 1000), "PA   04071") == 0);
	CHECK_EQ(ui_display_ms(&ui, 10000), 0);
	ui_pairing(&ui, true, 99999);
	CHECK(strcmp(glass(&ui, 1000), "PA   99999") == 0);
	/* Over a take that runs */
	act(&ui, GESTURE_PRESS, A, 0, 0, 1000, true);
	act(&ui, GESTURE_LONG, A, 0, 0, 1500, true);
	CHECK(strcmp(glass(&ui, 2000), "PA   99999") == 0);
	/* Ended: the take shows again, the glass goes dark as before */
	ui_pairing(&ui, false, 0);
	CHECK(strcmp(glass(&ui, 3000), "rE C 002  ") == 0);
	ui_init(&ui);
	ui_pairing(&ui, true, 12);
	ui_pairing(&ui, false, 0);
	CHECK_EQ(ui_display_ms(&ui, 10000), 10000);
}

static void a_take_that_could_not_begin(void)
{
	struct ui ui;

	/* The flash full at the press: the take ends before "rEC"; ALARM, still
	 * held, keeps nothing, and the next press is free */
	ui_init(&ui);
	act(&ui, GESTURE_PRESS, A, 0, 0, 1000, true);
	ui_take_ended(&ui);
	CHECK(!ui.ended_alone);
	CHECK_EQ(act(&ui, GESTURE_LONG, A, 0, 0, 1500, true), 0);
	CHECK(!ui.recording);
	act(&ui, GESTURE_LONG_END, A, 0, 600, 1600, true);
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 0, 0, 3000, true), UI_FX_TAKE_START | UI_FX_DISPLAY);
}

static void chords_without_alarm_keep_the_take(void)
{
	struct ui ui;

	ui_init(&ui);
	act(&ui, GESTURE_PRESS, A, 0, 0, 1000, true);
	act(&ui, GESTURE_LONG, A, 0, 0, 1500, true);
	act(&ui, GESTURE_LONG_END, A, 0, 600, 1600, true);
	CHECK_EQ(act(&ui, GESTURE_CHORD, (1U << L) | (1U << M), 0, 0, 5000, true), UI_FX_DISPLAY);
	CHECK(ui.recording);
	/* The counter stops at 9:59 */
	CHECK(strcmp(glass(&ui, 1000 + 700000), "rE C 959  ") == 0);
}

static void take_ended_by_its_length(void)
{
	struct ui ui;

	ui_init(&ui);
	act(&ui, GESTURE_PRESS, A, 0, 0, 1000, true);
	act(&ui, GESTURE_LONG, A, 0, 0, 1500, true);
	act(&ui, GESTURE_LONG_END, A, 0, 600, 1600, true);
	ui_take_ended(&ui);
	CHECK(!ui.recording);
	CHECK_EQ(ui_display_ms(&ui, 10000), 10000);
	CHECK(strcmp(glass(&ui, 200000), "WE23 81827") == 0);
	/* Once the glass has gone dark, the next press of ALARM starts a new take */
	ui_display_off(&ui);
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 0, 0, 200000, false), UI_FX_TAKE_START | UI_FX_DISPLAY);
}

static void reset_chord_pressed_apart(void)
{
	struct ui ui;
	uint32_t fx = 0;

	/* MODE stuck, ALARM pressed 3 s later and held to dictate: the chord
	 * drops what ALARM began, but arms no reset */
	ui_init(&ui);
	fx = act(&ui, GESTURE_CHORD, (1U << M) | (1U << A), 0, UI_RESET_SPREAD_MS + 1000, 5000,
		 true);
	CHECK(!ui.chord);
	for (uint32_t s = 1; s <= 12; s++) {
		fx |= act(&ui, GESTURE_CHORD_HOLD, (1U << M) | (1U << A), 0, 1000 * s, 5000 + 1000 * s,
			  true);
	}
	CHECK_EQ(fx & UI_FX_RESET, 0);
	CHECK(strstr(glass(&ui, 17000), "rESEt") == NULL);
	/* Pressed within 2 s of each other, as on purpose: armed */
	ui_init(&ui);
	(void)act(&ui, GESTURE_CHORD, (1U << M) | (1U << A), 0, UI_RESET_SPREAD_MS, 5000, true);
	CHECK(ui.chord);
}

static void reset_chord(void)
{
	struct ui ui;
	uint32_t fx = 0;

	ui_init(&ui);
	/* ALARM first: a take starts, then MODE joins: the chord drops it */
	act(&ui, GESTURE_PRESS, A, 0, 0, 1000, true);
	CHECK_EQ(act(&ui, GESTURE_CHORD, (1U << M) | (1U << A), 0, 0, 1300, true),
		 UI_FX_TAKE_DROP | UI_FX_DISPLAY);
	CHECK(!ui.taking);
	CHECK(ui.chord);
	CHECK(strcmp(glass(&ui, 1300), "  10 rESEt") == 0);
	for (uint32_t s = 1; s <= 10; s++) {
		fx |= act(&ui, GESTURE_CHORD_HOLD, (1U << M) | (1U << A), 0, 1000 * s, 1300 + 1000 * s,
			  true);
		if (s == 3) {
			CHECK(strcmp(glass(&ui, 4300), "   7 rESEt") == 0);
		}
	}
	CHECK(fx & UI_FX_RESET);
	/* Once only */
	CHECK_EQ(act(&ui, GESTURE_CHORD_HOLD, (1U << M) | (1U << A), 0, 11000, 12300, true) &
		 UI_FX_RESET, 0);
	CHECK_EQ(act(&ui, GESTURE_CHORD_END, (1U << M) | (1U << A), 0, 11500, 12800, true),
		 UI_FX_DISPLAY);
	CHECK(!ui.chord);
}

static void the_temperature(void)
{
	struct ui ui;
	struct ui_frame f;
	struct ui_context c = ctx_at(0, true);
	static const struct {
		int32_t cc;
		const char *glass;
	} cases[] = {
		/* '#' is the degree sign on the glass, before the C */
		{ 2430, "tE P  24#C" },  /* whole degrees, rounded */
		{ 2450, "tE P  25#C" },
		{ 500, "tE P   5#C" },
		{ 40, "tE P   0#C" },
		{ -40, "tE P   0#C" },
		{ -520, "tE P  -5#C" },
		{ -1230, "tE P -12#C" },
		{ 8500, "tE P  85#C" },  /* the top of the nRF52840's range */
		{ -4000, "tE P -40#C" }, /* its bottom */
	};

	ui_init(&ui);
	ui.screen = UI_SCREEN_TEMP;
	c.temp_known = true;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		c.temp_c = ui_temp_follow(0, false, cases[i].cc);
		ui_render(&ui, &c, &f);
		f.text[10] = '\0';
		CHECK(strcmp(f.text, cases[i].glass) == 0);
	}
	/* No reading, or an old one: dashes, the unit kept */
	c.temp_known = false;
	ui_render(&ui, &c, &f);
	f.text[10] = '\0';
	CHECK(strcmp(f.text, "tE P  --#C") == 0);
	/* Lit for the usual time */
	CHECK_EQ(ui_display_ms(&ui, 10000), 10000);
}

/* The degree shown holds until a reading is more than 0.75 degC away: a whole
 * degree with the quarters of the sensor (2026-09-25: no more flips
 * between two neighbours) */
static void the_temperature_holds(void)
{
	static const int32_t on_the_half[] = { 2150, 2125, 2150, 2175, 2150, 2125, 2150 };
	static const int16_t shown[] = { 22, 22, 22, 22, 22, 22, 22 };
	int16_t t;

	/* Nothing before, or an old reading: rounded, half away from zero */
	CHECK_EQ(ui_temp_follow(0, false, 2125), 21);
	CHECK_EQ(ui_temp_follow(0, false, 2150), 22);
	CHECK_EQ(ui_temp_follow(30, false, 2149), 21);
	CHECK_EQ(ui_temp_follow(0, false, -25), 0);
	CHECK_EQ(ui_temp_follow(0, false, -50), -1);
	/* 21 shown: it stays while the reading is within 0.75 degC of it */
	CHECK_EQ(ui_temp_follow(21, true, 2150), 21);
	CHECK_EQ(ui_temp_follow(21, true, 2175), 21);
	CHECK_EQ(ui_temp_follow(21, true, 2176), 22);
	CHECK_EQ(ui_temp_follow(21, true, 2200), 22);
	CHECK_EQ(ui_temp_follow(21, true, 2025), 21);
	CHECK_EQ(ui_temp_follow(21, true, 2000), 20);
	CHECK_EQ(ui_temp_follow(21, true, 2300), 23); /* a jump: the reading, rounded */
	/* Below zero the same */
	CHECK_EQ(ui_temp_follow(-1, true, -25), -1);
	CHECK_EQ(ui_temp_follow(-1, true, 0), 0);
	CHECK_EQ(ui_temp_follow(-1, true, -175), -1);
	CHECK_EQ(ui_temp_follow(-1, true, -200), -2);
	/* A case sitting on 21.5 degC, the sensor a quarter either way: the glass
	 * does not flip */
	t = ui_temp_follow(0, false, on_the_half[0]);
	for (size_t i = 0; i < sizeof(on_the_half) / sizeof(on_the_half[0]); i++) {
		t = ui_temp_follow(t, true, on_the_half[i]);
		CHECK_EQ(t, shown[i]);
	}
}

static void the_reset_runs(void)
{
	struct ui ui;

	ui_init(&ui);
	act(&ui, GESTURE_CHORD, (1U << M) | (1U << A), 0, 0, 1000, true);
	CHECK(act(&ui, GESTURE_CHORD_HOLD, (1U << M) | (1U << A), 0, 10000, 11000, true) &
	      UI_FX_RESET);
	/* The loop starts the reset: "rESEt" alone, blinking twice a second, no
	 * counter, the glass lit until the restart (2026-09-25) */
	ui_resetting(&ui);
	CHECK(strcmp(glass(&ui, 11000), "     rESEt") == 0);
	CHECK(strcmp(glass(&ui, 11500), "          ") == 0);
	CHECK(strcmp(glass(&ui, 12000), "     rESEt") == 0);
	CHECK_EQ(ui_tick_ms(&ui), UI_BLINK_MS);
	CHECK_EQ(ui_display_ms(&ui, 10000), 0);
	/* Held on: nothing more, not a second reset */
	CHECK_EQ(act(&ui, GESTURE_CHORD_HOLD, (1U << M) | (1U << A), 0, 11000, 12000, true), 0);
	CHECK(ui.chord);
	/* Let go: the chord ends, which the loop waits for to restart */
	CHECK_EQ(act(&ui, GESTURE_CHORD_END, (1U << M) | (1U << A), 0, 12000, 13000, true), 0);
	CHECK(!ui.chord);
	/* Nothing else acts meanwhile: no take, no light, no screen */
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 0, 0, 13200, true), 0);
	CHECK_EQ(act(&ui, GESTURE_PRESS, L, 0, 0, 13300, true), 0);
	CHECK_EQ(act(&ui, GESTURE_PRESS, M, 0, 0, 13400, true), 0);
	CHECK(!ui.taking);
	CHECK(strcmp(glass(&ui, 14000), "     rESEt") == 0);
}

static void chord_keeps_a_running_take(void)
{
	struct ui ui;

	ui_init(&ui);
	act(&ui, GESTURE_PRESS, A, 0, 0, 1000, true);
	act(&ui, GESTURE_LONG, A, 0, 0, 1500, true);
	act(&ui, GESTURE_LONG_END, A, 0, 700, 1700, true);
	/* "rEC" runs on; MODE then ALARM together: the note is kept, as by a press */
	CHECK_EQ(act(&ui, GESTURE_CHORD, (1U << M) | (1U << A), 0, 0, 9000, true),
		 UI_FX_TAKE_STOP | UI_FX_DISPLAY);
	CHECK(!ui.recording);
}

static void a_tap_then_a_chord_drops_the_take(void)
{
	struct ui ui;

	/* ALARM tapped, then LIGHT and MODE together within 300 ms: the gesture
	 * machine drops the tap's click; the chord drops the take */
	ui_init(&ui);
	act(&ui, GESTURE_PRESS, A, 0, 0, 1000, true);
	CHECK_EQ(act(&ui, GESTURE_CHORD, (1U << L) | (1U << M), 0, 0, 1200, true),
		 UI_FX_TAKE_DROP | UI_FX_DISPLAY);
	CHECK(!ui.taking);
	/* The next press of ALARM starts a take of its own */
	CHECK_EQ(act(&ui, GESTURE_PRESS, A, 0, 0, 3000, true), UI_FX_TAKE_START | UI_FX_DISPLAY);
}

static void other_chords_reset_nothing(void)
{
	struct ui ui;

	ui_init(&ui);
	CHECK_EQ(act(&ui, GESTURE_CHORD, (1U << L) | (1U << M), 0, 0, 1000, true), UI_FX_DISPLAY);
	CHECK(!ui.chord);
	CHECK_EQ(act(&ui, GESTURE_CHORD_HOLD, (1U << L) | (1U << M), 0, 10000, 11000, true), 0);
}

static void time_in_twenty_four_hours_and_unset(void)
{
	struct ui ui;
	struct ui_frame f;
	struct ui_context c = ctx_at(0, true);

	ui_init(&ui);
	c.h24 = true;
	ui_render(&ui, &c, &f);
	f.text[10] = '\0';
	CHECK(strcmp(f.text, "WE23201507") == 0);
	CHECK(f.h24);
	CHECK(!f.pm);
	CHECK(f.colon);
	c.h24 = false;
	ui_render(&ui, &c, &f);
	CHECK(f.pm);
	/* Unset until the phone gives the time */
	c.time_set = false;
	ui_render(&ui, &c, &f);
	f.text[10] = '\0';
	CHECK(strcmp(f.text, "     -----") == 0);
	CHECK(!f.colon);
	/* The other screens need no time */
	ui.screen = UI_SCREEN_LINK;
	ui_render(&ui, &c, &f);
	f.text[10] = '\0';
	CHECK(strcmp(f.text, "bL E 3 OFF") == 0);
}

int main(void)
{
	RUN(a_dark_glass_only_lights);
	RUN(mode_walks_the_screens);
	RUN(nothing_is_set_on_the_watch);
	RUN(light_lights_the_led);
	RUN(alarm_click_syncs_and_drops_the_take);
	RUN(alarm_click_on_a_dark_glass_only_lights);
	RUN(alarm_held_records_until_a_press);
	RUN(quick_presses_after_a_stop);
	RUN(a_press_after_the_longest_take);
	RUN(a_take_that_could_not_begin);
	RUN(chords_without_alarm_keep_the_take);
	RUN(take_ended_by_its_length);
	RUN(reset_chord);
	RUN(reset_chord_pressed_apart);
	RUN(the_reset_runs);
	RUN(the_temperature);
	RUN(the_temperature_holds);
	RUN(the_code_of_a_pairing);
	RUN(chord_keeps_a_running_take);
	RUN(a_tap_then_a_chord_drops_the_take);
	RUN(other_chords_reset_nothing);
	RUN(time_in_twenty_four_hours_and_unset);
	return harness_report("ui");
}
