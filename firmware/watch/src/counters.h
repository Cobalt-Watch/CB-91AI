/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The boots and the watchdog bites of the watch (EF-72, EF-73), read by the
 * phone in the counters of Cobalt Link (key 0x10). Counted at once in the
 * retained RAM (counters_retention in the devicetree), so that a watch
 * rebooting in a loop, the very case to see, is counted too; kept in the
 * settings once the watch has run a minute, as nothing is written to the flash
 * early in a boot (on the cell). The reset of phase S keeps them,
 * as it keeps the quartz's correction: they belong to the watch.
 */

#ifndef CB91AI_WATCH_COUNTERS_H
#define CB91AI_WATCH_COUNTERS_H

#include <stdint.h>

/* The settings running: this boot counted, and its reset cause (hwinfo) */
void counters_boot(uint32_t reset_cause);

/* From the loop, now and then: into the settings, once past the first minute
 * and only when they changed */
void counters_keep(void);

uint16_t counters_boots(void);
uint16_t counters_bites(void);

#endif /* CB91AI_WATCH_COUNTERS_H */
