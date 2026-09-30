/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The pairing of the watch over Bluetooth (lot S1), the rules being pair.h's:
 * LE Secure Connections only, the passkey shown on the glass and typed on the
 * phone, one bond, kept in the settings with the count of failures. The
 * Bluetooth host calls in its own threads: this file counts there, posts
 * EVT_PAIRING, and the event loop draws the code and writes the count
 * (pairing_keep()).
 */

#ifndef CB91AI_WATCH_PAIRING_H
#define CB91AI_WATCH_PAIRING_H

#include <stdbool.h>
#include <stdint.h>

/* EVT_PAIRING: `arg` says what, `data` holds the passkey to show */
#define PAIRING_SHOW   1U
#define PAIRING_BONDED 2U
#define PAIRING_FAILED 3U /* counted: a wrong code, a time-out, a refusal */
#define PAIRING_HIDE   4U /* the code goes; the failure, if any, comes apart */

/* After bt_enable() and the load of the settings: the callbacks, and whether
 * a phone holds the bond */
int pairing_init(void);

bool pairing_bonded(void);
bool pairing_open(void);

/* From the loop, at every wake-up: the count of failures, when it moved,
 * written to the settings (the callbacks count, the loop writes) */
void pairing_keep(void);

/* The watch reset (lot S3): every bond forgotten, the failures too */
void pairing_reset(void);

#endif /* CB91AI_WATCH_PAIRING_H */
