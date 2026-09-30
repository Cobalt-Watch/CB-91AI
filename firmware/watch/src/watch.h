/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * What the event loop (main.c) lends the rest of the watch.
 */

#ifndef CB91AI_WATCH_WATCH_H
#define CB91AI_WATCH_WATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The cell at the last reading, in mV (0 before the first) */
uint16_t watch_battery_mv(void);

/* How long the display stays lit after a wake-up (EF-44), 1 to 120 s; kept in
 * the settings. From the loop only, as the setters below. */
uint8_t watch_display_s(void);
bool watch_set_display_s(uint8_t seconds);

/* The light of LIGHT (Cobalt Link key 0x05, light.h): false when refused */
bool watch_set_light(const uint8_t *value, size_t len);
size_t watch_light(uint8_t *out, size_t room);

/* The name of the watch (Cobalt Link key 0x06): false when refused (1 to 11
 * bytes of printable ASCII); taken once the link is down, and kept through
 * the reset. The name to be, or the one in force. */
bool watch_set_name(const uint8_t *value, size_t len);
size_t watch_name(uint8_t *out, size_t room);

/* The status line: version, cell, uptime, image, radio, clock... */
size_t watch_status(char *buf, size_t room);

/* A take of `seconds` without the voice detector, for the harness
 * (CONFIG_CB91AI_WATCH_DEBUG_TAKE): false when one runs already */
bool watch_debug_take(uint8_t seconds);

/* The period of the journal (lot T1), 1 to 600 s until the next boot, for the
 * harness (CONFIG_CB91AI_WATCH_DEBUG_JOURNAL): false out of range */
bool watch_set_journal_period(uint16_t seconds);

/* The period of the daily call, 60 to 86400 s until the next boot, for the
 * harness (CONFIG_CB91AI_WATCH_DEBUG_DAILY): false out of range */
bool watch_set_daily_period(uint32_t seconds);

#endif /* CB91AI_WATCH_WATCH_H */
