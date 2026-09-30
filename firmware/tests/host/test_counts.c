/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the boots and the watchdog bites (firmware/watch/src/counts.c,
 * EF-72, EF-73): counted from the retained RAM or the settings, whichever
 * holds more, a boot loop included, a new cell included, saturating.
 */

#include "harness.h"
#include "counts.h"

static void the_first_boot(void)
{
	const struct counts none = { 0, 0 };
	struct counts c = counts_boot(&none, false, &none, false, false);

	CHECK_EQ(c.boots, 1);
	CHECK_EQ(c.bites, 0);
	/* Bitten on the very first boot: counted */
	c = counts_boot(&none, false, &none, false, true);
	CHECK_EQ(c.boots, 1);
	CHECK_EQ(c.bites, 1);
}

static void a_boot_loop_counts(void)
{
	/* The settings kept 10 boots; the loop bites before any minute: the
	 * retained RAM carries the count from boot to boot */
	const struct counts stored = { 10, 2 };
	struct counts kept = { 0, 0 };
	bool kept_ok = false;

	for (int i = 0; i < 5; i++) {
		kept = counts_boot(&kept, kept_ok, &stored, true, true);
		kept_ok = true;
	}
	CHECK_EQ(kept.boots, 15);
	CHECK_EQ(kept.bites, 7);
}

static void a_new_cell(void)
{
	/* The RAM gone (its CRC fails), the settings hold what was kept */
	const struct counts stored = { 40, 3 };
	const struct counts garbage = { 60000, 60000 };
	struct counts c = counts_boot(&garbage, false, &stored, true, false);

	CHECK_EQ(c.boots, 41);
	CHECK_EQ(c.bites, 3);
	/* Both there: the larger of each, whichever it is */
	const struct counts ram = { 45, 2 };

	c = counts_boot(&ram, true, &stored, true, false);
	CHECK_EQ(c.boots, 46);
	CHECK_EQ(c.bites, 3);
}

static void saturating(void)
{
	const struct counts full = { UINT16_MAX, UINT16_MAX };
	struct counts c = counts_boot(&full, true, &full, true, true);

	CHECK_EQ(c.boots, UINT16_MAX);
	CHECK_EQ(c.bites, UINT16_MAX);
	c = counts_boot(&full, true, &full, false, false);
	CHECK_EQ(c.boots, UINT16_MAX);
}

int main(void)
{
	RUN(the_first_boot);
	RUN(a_boot_loop_counts);
	RUN(a_new_cell);
	RUN(saturating);
	return harness_report("counts");
}
