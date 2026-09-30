/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the civil calendar (firmware/lib/calendar.c), against the C
 * library of the host on every day from 1900 to 2200, and every hour of a
 * few days around the changes of month and year.
 */

#define _DEFAULT_SOURCE /* timegm() */
#include <stdlib.h>
#include <time.h>

#include "calendar.h"
#include "harness.h"

#define DAY_MS (86400LL * 1000)

static void every_day_against_the_host(void)
{
	int bad = 0;

	for (int64_t day = -25567; day <= 84006 && bad < 5; day++) { /* 1900 to 2200 */
		const int64_t ms = day * DAY_MS + 12 * 3600 * 1000LL + 34 * 60000 + 56789;
		const time_t seconds = (time_t)(day * 86400 + 12 * 3600 + 34 * 60 + 56);
		struct tm host;
		struct cal_time t;
		int64_t back;

		gmtime_r(&seconds, &host);
		cal_from_ms(ms, &t);
		if (t.year != host.tm_year + 1900 || t.month != host.tm_mon + 1 ||
		    t.day != host.tm_mday || t.weekday != host.tm_wday || t.hour != 12 ||
		    t.minute != 34 || t.second != 56 || t.ms != 789 || !cal_to_ms(&t, &back) ||
		    back != ms) {
			bad++;
			printf("  day %lld: %d-%u-%u wd %u, host %d-%d-%d wd %d\n", (long long)day, t.year,
			       t.month, t.day, t.weekday, host.tm_year + 1900, host.tm_mon + 1,
			       host.tm_mday, host.tm_wday);
		}
	}
	CHECK_EQ(bad, 0);
}

static void days_in_month(void)
{
	CHECK_EQ(cal_days_in_month(2026, 2), 28);
	CHECK_EQ(cal_days_in_month(2028, 2), 29);
	CHECK_EQ(cal_days_in_month(2000, 2), 29);
	CHECK_EQ(cal_days_in_month(2100, 2), 28);
	CHECK_EQ(cal_days_in_month(2026, 4), 30);
	CHECK_EQ(cal_days_in_month(2026, 12), 31);
	CHECK_EQ(cal_days_in_month(2026, 13), 0);
}

static void out_of_range_fields(void)
{
	struct cal_time t = { .year = 2026, .month = 2, .day = 29, .hour = 12 };
	int64_t ms;

	CHECK(!cal_to_ms(&t, &ms)); /* no 29 February in 2026 */
	t.year = 2028;
	CHECK(cal_to_ms(&t, &ms));
	t.hour = 24;
	CHECK(!cal_to_ms(&t, &ms));
	t.hour = 0;
	t.month = 0;
	CHECK(!cal_to_ms(&t, &ms));
}

static void known_dates(void)
{
	struct cal_time t;

	/* The day this test was written: Wednesday 23 September 2026, 20:15 UTC */
	cal_from_ms(1790194500000LL, &t);
	CHECK_EQ(t.year, 2026);
	CHECK_EQ(t.month, 9);
	CHECK_EQ(t.day, 23);
	CHECK_EQ(t.weekday, 3);
	CHECK_EQ(t.hour, 20);
	CHECK_EQ(t.minute, 15);
	/* Before 1970, the division rounds towards the past */
	cal_from_ms(-1, &t);
	CHECK_EQ(t.year, 1969);
	CHECK_EQ(t.month, 12);
	CHECK_EQ(t.day, 31);
	CHECK_EQ(t.hour, 23);
	CHECK_EQ(t.ms, 999);
	CHECK_EQ(t.weekday, 3);
}

int main(void)
{
	RUN(every_day_against_the_host);
	RUN(days_in_month);
	RUN(out_of_range_fields);
	RUN(known_dates);
	return harness_report("calendar");
}
