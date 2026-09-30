/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The daily call (the Cobalt Link specification): after a day without
 * any contact with the phone, counted from the end of the window of the last
 * call or from the end of the last link, whichever came later, the watch calls
 * it with the reason "daily" (3), so that the time is set again, the crystal
 * calibrated, the status and the journal read and the settings changed in the
 * app delivered. Not during a link, not without a phone to come (the bonded
 * one; any in the development build), and not while a note waits: the recall
 * calls for those already (recall.h). Unanswered, the next comes a day later.
 * Pure logic, tested on the PC (firmware/tests/host/test_daily.c); the event
 * loop asks it at every wake-up.
 */

#ifndef CB91AI_WATCH_DAILY_H
#define CB91AI_WATCH_DAILY_H

#include <stdbool.h>
#include <stdint.h>

/* A day, and the shortest period the development build may set (key 0x7E) */
#define DAILY_PERIOD_S     86400U
#define DAILY_PERIOD_MIN_S 60U

/* The uptime, in s, from which the daily call is due: `period_s` after the end
 * of the window of the last call (`window_s` long) or after the end of the last
 * link, whichever came later (0 for none since boot) */
uint32_t daily_at(uint32_t call_s, uint32_t window_s, uint32_t link_end_s, uint32_t period_s);

/* Whether the daily call is due at `now_s`, the notes aside: no link up
 * (`linked`), a phone to come (`reachable`), a day of silence. The caller
 * counts the notes last, when this says yes, and calls only when none waits. */
bool daily_due(uint32_t now_s, uint32_t call_s, uint32_t window_s, uint32_t link_end_s,
	       uint32_t period_s, bool linked, bool reachable);

#endif /* CB91AI_WATCH_DAILY_H */
