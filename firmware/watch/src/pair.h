/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The pairing of the watch (lot S1): one
 * phone only; pairing open only on a watch that holds no bond, new or reset by
 * the two bottom buttons held 10 s, which also erases the notes
 * (2026-09-24: no gesture reopens it otherwise); closed after three failures
 * until that reset; a passkey under 100 000, the five digits the V1 glass can
 * draw, the phone taking a 0 before them. Pure logic, tested on the PC
 * (firmware/tests/host/test_pair.c); pairing.c gives it Bluetooth.
 */

#ifndef CB91AI_WATCH_PAIR_H
#define CB91AI_WATCH_PAIR_H

#include <stdbool.h>
#include <stdint.h>

#define PAIR_FAILURES_MAX  3U
#define PAIR_PASSKEY_LIMIT 100000U /* five digits on the V1 */

struct pair {
	bool bonded;       /* a phone holds the bond */
	uint8_t failures;  /* since the last reset, kept across a reboot */
};

void pair_init(struct pair *p, bool bonded, uint8_t failures);

/* A pairing may begin: no bond, fewer than three failures */
bool pair_open(const struct pair *p);

/* The passkey of a pairing, from 32 random bits: under 100 000 */
uint32_t pair_passkey(uint32_t random);

/* A pairing ended: bonded, or one failure more (a wrong code, a cancel) */
void pair_ended(struct pair *p, bool bonded);

/* The watch is reset: no bond, no failure, open again */
void pair_reset(struct pair *p);

#endif /* CB91AI_WATCH_PAIR_H */
