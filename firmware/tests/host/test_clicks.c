/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the clicks of the buttons in the frames of a take
 * (firmware/watch/src/clicks.c, lot E1, its finishing touches): the map of the
 * uptime onto the frames, the frames deaf around a press or a release, and
 * the release of ALARM after the latch.
 */

#include <stdbool.h>

#include "clicks.h"
#include "harness.h"

static uint32_t frame_of(const struct clicks *c, uint32_t at_ms)
{
	uint32_t f = 0xffffffffU;

	CHECK(clicks_frame(c, at_ms, &f));
	return f;
}

static void before_the_first_block(void)
{
	struct clicks c;
	uint32_t f;

	clicks_init(&c, 175);
	/* Nothing maps yet: the caller tries again at the next block */
	CHECK(!clicks_frame(&c, 1000, &f));
	CHECK(!clicks_edge(&c, 1000, true));
	CHECK(!clicks_listen(&c, 1000));
	/* The first frames are deaf all the same, until ALARM is released */
	CHECK(clicks_deaf(&c, 0));
	CHECK(clicks_deaf(&c, 174));
	CHECK(!clicks_deaf(&c, 175));
}

static void the_map_from_the_blocks(void)
{
	struct clicks c;

	clicks_init(&c, 0);
	/* Ten frames came at 1000 ms: frame 0 started at 900 at the latest */
	clicks_block(&c, 1000, 10);
	CHECK_EQ(c.t0_ms, 900);
	CHECK_EQ(frame_of(&c, 900), 0);
	CHECK_EQ(frame_of(&c, 909), 0);
	CHECK_EQ(frame_of(&c, 910), 1);
	CHECK_EQ(frame_of(&c, 999), 9);
	CHECK_EQ(frame_of(&c, 1000), 10);
	/* A block taken late tells nothing new */
	clicks_block(&c, 1150, 20);
	CHECK_EQ(c.t0_ms, 900);
	/* One taken sooner after its end moves the map */
	clicks_block(&c, 1195, 30);
	CHECK_EQ(c.t0_ms, 895);
	CHECK_EQ(frame_of(&c, 1195), 30);
	/* Before the take: its first frame */
	CHECK_EQ(frame_of(&c, 500), 0);
}

static void a_press_and_a_release(void)
{
	struct clicks c;

	clicks_init(&c, 0);
	clicks_block(&c, 10000, 100); /* frame 0 at 9000 ms */
	/* LIGHT pressed at frame 80: from 300 ms before to 100 ms after */
	CHECK(clicks_edge(&c, 9800, true));
	CHECK(!clicks_deaf(&c, 49));
	CHECK(clicks_deaf(&c, 50));
	CHECK(clicks_deaf(&c, 89));
	CHECK(!clicks_deaf(&c, 90));
	/* Released at frame 95: from 50 ms before to 200 ms after */
	CHECK(clicks_edge(&c, 9950, false));
	CHECK(clicks_deaf(&c, 90));
	CHECK(clicks_deaf(&c, 114));
	CHECK(!clicks_deaf(&c, 115));
	/* A press in the first frames: from frame 0 */
	CHECK(clicks_edge(&c, 9100, true));
	CHECK(clicks_deaf(&c, 0));
	CHECK(clicks_deaf(&c, 19));
	CHECK(!clicks_deaf(&c, 20));
}

static void alarm_released_after_the_latch(void)
{
	struct clicks c;

	/* Deaf for 175 frames at most, until ALARM is released and 200 ms more */
	clicks_init(&c, 175);
	clicks_block(&c, 1100, 10); /* frame 0 at 1000 ms */
	CHECK(clicks_listen(&c, 1700)); /* released at frame 70 */
	CHECK(clicks_deaf(&c, 89));
	CHECK(!clicks_deaf(&c, 90));
	/* A later release does not deafen again from the start */
	CHECK(clicks_listen(&c, 5000));
	CHECK(!clicks_deaf(&c, 100));
}

static void alarm_held_through_the_sentence(void)
{
	struct clicks c;

	/* Held 3 s: the detector hears from frame 175 on, the voice under ALARM;
	 * the release, at frame 300, is an edge like any other: not a word */
	clicks_init(&c, 175);
	clicks_block(&c, 1100, 10);
	CHECK(clicks_listen(&c, 4000));
	CHECK(!clicks_deaf(&c, 175));
	CHECK(!clicks_deaf(&c, 294));
	CHECK(clicks_edge(&c, 4000, false));
	CHECK(clicks_deaf(&c, 295));
	CHECK(clicks_deaf(&c, 319));
	CHECK(!clicks_deaf(&c, 320));
}

static void the_latest_edges(void)
{
	struct clicks c;

	clicks_init(&c, 0);
	clicks_block(&c, 100000, 10000); /* frame 0 at 0 ms */
	/* One edge more than the windows: the oldest is forgotten */
	for (uint32_t i = 0; i <= CLICKS_WINDOWS; i++) {
		CHECK(clicks_edge(&c, 10000 + i * 1000, false));
	}
	CHECK(!clicks_deaf(&c, 1000)); /* the first, at 10 s */
	CHECK(clicks_deaf(&c, 1100));  /* the second, at 11 s */
	CHECK(clicks_deaf(&c, 1800));  /* the last, at 18 s */
}

static void the_uptime_wraps(void)
{
	struct clicks c;

	/* The 32-bit uptime of ms wraps after 49 days: a take across it */
	clicks_init(&c, 0);
	clicks_block(&c, 0xfffffff0U, 10); /* frame 0 at 0xffffff8c */
	CHECK_EQ(frame_of(&c, 0xfffffff0U), 10);
	CHECK_EQ(frame_of(&c, 0x00000010U), 13);
	CHECK(clicks_edge(&c, 0x00000100U, false)); /* frame 37 */
	CHECK(clicks_deaf(&c, 37));
	CHECK(!clicks_deaf(&c, 60));
}

int main(void)
{
	RUN(before_the_first_block);
	RUN(the_map_from_the_blocks);
	RUN(a_press_and_a_release);
	RUN(alarm_released_after_the_latch);
	RUN(alarm_held_through_the_sentence);
	RUN(the_latest_edges);
	RUN(the_uptime_wraps);
	return harness_report("clicks");
}
