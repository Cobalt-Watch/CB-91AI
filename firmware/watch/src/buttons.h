/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The three case buttons of the watch (lot D3). Their edges wake the event
 * loop; the loop reads the levels and runs the gesture machine of
 * lib/gesture.c, which says when to look again: nothing polls.
 */

#ifndef CB91AI_WATCH_BUTTONS_H
#define CB91AI_WATCH_BUTTONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gesture.h"

/* Index of each button in the gestures, and bit in a chord mask */
enum button {
	BUTTON_LIGHT, /* L, top left */
	BUTTON_MODE,  /* C, bottom left */
	BUTTON_ALARM, /* A, bottom right: the voice */
	BUTTON_COUNT,
};

#define BUTTONS_RESET_CHORD (BIT(BUTTON_MODE) | BIT(BUTTON_ALARM))

/* Inputs and edge interrupts from the devicetree. 0 on success. */
int buttons_init(void);

/* On EVT_BUTTON_EDGE, from the event loop only: what the buttons did since the
 * last call, oldest first; `out` has room for GESTURE_OUT_MAX(BUTTON_COUNT). */
size_t buttons_process(struct gesture *out);

/* When button `b` last moved, in ms of uptime, stamped by its interrupt
 * (before the debounce), and whether it is down as the loop last read it:
 * where its click falls in a take (clicks.h) */
uint32_t buttons_changed_at(enum button b);
bool buttons_down(enum button b);

/* The buttons taken for stuck, a bit each (stuck.h), for the status line */
uint8_t buttons_stuck(void);

#endif /* CB91AI_WATCH_BUTTONS_H */
