/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Civil calendar, see calendar.h. The two conversions are Howard Hinnant's
 * days_from_civil and civil_from_days (public domain): eras of 400 years,
 * years counted from March so that the leap day ends the year.
 */

#include "calendar.h"

#define MS_PER_DAY (24LL * 60 * 60 * 1000)

int32_t cal_days_from_civil(int32_t year, uint32_t month, uint32_t day)
{
	const int32_t y = year - (month <= 2 ? 1 : 0);
	const int32_t era = (y >= 0 ? y : y - 399) / 400;
	const uint32_t yoe = (uint32_t)(y - era * 400);                           /* [0, 399] */
	const uint32_t doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1; /* [0, 365] */
	const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                 /* [0, 146096] */

	return era * 146097 + (int32_t)doe - 719468;
}

void cal_civil_from_days(int32_t days, int32_t *year, uint32_t *month, uint32_t *day)
{
	const int32_t z = days + 719468;
	const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
	const uint32_t doe = (uint32_t)(z - era * 146097);                         /* [0, 146096] */
	const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399] */
	const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);             /* [0, 365] */
	const uint32_t mp = (5 * doy + 2) / 153;                                  /* [0, 11] */
	const uint32_t d = doy - (153 * mp + 2) / 5 + 1;                          /* [1, 31] */
	const uint32_t m = mp < 10 ? mp + 3 : mp - 9;                             /* [1, 12] */

	*year = (int32_t)yoe + era * 400 + (m <= 2 ? 1 : 0);
	*month = m;
	*day = d;
}

static bool leap(int32_t year)
{
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

uint8_t cal_days_in_month(int32_t year, uint32_t month)
{
	static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	if (month < 1 || month > 12) {
		return 0;
	}
	return (uint8_t)(days[month - 1] + (month == 2 && leap(year) ? 1 : 0));
}

void cal_from_ms(int64_t ms, struct cal_time *out)
{
	int64_t days = ms / MS_PER_DAY;
	int64_t rest = ms % MS_PER_DAY;
	int32_t year;
	uint32_t month, day;

	if (rest < 0) {
		rest += MS_PER_DAY;
		days--;
	}
	cal_civil_from_days((int32_t)days, &year, &month, &day);
	out->year = (int16_t)year;
	out->month = (uint8_t)month;
	out->day = (uint8_t)day;
	out->hour = (uint8_t)(rest / 3600000);
	out->minute = (uint8_t)(rest / 60000 % 60);
	out->second = (uint8_t)(rest / 1000 % 60);
	out->ms = (uint16_t)(rest % 1000);
	/* 1970-01-01 was a Thursday */
	out->weekday = (uint8_t)(days >= -4 ? (days + 4) % 7 : 6 - (-days - 5) % 7);
}

bool cal_to_ms(const struct cal_time *t, int64_t *ms)
{
	if (t->month < 1 || t->month > 12 || t->day < 1 ||
	    t->day > cal_days_in_month(t->year, t->month) || t->hour > 23 || t->minute > 59 ||
	    t->second > 59 || t->ms > 999) {
		return false;
	}
	*ms = (int64_t)cal_days_from_civil(t->year, t->month, t->day) * MS_PER_DAY +
	      ((int64_t)t->hour * 3600 + t->minute * 60 + t->second) * 1000 + t->ms;
	return true;
}
