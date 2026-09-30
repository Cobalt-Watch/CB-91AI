/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CB91AI_LCDWALK_H
#define CB91AI_LCDWALK_H

#include <stdbool.h>

/* One pixel (COM, SEG) per step, or one SEG plot with its three COM lines */
enum lcdwalk_unit { LCDWALK_PIXEL, LCDWALK_SEG };

int lcdwalk_start(enum lcdwalk_unit unit, int step);	/* step is 0-based */
int lcdwalk_move(int delta);				/* wraps around */
int lcdwalk_goto(int step);
void lcdwalk_stop(void);
bool lcdwalk_active(void);
int lcdwalk_step(void);
int lcdwalk_count(void);

#endif /* CB91AI_LCDWALK_H */
