/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the button gestures (firmware/lib/gesture.c, lot D3).
 *
 * The simulator calls the machine the way the watch will: at every edge of a
 * script of levels (the edge interrupt) and whenever the wait it asked for is
 * over (the timer), and notes when each gesture is reported. Times in the
 * scripts are relative to a start that can sit just before the counter wraps.
 */

#include <string.h>

#include "gesture.h"
#include "harness.h"

#define B0 0x1U
#define B1 0x2U
#define B2 0x4U

struct step {
	uint32_t at;     /* ms from the start */
	uint32_t levels; /* from then on */
};

struct seen {
	uint32_t at; /* ms from the start */
	struct gesture g;
};

#define SEEN_MAX 64

static const struct gesture_cfg v1 = GESTURE_CFG_V1;

static size_t simulate(uint32_t start, uint8_t buttons, const struct step *script, size_t steps,
		       uint32_t until, struct seen *seen)
{
	struct gesture_state s;
	struct gesture out[GESTURE_OUT_MAX(GESTURE_BUTTONS_MAX)];
	uint32_t rel = 0, levels = 0, wait;
	size_t k = 0, n = 0;
	unsigned int calls = 0;

	gesture_init(&s, &v1, buttons);
	for (;;) {
		uint32_t next = until;
		bool is_edge = false;

		/* A machine that asks to be called again and again without moving on
		 * would spin the watch: count the calls */
		if (++calls > 1000) {
			CHECK(calls <= 1000);
			break;
		}

		if (k < steps && script[k].at <= next) {
			next = script[k].at;
			is_edge = true;
		}
		if (gesture_next(&s, start + rel, &wait) && rel + wait < next) {
			next = rel + wait;
			is_edge = false;
		}
		if (next >= until && !is_edge) {
			break;
		}
		rel = next;
		while (is_edge && k < steps && script[k].at == rel) {
			levels = script[k++].levels;
		}
		const size_t got = gesture_update(&s, levels, start + rel, out);

		CHECK(got <= GESTURE_OUT_MAX(buttons));
		for (size_t i = 0; i < got && n < SEEN_MAX; i++) {
			seen[n].at = rel;
			seen[n].g = out[i];
			n++;
		}
	}
	return n;
}

#define EXPECT(seen, i, when, kind, btn, cnt, dur)                                                 \
	do {                                                                                       \
		CHECK_EQ((seen)[i].at, (when));                                                    \
		CHECK_EQ((seen)[i].g.type, (kind));                                                \
		CHECK_EQ((seen)[i].g.button, (btn));                                               \
		CHECK_EQ((seen)[i].g.count, (cnt));                                                \
		CHECK_EQ((seen)[i].g.ms, (dur));                                                   \
	} while (0)

static void single_click(void)
{
	const struct step script[] = { { 0, B0 }, { 100, 0 } };
	struct seen seen[SEEN_MAX];
	const size_t n = simulate(1000, 3, script, 2, 5000, seen);

	CHECK_EQ(n, 2);
	/* Reported once the level held 50 ms, dated by the edge */
	EXPECT(seen, 0, 50, GESTURE_PRESS, 0, 0, 0);
	/* 300 ms after the release: no second click came */
	EXPECT(seen, 1, 400, GESTURE_CLICK, 0, 1, 0);
}

static void double_click(void)
{
	const struct step script[] = { { 0, B1 }, { 100, 0 }, { 250, B1 }, { 350, 0 } };
	struct seen seen[SEEN_MAX];
	const size_t n = simulate(0, 3, script, 4, 5000, seen);

	CHECK_EQ(n, 3);
	EXPECT(seen, 0, 50, GESTURE_PRESS, 1, 0, 0);
	/* The second press says one click is behind it */
	EXPECT(seen, 1, 300, GESTURE_PRESS, 1, 1, 0);
	EXPECT(seen, 2, 650, GESTURE_CLICK, 1, 2, 0);
}

static void triple_click(void)
{
	const struct step script[] = { { 0, B2 },   { 80, 0 },  { 200, B2 },
				       { 280, 0 }, { 400, B2 }, { 480, 0 } };
	struct seen seen[SEEN_MAX];
	const size_t n = simulate(0, 3, script, 6, 5000, seen);

	CHECK_EQ(n, 4);
	EXPECT(seen, 2, 450, GESTURE_PRESS, 2, 2, 0);
	EXPECT(seen, 3, 780, GESTURE_CLICK, 2, 3, 0);
}

static void more_than_three_clicks_count_three(void)
{
	const struct step script[] = { { 0, B0 },   { 60, 0 },  { 150, B0 }, { 210, 0 },
				       { 300, B0 }, { 360, 0 }, { 450, B0 }, { 510, 0 } };
	struct seen seen[SEEN_MAX];
	const size_t n = simulate(0, 1, script, 8, 5000, seen);

	CHECK_EQ(n, 5);
	EXPECT(seen, 3, 500, GESTURE_PRESS, 0, 3, 0);
	EXPECT(seen, 4, 810, GESTURE_CLICK, 0, 3, 0);
}

static void long_press(void)
{
	const struct step script[] = { { 0, B2 }, { 1200, 0 } };
	struct seen seen[SEEN_MAX];
	const size_t n = simulate(0, 3, script, 2, 5000, seen);

	CHECK_EQ(n, 3);
	EXPECT(seen, 0, 50, GESTURE_PRESS, 2, 0, 0);
	EXPECT(seen, 1, 500, GESTURE_LONG, 2, 0, 0);
	/* Held 1.2 s, from edge to edge */
	EXPECT(seen, 2, 1250, GESTURE_LONG_END, 2, 0, 1200);
}

static void bounces_on_press_and_release(void)
{
	const struct step script[] = { { 0, B0 },   { 3, 0 },   { 6, B0 },   { 9, 0 },
				       { 12, B0 },  { 212, 0 }, { 214, B0 }, { 217, 0 } };
	struct seen seen[SEEN_MAX];
	const size_t n = simulate(0, 1, script, 8, 5000, seen);

	CHECK_EQ(n, 2);
	/* The press counts from its last edge, the release from its last edge */
	EXPECT(seen, 0, 62, GESTURE_PRESS, 0, 0, 0);
	EXPECT(seen, 1, 517, GESTURE_CLICK, 0, 1, 0);
}

static void short_bounce_is_no_press(void)
{
	const struct step script[] = { { 0, B0 }, { 30, 0 } };
	struct seen seen[SEEN_MAX];

	CHECK_EQ(simulate(0, 1, script, 2, 5000, seen), 0);
}

static void release_just_before_the_long_press(void)
{
	/* Released at 480 ms: when the long press would fall due, at 500 ms, the
	 * release is still settling. It was a click. */
	const struct step script[] = { { 0, B0 }, { 480, 0 } };
	struct seen seen[SEEN_MAX];
	const size_t n = simulate(0, 1, script, 2, 5000, seen);

	CHECK_EQ(n, 2);
	EXPECT(seen, 0, 50, GESTURE_PRESS, 0, 0, 0);
	EXPECT(seen, 1, 780, GESTURE_CLICK, 0, 1, 0);
}

static void press_just_before_the_window_closes(void)
{
	/* The second press comes 280 ms after the release: when the window closes,
	 * at 300 ms, it is still settling. It was a double click. */
	const struct step script[] = { { 0, B0 }, { 100, 0 }, { 380, B0 }, { 450, 0 } };
	struct seen seen[SEEN_MAX];
	const size_t n = simulate(0, 1, script, 4, 5000, seen);

	CHECK_EQ(n, 3);
	EXPECT(seen, 1, 430, GESTURE_PRESS, 0, 1, 0);
	EXPECT(seen, 2, 750, GESTURE_CLICK, 0, 2, 0);
}

static void click_then_long_press(void)
{
	/* As in the V1, the click before a long press is dropped, and said */
	const struct step script[] = { { 0, B2 }, { 100, 0 }, { 250, B2 }, { 1500, 0 } };
	struct seen seen[SEEN_MAX];
	const size_t n = simulate(0, 3, script, 4, 5000, seen);

	CHECK_EQ(n, 4);
	EXPECT(seen, 1, 300, GESTURE_PRESS, 2, 1, 0);
	EXPECT(seen, 2, 750, GESTURE_LONG, 2, 1, 0);
	EXPECT(seen, 3, 1550, GESTURE_LONG_END, 2, 0, 1250);
}

static void two_buttons_one_after_the_other(void)
{
	/* B0 released before B1 is pressed: no chord, two clicks */
	const struct step script[] = { { 0, B0 }, { 100, 0 }, { 200, B1 }, { 300, 0 } };
	struct seen seen[SEEN_MAX];
	const size_t n = simulate(0, 3, script, 4, 5000, seen);

	CHECK_EQ(n, 4);
	EXPECT(seen, 0, 50, GESTURE_PRESS, 0, 0, 0);
	EXPECT(seen, 1, 250, GESTURE_PRESS, 1, 0, 0);
	EXPECT(seen, 2, 400, GESTURE_CLICK, 0, 1, 0);
	EXPECT(seen, 3, 600, GESTURE_CLICK, 1, 1, 0);
}

static void reset_chord_held_ten_seconds(void)
{
	/* The reset of phase S: MODE (1) held, then ALARM (2) too, for 10 s. The
	 * long press of MODE is reported before ALARM joins; then only the chord,
	 * every second; no end of long press for MODE, which the chord cancelled. */
	const struct step script[] = { { 0, B1 }, { 1000, B1 | B2 }, { 11500, B1 }, { 12000, 0 } };
	struct seen seen[SEEN_MAX];
	const size_t n = simulate(0, 3, script, 4, 15000, seen);

	/* ... and nothing when MODE, left alone, is released: the chord muted it */
	CHECK_EQ(n, 14);
	EXPECT(seen, 0, 50, GESTURE_PRESS, 1, 0, 0);
	EXPECT(seen, 1, 500, GESTURE_LONG, 1, 0, 0);
	/* ALARM pressed 1 s after MODE: the spread the chord carries */
	EXPECT(seen, 2, 1050, GESTURE_CHORD, B1 | B2, 0, 1000);
	for (unsigned int i = 1; i <= 10; i++) {
		EXPECT(seen, 2 + i, 1000 + 1000 * i, GESTURE_CHORD_HOLD, B1 | B2, 0, 1000 * i);
	}
	EXPECT(seen, 13, 11550, GESTURE_CHORD_END, B1 | B2, 0, 10500);
}

static void a_chord_says_its_spread(void)
{
	/* MODE held (stuck), ALARM pressed 60 s later: a chord 60 s apart, which
	 * the reset refuses (ui.c) */
	const struct step late[] = { { 0, B1 }, { 60000, B1 | B2 }, { 61000, 0 } };
	/* LIGHT, then MODE and ALARM together, LIGHT let go: the chord of two
	 * that is left carries the spread of its own presses */
	const struct step shrunk[] = { { 0, B0 }, { 5000, B0 | B1 | B2 }, { 6000, B1 | B2 },
				       { 7000, 0 } };
	struct seen seen[SEEN_MAX];
	size_t n = simulate(0, 3, late, 3, 65000, seen);

	CHECK(n >= 3);
	EXPECT(seen, 2, 60050, GESTURE_CHORD, B1 | B2, 0, 60000);
	n = simulate(0, 3, shrunk, 4, 10000, seen);
	EXPECT(seen, 2, 5050, GESTURE_CHORD, B0 | B1 | B2, 0, 5000);
	{
		bool found = false;

		for (size_t i = 3; i < n; i++) {
			if (seen[i].g.type == GESTURE_CHORD && seen[i].g.button == (B1 | B2)) {
				CHECK_EQ(seen[i].at, 6050);
				CHECK_EQ(seen[i].g.ms, 0);
				found = true;
			}
		}
		CHECK(found);
	}
}

static void chord_pressed_at_once(void)
{
	const struct step script[] = { { 0, B0 | B2 }, { 700, 0 } };
	struct seen seen[SEEN_MAX];
	const size_t n = simulate(0, 3, script, 2, 5000, seen);

	CHECK_EQ(n, 2);
	/* No press of either button: the chord from the start */
	EXPECT(seen, 0, 50, GESTURE_CHORD, B0 | B2, 0, 0);
	EXPECT(seen, 1, 750, GESTURE_CHORD_END, B0 | B2, 0, 700);
}

static void clicks_after_a_chord(void)
{
	const struct step script[] = { { 0, B0 | B1 }, { 700, B1 }, { 800, 0 },
				       { 1000, B0 },   { 1100, 0 } };
	struct seen seen[SEEN_MAX];
	const size_t n = simulate(0, 3, script, 5, 5000, seen);

	CHECK_EQ(n, 4);
	EXPECT(seen, 0, 50, GESTURE_CHORD, B0 | B1, 0, 0);
	EXPECT(seen, 1, 750, GESTURE_CHORD_END, B0 | B1, 0, 700);
	/* B1 released alone at 800: still muted, nothing */
	EXPECT(seen, 2, 1050, GESTURE_PRESS, 0, 0, 0);
	EXPECT(seen, 3, 1400, GESTURE_CLICK, 0, 1, 0);
}

static void across_the_wrap_of_the_counter(void)
{
	const struct step script[] = { { 0, B0 }, { 100, 0 }, { 250, B0 }, { 1500, 0 } };
	struct seen seen[SEEN_MAX];
	/* 0xffffff00: the counter wraps 256 ms after the start */
	const size_t n = simulate(0xffffff00U, 1, script, 4, 5000, seen);

	CHECK_EQ(n, 4);
	EXPECT(seen, 0, 50, GESTURE_PRESS, 0, 0, 0);
	EXPECT(seen, 1, 300, GESTURE_PRESS, 0, 1, 0);
	EXPECT(seen, 2, 750, GESTURE_LONG, 0, 1, 0);
	EXPECT(seen, 3, 1550, GESTURE_LONG_END, 0, 0, 1250);
}

static void calls_only_on_edges(void)
{
	/* A caller that forgot the timer: the long press is still told, late, at
	 * the next call, and dated by the edges */
	struct gesture_state s;
	struct gesture out[GESTURE_OUT_MAX(1)];

	gesture_init(&s, &v1, 1);
	CHECK_EQ(gesture_update(&s, 1, 0, out), 0);
	CHECK_EQ(gesture_update(&s, 1, 60, out), 1);
	CHECK_EQ(out[0].type, GESTURE_PRESS);
	/* Released at 700 ms: the press was long, whatever the call */
	CHECK_EQ(gesture_update(&s, 0, 700, out), 1);
	CHECK_EQ(out[0].type, GESTURE_LONG);
	CHECK_EQ(gesture_update(&s, 0, 760, out), 1);
	CHECK_EQ(out[0].type, GESTURE_LONG_END);
	CHECK_EQ(out[0].ms, 700);
}

static void waits_asked_for(void)
{
	struct gesture_state s;
	struct gesture out[GESTURE_OUT_MAX(1)];
	uint32_t wait = 0;

	gesture_init(&s, &v1, 1);
	CHECK(!gesture_next(&s, 0, &wait));
	(void)gesture_update(&s, 1, 0, out);
	CHECK(gesture_next(&s, 0, &wait));
	CHECK_EQ(wait, 50); /* the debounce */
	(void)gesture_update(&s, 1, 50, out);
	CHECK(gesture_next(&s, 50, &wait));
	CHECK_EQ(wait, 450); /* the long press, from the edge at 0 */
	/* Overdue: at once */
	CHECK(gesture_next(&s, 600, &wait));
	CHECK_EQ(wait, 0);
}

int main(void)
{
	RUN(single_click);
	RUN(double_click);
	RUN(triple_click);
	RUN(more_than_three_clicks_count_three);
	RUN(long_press);
	RUN(bounces_on_press_and_release);
	RUN(short_bounce_is_no_press);
	RUN(release_just_before_the_long_press);
	RUN(press_just_before_the_window_closes);
	RUN(click_then_long_press);
	RUN(two_buttons_one_after_the_other);
	RUN(reset_chord_held_ten_seconds);
	RUN(a_chord_says_its_spread);
	RUN(chord_pressed_at_once);
	RUN(clicks_after_a_chord);
	RUN(across_the_wrap_of_the_counter);
	RUN(calls_only_on_edges);
	RUN(waits_asked_for);
	return harness_report("gesture");
}
