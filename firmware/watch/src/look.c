/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * A look at the watch, see look.h. Pure C: the host tests build this file as
 * it is.
 */

#include <stddef.h>

#include "look.h"

bool look_raised(const int32_t before[3], const int32_t now[3], const struct look_limits *l)
{
	if (now[2] < l->face_up_mg) {
		return false; /* the arm let down, turned away: nothing to show */
	}
	if (before == NULL) {
		return true;
	}
	/* The glass tipped up toward the eyes (Z), as from the arm hanging */
	if (now[2] - before[2] >= l->tip_mg) {
		return true;
	}
	/* The wrist turned until 12 o'clock stands up (Y), as from the keyboard;
	 * the hand moving over the keys turns it too, but never that far */
	return now[1] - before[1] >= l->toward_mg && now[1] >= l->twelve_up_mg;
}
