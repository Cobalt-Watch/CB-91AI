/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the stuck button (firmware/watch/src/stuck.c, lot D5): taken
 * for stuck past any gesture, probed once a minute, a button again once let
 * go; every other button untouched meanwhile; a chord taken whole, so that no
 * chord of two is left (the reset's); the uptime wrapping.
 */

#include "harness.h"
#include "stuck.h"

#define LIGHT 0U
#define MODE  1U
#define ALARM 2U

static void a_gesture_is_never_stuck(void)
{
	struct stuck s;
	uint32_t wait = 0;

	stuck_init(&s, 3);
	CHECK(!stuck_next(&s, 0, &wait));
	/* A take held down its 2 min, then the reset: 10 s and a minute */
	CHECK_EQ(stuck_update(&s, 1U << ALARM, 1000), 0);
	CHECK(stuck_next(&s, 1000, &wait));
	CHECK_EQ(wait, STUCK_HELD_MS);
	CHECK_EQ(stuck_update(&s, 1U << ALARM, 1000 + 120000), 0);
	CHECK_EQ(stuck_update(&s, 0, 1000 + 121000), 0);
	CHECK(!stuck_next(&s, 1000 + 121000, &wait));
	CHECK_EQ(stuck_update(&s, (1U << ALARM) | (1U << MODE), 200000), 0);
	CHECK_EQ(stuck_update(&s, (1U << ALARM) | (1U << MODE), 200000 + 70000), 0);
	CHECK_EQ(s.stuck, 0);
	/* Held on and on: stuck at 3 min to the ms, not before */
	CHECK_EQ(stuck_update(&s, 1U << MODE, 200000 + STUCK_HELD_MS - 1), 0);
	CHECK_EQ(stuck_update(&s, 1U << MODE, 200000 + STUCK_HELD_MS), 1U << MODE);
	CHECK_EQ(s.stuck, 1U << MODE);
	CHECK_EQ(s.down & (1U << MODE), 0);
}

static void probed_until_let_go(void)
{
	struct stuck s;
	uint32_t t = 5000;
	uint32_t wait = 0;

	stuck_init(&s, 3);
	(void)stuck_update(&s, 1U << MODE, t);
	CHECK_EQ(stuck_update(&s, 1U << MODE, t + STUCK_HELD_MS), 1U << MODE);
	t += STUCK_HELD_MS;
	/* Its level no longer counts: the input is disconnected */
	CHECK_EQ(stuck_update(&s, 0, t + 10), 0);
	CHECK_EQ(stuck_update(&s, 1U << MODE, t + 20), 0);
	CHECK_EQ(s.stuck, 1U << MODE);
	/* A probe a minute later, not before */
	CHECK_EQ(stuck_due(&s, t + STUCK_PROBE_MS - 1), 0);
	CHECK(stuck_next(&s, t + 30000, &wait));
	CHECK_EQ(wait, STUCK_PROBE_MS - 30000);
	CHECK_EQ(stuck_due(&s, t + STUCK_PROBE_MS), 1U << MODE);
	/* Still down: another minute */
	stuck_probed(&s, MODE, true, t + STUCK_PROBE_MS);
	CHECK_EQ(stuck_due(&s, t + STUCK_PROBE_MS + 1), 0);
	CHECK_EQ(stuck_due(&s, t + 2 * STUCK_PROBE_MS), 1U << MODE);
	/* Let go: a button like the others, a new press counts from zero */
	stuck_probed(&s, MODE, false, t + 2 * STUCK_PROBE_MS);
	CHECK_EQ(s.stuck, 0);
	CHECK(!stuck_next(&s, t + 2 * STUCK_PROBE_MS, &wait));
	CHECK_EQ(stuck_update(&s, 1U << MODE, t + 2 * STUCK_PROBE_MS + 5), 0);
	CHECK_EQ(s.since[MODE], t + 2 * STUCK_PROBE_MS + 5);
	/* A probe of a button that is not stuck, or out of range: nothing */
	stuck_probed(&s, ALARM, false, t);
	stuck_probed(&s, 7, true, t);
	CHECK_EQ(s.stuck, 0);
}

static void the_others_still_work(void)
{
	struct stuck s;
	uint32_t wait = 0;

	stuck_init(&s, 3);
	(void)stuck_update(&s, 1U << MODE, 0);
	CHECK_EQ(stuck_update(&s, 1U << MODE, STUCK_HELD_MS), 1U << MODE);
	/* ALARM pressed and let go meanwhile, as ever */
	CHECK_EQ(stuck_update(&s, (1U << MODE) | (1U << ALARM), STUCK_HELD_MS + 100), 0);
	CHECK_EQ(s.down, 1U << ALARM);
	CHECK_EQ(stuck_update(&s, 1U << MODE, STUCK_HELD_MS + 900), 0);
	CHECK_EQ(s.down, 0);
	/* The next decision: MODE's probe */
	CHECK(stuck_next(&s, STUCK_HELD_MS + 900, &wait));
	CHECK_EQ(wait, STUCK_PROBE_MS - 900);
	/* Two stuck at once, probed each on its own */
	(void)stuck_update(&s, 1U << ALARM, STUCK_HELD_MS + 1000);
	CHECK_EQ(stuck_update(&s, 1U << ALARM, 2 * STUCK_HELD_MS + 1000), 1U << ALARM);
	CHECK_EQ(s.stuck, (1U << MODE) | (1U << ALARM));
	CHECK_EQ(stuck_due(&s, 2 * STUCK_HELD_MS + 1000), 1U << MODE);
}

static void a_chord_goes_whole(void)
{
	struct stuck s;
	const uint8_t all = (1U << LIGHT) | (1U << MODE) | (1U << ALARM);

	/* In a pocket: LIGHT held, then MODE and ALARM 20 s later, all three on.
	 * Hidden alone at its 3 min, LIGHT would leave MODE and ALARM, a chord
	 * the gesture machine starts again: the reset, 10 s later */
	stuck_init(&s, 3);
	(void)stuck_update(&s, 1U << LIGHT, 0);
	(void)stuck_update(&s, all, 20000);
	CHECK_EQ(stuck_update(&s, all, STUCK_HELD_MS - 1), 0);
	CHECK_EQ(stuck_update(&s, all, STUCK_HELD_MS), all);
	CHECK_EQ(s.stuck, all);
	CHECK_EQ(s.down, 0);
	/* Then probed each on its own, and let go one by one */
	CHECK_EQ(stuck_due(&s, STUCK_HELD_MS + STUCK_PROBE_MS), all);
	stuck_probed(&s, MODE, false, STUCK_HELD_MS + STUCK_PROBE_MS);
	CHECK_EQ(s.stuck, (1U << LIGHT) | (1U << ALARM));
	/* A button stuck alone still goes alone: nothing else was down */
	stuck_init(&s, 3);
	(void)stuck_update(&s, 1U << MODE, 0);
	CHECK_EQ(stuck_update(&s, 1U << MODE, STUCK_HELD_MS), 1U << MODE);
	CHECK_EQ(s.stuck, 1U << MODE);
}

static void the_uptime_wraps(void)
{
	struct stuck s;
	const uint32_t t = 0xffffffffU - 60000U;
	uint32_t wait = 0;

	stuck_init(&s, 3);
	(void)stuck_update(&s, 1U << MODE, t);
	CHECK(stuck_next(&s, t + 1000, &wait));
	CHECK_EQ(wait, STUCK_HELD_MS - 1000);
	CHECK_EQ(stuck_update(&s, 1U << MODE, t + STUCK_HELD_MS - 1), 0);
	CHECK_EQ(stuck_update(&s, 1U << MODE, t + STUCK_HELD_MS), 1U << MODE);
	CHECK_EQ(stuck_due(&s, t + STUCK_HELD_MS + STUCK_PROBE_MS), 1U << MODE);
	/* No more buttons than it holds */
	stuck_init(&s, 12);
	CHECK_EQ(s.buttons, STUCK_MAX);
}

int main(void)
{
	RUN(a_gesture_is_never_stuck);
	RUN(probed_until_let_go);
	RUN(the_others_still_work);
	RUN(a_chord_goes_whole);
	RUN(the_uptime_wraps);
	return harness_report("stuck");
}
