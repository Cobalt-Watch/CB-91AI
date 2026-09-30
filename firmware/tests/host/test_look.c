/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of a look at the watch (firmware/watch/src/look.c), on the
 * orientations read at the wrist by `cobalt_link.py --trace wrist` on 27
 * and 28/09: x, y, z in mg, each pair the orientation
 * before a change and the one after, as the watch holds them.
 */

#include <stdbool.h>
#include <stddef.h>

#include "harness.h"
#include "look.h"

static const struct look_limits limits = LOOK_LIMITS_DEFAULT;

static bool look(const int32_t before[3], const int32_t now[3])
{
	return look_raised(before, now, &limits);
}

/* 28/09 at 13:02 and 13:03 (0.2.1+28), standing: from the arm hanging */
static void standing(void)
{
	static const int32_t hanging1[3] = { -883, -311, 236 }, look1[3] = { 25, 598, 779 };
	static const int32_t hanging2[3] = { -918, -471, 84 }, look2[3] = { 92, 494, 795 };
	static const int32_t hanging3[3] = { -863, -340, 189 }, look3[3] = { 25, 410, 686 };
	/* 28/09 at noon (0.2.1+27): the glass flat up */
	static const int32_t hanging4[3] = { -900, -250, -200 }, look4[3] = { 0, 100, 1000 };

	CHECK(look(hanging1, look1)); /* Z up by 543 */
	CHECK(look(hanging2, look2)); /* Z up by 711 */
	CHECK(look(hanging3, look3)); /* Z up by 497 only, but 12 o'clock up at 410 */
	CHECK(look(hanging4, look4));
	CHECK(!look(look1, hanging1)); /* the arm let down */
	CHECK(!look(hanging1, hanging1));
}

/* 28/09, 13:03 to 13:05, at the keyboard (0.2.1+28) */
static const int32_t keys_a[3] = { 152, -477, 877 };
static const int32_t from_keys_a[3] = { 131, 555, 824 };
static const int32_t keys_b[3] = { 203, -416, 877 };
static const int32_t from_keys_b[3] = { 96, 562, 820 };

static void at_the_keyboard(void)
{
	/* The turn of the wrist that Z alone missed two times in three */
	CHECK(look(keys_a, from_keys_a));
	CHECK(look(keys_b, from_keys_b));
	/* Back to the keyboard: glass still up, but turned away */
	CHECK(!look(from_keys_a, keys_a));
}

static void typing(void)
{
	/* The two lights of 0.2.1+28 while typing: Y up by 318, then Z up by 480
	 * and Y by 574, 12 o'clock staying below the horizontal */
	static const int32_t a[3] = { 578, -820, 920 }, a_after[3] = { 367, -502, 801 };
	static const int32_t b[3] = { 254, -939, 420 }, b_after[3] = { 121, -365, 900 };

	CHECK(!look(a, a_after));
	CHECK(!look(b, b_after));
}

static void what_still_lights(void)
{
	/* From the arm hanging to the keyboard: the glass tips up by 930 mg, as a
	 * look would; known, and accepted (look.h) */
	static const int32_t hanging[3] = { -900, -250, -200 }, keys[3] = { 200, -650, 730 };
	/* The hand laid flat on the table from the keyboard (13:04:56): no more,
	 * 12 o'clock stays below the horizontal */
	static const int32_t keys_c[3] = { 295, -561, 805 }, flat[3] = { 16, -41, 1027 };
	/* A jolt with the glass turned down, then back to the keys (13:04:44):
	 * the glass tips up by 1 g, it lights */
	static const int32_t down[3] = { 338, -918, -242 }, back[3] = { 281, -594, 764 };

	CHECK(look(hanging, keys));
	CHECK(!look(keys_c, flat));
	CHECK(look(down, back));
}

static void the_looks_of_27_09(void)
{
	/* 27/09 at 23:50, seated: the arm down with 12 o'clock down, then looks
	 * that the watch counted; the third was a turn of the wrist that left 12
	 * o'clock below the horizontal, and no longer counts */
	static const int32_t down[3] = { 70, -990, 220 }, look1[3] = { 146, -328, 895 };
	static const int32_t before2[3] = { 471, -803, 39 }, look2[3] = { 248, -574, 775 };
	static const int32_t before3[3] = { -318, -1141, 594 }, look3[3] = { -568, -396, 762 };

	CHECK(look(down, look1));      /* Z up by 675 */
	CHECK(look(before2, look2));   /* Z up by 736 */
	CHECK(!look(before3, look3));  /* Z up by 168, Y up by 745 to -396 */
}

static void the_limits(void)
{
	static const int32_t from[3] = { 0, -500, 0 };
	int32_t to[3] = { 0, -500, 500 };

	CHECK(look(from, to)); /* Z at the glass up, up by exactly 500 */
	to[2] = 499;
	CHECK(!look(from, to)); /* the glass not up */
	{
		static const int32_t low[3] = { 0, -500, 100 };
		int32_t up[3] = { 0, -500, 599 };

		CHECK(!look(low, up)); /* Z up by 499 */
		up[2] = 600;
		CHECK(look(low, up));
	}
	/* A turn of the wrist: 12 o'clock up by 400, to the horizontal */
	{
		static const int32_t keys[3] = { 0, -400, 800 };
		int32_t turned[3] = { 0, 0, 800 };

		CHECK(look(keys, turned));
		turned[1] = -1;
		CHECK(!look(keys, turned)); /* up by 399, but below the horizontal */
		{
			static const int32_t near[3] = { 0, -299, 800 };
			static const int32_t across[3] = { 0, 0, 800 };
			static const int32_t short_turn[3] = { 0, 1, 800 };

			CHECK(look(near, short_turn));   /* up by exactly 300 */
			CHECK(!look(near, across));      /* up by 299 */
		}
	}
	/* Without an orientation before, the glass up is enough (the sensor moving
	 * its own reference) */
	{
		static const int32_t up[3] = { 0, 0, 900 }, down[3] = { 0, 0, -900 };

		CHECK(look_raised(NULL, up, &limits));
		CHECK(!look_raised(NULL, down, &limits));
	}
	/* Other limits */
	{
		const struct look_limits strict = {
			.face_up_mg = 800, .toward_mg = 600, .tip_mg = 900, .twelve_up_mg = 500
		};

		CHECK(look_raised(keys_a, from_keys_a, &strict));   /* Y up by 1032 to 555 */
		CHECK(look_raised(keys_b, from_keys_b, &strict));   /* Y up by 978 to 562 */
		CHECK(!look_raised(from_keys_a, keys_a, &strict));
	}
}

int main(void)
{
	RUN(standing);
	RUN(at_the_keyboard);
	RUN(typing);
	RUN(what_still_lights);
	RUN(the_looks_of_27_09);
	RUN(the_limits);
	return harness_report("look");
}
