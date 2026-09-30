/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the pairing policy of the watch (firmware/watch/src/pair.c,
 * lot S1): open on a blank watch only, closed after three failures until the
 * reset, a passkey of five digits.
 */

#include "harness.h"
#include "pair.h"

static void open_on_a_blank_watch_only(void)
{
	struct pair p;

	pair_init(&p, false, 0);
	CHECK(pair_open(&p));
	pair_ended(&p, true);
	CHECK(p.bonded);
	CHECK(!pair_open(&p)); /* one phone: the next is refused */
	/* A bond found at boot: closed too */
	pair_init(&p, true, 0);
	CHECK(!pair_open(&p));
	/* Only the reset opens it again */
	pair_reset(&p);
	CHECK(pair_open(&p));
}

static void three_failures_close_it(void)
{
	struct pair p;

	pair_init(&p, false, 0);
	pair_ended(&p, false);
	pair_ended(&p, false);
	CHECK(pair_open(&p));
	pair_ended(&p, false);
	CHECK(!pair_open(&p));
	CHECK_EQ(p.failures, PAIR_FAILURES_MAX);
	/* Counted no further, and still closed after a reboot */
	pair_ended(&p, false);
	CHECK_EQ(p.failures, PAIR_FAILURES_MAX);
	pair_init(&p, false, p.failures);
	CHECK(!pair_open(&p));
	/* A bond made meanwhile is impossible: the reset alone reopens it */
	pair_reset(&p);
	CHECK(pair_open(&p));
	CHECK_EQ(p.failures, 0);
}

static void five_digits(void)
{
	static const uint32_t samples[] = { 0U, 1U, 42949U, 42950U, 0x7fffffffU, 0x80000000U,
					    0xfffffffeU, 0xffffffffU };
	uint32_t seen_low = 0, seen_high = 0;

	for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
		CHECK(pair_passkey(samples[i]) < PAIR_PASSKEY_LIMIT);
	}
	CHECK_EQ(pair_passkey(0U), 0);
	CHECK_EQ(pair_passkey(0xffffffffU), 99999);
	CHECK_EQ(pair_passkey(0x80000000U), 50000);
	/* Spread: every ten-thousand band reached by a sweep of the bits */
	for (uint64_t r = 0; r <= 0xffffffffULL; r += 0x01000000ULL) {
		const uint32_t k = pair_passkey((uint32_t)r);

		seen_low |= k < 50000U;
		seen_high |= k >= 50000U;
	}
	CHECK(seen_low && seen_high);
}

int main(void)
{
	RUN(open_on_a_blank_watch_only);
	RUN(three_failures_close_it);
	RUN(five_digits);
	return harness_report("pair");
}
