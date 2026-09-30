/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Gestures of the case buttons, see gesture.h. Pure C, no Zephyr: the host
 * tests build this file as it is (firmware/tests/host).
 *
 * Each call first settles the level of every button (debounce), then either
 * follows the chords, when two buttons or more count as down or a chord has
 * not fully let go yet, or runs the machine of each button:
 *
 *   IDLE --press--> DOWN --release before long_ms--> COUNT --window over--> IDLE (CLICK)
 *                     |                                 |
 *                     +--held long_ms--> LONG           +--press--> DOWN
 *                                          |
 *                                          +--release--> IDLE (LONG_END)
 *
 * Presses and releases are dated by their edge (when the level last seen
 * appeared), not by the call that lets them count: the debounce delays the
 * report, not the measure. Two consequences, both on the side of what the
 * wearer meant: a release seen before long_ms but still settling holds the
 * long press back, and a press seen within the window of the next click but
 * still settling holds the click back.
 */

#include <string.h>

#include "gesture.h"

enum phase { IDLE, DOWN, LONG, COUNT };

/* Time from `then` to `now`, across a wrap of the counter */
static uint32_t since(uint32_t then, uint32_t now)
{
	return now - then;
}

static unsigned int bits(uint32_t mask)
{
	unsigned int n = 0;

	for (; mask != 0; mask &= mask - 1U) {
		n++;
	}
	return n;
}

static void emit(struct gesture *out, size_t *n, uint8_t type, uint8_t button, uint8_t count,
		 uint32_t ms)
{
	out[*n].type = type;
	out[*n].button = button;
	out[*n].count = count;
	out[*n].ms = ms;
	(*n)++;
}

void gesture_init(struct gesture_state *s, const struct gesture_cfg *cfg, uint8_t buttons)
{
	memset(s, 0, sizeof(*s));
	s->cfg = *cfg;
	s->buttons = buttons < GESTURE_BUTTONS_MAX ? buttons : GESTURE_BUTTONS_MAX;
	if (s->cfg.chord_step_ms == 0) {
		s->cfg.chord_step_ms = 1000;
	}
}

/* A settled edge of one button, dated by the edge */
static void edge(struct gesture_state *s, uint8_t i, bool pressed, uint32_t at,
		 struct gesture *out, size_t *n)
{
	struct gesture_button *b = &s->b[i];

	if (pressed) {
		if (b->phase == IDLE) {
			b->clicks = 0;
		}
		b->phase = DOWN;
		b->down_at = at;
		emit(out, n, GESTURE_PRESS, i, b->clicks, 0);
		return;
	}
	if (b->phase == LONG) {
		b->phase = IDLE;
		emit(out, n, GESTURE_LONG_END, i, 0, since(b->down_at, at));
	} else if (b->phase == DOWN) {
		if (since(b->down_at, at) >= s->cfg.long_ms) {
			/* Held long enough, though no call came in between to say so: a
			 * long press, and its end at once */
			emit(out, n, GESTURE_LONG, i, b->clicks, 0);
			emit(out, n, GESTURE_LONG_END, i, 0, since(b->down_at, at));
			b->clicks = 0;
			b->phase = IDLE;
		} else {
			if (b->clicks < GESTURE_CLICKS_MAX) {
				b->clicks++;
			}
			b->up_at = at;
			b->phase = COUNT;
		}
	}
}

/* The long press, and the end of the window of the next click */
static void expire(struct gesture_state *s, uint8_t i, uint32_t now, struct gesture *out,
		   size_t *n)
{
	struct gesture_button *b = &s->b[i];

	if (b->phase == DOWN && since(b->down_at, now) >= s->cfg.long_ms &&
	    (b->raw || since(b->down_at, b->raw_at) >= s->cfg.long_ms)) {
		/* Not while a release seen before long_ms settles */
		b->phase = LONG;
		emit(out, n, GESTURE_LONG, i, b->clicks, 0);
		b->clicks = 0;
	} else if (b->phase == COUNT && since(b->up_at, now) >= s->cfg.multi_ms &&
		   !(b->raw && since(b->up_at, b->raw_at) < s->cfg.multi_ms)) {
		/* Not while a press seen within the window settles */
		b->phase = IDLE;
		emit(out, n, GESTURE_CLICK, i, b->clicks, 0);
		b->clicks = 0;
	}
}

/* The latest edge among the buttons that settled in this call, or now */
static uint32_t latest_edge(const struct gesture_state *s, uint32_t settled, uint32_t now)
{
	uint32_t at = now, age = UINT32_MAX;

	for (uint8_t i = 0; i < s->buttons; i++) {
		if ((settled & (1U << i)) && since(s->b[i].raw_at, now) < age) {
			age = since(s->b[i].raw_at, now);
			at = s->b[i].raw_at;
		}
	}
	return at;
}

/* The time between the first and the last press of the buttons of `mask`,
 * all down: when each level appeared */
static uint32_t press_spread(const struct gesture_state *s, uint32_t mask, uint32_t now)
{
	uint32_t oldest = 0;
	uint32_t newest = UINT32_MAX;

	for (uint8_t i = 0; i < s->buttons; i++) {
		if (mask & (1U << i)) {
			const uint32_t age = since(s->b[i].raw_at, now);

			oldest = age > oldest ? age : oldest;
			newest = age < newest ? age : newest;
		}
	}
	return newest == UINT32_MAX ? 0U : oldest - newest;
}

/* Chords: the machines of the buttons stand aside until every button is up */
static void chords(struct gesture_state *s, uint32_t mask, uint32_t settled, uint32_t now,
		   struct gesture *out, size_t *n)
{
	if (bits(mask) >= 2 && mask != s->chord) {
		/* A new chord, from the edge that made it */
		const uint32_t at = latest_edge(s, settled, now);

		if (s->chord != 0) {
			emit(out, n, GESTURE_CHORD_END, s->chord, 0, since(s->chord_at, at));
		}
		for (uint8_t i = 0; i < s->buttons; i++) {
			s->b[i].phase = IDLE;
			s->b[i].clicks = 0;
		}
		s->chord = (uint8_t)mask;
		s->chord_at = at;
		s->chord_steps = 0;
		s->muted = true;
		emit(out, n, GESTURE_CHORD, s->chord, 0, press_spread(s, mask, now));
	} else if (bits(mask) < 2 && s->chord != 0) {
		emit(out, n, GESTURE_CHORD_END, s->chord, 0,
		     since(s->chord_at, latest_edge(s, settled, now)));
		s->chord = 0;
	}
	if (s->chord != 0) {
		const uint32_t steps = since(s->chord_at, now) / s->cfg.chord_step_ms;

		if (steps > s->chord_steps) {
			s->chord_steps = steps;
			emit(out, n, GESTURE_CHORD_HOLD, s->chord, 0, steps * s->cfg.chord_step_ms);
		}
	}
	if (mask == 0) {
		s->muted = false;
	}
}

size_t gesture_update(struct gesture_state *s, uint32_t levels, uint32_t now,
		      struct gesture *out)
{
	uint32_t settled = 0; /* buttons whose level counts from this call on */
	uint32_t mask = 0;
	size_t n = 0;

	for (uint8_t i = 0; i < s->buttons; i++) {
		struct gesture_button *b = &s->b[i];
		const bool level = ((levels >> i) & 1U) != 0;

		if (level != b->raw) {
			b->raw = level;
			b->raw_at = now;
		}
		if (b->raw != b->stable && since(b->raw_at, now) >= s->cfg.debounce_ms) {
			b->stable = b->raw;
			settled |= 1U << i;
		}
		if (b->stable) {
			mask |= 1U << i;
		}
	}
	if (s->muted || bits(mask) >= 2) {
		chords(s, mask, settled, now, out, &n);
		return n;
	}
	for (uint8_t i = 0; i < s->buttons; i++) {
		if (settled & (1U << i)) {
			edge(s, i, s->b[i].stable, s->b[i].raw_at, out, &n);
		}
		expire(s, i, now, out, &n);
	}
	return n;
}

/* Keeps the nearest of the deadlines, as a wait from now (0 when overdue) */
static void nearest(bool *any, uint32_t *wait, uint32_t deadline, uint32_t now)
{
	const int32_t left = (int32_t)(deadline - now);
	const uint32_t candidate = left > 0 ? (uint32_t)left : 0U;

	if (!*any || candidate < *wait) {
		*wait = candidate;
		*any = true;
	}
}

bool gesture_next(const struct gesture_state *s, uint32_t now, uint32_t *wait_ms)
{
	bool any = false;

	for (uint8_t i = 0; i < s->buttons; i++) {
		const struct gesture_button *b = &s->b[i];

		if (b->raw != b->stable) {
			/* Nothing else is due before the level settles: a long press or a
			 * window held back by it would otherwise ask for a call at once,
			 * again and again, until then */
			nearest(&any, wait_ms, b->raw_at + s->cfg.debounce_ms, now);
			continue;
		}
		if (s->muted) {
			continue;
		}
		if (b->phase == DOWN) {
			nearest(&any, wait_ms, b->down_at + s->cfg.long_ms, now);
		} else if (b->phase == COUNT) {
			nearest(&any, wait_ms, b->up_at + s->cfg.multi_ms, now);
		}
	}
	if (s->chord != 0) {
		nearest(&any, wait_ms, s->chord_at + (s->chord_steps + 1U) * s->cfg.chord_step_ms,
			now);
	}
	return any;
}
