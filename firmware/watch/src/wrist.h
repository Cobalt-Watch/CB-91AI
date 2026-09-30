/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The wrist (lot D4, CdC 4.8, EF-19, EF-44): the display on demand from the
 * BMA400 (lib/accel.h, accel_wrist_start()). INT1, an orientation change,
 * becomes a raise of the wrist when it is a look (look.h: the glass up, turned
 * toward the eyes since the orientation before); INT2 is a double tap, the
 * fallback. Both lines wake the core through the SENSE of their port
 * (sense-edge-mask of gpio1 in the board): nothing to pay at rest. Never a
 * take from a gesture (EF-19): the loop only lights the glass.
 */

#ifndef CB91AI_WATCH_WRIST_H
#define CB91AI_WATCH_WRIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "accel.h"
#include "look.h"

struct wrist_tuning {
	struct accel_wrist sensor;
	struct look_limits look; /* what a change must be to light the glass (look.h) */
};

#define WRIST_TUNING_DEFAULT { .sensor = ACCEL_WRIST_DEFAULT, .look = LOOK_LIMITS_DEFAULT }

/* The two lines armed, the sensor started with the default tuning */
int wrist_init(void);

enum wrist_event {
	WRIST_NONE,
	WRIST_RAISE, /* the orientation changed into a look at the watch */
	WRIST_TAP,   /* a double tap */
};

/* From the loop, on EVT_MOTION: the status read (the latched lines released)
 * and what came */
enum wrist_event wrist_event(void);

/* A line still up: its event was lost; a latched line rises again only once
 * the status is read (from the loop, now and then), or once the sensor leaves
 * normal mode, which clears every status (datasheet p. 36) */
bool wrist_line_up(void);

/* Counted since boot, and the Z of the last change, for the status line */
void wrist_counts(uint32_t *changes, uint32_t *raises, uint32_t *taps, int32_t *last_z_mg);

/* The tuning, as the value of Cobalt Link key 0x7C on development images: u8
 * wake threshold, u8 wake samples, u8 orientation threshold, u8 orientation
 * duration, u8 tap sensitivity, u16 low-power delay in ms, i16 face-up Z in mg,
 * then (since 0.2.1+17) u8 wake reference, u8 orientation reference, u8 osr_lp,
 * u8 flags (lib/accel.h), then (since 0.2.1+28) i16 the turn of the wrist
 * toward the eyes in mg, then (since 0.2.1+29) i16 the tip of the glass and i16
 * the height of 12 o'clock after a turn, in mg (look.h). A SET of 9, 13 or 15
 * bytes keeps the bytes after them. */
#define WRIST_TUNING_SIZE     19
#define WRIST_TUNING_SIZE_28  15
#define WRIST_TUNING_SIZE_17  13
#define WRIST_TUNING_SIZE_OLD 9
size_t wrist_tuning_get(uint8_t *out, size_t room);

/* A new tuning from the harness: the sensor configured again at once */
bool wrist_tuning_set(const uint8_t *value, size_t len);

/* What the accelerometer said since boot and its registers now, as the value
 * of key 0x7B on development images: u8 layout (1), u8 INT_STAT0 and u8
 * INT_STAT1 of the last event, u8 and u8 every bit of them seen, u16 wake-ups
 * seen (ACCEL_WRIST_WAKE_INT1), u16 engine overruns, u16 lines with nothing in
 * the status, u8 0, then the ACCEL_DUMP_SIZE registers of accel_dump(). 0 if
 * the sensor does not answer. */
#define WRIST_DIAG_SIZE (12 + ACCEL_DUMP_SIZE)
size_t wrist_diag(uint8_t *out, size_t room);

#endif /* CB91AI_WATCH_WRIST_H */
