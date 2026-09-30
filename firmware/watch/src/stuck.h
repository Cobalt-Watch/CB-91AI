/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * A stuck button (lot D5): a press ties its input to the + of the cell across
 * the internal pull-down, 11 to 16 kOhm (nRF52840 PS v1.11, p. 371): 0.18 to
 * 0.27 mA at 2.9 to 3 V, so a button stuck down (MODE, on the second watch,
 * after its assembly on 2026-09-25) would drain a CR2016 in two to three
 * weeks, some twenty times the whole watch at rest. Held longer
 * than any gesture, it is taken for stuck: the caller disconnects its input,
 * pull-down included, and hides it from the gesture machine; once a minute it
 * connects it again for a probe, until the button reads up. Pure logic, no
 * Zephyr: the host tests run it (firmware/tests/host).
 */

#ifndef CB91AI_WATCH_STUCK_H
#define CB91AI_WATCH_STUCK_H

#include <stdbool.h>
#include <stdint.h>

/* Longer than any gesture: a take held down for its 2 min, the reset's 10 s
 * and the minute it waits for the buttons to be let go */
#define STUCK_HELD_MS  180000U
/* A stuck button is probed this often */
#define STUCK_PROBE_MS 60000U
#define STUCK_MAX      8

struct stuck {
	uint8_t buttons;
	uint8_t down;  /* held, as last read (buttons not stuck) */
	uint8_t stuck; /* taken for stuck: input disconnected */
	uint32_t since[STUCK_MAX]; /* down: when it went down; stuck: the last probe */
};

void stuck_init(struct stuck *s, uint8_t buttons);

/* The levels of the buttons not stuck, read at `now` (the bits of the stuck
 * ones are ignored): the buttons held past STUCK_HELD_MS, and with them any
 * other still down (a chord goes whole), taken for stuck from now on, which
 * the caller disconnects */
uint8_t stuck_update(struct stuck *s, uint8_t levels, uint32_t now);

/* The stuck buttons due for a probe at `now` */
uint8_t stuck_due(const struct stuck *s, uint32_t now);

/* A probe's reading: still down, the button stays stuck until the next probe;
 * up, it is a button like the others again */
void stuck_probed(struct stuck *s, unsigned int button, bool down, uint32_t now);

/* True with the time to the next decision (a button to take for stuck, a
 * probe), false if there is none */
bool stuck_next(const struct stuck *s, uint32_t now, uint32_t *wait);

#endif /* CB91AI_WATCH_STUCK_H */
