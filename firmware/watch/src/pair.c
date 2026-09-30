/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The pairing of the watch, see pair.h. Pure C: the host tests build this file
 * as it is.
 */

#include "pair.h"

void pair_init(struct pair *p, bool bonded, uint8_t failures)
{
	p->bonded = bonded;
	p->failures = failures;
}

bool pair_open(const struct pair *p)
{
	return !p->bonded && p->failures < PAIR_FAILURES_MAX;
}

uint32_t pair_passkey(uint32_t random)
{
	/* The random bits scaled to [0, 100 000): uniform to one part in 43 000 */
	return (uint32_t)(((uint64_t)random * PAIR_PASSKEY_LIMIT) >> 32);
}

void pair_ended(struct pair *p, bool bonded)
{
	if (bonded) {
		p->bonded = true;
	} else if (p->failures < PAIR_FAILURES_MAX) {
		p->failures++;
	}
}

void pair_reset(struct pair *p)
{
	p->bonded = false;
	p->failures = 0;
}
