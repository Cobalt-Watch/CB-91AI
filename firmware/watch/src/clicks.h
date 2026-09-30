/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The clicks of the buttons in the frames of a take (lot E1, its finishing
 * touches of 2026-09-25). Through the case, a button is heard as loud as a
 * voice: a press from about 300 ms before its contact (the finger lands, then
 * pushes), a release for 100 to 200 ms after it (at the wrist, 2026-09-24).
 * The event loop knows when each edge came, in ms of uptime; the audio thread
 * knows when each block of the microphone came. This maps the one onto the
 * other, frame by frame, so that the voice detector does not take a click for
 * a word, and a note stopped by a press ends before the finger is heard.
 *
 * The frames are those of 10 ms of the take, from 0. Each block anchors the
 * map: its frames ended at most when it came, and the block that came the
 * soonest after its end gives the map (the thread may take a block late,
 * never early). Pure logic, tested on the PC (firmware/tests/host/test_clicks.c).
 */

#ifndef CB91AI_WATCH_CLICKS_H
#define CB91AI_WATCH_CLICKS_H

#include <stdbool.h>
#include <stdint.h>

#define CLICKS_FRAME_MS      10U
#define CLICKS_PRESS_BEFORE  30U /* frames deaf before the contact of a press */
#define CLICKS_PRESS_AFTER   10U /* and after it */
#define CLICKS_RELEASE_BEFORE 5U /* around a release */
#define CLICKS_RELEASE_AFTER 20U
#define CLICKS_WINDOWS       8U  /* the latest edges of a take */

struct clicks {
	bool anchored;
	uint32_t t0_ms;       /* uptime at the start of frame 0, the soonest seen */
	uint32_t deaf_until;  /* the first frames, deaf: until ALARM is released */
	uint32_t from[CLICKS_WINDOWS];
	uint32_t to[CLICKS_WINDOWS]; /* frames deaf around an edge, to excluded */
	uint8_t next;
};

/* A take starts: deaf for its first `deaf_first` frames, whatever comes */
void clicks_init(struct clicks *c, uint32_t deaf_first);

/* A block came at `at_ms`, and with it the frames up to `frames` (the count
 * of the take so far, this block included) */
void clicks_block(struct clicks *c, uint32_t at_ms, uint32_t frames);

/* The frame a time falls in: false before the first block, or before the take */
bool clicks_frame(const struct clicks *c, uint32_t at_ms, uint32_t *frame);

/* An edge of a button during the take, pressed (`down`) or released: the
 * frames around it made deaf. False when it falls outside the frames known. */
bool clicks_edge(struct clicks *c, uint32_t at_ms, bool down);

/* ALARM released after the latch: deaf until its click is over, no later
 * than the first frames set by clicks_init(). False before the first block:
 * the caller tries again with the next one. */
bool clicks_listen(struct clicks *c, uint32_t at_ms);

/* Whether the voice detector must not hear frame `frame` */
bool clicks_deaf(const struct clicks *c, uint32_t frame);

#endif /* CB91AI_WATCH_CLICKS_H */
