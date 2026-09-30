/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the calls of the phone again (firmware/watch/src/recall.c, lot
 * E2): after 15 min of silence while a note waits, then 30 min, then every
 * hour, back to 15 min at each call for another reason; the window of a call
 * and the end of a link counting as sound; never while a link is up, never
 * without a note or without a phone to take it. The event loop of main.c is
 * played here, one wake-up every 5 s at the least.
 */

#include <stdbool.h>

#include "harness.h"
#include "recall.h"

#define WAKE_S 5U   /* main.c: LOOP_WAKE_MS */
#define DAY_S  86400U

/* The watch, as its event loop sees it */
struct watch {
	uint32_t call_s;      /* radio_last_call_s() */
	uint32_t link_end_s;  /* radio_last_link_end_s() */
	bool linked;          /* radio_connected() */
	bool bonded;          /* pairing_bonded(), or the link open in development */
	unsigned int notes;   /* notes_count() */
	uint32_t again;       /* calls again since a call for another reason */
	unsigned int recalls; /* calls again in all */
	uint32_t recall_s[8]; /* the first of them */
};

static void init(struct watch *w)
{
	*w = (struct watch){ .bonded = true };
}

/* A call for another reason: a note kept, a press, the boot. main.c sees it
 * by the count of radio_calls() and starts the rhythm again */
static void call(struct watch *w, uint32_t now)
{
	w->call_s = now;
	w->again = 0;
}

static void link_up(struct watch *w)
{
	w->linked = true;
}

static void link_down(struct watch *w, uint32_t now)
{
	w->linked = false;
	w->link_end_s = now;
}

/* The wake-ups of the loop from `from` to `to`, as main.c does them */
static void run(struct watch *w, uint32_t from, uint32_t to)
{
	for (uint32_t now = from; now < to; now += WAKE_S) {
		if (recall_due(now, w->call_s, w->link_end_s, w->again, w->linked, w->bonded) &&
		    w->notes > 0) {
			if (w->recalls < sizeof(w->recall_s) / sizeof(w->recall_s[0])) {
				w->recall_s[w->recalls] = now;
			}
			w->recalls++;
			w->call_s = now;
			w->again++;
		}
	}
}

static void the_arithmetic(void)
{
	/* 15 min, 30 min, then an hour, and never more */
	CHECK_EQ(recall_silence(0), 900);
	CHECK_EQ(recall_silence(1), 1800);
	CHECK_EQ(recall_silence(2), RECALL_SILENCE_MAX_S);
	CHECK_EQ(recall_silence(3), 3600);
	CHECK_EQ(recall_silence(0xffffffffU), 3600);
	/* Nothing since boot: silent from the end of a window that never was */
	CHECK_EQ(recall_at(0, 0, 0), RECALL_WINDOW_S + RECALL_SILENCE_S);
	CHECK_EQ(recall_at(0, 0, 0), 1030);
	/* A call: 2 min 10 of advertising, then the silence */
	CHECK_EQ(recall_at(100, 0, 0), 1130);
	CHECK_EQ(recall_at(100, 0, 1), 2030);
	CHECK_EQ(recall_at(100, 0, 2), 3830);
	CHECK_EQ(recall_at(100, 0, 9), 3830);
	/* A link that ended within the window leaves the window to run */
	CHECK_EQ(recall_at(100, 150, 0), 1130);
	CHECK_EQ(recall_at(100, 230, 0), 1130);
	/* One that ended after it counts from its end */
	CHECK_EQ(recall_at(100, 231, 0), 1131);
	CHECK_EQ(recall_at(100, 5000, 0), 5900);
	CHECK_EQ(recall_at(100, 5000, 2), 8600);
	/* A link long gone does not hide a later call */
	CHECK_EQ(recall_at(7000, 5000, 0), 8030);
	/* Due from that second on, not one before */
	CHECK(!recall_due(1129, 100, 0, 0, false, true));
	CHECK(recall_due(1130, 100, 0, 0, false, true));
	/* Never while a link is up, nor for a watch no phone may take notes from */
	CHECK(!recall_due(5000, 100, 0, 0, true, true));
	CHECK(!recall_due(5000, 100, 0, 0, false, false));
}

static void a_watch_with_nothing_to_say(void)
{
	struct watch w;

	init(&w);
	call(&w, 0); /* the boot of a blank watch */
	run(&w, 0, DAY_S);
	CHECK_EQ(w.recalls, 0);
}

static void a_note_nobody_takes(void)
{
	struct watch w;
	uint32_t at = 100, expected = 0;

	init(&w);
	/* A note kept at 100 s: the phone called then, and never answers */
	w.notes = 1;
	call(&w, 100);
	run(&w, 0, DAY_S);
	/* 17 min 10 later, then 32 min 10 after that, then every 62 min 10 */
	CHECK_EQ(w.recall_s[0], 1130);
	CHECK_EQ(w.recall_s[1], 3060);
	CHECK_EQ(w.recall_s[2], 6790);
	CHECK_EQ(w.recall_s[3], 10520);
	CHECK_EQ(w.recall_s[7], 25440);
	/* And no more than that in a day: 24 calls, one window of 0.9 uAh an
	 * hour or so */
	for (uint32_t again = 0;; again++) {
		at += RECALL_WINDOW_S + recall_silence(again);
		if (at >= DAY_S) {
			break;
		}
		expected++;
	}
	CHECK_EQ(w.recalls, expected);
	CHECK_EQ(w.recalls, 24);
}

static void a_new_note_starts_again_at_15_min(void)
{
	struct watch w;

	init(&w);
	/* Nobody answers for hours: the calls come every hour */
	w.notes = 1;
	call(&w, 0);
	run(&w, 0, 20000);
	CHECK(w.again >= 3);
	/* A new note at 20 000 s (or a click of ALARM): called at once, then
	 * 15 min after the window, as at first */
	w.notes = 2;
	call(&w, 20000);
	w.recalls = 0;
	run(&w, 20000, 30000);
	CHECK_EQ(w.recall_s[0], 21030);
	CHECK_EQ(w.recall_s[1], 22960);
}

static void off_the_grid_of_the_wake_ups(void)
{
	struct watch w;

	init(&w);
	/* A call at 101 s, the loop waking at multiples of 5: each call at the
	 * first wake-up once due */
	w.notes = 2;
	call(&w, 101);
	run(&w, 0, 4000);
	CHECK_EQ(w.recall_s[0], 1135);
	CHECK(w.recall_s[1] - w.recall_s[0] >= RECALL_WINDOW_S + 1800U);
	CHECK(w.recall_s[1] - w.recall_s[0] < RECALL_WINDOW_S + 1800U + WAKE_S);
}

static void a_note_put_off(void)
{
	struct watch w;

	init(&w);
	/* A note kept at 100 s; the phone comes at 105 s and puts it off (a
	 * NOTE_ACK "damaged"): the session ends at 120 s */
	w.notes = 1;
	call(&w, 100);
	run(&w, 0, 105);
	link_up(&w);
	run(&w, 105, 120);
	link_down(&w, 120);
	run(&w, 120, 1135);
	CHECK_EQ(w.recalls, 1);
	CHECK_EQ(w.recall_s[0], 1130);
	/* It comes again, and takes it this time */
	link_up(&w);
	run(&w, 1135, 1150);
	w.notes = 0;
	link_down(&w, 1150);
	run(&w, 1150, DAY_S);
	CHECK_EQ(w.recalls, 1);
}

static void a_phone_that_puts_off_keeps_the_rhythm(void)
{
	struct watch w;

	init(&w);
	/* Three calls again unanswered, then the phone comes and puts the note
	 * off: no call of another reason, the next one an hour after that link */
	w.notes = 1;
	call(&w, 0);
	run(&w, 0, 10000);
	CHECK_EQ(w.again, 3);
	link_up(&w);
	run(&w, 10000, 10020);
	link_down(&w, 10020);
	w.recalls = 0;
	run(&w, 10020, 20000);
	CHECK_EQ(w.recall_s[0], 13620);
}

static void a_link_after_the_window(void)
{
	struct watch w;

	init(&w);
	/* The PC or a phone connects at 200 s, past the window, for 5 min: the
	 * silence counts from the end of that link */
	w.notes = 1;
	call(&w, 0);
	run(&w, 0, 200);
	link_up(&w);
	run(&w, 200, 500);
	link_down(&w, 500);
	run(&w, 500, 2000);
	CHECK_EQ(w.recall_s[0], 1400);
}

static void never_while_linked(void)
{
	struct watch w;

	init(&w);
	/* A session held an hour and more with a note left in it: no call while
	 * it lasts, the first 15 min after its end */
	w.notes = 3;
	call(&w, 0);
	link_up(&w);
	run(&w, 0, 5000);
	CHECK_EQ(w.recalls, 0);
	link_down(&w, 5000);
	run(&w, 5000, 6000);
	CHECK_EQ(w.recalls, 1);
	CHECK_EQ(w.recall_s[0], 5900);
}

static void a_watch_never_paired(void)
{
	struct watch w;

	init(&w);
	/* Notes recorded on a product no phone was paired with: they wait
	 * without a call, and the watch keeps quiet */
	w.bonded = false;
	w.notes = 2;
	call(&w, 0);
	run(&w, 0, DAY_S);
	CHECK_EQ(w.recalls, 0);
	/* A press calls, a phone pairs at once and puts the notes off: from then
	 * on, the calls again */
	call(&w, DAY_S);
	link_up(&w);
	w.bonded = true;
	link_down(&w, DAY_S + 60);
	run(&w, DAY_S, DAY_S + 1200);
	CHECK_EQ(w.recalls, 1);
	CHECK_EQ(w.recall_s[0], DAY_S + RECALL_WINDOW_S + RECALL_SILENCE_S);
}

static void a_reset_hides_the_notes(void)
{
	struct watch w;

	init(&w);
	/* The notes hidden by the reset of lot S3 count as none */
	w.notes = 1;
	call(&w, 0);
	run(&w, 0, 500);
	w.notes = 0;
	run(&w, 500, DAY_S);
	CHECK_EQ(w.recalls, 0);
}

int main(void)
{
	RUN(the_arithmetic);
	RUN(a_watch_with_nothing_to_say);
	RUN(a_note_nobody_takes);
	RUN(a_new_note_starts_again_at_15_min);
	RUN(off_the_grid_of_the_wake_ups);
	RUN(a_note_put_off);
	RUN(a_phone_that_puts_off_keeps_the_rhythm);
	RUN(a_link_after_the_window);
	RUN(never_while_linked);
	RUN(a_watch_never_paired);
	RUN(a_reset_hides_the_notes);
	return harness_report("recall");
}
