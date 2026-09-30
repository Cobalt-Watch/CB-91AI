/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the journal of the temperature and the cell (firmware/watch/
 * src/tlog.c, lot T1), on the simulated NOR flash of test_store.c: programming
 * only clears bits, an erase sets a whole 4 KB sector, writes are whole aligned
 * words within a page. Power cuts stop the programming at any byte, and a
 * remount plays the reboot.
 */

#include <errno.h>
#include <string.h>

#include "harness.h"
#include "tlog.h"

static uint8_t flash[STORE_SIZE];
static long budget = -1; /* bytes that may still be programmed; -1: no cut */
static long erase_budget = -1; /* erases that may still happen */
static unsigned int erases;

#define FLASH_RULE(cond)                                                                           \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			CHECK(cond);                                                               \
			return -EINVAL;                                                            \
		}                                                                                  \
	} while (0)

static int f_read(void *ctx, uint32_t addr, void *buf, size_t len)
{
	(void)ctx;
	FLASH_RULE(addr < STORE_SIZE && len <= STORE_SIZE - addr);
	/* The journal reads its tail only */
	FLASH_RULE(addr >= (uint32_t)TLOG_FIRST * STORE_SECTOR);
	memcpy(buf, &flash[addr], len);
	return 0;
}

static int f_write(void *ctx, uint32_t addr, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	(void)ctx;
	FLASH_RULE(addr % 4 == 0 && len % 4 == 0 && len > 0 && addr < STORE_SIZE);
	FLASH_RULE(addr >= (uint32_t)TLOG_FIRST * STORE_SECTOR);
	FLASH_RULE(addr / STORE_PAGE == (addr + len - 1) / STORE_PAGE);
	FLASH_RULE((uintptr_t)buf % 4 == 0);
	for (size_t i = 0; i < len; i++) {
		if (budget == 0) {
			return -EIO; /* the power went */
		}
		FLASH_RULE(p[i] == 0xff || flash[addr + i] == 0xff);
		flash[addr + i] &= p[i];
		if (budget > 0) {
			budget--;
		}
	}
	return 0;
}

static int f_erase(void *ctx, uint32_t addr, size_t len)
{
	(void)ctx;
	FLASH_RULE(addr % STORE_SECTOR == 0 && len == STORE_SECTOR);
	FLASH_RULE(addr >= (uint32_t)TLOG_FIRST * STORE_SECTOR && addr < STORE_SIZE);
	if (erase_budget == 0) {
		/* The power went half way: bits left anywhere */
		for (size_t i = 0; i < STORE_SECTOR; i += 97) {
			flash[addr + i] = 0x00;
		}
		return -EIO;
	}
	if (erase_budget > 0) {
		erase_budget--;
	}
	memset(&flash[addr], 0xff, len);
	erases++;
	return 0;
}

static const struct store_io io = { NULL, f_read, f_write, f_erase };
static struct tlog tl;

static void blank_flash(void)
{
	memset(flash, 0xff, sizeof(flash));
	budget = -1;
	erase_budget = -1;
	erases = 0;
}

static struct tlog_record rec(uint32_t i)
{
	const struct tlog_record r = {
		.time_s = 1790000000U + 600U * i,
		.temp_cc = (int16_t)(2150 - (int32_t)(i % 90) * 50), /* 21.5 down to -23 degC */
		.cell = (uint16_t)((2900U - i % 400U) | (i % 7U == 0 ? TLOG_CELL_LINK : 0U)),
	};
	return r;
}

static bool same(const struct tlog_record *a, const struct tlog_record *b)
{
	return a->time_s == b->time_s && a->temp_cc == b->temp_cc && a->cell == b->cell;
}

/* Every record of the journal from `from`, page by page of `per`, checked
 * against rec(): how many, the first index in *first */
static uint32_t read_all(uint32_t from, size_t per, uint32_t *first)
{
	struct tlog_record page[29];
	uint32_t count = 0;
	uint32_t at = from;

	*first = UINT32_MAX;
	for (;;) {
		uint32_t f;
		const size_t n = tlog_read(&tl, at, page, per, &f);

		if (n == 0) {
			return count;
		}
		if (*first == UINT32_MAX) {
			*first = f;
		}
		CHECK(f >= at);
		for (size_t i = 0; i < n; i++) {
			const struct tlog_record want = rec(f + (uint32_t)i);

			if (!same(&page[i], &want)) {
				CHECK(false);
				return count;
			}
		}
		count += (uint32_t)n;
		at = f + (uint32_t)n;
	}
}

static void append(uint32_t from, uint32_t to)
{
	for (uint32_t i = from; i < to; i++) {
		const struct tlog_record r = rec(i);

		CHECK_EQ(tlog_append(&tl, &r), 0);
	}
}

static void a_tail_without_journal(void)
{
	struct tlog_record page[4];
	const struct tlog_record r = rec(0);
	uint32_t first;

	blank_flash();
	CHECK_EQ(tlog_mount(&tl, &io), 0);
	CHECK(!tlog_found(&tl));
	CHECK_EQ(tlog_next(&tl), 0);
	CHECK_EQ(tlog_read(&tl, 0, page, 4, &first), 0);
	CHECK_EQ(tlog_append(&tl, &r), -ENODEV);
	CHECK_EQ(tlog_wipe(&tl, 7), -ENODEV); /* the tail is the notes' */
	/* Notes left there: no journal either */
	memset(&flash[(size_t)TLOG_FIRST * STORE_SECTOR], 0x42, 3000);
	CHECK_EQ(tlog_mount(&tl, &io), 0);
	CHECK(!tlog_found(&tl));
	CHECK_EQ(erases, 0);
}

static void a_journal_starts(void)
{
	struct tlog_record page[29];
	uint32_t first;

	blank_flash();
	/* Delivered notes left in the tail: all of it erased as the journal starts */
	memset(&flash[(size_t)TLOG_FIRST * STORE_SECTOR], 0x42, (size_t)TLOG_SECTORS * STORE_SECTOR);
	tlog_mount(&tl, &io);
	CHECK_EQ(tlog_start(&tl, 0xC0B91AA1U), 0);
	CHECK(tlog_found(&tl));
	for (size_t i = (size_t)(TLOG_FIRST + 1U) * STORE_SECTOR; i < STORE_SIZE; i++) {
		if (flash[i] != 0xff) {
			CHECK(false);
			break;
		}
	}
	CHECK_EQ(tlog_start(&tl, 1), -EALREADY);
	CHECK_EQ(tlog_next(&tl), 0);
	append(0, 3);
	CHECK_EQ(tlog_next(&tl), 3);
	CHECK_EQ(tlog_read(&tl, 0, page, 29, &first), 3);
	CHECK_EQ(first, 0);
	CHECK(same(&page[2], &(struct tlog_record){ rec(2).time_s, rec(2).temp_cc, rec(2).cell }));
	CHECK_EQ(tlog_read(&tl, 3, page, 29, &first), 0);
	/* Rebooted: the same journal, the next record after them */
	CHECK_EQ(tlog_mount(&tl, &io), 0);
	CHECK(tlog_found(&tl));
	CHECK_EQ(tl.id, 0xC0B91AA1U);
	CHECK_EQ(tlog_next(&tl), 3);
	append(3, 5);
	CHECK_EQ(read_all(0, 29, &first), 5);
	CHECK_EQ(first, 0);
	/* The flags come back as they went (read_all checks the rest, negative
	 * temperatures included, in the tests below) */
	CHECK_EQ(tlog_read(&tl, 0, page, 1, &first), 1);
	CHECK_EQ(page[0].cell & TLOG_CELL_LINK, TLOG_CELL_LINK);
}

/* Pages of any size, from any index; the sectors taken one after the other */
static void pages_and_sectors(void)
{
	struct tlog_record page[29];
	uint32_t first;

	blank_flash();
	tlog_mount(&tl, &io);
	tlog_start(&tl, 11);
	append(0, 1200); /* two sectors full, the third begun */
	CHECK_EQ(tlog_next(&tl), 1200);
	CHECK_EQ(read_all(0, 29, &first), 1200);
	CHECK_EQ(first, 0);
	CHECK_EQ(read_all(0, 1, &first), 1200);
	CHECK_EQ(read_all(509, 21, &first), 691); /* across the end of a sector */
	CHECK_EQ(first, 509);
	CHECK_EQ(tlog_read(&tl, 1199, page, 29, &first), 1);
	CHECK_EQ(first, 1199);
	CHECK_EQ(tlog_read(&tl, 1200, page, 29, &first), 0);
	CHECK_EQ(tlog_read(&tl, 5000, page, 29, &first), 0);
	/* The slot of a full sector, and of one just taken, found again */
	CHECK_EQ(tlog_mount(&tl, &io), 0);
	CHECK_EQ(tlog_next(&tl), 1200);
	blank_flash();
	tlog_mount(&tl, &io);
	tlog_start(&tl, 12);
	append(0, TLOG_PER_SECTOR);
	CHECK_EQ(tlog_mount(&tl, &io), 0);
	CHECK_EQ(tlog_next(&tl), TLOG_PER_SECTOR);
	append(TLOG_PER_SECTOR, TLOG_PER_SECTOR + 1);
	CHECK_EQ(tlog_mount(&tl, &io), 0);
	CHECK_EQ(tlog_next(&tl), TLOG_PER_SECTOR + 1);
	CHECK_EQ(read_all(0, 29, &first), TLOG_PER_SECTOR + 1);
}

/* Round the tail: the oldest sector goes, the indexes go on */
static void the_journal_goes_round(void)
{
	const uint32_t total = TLOG_PER_SECTOR * 9U + 5U;
	uint32_t first;

	blank_flash();
	tlog_mount(&tl, &io);
	tlog_start(&tl, 21);
	append(0, total);
	CHECK_EQ(tlog_next(&tl), total);
	/* Sectors 2 to 9 of its life are left: the first two went */
	CHECK_EQ(read_all(0, 29, &first), total - 2U * TLOG_PER_SECTOR);
	CHECK_EQ(first, 2U * TLOG_PER_SECTOR);
	CHECK_EQ(tlog_mount(&tl, &io), 0);
	CHECK_EQ(tlog_next(&tl), total);
	CHECK_EQ(read_all(2U * TLOG_PER_SECTOR + 100U, 29, &first),
		 total - 2U * TLOG_PER_SECTOR - 100U);
	/* Some weeks on */
	append(total, total + 3U * TLOG_PER_SECTOR);
	CHECK_EQ(read_all(0, 29, &first), TLOG_SECTORS * TLOG_PER_SECTOR - (TLOG_PER_SECTOR - 5U));
	CHECK_EQ(first, 5U * TLOG_PER_SECTOR);
}

/* Power cuts in a record: that record is skipped, and splits the pages */
static void a_record_cut_short(void)
{
	struct tlog_record page[29];
	const struct tlog_record r = rec(10);
	uint32_t first;

	for (long cut = 0; cut < 8; cut++) {
		blank_flash();
		tlog_mount(&tl, &io);
		tlog_start(&tl, 31);
		append(0, 10);
		budget = cut;
		(void)tlog_append(&tl, &r);
		budget = -1;
		CHECK_EQ(tlog_mount(&tl, &io), 0);
		/* A byte begun spends the slot; none, and the slot is the next one's */
		CHECK_EQ(tlog_next(&tl), cut == 0 ? 10U : 11U);
		append(tlog_next(&tl), tlog_next(&tl) + 5U);
		CHECK_EQ(tlog_read(&tl, 0, page, 29, &first), cut == 0 ? 15 : 10);
		CHECK_EQ(first, 0);
		/* After the torn one, the rest */
		CHECK_EQ(tlog_read(&tl, 10, page, 29, &first), 5);
		CHECK_EQ(first, cut == 0 ? 10U : 11U);
	}
}

/* A write refused before any bit programmed leaves a blank slot behind the
 * records that follow it: after a reboot the journal goes on after the last
 * of them, never over them */
static void a_blank_slot_behind_records(void)
{
	const struct tlog_record r = rec(15);
	uint32_t first;

	blank_flash();
	tlog_mount(&tl, &io);
	tlog_start(&tl, 35);
	append(0, 15);
	budget = 0;
	CHECK(tlog_append(&tl, &r) != 0);
	budget = -1;
	append(16, 21);
	CHECK_EQ(tlog_mount(&tl, &io), 0);
	CHECK_EQ(tlog_next(&tl), 21);
	append(21, 25);
	CHECK_EQ(read_all(0, 29, &first), 24); /* all but the blank slot 15 */
	CHECK_EQ(read_all(16, 29, &first), 9);
	CHECK_EQ(first, 16);
	/* The last record of a full sector, and only it */
	blank_flash();
	tlog_mount(&tl, &io);
	tlog_start(&tl, 36);
	append(0, 1);
	budget = 0;
	for (uint32_t i = 1; i < TLOG_PER_SECTOR - 1U; i++) {
		const struct tlog_record q = rec(i);

		CHECK(tlog_append(&tl, &q) != 0);
	}
	budget = -1;
	append(TLOG_PER_SECTOR - 1U, TLOG_PER_SECTOR);
	CHECK_EQ(tlog_mount(&tl, &io), 0);
	CHECK_EQ(tlog_next(&tl), TLOG_PER_SECTOR);
}

/* Power cuts while a sector is taken: its erase or its header */
static void a_sector_taken_cut_short(void)
{
	struct tlog_record page[29];
	const struct tlog_record r = rec(TLOG_PER_SECTOR);
	uint32_t first;

	/* The erase cut short: nothing lost, the next record takes it again */
	blank_flash();
	tlog_mount(&tl, &io);
	tlog_start(&tl, 41);
	append(0, TLOG_PER_SECTOR);
	erase_budget = 0;
	CHECK(tlog_append(&tl, &r) != 0);
	erase_budget = -1;
	CHECK_EQ(tlog_mount(&tl, &io), 0);
	CHECK_EQ(tlog_next(&tl), TLOG_PER_SECTOR);
	append(TLOG_PER_SECTOR, TLOG_PER_SECTOR + 3U);
	CHECK_EQ(read_all(0, 29, &first), TLOG_PER_SECTOR + 3U);
	/* The header cut short at every byte: not taken, taken again */
	for (long cut = 0; cut < (long)TLOG_HEAD; cut++) {
		blank_flash();
		tlog_mount(&tl, &io);
		tlog_start(&tl, 42);
		append(0, TLOG_PER_SECTOR);
		budget = cut;
		CHECK(tlog_append(&tl, &r) != 0);
		budget = -1;
		CHECK_EQ(tlog_mount(&tl, &io), 0);
		CHECK_EQ(tl.id, 42);
		CHECK_EQ(tlog_next(&tl), TLOG_PER_SECTOR);
		append(TLOG_PER_SECTOR, TLOG_PER_SECTOR + 2U);
		CHECK_EQ(tlog_read(&tl, TLOG_PER_SECTOR - 1U, page, 29, &first), 3);
	}
	/* The first header cut short: no journal yet, the tail left as it was */
	for (long cut = 0; cut < (long)TLOG_HEAD; cut++) {
		blank_flash();
		tlog_mount(&tl, &io);
		budget = cut;
		CHECK(tlog_start(&tl, 43) != 0);
		budget = -1;
		CHECK_EQ(tlog_mount(&tl, &io), 0);
		CHECK(!tlog_found(&tl));
	}
}

/* The reset: all erased, a new name, the indexes from 0 */
static void a_reset_wipes_the_journal(void)
{
	uint32_t first;

	blank_flash();
	tlog_mount(&tl, &io);
	tlog_start(&tl, 51);
	append(0, 2000);
	CHECK_EQ(tlog_wipe(&tl, 52), 0);
	CHECK(tlog_found(&tl));
	CHECK_EQ(tl.id, 52);
	CHECK_EQ(tlog_next(&tl), 0);
	CHECK_EQ(read_all(0, 29, &first), 0);
	CHECK_EQ(tlog_mount(&tl, &io), 0);
	CHECK_EQ(tl.id, 52);
	CHECK_EQ(tlog_next(&tl), 0);
	for (uint16_t k = 1; k < TLOG_SECTORS; k++) {
		CHECK(!tl.used[k]);
	}
	append(0, 4);
	CHECK_EQ(read_all(0, 29, &first), 4);
	/* Cut short after some sectors: the old name's sectors left are not the
	 * journal's (the reset starts again at the next boot) */
	append(4, 3000);
	erase_budget = 3;
	CHECK(tlog_wipe(&tl, 53) != 0);
	erase_budget = -1;
	CHECK(!tlog_found(&tl));
	CHECK_EQ(tlog_mount(&tl, &io), 0);
	CHECK_EQ(tl.id, 52); /* what is left of it */
	CHECK_EQ(tlog_wipe(&tl, 53), 0);
	CHECK_EQ(tl.id, 53);
	CHECK_EQ(tlog_next(&tl), 0);
}

/* The marks of the period: on 27/09 every record came twice, at 9:59 then at
 * 10:00, as the watch's timer counts the crystal's time and its clock corrects
 * it by 120 ppm. The timer is played here as main.c arms it, the clock running
 * (1 - correction) as fast as the timer's time, from a time given at an odd
 * moment: one record a period, on its mark, for any correction of the clock */
static void the_marks_of_the_period(void)
{
	static const double corrections_ppm[] = {250.0, 120.274, 7.773, 0.0, -120.0, -250.0};
	static const int64_t given_at[] = {500, 300000, 599990, 599999}; /* into a period */
	const int64_t period_ms = 600000;
	const int64_t day0 = 1790505600000LL; /* 27/09, 10:40 UTC, a mark */

	CHECK_EQ(tlog_next_mark(0, 600), period_ms);
	CHECK_EQ(tlog_next_mark(599999, 600), period_ms);
	CHECK_EQ(tlog_next_mark(period_ms, 600), 2 * period_ms);
	CHECK_EQ(tlog_next_mark(-1, 600), 0);
	CHECK_EQ(tlog_next_mark(day0 + 1, 1), day0 + 1000);
	CHECK(tlog_early(day0 - 1, day0));
	CHECK(!tlog_early(day0, day0));
	for (size_t i = 0; i < sizeof(corrections_ppm) / sizeof(corrections_ppm[0]); i++) {
		const double rate = 1.0 - corrections_ppm[i] * 1e-6;

		for (size_t j = 0; j < sizeof(given_at) / sizeof(given_at[0]); j++) {
			double clock_ms = (double)(day0 + given_at[j]);
			int64_t mark = tlog_next_mark((int64_t)clock_ms, 600);
			int64_t last_s = 0;
			int records = 0;
			int fires = 0;

			while (records < 1000 && fires < 3000) {
				const uint32_t delay = (uint32_t)(mark - (int64_t)clock_ms) + TLOG_MARK_PAST_MS;
				int64_t now;
				bool early;

				clock_ms += delay * rate; /* the timer's ms, seen on the clock */
				fires++;
				now = (int64_t)clock_ms;
				early = tlog_early(now, mark);
				mark = tlog_next_mark(now, 600); /* journal_arm() */
				if (early) {
					continue;
				}
				CHECK_EQ((now / 1000) % 600, 0);
				if (last_s != 0) {
					CHECK_EQ(now / 1000 - last_s, 600);
				}
				last_s = now / 1000;
				records++;
			}
			CHECK_EQ(records, 1000);
			CHECK(fires <= 2 * records + 1); /* early at most once a mark */
		}
	}
}

int main(void)
{
	RUN(the_marks_of_the_period);
	RUN(a_tail_without_journal);
	RUN(a_journal_starts);
	RUN(pages_and_sectors);
	RUN(the_journal_goes_round);
	RUN(a_record_cut_short);
	RUN(a_blank_slot_behind_records);
	RUN(a_sector_taken_cut_short);
	RUN(a_reset_wipes_the_journal);
	return harness_report("tlog");
}
