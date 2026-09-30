/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The boots and the watchdog bites (EF-72, EF-73), the pure part of
 * counters.h: where this boot counts from. No Zephyr: the host tests run it
 * (firmware/tests/host).
 */

#ifndef CB91AI_WATCH_COUNTS_H
#define CB91AI_WATCH_COUNTS_H

#include <stdbool.h>
#include <stdint.h>

struct counts {
	uint16_t boots;
	uint16_t bites; /* watchdog bites */
};

/* The counts of this boot. The retained RAM survives a soft reset; a bite may
 * corrupt it (nRF52840 PS v1.11, reset behavior, p. 90), which its CRC tells
 * (retained_ok false). It holds the boots counted since the settings last kept
 * them; the settings survive a new cell. Each count is the larger of the two it has, then this
 * boot adds one, and one bite if it came from the watchdog, both stopping at
 * 65535 (u16 in Cobalt Link, key 0x10). */
struct counts counts_boot(const struct counts *retained, bool retained_ok,
			  const struct counts *stored, bool stored_ok, bool bitten);

#endif /* CB91AI_WATCH_COUNTS_H */
