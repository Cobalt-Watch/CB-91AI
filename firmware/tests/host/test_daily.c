/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the daily call (firmware/watch/src/daily.c): a day after the
 * last contact with the phone, the end of a call's window or of a link,
 * whichever came later; never while a link is up or without a phone to come;
 * none while the phone comes every day anyway. The event loop of main.c is
 * played here, one wake-up every 5 s at the least, as in test_recall.c.
 */

#include <stdbool.h>

#include "harness.h"
#include "daily.h"

#define WAKE_S   5U   /* main.c: LOOP_WAKE_MS */
#define WINDOW_S 130U /* radio.c: the window of a call, as recall.h counts it */

static void a_day_after_the_boot(void)
{
	/* No call and no link since the boot (both 0): a day after the window
	 * the boot would have had */
	const uint32_t at = daily_at(0, WINDOW_S, 0, DAILY_PERIOD_S);

	CHECK_EQ(at, WINDOW_S + DAILY_PERIOD_S);
	CHECK(!daily_due(at - 1, 0, WINDOW_S, 0, DAILY_PERIOD_S, false, true));
	CHECK(daily_due(at, 0, WINDOW_S, 0, DAILY_PERIOD_S, false, true));
}

static void any_contact_puts_it_off(void)
{
	/* A link that ends after the window of the last call: a day from its end */
	CHECK_EQ(daily_at(1000, WINDOW_S, 50000, DAILY_PERIOD_S), 50000 + DAILY_PERIOD_S);
	/* A call after the last link: a day from the end of its window */
	CHECK_EQ(daily_at(60000, WINDOW_S, 50000, DAILY_PERIOD_S), 60000 + WINDOW_S + DAILY_PERIOD_S);
	/* A link lost within the window of its call: the window counts */
	CHECK_EQ(daily_at(60000, WINDOW_S, 60010, DAILY_PERIOD_S), 60000 + WINDOW_S + DAILY_PERIOD_S);
}

static void never_linked_nor_alone(void)
{
	const uint32_t late = 10U * DAILY_PERIOD_S;

	/* During a link, the phone is there already */
	CHECK(!daily_due(late, 0, WINDOW_S, 0, DAILY_PERIOD_S, true, true));
	/* No phone may come: a watch new or reset keeps quiet */
	CHECK(!daily_due(late, 0, WINDOW_S, 0, DAILY_PERIOD_S, false, false));
	CHECK(daily_due(late, 0, WINDOW_S, 0, DAILY_PERIOD_S, false, true));
}

/* A week in the life of the watch, woken every WAKE_S: `link_every_s` apart,
 * the phone comes by itself and stays 10 s (0: never). The daily calls made,
 * and the shortest and longest time between two. */
static unsigned int week(uint32_t link_every_s, uint32_t period_s, uint32_t *min_gap,
			 uint32_t *max_gap)
{
	uint32_t call_s = 0, link_end_s = 0, last_daily = 0;
	unsigned int calls = 0;

	*min_gap = UINT32_MAX;
	*max_gap = 0;
	for (uint32_t now = WAKE_S; now <= 7U * DAILY_PERIOD_S; now += WAKE_S) {
		const bool linked = link_every_s != 0 && now % link_every_s < 10U;

		if (link_every_s != 0 && now % link_every_s == 10U) {
			link_end_s = now; /* the phone leaves */
		}
		if (daily_due(now, call_s, WINDOW_S, link_end_s, period_s, linked, true)) {
			if (calls > 0) {
				const uint32_t gap = now - last_daily;

				*min_gap = gap < *min_gap ? gap : *min_gap;
				*max_gap = gap > *max_gap ? gap : *max_gap;
			}
			call_s = now; /* radio_call(): stamped at once */
			last_daily = now;
			calls++;
		}
	}
	return calls;
}

static void alone_for_a_week(void)
{
	uint32_t min_gap, max_gap;
	const unsigned int calls = week(0, DAILY_PERIOD_S, &min_gap, &max_gap);

	/* Days 1 to 6: the seventh would come after the week, the window counting */
	CHECK_EQ(calls, 6);
	CHECK(min_gap >= DAILY_PERIOD_S + WINDOW_S);
	CHECK(max_gap <= DAILY_PERIOD_S + WINDOW_S + WAKE_S);
}

static void the_phone_comes_every_day(void)
{
	uint32_t min_gap, max_gap;

	/* Every 20 h, or every day on the dot: no daily call at all */
	CHECK_EQ(week(20U * 3600U, DAILY_PERIOD_S, &min_gap, &max_gap), 0);
	CHECK_EQ(week(DAILY_PERIOD_S, DAILY_PERIOD_S, &min_gap, &max_gap), 0);
	/* Every 30 h: a call between two of its visits, now and then */
	CHECK(week(30U * 3600U, DAILY_PERIOD_S, &min_gap, &max_gap) > 0);
}

static void minutes_in_development(void)
{
	uint32_t min_gap, max_gap;

	/* Key 0x7E: 60 s, for a test in minutes; the window still counts */
	CHECK_EQ(daily_at(1000, WINDOW_S, 900, DAILY_PERIOD_MIN_S), 1000 + WINDOW_S + 60);
	CHECK(week(0, DAILY_PERIOD_MIN_S, &min_gap, &max_gap) > 3000);
	CHECK(min_gap >= DAILY_PERIOD_MIN_S + WINDOW_S);
}

int main(void)
{
	RUN(a_day_after_the_boot);
	RUN(any_contact_puts_it_off);
	RUN(never_linked_nor_alone);
	RUN(alone_for_a_week);
	RUN(the_phone_comes_every_day);
	RUN(minutes_in_development);
	return harness_report("daily");
}
