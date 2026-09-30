/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Firmware update helpers of the CB-91AI: the spare image slot, the one this
 * image does not run from, is erased ahead of the next update, see update.c.
 */

#ifndef CB91AI_UPDATE_H_
#define CB91AI_UPDATE_H_

#include <stdbool.h>
#include <stdint.h>

/* Slot this image runs from, 0 or 1: MCUboot executes it in place */
int update_running_slot(void);

/* Look at the spare slot once, at boot: is there an old image to clear away? */
void update_spare_init(void);

/*
 * Erase a few pages of the spare slot when the conditions are met. Main loop
 * only (flash writes never run in a Bluetooth or MCUmgr callback). True while
 * work remains: the loop then comes back soon instead of sleeping.
 */
bool update_spare_process(void);

/* A host touched the image slots: they are its business until the next boot. */
void update_spare_cancel(void);

/* 'e' to erase, 'b' busy, 'c' clean, 'k' kept (newer or host image), for the status line */
char update_spare_state(void);

#endif /* CB91AI_UPDATE_H_ */
