/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * What the RGB LED shows, see light.h. Pure C: the host tests build this file
 * as it is.
 */

#include "light.h"
#include "link_proto.h"

static const struct light_rgb off = { 0, 0, 0 };
static const struct light_rgb red = { 255, 0, 0 };

void light_init(struct light_state *s, const struct light_style *style)
{
	const struct light_state clear = { .style = *style };

	*s = clear;
}

bool light_style_parse(const uint8_t *value, size_t len, struct light_style *out)
{
	if (len < 4 || value[0] > LINK_LIGHT_RAINBOW) {
		return false;
	}
	/* A light that does not light is a mistake of the app */
	if (value[0] != LINK_LIGHT_RAINBOW && (value[1] | value[2] | value[3]) == 0) {
		return false;
	}
	out->effect = value[0];
	out->color.r = value[1];
	out->color.g = value[2];
	out->color.b = value[3];
	return true;
}

size_t light_style_put(const struct light_style *style, uint8_t *out, size_t room)
{
	if (room < 4) {
		return 0;
	}
	out[0] = style->effect;
	out[1] = style->color.r;
	out[2] = style->color.g;
	out[3] = style->color.b;
	return 4;
}

static struct light_rgb scaled(struct light_rgb c, uint32_t level)
{
	const struct light_rgb out = {
		(uint8_t)(c.r * level / 255U),
		(uint8_t)(c.g * level / 255U),
		(uint8_t)(c.b * level / 255U),
	};

	return out;
}

/* One turn of the colour wheel, as the self-test's boot: six sectors */
static struct light_rgb wheel(uint32_t t_ms)
{
	const uint32_t h = (t_ms % LIGHT_RAINBOW_MS) * 1536U / LIGHT_RAINBOW_MS;
	const uint8_t f = (uint8_t)(h % 256U);
	struct light_rgb c;

	switch (h / 256U) {
	case 0:
		c = (struct light_rgb){ 255, f, 0 };
		break;
	case 1:
		c = (struct light_rgb){ (uint8_t)(255U - f), 255, 0 };
		break;
	case 2:
		c = (struct light_rgb){ 0, 255, f };
		break;
	case 3:
		c = (struct light_rgb){ 0, (uint8_t)(255U - f), 255 };
		break;
	case 4:
		c = (struct light_rgb){ f, 0, 255 };
		break;
	default:
		c = (struct light_rgb){ 255, 0, (uint8_t)(255U - f) };
		break;
	}
	return c;
}

/* The light `t_ms` after the press, and when it next changes */
static struct light_rgb effect(const struct light_style *style, uint32_t t_ms, uint32_t *step)
{
	switch (style->effect) {
	case LINK_LIGHT_BREATHE: {
		/* Full at the press, down to a fifth and up again, squared for the eye */
		const uint32_t p = t_ms % LIGHT_BREATHE_MS;
		const uint32_t half = LIGHT_BREATHE_MS / 2U;
		const uint32_t lin = p < half ? half - p : p - half;
		const uint32_t level = 51U + 204U * lin / half;

		*step = LIGHT_STEP_MS;
		return scaled(style->color, level * level / 255U);
	}
	case LINK_LIGHT_BLINK: {
		const uint32_t p = t_ms % (2U * LIGHT_BLINK_MS);

		*step = LIGHT_BLINK_MS - p % LIGHT_BLINK_MS;
		return p < LIGHT_BLINK_MS ? style->color : off;
	}
	case LINK_LIGHT_RAINBOW:
		*step = LIGHT_STEP_MS;
		return wheel(t_ms);
	default:
		*step = LIGHT_NO_CHANGE;
		return style->color;
	}
}

void light_press(struct light_state *s, uint32_t now_ms)
{
	s->light = true;
	s->light_since = now_ms;
	s->light_until = now_ms + LIGHT_MIN_MS;
}

void light_hold(struct light_state *s, uint32_t now_ms)
{
	(void)now_ms;
	if (s->light) {
		s->light_until = s->light_since + LIGHT_MAX_MS;
	}
}

void light_release(struct light_state *s, uint32_t now_ms)
{
	uint32_t end = s->light_since + LIGHT_MIN_MS;

	if (!s->light) {
		return;
	}
	/* The 1.5 s of a press at least, and never past the longest light */
	if ((int32_t)(now_ms - end) > 0) {
		end = now_ms;
	}
	if ((int32_t)(end - (s->light_since + LIGHT_MAX_MS)) > 0) {
		end = s->light_since + LIGHT_MAX_MS;
	}
	s->light_until = end;
}

void light_recording(struct light_state *s, bool on, uint32_t now_ms)
{
	if (on && !s->recording) {
		s->rec_since = now_ms;
	}
	s->recording = on;
}

void light_pulse(struct light_state *s, struct light_rgb color, uint32_t ms, uint32_t now_ms)
{
	s->pulse = true;
	s->pulse_color = color;
	s->pulse_until = now_ms + ms;
}

struct light_rgb light_frame(struct light_state *s, uint32_t now_ms, uint32_t *next_ms)
{
	if (s->pulse) {
		const int32_t left = (int32_t)(s->pulse_until - now_ms);

		if (left > 0) {
			*next_ms = (uint32_t)left;
			return s->pulse_color;
		}
		s->pulse = false;
	}
	if (s->light) {
		const int32_t left = (int32_t)(s->light_until - now_ms);

		if (left > 0) {
			uint32_t step;
			const struct light_rgb c = effect(&s->style, now_ms - s->light_since, &step);

			*next_ms = step < (uint32_t)left ? step : (uint32_t)left;
			return c;
		}
		s->light = false;
	}
	if (s->recording) {
		const uint32_t p = (now_ms - s->rec_since) % LIGHT_REC_PERIOD_MS;

		if (p < LIGHT_REC_ON_MS) {
			*next_ms = LIGHT_REC_ON_MS - p;
			return red;
		}
		*next_ms = LIGHT_REC_PERIOD_MS - p;
		return off;
	}
	*next_ms = LIGHT_NO_CHANGE;
	return off;
}
