/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The journal of the temperature and the cell (lot T1, a voice note of
 * 2026-09-25): a record of 8 bytes
 * every 10 minutes, in the tail of the storage flash that the notes left to it
 * (store.h), 510 a sector after a header of 16 bytes; the oldest sector is
 * erased when the journal goes round. A record's index is its place in the
 * journal's life: the number of its sector times 510, plus its slot.
 *
 * Pure logic over the flash functions of the store, so that the host tests run
 * it on a simulated NOR flash, power cuts included (tests/host/test_tlog.c).
 * Not thread-safe: the Zephyr glue (notes.c) holds the lock of the notes.
 */

#ifndef CB91AI_WATCH_TLOG_H
#define CB91AI_WATCH_TLOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "store.h"

#define TLOG_FIRST      STORE_TAIL_FIRST /* its first sector in the flash */
#define TLOG_SECTORS    STORE_TAIL
#define TLOG_HEAD       16U /* magic, version, check, 2 reserved, number, identifier */
#define TLOG_RECORD     8U
#define TLOG_PER_SECTOR ((STORE_SECTOR - TLOG_HEAD) / TLOG_RECORD) /* 510 */
#define TLOG_MAGIC      0x4C544243U /* "CBTL", little-endian */
#define TLOG_VERSION    1U

/* The cell of a record: millivolts, and what loaded the cell. Its last byte is
 * the last one written: a record cut short by a power cut keeps it blank, so
 * that TLOG_CELL_TORN, 0 in a whole record, says it. */
#define TLOG_CELL_MV   0x1FFFU
#define TLOG_CELL_TORN 0x2000U
#define TLOG_CELL_TAKE 0x4000U /* a take was being recorded */
#define TLOG_CELL_LINK 0x8000U /* a radio link was up */

struct tlog_record {
	uint32_t time_s; /* UTC, seconds since 1970 */
	int16_t temp_cc; /* hundredths of a degree C */
	uint16_t cell;   /* millivolts (TLOG_CELL_MV) and flags */
};

struct tlog {
	struct store_io io;
	bool found;                    /* a header of the journal in the tail */
	uint32_t id;                   /* the journal's identifier */
	bool used[TLOG_SECTORS];       /* holds a header of this journal */
	uint32_t number[TLOG_SECTORS]; /* the sector's number in the journal's life */
	uint16_t current;              /* the sector written */
	uint16_t slot;                 /* its next record; TLOG_PER_SECTOR: full */
};

/* Read the tail: the journal's headers (none: the tail is still the notes',
 * or the first header never got written), the sector written, its next slot */
int tlog_mount(struct tlog *t, const struct store_io *io);

/* A journal in the tail: store_mount() leaves the tail to it */
bool tlog_found(const struct tlog *t);

/* The journal starts, named `id`: the whole tail erased (what the notes left
 * there goes at once), the header of its first sector written and read back.
 * Only once store_tail_free() said so, under the same lock; store_give_tail()
 * then, or store_tail_dirty() if it failed. */
int tlog_start(struct tlog *t, uint32_t id);

/* A record at the end of the journal; its sector full, the next one is erased
 * and taken (the oldest, once the journal goes round). The caller never gives
 * a time of all ones nor TLOG_CELL_TORN. */
int tlog_append(struct tlog *t, const struct tlog_record *r);

/* The index the next record will take: the records written in the journal's
 * life, torn ones included; 0 without a journal */
uint32_t tlog_next(const struct tlog *t);

/* Up to `max` records of consecutive indexes, from the first whole one at or
 * after `from`, the index of the first in *first: how many (0: none left) */
size_t tlog_read(struct tlog *t, uint32_t from, struct tlog_record *out, size_t max,
		 uint32_t *first);

/* Everything erased, and a new journal started, named `id` (the reset, S3).
 * Only on a journal found or started: without one, the tail is the notes'. */
int tlog_wipe(struct tlog *t, uint32_t id);

/* The marks of the journal: a record at every multiple of the period in UTC.
 * The watch's timers count the crystal's time, which the clock corrects (by
 * 120 ppm on the second V1, whose crystal runs 140 ppm fast): a timer aimed at
 * a mark then fires up to 72 ms early in 10 minutes, and a record at 9:59, then
 * another at 10:00, came of it (27/09). So the timer is aimed TLOG_MARK_PAST_MS
 * past the mark, and a record is taken only once the clock reached the mark;
 * a timer that fired early is aimed again at the same mark. */
#define TLOG_MARK_PAST_MS 20U

/* The first mark after `now_ms`, in ms of UTC (a period in s) */
int64_t tlog_next_mark(int64_t now_ms, uint32_t period_s);

/* A timer aimed at `mark_ms` fired at `now_ms` of the clock: before the mark,
 * no record yet */
bool tlog_early(int64_t now_ms, int64_t mark_ms);

#endif /* CB91AI_WATCH_TLOG_H */
