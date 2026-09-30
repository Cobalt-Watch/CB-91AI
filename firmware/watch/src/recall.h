/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * When the watch calls the phone again for the notes that wait (lot E2, EF-30,
 * the Cobalt Link specification): once the radio has been silent long
 * enough, silent meaning past the window of the last call and past the end of
 * the last link, as long as a note waits, no link is up and a phone can take
 * the notes (the bonded one; any in the development build). The silence is
 * 15 min after a call for another reason (a note kept, a press, the boot), then
 * doubles at each call again, up to an hour (2026-09-25): calls at
 * 17 min 10, 49 min 20, then about once an hour while nobody answers.
 * Pure logic, tested on the PC (firmware/tests/host/test_recall.c); the event
 * loop asks it at every wake-up.
 */

#ifndef CB91AI_WATCH_RECALL_H
#define CB91AI_WATCH_RECALL_H

#include <stdbool.h>
#include <stdint.h>

/* The window of a call (radio.c): 100 ms for 10 s, then 1 s up to 2 min 10 */
#define RECALL_WINDOW_S      130U
/* The silence after it: 15 min, doubled at each call again up to an hour */
#define RECALL_SILENCE_S     900U
#define RECALL_SILENCE_MAX_S 3600U

/* The silence before call again number `again` since a call for another
 * reason (0: the first): 15 min, 30 min, then an hour */
uint32_t recall_silence(uint32_t again);

/* The uptime, in s, from which the phone is due call again number `again`,
 * from the uptimes of the last call and of the end of the last link (0 for
 * none since boot) */
uint32_t recall_at(uint32_t call_s, uint32_t link_end_s, uint32_t again);

/* Whether the phone is due another call at `now_s`, the notes aside: no link
 * up (`linked`), a phone to take them (`reachable`), silence long enough. The
 * caller counts the notes last, when this says yes. */
bool recall_due(uint32_t now_s, uint32_t call_s, uint32_t link_end_s, uint32_t again,
		bool linked, bool reachable);

#endif /* CB91AI_WATCH_RECALL_H */
