/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Gestures of the case buttons (EF-10 to EF-17, lot D3): debounce, clicks, long
 * press, and chords of several buttons, as a pure state machine. No Zephyr in
 * here: the machine is given the levels of the buttons and the time, and says
 * when it wants to be called again. The same code runs in the watch, in the
 * self-test and in the host tests (firmware/tests/host).
 *
 * The timings are those of the V1 (EF-17; Jean-de-B/Cobalt, button_manager.cpp):
 * a level must hold 50 ms to count, clicks less than 300 ms apart add up to a
 * double or a triple, and a press held 500 ms is a long press. As in the V1, a
 * press is reported as soon as it counts, before anyone knows whether it will
 * be a click or a long press: the recorder starts on it and drops the take if
 * a click comes instead ("record then cancel", EF-10), so that the first words
 * are not lost to the 500 ms of the long press.
 *
 * Two buttons or more held together make a chord (the reset of phase S: the
 * two bottom buttons for 10 s). A chord cancels what its presses began: no
 * click, long press or end of long press follows from any button until every
 * button is up again.
 */

#ifndef CB91AI_GESTURE_H
#define CB91AI_GESTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GESTURE_BUTTONS_MAX 8
#define GESTURE_CLICKS_MAX  3

/* Room for what one call to gesture_update() can report, for n buttons */
#define GESTURE_OUT_MAX(n) (2U * (n) + 2U)

struct gesture_cfg {
	uint16_t debounce_ms;   /* a level must hold this long to count */
	uint16_t multi_ms;      /* from a release to the next press, clicks add up */
	uint16_t long_ms;       /* a press held this long is a long press */
	uint16_t chord_step_ms; /* a chord held reports its duration at each step */
};

/* EF-17, the values of the V1, and a chord reported every second */
#define GESTURE_CFG_V1                                                                             \
	{ .debounce_ms = 50, .multi_ms = 300, .long_ms = 500, .chord_step_ms = 1000 }

enum gesture_type {
	/* A button counts as down (count: clicks already made in this series).
	 * A take may start here, and be dropped if a click follows. */
	GESTURE_PRESS,
	/* 1 to 3 short presses (count), reported once the window after the last
	 * release has closed; more than three count as three */
	GESTURE_CLICK,
	/* Held past long_ms (count: short presses just before, dropped) */
	GESTURE_LONG,
	/* Released after a long press; ms: how long it was held */
	GESTURE_LONG_END,
	/* Two buttons or more down together; button: the bit mask of them; ms:
	 * the time between the first and the last press of them (a chord pressed
	 * on purpose is pressed at once, a stuck button makes it long) */
	GESTURE_CHORD,
	/* The same chord still held; ms: for how long, at every chord_step_ms */
	GESTURE_CHORD_HOLD,
	/* The chord broke up; ms: how long it was held */
	GESTURE_CHORD_END,
};

struct gesture {
	uint8_t type;   /* enum gesture_type */
	uint8_t button; /* index of the button; for a chord, bit mask of its buttons */
	uint8_t count;
	uint32_t ms;
};

/* The machine of one button; internal, in the header for its size only */
struct gesture_button {
	uint8_t phase;
	uint8_t clicks;
	bool raw;       /* level last seen */
	bool stable;    /* level that counts */
	uint32_t raw_at; /* when the level last seen appeared */
	uint32_t down_at;
	uint32_t up_at;
};

struct gesture_state {
	struct gesture_cfg cfg;
	uint8_t buttons;
	uint8_t chord;       /* mask of the chord in force, 0: none */
	bool muted;          /* after a chord, until every button is up */
	uint32_t chord_at;
	uint32_t chord_steps; /* steps of the chord reported so far */
	struct gesture_button b[GESTURE_BUTTONS_MAX];
};

/* Every button up. `buttons` is at most GESTURE_BUTTONS_MAX. */
void gesture_init(struct gesture_state *s, const struct gesture_cfg *cfg, uint8_t buttons);

/*
 * The levels of the buttons (bit n set: button n pressed) as read at `now`, in
 * ms, on any edge of any button and whenever the wait of gesture_next() is
 * over. Writes what happened to `out`, which must have room for
 * GESTURE_OUT_MAX(buttons) gestures, oldest first, and returns how many. The
 * clock may wrap around: only differences of less than 24 days are compared.
 */
size_t gesture_update(struct gesture_state *s, uint32_t levels, uint32_t now,
		      struct gesture *out);

/* How long from `now` until gesture_update() wants to be called again if no
 * edge comes first (0: at once); false when nothing is pending. */
bool gesture_next(const struct gesture_state *s, uint32_t now, uint32_t *wait_ms);

#endif /* CB91AI_GESTURE_H */
