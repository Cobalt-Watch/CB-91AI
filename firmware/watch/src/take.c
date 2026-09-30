/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * What becomes of a take, see take.h. Pure C: the host tests build this file
 * as it is.
 */

#include "take.h"

uint32_t take_keep(const struct take_ended *t, uint8_t *report)
{
	if (t->dropped) {
		*report = RECORDER_DROPPED;
		return 0;
	}
	if (!t->latched) {
		/* A chord that swallowed the click, or a fault before "rEC": a
		 * fault still says so, in red */
		*report = t->end == NOTE_END_FAULT ? RECORDER_FAILED : RECORDER_DROPPED;
		return 0;
	}
	if (t->end == RECORDER_NOTHING || t->end == RECORDER_FAILED) {
		*report = t->end;
		return 0;
	}
	if (t->frames == 0) {
		*report = RECORDER_FAILED; /* nothing came at all */
		return 0;
	}
	*report = t->end;
	switch (t->end) {
	case NOTE_END_SILENCE:
		/* Up to the last word and half a second more */
		return t->vad_keep < t->frames ? t->vad_keep : t->frames;
	case NOTE_END_PRESS:
		if (t->stop_frame != TAKE_STOP_UNKNOWN && t->stop_frame > 2U * TAKE_STOP_LEAD_FRAMES &&
		    t->stop_frame - TAKE_STOP_LEAD_FRAMES < t->frames) {
			/* Up to where the finger that came to press was first heard */
			return t->stop_frame - TAKE_STOP_LEAD_FRAMES;
		}
		/* A press that cannot be placed (too soon, or past the frames):
		 * all but the click of the press, if it leaves something */
		return t->frames > 2U * TAKE_STOP_CLICK_FRAMES ? t->frames - TAKE_STOP_CLICK_FRAMES
								: t->frames;
	default:
		/* Its longest length, the flash full, a fault: all of it */
		return t->frames;
	}
}
