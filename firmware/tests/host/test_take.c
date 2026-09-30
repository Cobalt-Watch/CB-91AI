/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of what becomes of a take (firmware/watch/src/take.c, lot E1):
 * which takes leave a note, how much of it, and how each is reported.
 */

#include "harness.h"
#include "take.h"

static uint32_t keep(bool dropped, bool latched, uint8_t end, uint32_t frames, uint32_t vad_keep,
		     uint8_t *report)
{
	const struct take_ended t = { dropped, latched, end, frames, vad_keep, TAKE_STOP_UNKNOWN };

	return take_keep(&t, report);
}

/* A take stopped by a press whose frame is known (clicks.h) */
static uint32_t keep_press(uint32_t frames, uint32_t stop_frame)
{
	const struct take_ended t = { false, true, NOTE_END_PRESS, frames, frames, stop_frame };
	uint8_t r = 0;
	const uint32_t k = take_keep(&t, &r);

	CHECK_EQ(r, NOTE_END_PRESS);
	return k;
}

static void notes_kept(void)
{
	uint8_t r;

	/* Silence: to the last word and its tail */
	CHECK_EQ(keep(false, true, NOTE_END_SILENCE, 800, 520, &r), 520);
	CHECK_EQ(r, NOTE_END_SILENCE);
	CHECK_EQ(keep(false, true, NOTE_END_SILENCE, 400, 520, &r), 400);
	/* A press: all but its click */
	CHECK_EQ(keep(false, true, NOTE_END_PRESS, 780, 780, &r), 765);
	CHECK_EQ(r, NOTE_END_PRESS);
	CHECK_EQ(keep(false, true, NOTE_END_PRESS, 20, 20, &r), 20);
	/* The longest length, the flash full, a fault: all */
	CHECK_EQ(keep(false, true, NOTE_END_LONGEST, 12000, 900, &r), 12000);
	CHECK_EQ(r, NOTE_END_LONGEST);
	CHECK_EQ(keep(false, true, NOTE_END_FULL, 300, 100, &r), 300);
	CHECK_EQ(r, NOTE_END_FULL);
	CHECK_EQ(keep(false, true, NOTE_END_FAULT, 50, 0, &r), 50);
	CHECK_EQ(r, NOTE_END_FAULT);
}

static void a_press_known_to_the_frame(void)
{
	/* The contact at frame 800, seen at the end of the block after it: the
	 * note ends 300 ms before, where the finger is first heard */
	CHECK_EQ(keep_press(812, 800), 770);
	/* The contact in the last frame, or past it (the loop was quick) */
	CHECK_EQ(keep_press(800, 800), 770);
	CHECK_EQ(keep_press(790, 805), 775);
	CHECK_EQ(keep_press(70, 61), 31);
	/* A press that cannot be placed, too soon or past the frames written
	 * (ALARM held from the start, a chord): all but the last 150 ms */
	CHECK_EQ(keep_press(70, 60), 55);
	CHECK_EQ(keep_press(70, 0), 55);
	CHECK_EQ(keep_press(100, 500), 85);
	CHECK_EQ(keep_press(20, 0), 20);
}

static void takes_that_leave_nothing(void)
{
	uint8_t r;

	/* A click */
	CHECK_EQ(keep(true, false, NOTE_END_PRESS, 0, 0, &r), 0);
	CHECK_EQ(r, RECORDER_DROPPED);
	/* Dropped whatever else it was */
	CHECK_EQ(keep(true, true, NOTE_END_SILENCE, 500, 400, &r), 0);
	CHECK_EQ(r, RECORDER_DROPPED);
	/* Never latched: a chord swallowed the click, then speech nearby */
	CHECK_EQ(keep(false, false, NOTE_END_SILENCE, 400, 300, &r), 0);
	CHECK_EQ(r, RECORDER_DROPPED);
	CHECK_EQ(keep(false, false, NOTE_END_LONGEST, 12000, 0, &r), 0);
	/* ... or a fault before "rEC": said in red */
	CHECK_EQ(keep(false, false, NOTE_END_FAULT, 30, 0, &r), 0);
	CHECK_EQ(r, RECORDER_FAILED);
	/* Nothing said in 5 s, a pocket */
	CHECK_EQ(keep(false, true, RECORDER_NOTHING, 500, 500, &r), 0);
	CHECK_EQ(r, RECORDER_NOTHING);
	/* Nothing came at all */
	CHECK_EQ(keep(false, true, NOTE_END_PRESS, 0, 0, &r), 0);
	CHECK_EQ(r, RECORDER_FAILED);
	CHECK_EQ(keep(false, true, RECORDER_FAILED, 10, 0, &r), 0);
	CHECK_EQ(r, RECORDER_FAILED);
}

int main(void)
{
	RUN(notes_kept);
	RUN(a_press_known_to_the_frame);
	RUN(takes_that_leave_nothing);
	return harness_report("take");
}
