/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * What is left of the CR2016, from its voltage (EF-42, lot D5): the Battery
 * Service percentage and the HELLO of Cobalt Link. Pure logic, no Zephyr: the
 * host tests run it (firmware/tests/host).
 *
 * A lithium coin cell says little through its voltage: the discharge curve of
 * Energizer's CR2016 datasheet (30 kohm, 21 degC) stays on a
 * plateau at 2.93 V for more than half its life, then bends down to 2.0 V
 * (1040 h). On the plateau nothing tells how much is left: 100 %. Past the
 * bend (2.90 V, 600 h), the percentage is the part of the rest of the curve
 * still to come, down to 0 at 2.0 V. The watch reads its cell some 60 mV below
 * that curve (on 2026-09-26: 2.85 to 2.88 V at rest on the plateau), its own
 * load while it reads, and more as the cell ages: the table below is the
 * curve minus 60 mV, and errs on the low side at the end of life.
 *
 * Nothing depends on it in the watch, which behaves the same over the whole
 * range (2026-09-22): it only goes to the phone.
 */

#ifndef CB91AI_WATCH_CELL_H
#define CB91AI_WATCH_CELL_H

#include <stdbool.h>
#include <stdint.h>

/* The reading of the watch at rest, in mV, to 0..100 */
uint8_t cell_percent(int32_t mv);

/* The same at a temperature, in hundredths of a degree (known: false when the
 * thermometer has not read yet): the curve is the datasheet's at 21 degC, and a
 * lithium cell reads lower in the cold without being emptier
 * (2026-09-29). The voltage is brought to 21 degC first, at CELL_MV_PER_C a
 * degree: the slope of a watch's journal at rest, 20 to 35 degC (+0.95 mV,
 * the datasheet giving no other temperature), as the app does. */
#define CELL_REF_CC    2100
#define CELL_MV_PER_C  1
uint8_t cell_percent_at(int32_t mv, int32_t temp_cc, bool known);

#endif /* CB91AI_WATCH_CELL_H */
