/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CB91AI_WATCH_POWER_H
#define CB91AI_WATCH_POWER_H

#include <stdint.h>

/* The SAADC channel of VDD set up. 0 on success. */
int power_init(void);

/* The cell in mV (the rail, which it is on the reworked V1 and on the V2);
 * its percentage: cell.h */
int power_vdd_mv(int32_t *mv);

#endif /* CB91AI_WATCH_POWER_H */
