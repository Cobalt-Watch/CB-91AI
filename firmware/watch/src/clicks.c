/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The clicks of the buttons in the frames of a take, see clicks.h. Pure C: the
 * host tests build this file as it is.
 */

#include "clicks.h"

void clicks_init(struct clicks *c, uint32_t deaf_first)
{
	*c = (struct clicks){ .deaf_until = deaf_first };
}

void clicks_block(struct clicks *c, uint32_t at_ms, uint32_t frames)
{
	/* The frames ended when the block came, or before: frame 0 started
	 * `frames` frames before that at the latest */
	const uint32_t t0 = at_ms - frames * CLICKS_FRAME_MS;

	if (!c->anchored || (int32_t)(t0 - c->t0_ms) < 0) {
		c->t0_ms = t0;
		c->anchored = true;
	}
}

bool clicks_frame(const struct clicks *c, uint32_t at_ms, uint32_t *frame)
{
	int32_t since;

	if (!c->anchored) {
		return false;
	}
	since = (int32_t)(at_ms - c->t0_ms);
	/* Before the first frame (the press that started the take, a bounce of
	 * it): the first frame */
	*frame = since < 0 ? 0U : (uint32_t)since / CLICKS_FRAME_MS;
	return true;
}

bool clicks_edge(struct clicks *c, uint32_t at_ms, bool down)
{
	const uint32_t before = down ? CLICKS_PRESS_BEFORE : CLICKS_RELEASE_BEFORE;
	const uint32_t after = down ? CLICKS_PRESS_AFTER : CLICKS_RELEASE_AFTER;
	uint32_t f;

	if (!clicks_frame(c, at_ms, &f)) {
		return false;
	}
	c->from[c->next] = f > before ? f - before : 0U;
	c->to[c->next] = f + after;
	c->next = (uint8_t)((c->next + 1U) % CLICKS_WINDOWS);
	return true;
}

bool clicks_listen(struct clicks *c, uint32_t at_ms)
{
	uint32_t f;

	if (!clicks_frame(c, at_ms, &f)) {
		return false;
	}
	if (f + CLICKS_RELEASE_AFTER < c->deaf_until) {
		c->deaf_until = f + CLICKS_RELEASE_AFTER;
	}
	return true;
}

bool clicks_deaf(const struct clicks *c, uint32_t frame)
{
	if (frame < c->deaf_until) {
		return true;
	}
	for (uint32_t i = 0; i < CLICKS_WINDOWS; i++) {
		if (frame >= c->from[i] && frame < c->to[i]) {
			return true;
		}
	}
	return false;
}
