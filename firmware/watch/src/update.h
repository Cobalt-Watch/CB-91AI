/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Updates of the watch over SMP (lot D6): the MCUmgr hooks, "UPd" on the glass
 * before the reset, and the safety net of a first boot. See update.c.
 */

#ifndef CB91AI_WATCH_UPDATE_H
#define CB91AI_WATCH_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

/* Hooks registered; the countdown of an image in test started */
void update_init(void (*wdt_feed)(void));

/* From the event loop at each event: an image nobody confirmed reboots,
 * and MCUboot takes the previous one back */
void update_process(void);

/* From the event loop on EVT_UPDATE_RESET, after "UPd" is on the glass:
 * lets the reset go */
void update_reset_painted(void);

bool update_confirmed(void);
int update_slot(void);

/* An image is on its way: a chunk came less than 30 s ago. A take does not
 * begin meanwhile, it would stop the upload. */
bool update_busy(void);

#endif /* CB91AI_WATCH_UPDATE_H */
