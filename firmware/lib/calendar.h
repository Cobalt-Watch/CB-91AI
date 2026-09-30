/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Civil calendar for the watch: milliseconds since 1970 to date and time, and
 * back, in pure C (no libc time functions, no Zephyr), so that the host tests
 * check it on every day of the range (firmware/tests/host). Proleptic
 * Gregorian calendar, the algorithms of Howard Hinnant's "chrono-compatible
 * low-level date algorithms" (public domain).
 */

#ifndef CB91AI_CALENDAR_H
#define CB91AI_CALENDAR_H

#include <stdbool.h>
#include <stdint.h>

struct cal_time {
	int16_t year;    /* 1970 to 2099 for the watch; the algorithms go further */
	uint8_t month;   /* 1 to 12 */
	uint8_t day;     /* 1 to 31 */
	uint8_t hour;    /* 0 to 23 */
	uint8_t minute;
	uint8_t second;
	uint8_t weekday; /* 0 Sunday to 6 Saturday */
	uint16_t ms;
};

/* Days since 1970-01-01 of a date, and back */
int32_t cal_days_from_civil(int32_t year, uint32_t month, uint32_t day);
void cal_civil_from_days(int32_t days, int32_t *year, uint32_t *month, uint32_t *day);

/* 28 to 31 */
uint8_t cal_days_in_month(int32_t year, uint32_t month);

/* Milliseconds since 1970 (in whatever zone they are counted) to fields */
void cal_from_ms(int64_t ms, struct cal_time *out);

/* Fields to milliseconds since 1970; false if a field is out of range */
bool cal_to_ms(const struct cal_time *t, int64_t *ms);

#endif /* CB91AI_CALENDAR_H */
