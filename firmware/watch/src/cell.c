/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * What is left of the CR2016, see cell.h.
 */

#include <stddef.h>

#include "cell.h"

/* The Energizer curve read at these points (hours to 2.0 V: 1040), minus the
 * 60 mV of the watch's reading; percent: hours left over the 440 h from the
 * bend (600 h) to 2.0 V */
static const struct {
	int16_t mv;
	uint8_t percent;
} points[] = {
	{ 2840, 100 }, /* 2.90 V, 600 h: the end of the plateau */
	{ 2770, 77 },  /* 2.83 V, 700 h */
	{ 2680, 55 },  /* 2.74 V, 800 h */
	{ 2620, 43 },  /* 2.68 V, 850 h */
	{ 2520, 32 },  /* 2.58 V, 900 h */
	{ 2390, 20 },  /* 2.45 V, 950 h */
	{ 2190, 9 },   /* 2.25 V, 1000 h */
	{ 2000, 2 },   /* 2.06 V, 1030 h */
	{ 1940, 0 },   /* 2.00 V, 1040 h */
};

uint8_t cell_percent(int32_t mv)
{
	const size_t n = sizeof(points) / sizeof(points[0]);

	if (mv >= points[0].mv) {
		return points[0].percent;
	}
	for (size_t i = 1; i < n; i++) {
		if (mv >= points[i].mv) {
			const int32_t span_mv = points[i - 1].mv - points[i].mv;
			const int32_t span_pc = points[i - 1].percent - points[i].percent;

			/* Linear between two points, rounded to the nearest */
			return (uint8_t)(points[i].percent +
					 ((mv - points[i].mv) * span_pc + span_mv / 2) / span_mv);
		}
	}
	return 0;
}

uint8_t cell_percent_at(int32_t mv, int32_t temp_cc, bool known)
{
	if (!known) {
		return cell_percent(mv);
	}
	/* (21 degC - t) x 1 mV a degree, rounded to the nearest mV */
	const int32_t diff_cc = CELL_REF_CC - temp_cc;
	const int32_t shift = (diff_cc * CELL_MV_PER_C + (diff_cc >= 0 ? 50 : -50)) / 100;

	return cell_percent(mv + shift);
}
