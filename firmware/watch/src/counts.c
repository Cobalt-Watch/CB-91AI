/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The boots and the watchdog bites, see counts.h.
 */

#include "counts.h"

static uint16_t larger(uint16_t a, bool a_ok, uint16_t b, bool b_ok)
{
	const uint16_t x = a_ok ? a : 0U;
	const uint16_t y = b_ok ? b : 0U;

	return x > y ? x : y;
}

static uint16_t plus_one(uint16_t n)
{
	return n == UINT16_MAX ? n : (uint16_t)(n + 1U);
}

struct counts counts_boot(const struct counts *retained, bool retained_ok,
			  const struct counts *stored, bool stored_ok, bool bitten)
{
	struct counts c = {
		.boots = larger(retained->boots, retained_ok, stored->boots, stored_ok),
		.bites = larger(retained->bites, retained_ok, stored->bites, stored_ok),
	};

	c.boots = plus_one(c.boots);
	if (bitten) {
		c.bites = plus_one(c.bites);
	}
	return c;
}
