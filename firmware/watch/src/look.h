/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * A look at the watch (lot D4), decided from two orientations of the
 * accelerometer: the one held before a change, and the new one. Axes of the
 * board: X toward 3 o'clock, Y toward 12, Z out of the glass. Traces at the
 * wrist (27 and 28/09;
 * the left one by the axes: arm hanging, 3 o'clock points down), in mg:
 *
 *   arm hanging                x -900  y -470 to -250  z -200 to  350
 *   a look, standing           x    0  y  100 to  600  z  690 to 1050
 *   hands on the keyboard      x  250  y -940 to -390  z  730 to  930
 *   a look from the keyboard   x  100  y  400 to  560  z  800 to  870
 *   the hand flat on the table x   50  y -170 to  -40  z  980 to 1060
 *
 * From the keyboard, a look is a turn of the wrist: Y gains 1 g while Z hardly
 * moves, so a change judged on Z alone missed two looks in three (0.2.1+27).
 * With Y watched too, and any Y gain taken, the hand moving over the keys lit
 * the glass twice in half a minute (0.2.1+28): Y swings there between -940 and
 * -390 mg, but never climbs above the horizontal, as it does in every look.
 * So a look is a new orientation with the glass up (Z at least `face_up_mg`),
 * reached either by tipping the glass up (Z up by at least `tip_mg`), or by
 * turning the wrist until 12 o'clock stands above the horizontal (Y up by at
 * least `toward_mg`, and at least `twelve_up_mg`). Going from the arm hanging
 * to the keyboard still lights the glass: it tips it up as a look does.
 *
 * Pure C, tested on the host (tests/host/test_look.c).
 */

#ifndef CB91AI_WATCH_LOOK_H
#define CB91AI_WATCH_LOOK_H

#include <stdbool.h>
#include <stdint.h>

struct look_limits {
	int16_t face_up_mg;   /* Z (out of the glass) at least this in the new orientation */
	int16_t toward_mg;    /* a turn of the wrist: Y up by at least this... */
	int16_t tip_mg;       /* or a tip of the glass: Z up by at least this */
	int16_t twelve_up_mg; /* after a turn, Y (12 o'clock) at least this */
};

#define LOOK_LIMITS_DEFAULT                                                                        \
	{ .face_up_mg = 500, .toward_mg = 300, .tip_mg = 500, .twelve_up_mg = 0 }

/* A look: the orientation `now` after `before` (x, y, z in mg). Without an
 * orientation before (NULL: the sensor moved its own reference), the glass up
 * is enough, as up to 0.2.1+27. */
bool look_raised(const int32_t before[3], const int32_t now[3], const struct look_limits *l);

#endif /* CB91AI_WATCH_LOOK_H */
