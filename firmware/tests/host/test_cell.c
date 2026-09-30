/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the percentage of the cell (firmware/watch/src/cell.c, EF-42,
 * lot D5): 100 on the plateau, the points of the Energizer curve, never up as
 * the voltage goes down, 0 at the end.
 */

#include "harness.h"
#include "cell.h"

static void the_plateau_and_the_ends(void)
{
	CHECK_EQ(cell_percent(3300), 100);
	CHECK_EQ(cell_percent(2875), 100); /* a watch at rest, 2026-09-26 */
	CHECK_EQ(cell_percent(2840), 100);
	CHECK_EQ(cell_percent(1940), 0);
	CHECK_EQ(cell_percent(1000), 0);
	CHECK_EQ(cell_percent(0), 0);
	CHECK_EQ(cell_percent(-5), 0);
}

static void the_points_of_the_curve(void)
{
	CHECK_EQ(cell_percent(2770), 77);
	CHECK_EQ(cell_percent(2680), 55);
	CHECK_EQ(cell_percent(2620), 43);
	CHECK_EQ(cell_percent(2520), 32);
	CHECK_EQ(cell_percent(2390), 20);
	CHECK_EQ(cell_percent(2190), 9);
	CHECK_EQ(cell_percent(2000), 2);
	/* Between two points, on the line: halfway from 2.52 to 2.62 V */
	CHECK_EQ(cell_percent(2570), 38);
	/* The reading of the old linear scale that misled: 2.6 V, 60 % then */
	CHECK(cell_percent(2600) < 45);
}

static void never_up_as_it_goes_down(void)
{
	uint8_t last = 100;
	int turns = 0;

	for (int32_t mv = 3100; mv >= 1800; mv--) {
		const uint8_t p = cell_percent(mv);

		CHECK(p <= 100);
		if (p > last) {
			turns++;
		}
		last = p;
	}
	CHECK_EQ(turns, 0);
}

static void brought_to_21_degrees(void)
{
	/* On the knee, 10 degC colder: 10 mV lower, the same cell */
	CHECK_EQ(cell_percent_at(2770, 1100, true), cell_percent(2780));
	CHECK_EQ(cell_percent_at(2790, 3100, true), cell_percent(2780));
	CHECK_EQ(cell_percent_at(2780, 2100, true), cell_percent(2780));
	/* Unknown temperature: as it reads */
	CHECK_EQ(cell_percent_at(2770, -2000, false), cell_percent(2770));
	/* Rounded to the nearest mV: 20.55 degC is half a mV */
	CHECK_EQ(cell_percent_at(2780, 2045, true), cell_percent(2781));
	/* On the plateau, warm or cold: 100 */
	CHECK_EQ(cell_percent_at(2875, 3400, true), 100);
}

int main(void)
{
	RUN(the_plateau_and_the_ends);
	RUN(brought_to_21_degrees);
	RUN(the_points_of_the_curve);
	RUN(never_up_as_it_goes_down);
	return harness_report("cell");
}
