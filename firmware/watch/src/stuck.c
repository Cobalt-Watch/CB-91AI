/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * A stuck button, see stuck.h. Times are the uptime in ms on 32 bits: every
 * comparison goes through a difference, which survives the wrap.
 */

#include <string.h>

#include "stuck.h"

void stuck_init(struct stuck *s, uint8_t buttons)
{
	memset(s, 0, sizeof(*s));
	s->buttons = buttons > STUCK_MAX ? STUCK_MAX : buttons;
}

uint8_t stuck_update(struct stuck *s, uint8_t levels, uint32_t now)
{
	uint8_t now_stuck = 0;

	for (unsigned int b = 0; b < s->buttons; b++) {
		const uint8_t bit = (uint8_t)(1U << b);

		if (s->stuck & bit) {
			continue;
		}
		if ((levels & bit) == 0) {
			s->down &= (uint8_t)~bit;
			continue;
		}
		if ((s->down & bit) == 0) {
			s->down |= bit;
			s->since[b] = now;
		} else if (now - s->since[b] >= STUCK_HELD_MS) {
			s->down &= (uint8_t)~bit;
			s->stuck |= bit;
			s->since[b] = now;
			now_stuck |= bit;
		}
	}
	/* A chord held this long is no gesture either: the buttons still down
	 * go with it. Hiding one of three would leave a chord of two, which
	 * the gesture machine takes for a new one, the reset's MODE and ALARM
	 * among them (ui.c). */
	if (now_stuck != 0 && s->down != 0) {
		for (unsigned int b = 0; b < s->buttons; b++) {
			const uint8_t bit = (uint8_t)(1U << b);

			if (s->down & bit) {
				s->down &= (uint8_t)~bit;
				s->stuck |= bit;
				s->since[b] = now;
				now_stuck |= bit;
			}
		}
	}
	return now_stuck;
}

uint8_t stuck_due(const struct stuck *s, uint32_t now)
{
	uint8_t due = 0;

	for (unsigned int b = 0; b < s->buttons; b++) {
		if ((s->stuck & (1U << b)) && now - s->since[b] >= STUCK_PROBE_MS) {
			due |= (uint8_t)(1U << b);
		}
	}
	return due;
}

void stuck_probed(struct stuck *s, unsigned int button, bool down, uint32_t now)
{
	uint8_t bit;

	if (button >= s->buttons) {
		return;
	}
	bit = (uint8_t)(1U << button);
	if ((s->stuck & bit) == 0) {
		return;
	}
	if (down) {
		s->since[button] = now;
		return;
	}
	s->stuck &= (uint8_t)~bit;
	s->down &= (uint8_t)~bit;
}

bool stuck_next(const struct stuck *s, uint32_t now, uint32_t *wait)
{
	bool any = false;
	uint32_t best = 0;

	for (unsigned int b = 0; b < s->buttons; b++) {
		const uint8_t bit = (uint8_t)(1U << b);
		uint32_t limit;
		uint32_t left;

		if (s->stuck & bit) {
			limit = STUCK_PROBE_MS;
		} else if (s->down & bit) {
			limit = STUCK_HELD_MS;
		} else {
			continue;
		}
		left = now - s->since[b] >= limit ? 0 : limit - (now - s->since[b]);
		if (!any || left < best) {
			best = left;
			any = true;
		}
	}
	if (any) {
		*wait = best;
	}
	return any;
}
