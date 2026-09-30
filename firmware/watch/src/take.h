/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * What becomes of a take once it has ended (lot E1): the frames its note
 * keeps, or none, and the way it is reported. Pure logic, out of recorder.c so
 * that the host tests check every case (firmware/tests/host/test_take.c).
 */

#ifndef CB91AI_WATCH_TAKE_H
#define CB91AI_WATCH_TAKE_H

#include <stdbool.h>
#include <stdint.h>

#include "note.h"

/* How a take that left no note ended, after the NOTE_END_* of lib/note.h */
#define RECORDER_DROPPED 0x10 /* a click or a chord: dropped on purpose */
#define RECORDER_NOTHING 0x11 /* nothing said in its first 5 s */
#define RECORDER_FULL    0x12 /* no room in the flash to begin */
#define RECORDER_FAILED  0x13 /* the microphone or the flash failed */

/* The press that stops a take is heard from about 300 ms before its contact
 * (the finger lands, then pushes: at the wrist, 2026-09-24): the note ends
 * that much before it. When its time is not known, the last 150 ms go. */
#define TAKE_STOP_LEAD_FRAMES  30U
#define TAKE_STOP_CLICK_FRAMES 15U
#define TAKE_STOP_UNKNOWN      0xffffffffU

struct take_ended {
	bool dropped;       /* a click or a chord dropped it */
	bool latched;       /* it reached "rEC" (or the harness started it so) */
	uint8_t end;        /* NOTE_END_*, RECORDER_NOTHING or RECORDER_FAILED */
	uint32_t frames;    /* frames written to the store */
	uint32_t vad_keep;  /* frames up to the last word and its tail */
	uint32_t stop_frame; /* the frame of the press that stopped it, or
	                      * TAKE_STOP_UNKNOWN (clicks.h) */
};

/* The frames of the note, 0 for no note; `report` gets NOTE_END_* when a note
 * is kept, RECORDER_* otherwise. A take never latched never becomes a note. */
uint32_t take_keep(const struct take_ended *t, uint8_t *report);

#endif /* CB91AI_WATCH_TAKE_H */
