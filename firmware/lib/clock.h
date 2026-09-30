/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wall clock of the watch (EF-01 to EF-03): UTC time kept on the 32.768 kHz
 * crystal, with a software correction of the crystal error. See clock.c.
 */

#ifndef CB91AI_CLOCK_H
#define CB91AI_CLOCK_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <zephyr/sys/util.h>

/* Status bits, also sent in the BLE time characteristic */
#define CLOCK_FLAG_SET         BIT(0) /* the time was given by a host, or restored */
#define CLOCK_FLAG_APPROXIMATE BIT(1) /* restored after a reset: late by the length of the reset */
#define CLOCK_FLAG_CALIBRATED  BIT(2) /* the correction was refined from two time settings */

/* Load the stored correction, take the time back from retained RAM if a soft
 * reset or a watchdog left it there. Call once, from the main thread.
 */
int clock_init(void);

bool clock_is_set(void);
uint8_t clock_flags(void);

/* UTC, milliseconds since 1970, corrected; 0 while the clock is not set. */
int64_t clock_now_ms(void);

/* Local time (UTC + time zone given by the host), for the display. */
int16_t clock_tz_minutes(void);
bool clock_local_tm(struct tm *out);

/*
 * Set the time, as a host does at every connection (EI-03). When two settings
 * are more than an hour apart with no reset in between, the crystal error is
 * measured from them and the correction refined: the watch calibrates itself on
 * the clock of the phone, with no instrument. Callable from any thread; the
 * flash write of a new correction is left to clock_process().
 */
void clock_set(int64_t unix_ms, int16_t tz_minutes, bool calibrate);

/* Display format: 24-hour, or 12-hour with the PM indicator (EF-04). Default
 * from CONFIG_CB91AI_CLOCK_24H, then the choice stored in the settings.
 */
bool clock_24h(void);
void clock_set_24h(bool enable);

/* The time as the glass shows it: six characters HHMMSS for positions 4 to 9,
 * hours without a leading zero, and whether the PM indicator is lit.
 */
bool clock_display(char digits[7], bool *pm, struct tm *tm);

/* Crystal error in parts per billion, positive when the crystal runs fast
 * (the V1 boards do: load capacitors too small, V2-02). 100 ppm = 100000.
 */
int32_t clock_ppb(void);
int clock_set_ppb(int32_t ppb);

/* Copy the time to retained RAM, where it survives a reset. RAM only: safe
 * from any thread, the MCUmgr reset hook included.
 */
void clock_retain(void);

/* Main loop housekeeping: clock_retain(), and the flash write of a correction
 * that changed. Main thread only (flash write, stack). Cheap, call it at every
 * iteration.
 */
void clock_process(void);

#endif /* CB91AI_CLOCK_H */
