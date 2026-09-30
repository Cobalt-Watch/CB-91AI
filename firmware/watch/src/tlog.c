/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The journal of the temperature and the cell, see tlog.h. Pure C: the host
 * tests build this file as it is.
 */

#include <errno.h>
#include <string.h>

#include "tlog.h"

#define PAGE 256U

static uint32_t base(uint16_t k)
{
	return (uint32_t)(TLOG_FIRST + k) * STORE_SECTOR;
}

static uint32_t slot_addr(uint16_t k, uint32_t slot)
{
	return base(k) + TLOG_HEAD + slot * TLOG_RECORD;
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static bool blank(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (p[i] != 0xffU) {
			return false;
		}
	}
	return true;
}

/* CRC-8, polynomial 0x07, of the number and the identifier of a header: a
 * header cut short by a power cut is no header */
static uint8_t check(const uint8_t *p, size_t n)
{
	uint8_t crc = 0;

	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++) {
			const unsigned int shifted = (unsigned int)crc << 1;

			crc = (uint8_t)((crc & 0x80U) ? shifted ^ 0x07U : shifted);
		}
	}
	return crc;
}

static void decode(const uint8_t *p, struct tlog_record *r)
{
	r->time_s = get32(p);
	r->temp_cc = (int16_t)(uint16_t)(p[4] | (p[5] << 8));
	r->cell = (uint16_t)(p[6] | (p[7] << 8));
}

/* Written whole: a power cut leaves its last byte blank, TLOG_CELL_TORN set */
static bool whole(const struct tlog_record *r)
{
	return r->time_s != 0xffffffffU && (r->cell & TLOG_CELL_TORN) == 0U;
}

int tlog_mount(struct tlog *t, const struct store_io *io)
{
	uint32_t ids[TLOG_SECTORS] = { 0 };
	uint8_t head[TLOG_HEAD];
	uint16_t newest = 0;

	memset(t, 0, sizeof(*t));
	t->io = *io;
	for (uint16_t k = 0; k < TLOG_SECTORS; k++) {
		const int err = io->read(io->ctx, base(k), head, sizeof(head));

		if (err) {
			return err;
		}
		if (get32(head) != TLOG_MAGIC || head[4] != TLOG_VERSION ||
		    head[5] != check(&head[8], 8)) {
			continue;
		}
		t->used[k] = true;
		t->number[k] = get32(&head[8]);
		ids[k] = get32(&head[12]);
		if (!t->found || t->number[k] > t->number[newest]) {
			newest = k;
			t->found = true;
		}
	}
	if (!t->found) {
		return 0;
	}
	/* The newest sector names the journal: one of another name (a reset cut
	 * short) is not part of it */
	t->id = ids[newest];
	for (uint16_t k = 0; k < TLOG_SECTORS; k++) {
		t->used[k] = t->used[k] && ids[k] == t->id;
	}
	t->current = newest;
	/* Its next slot: after the last record that holds anything. Not the first
	 * blank one: a write refused before any bit programmed leaves a blank slot
	 * behind the records written after it. From the end, a page at a time. */
	t->slot = 0;
	for (uint32_t end = TLOG_PER_SECTOR; end > 0 && t->slot == 0;) {
		const uint32_t first = end > PAGE / TLOG_RECORD ? end - PAGE / TLOG_RECORD : 0U;
		uint8_t page[PAGE];
		const int err = io->read(io->ctx, slot_addr(newest, first), page,
					 (end - first) * TLOG_RECORD);

		if (err) {
			return err;
		}
		for (uint32_t k = end; k > first; k--) {
			if (!blank(&page[(k - 1U - first) * TLOG_RECORD], TLOG_RECORD)) {
				t->slot = (uint16_t)k;
				break;
			}
		}
		end = first;
	}
	return 0;
}

bool tlog_found(const struct tlog *t)
{
	return t->found;
}

/* A sector taken: erased, checked blank throughout (an erase cut short leaves
 * bits anywhere), then its header, read back */
static int claim(struct tlog *t, uint16_t k, uint32_t number)
{
	_Alignas(4) uint8_t head[TLOG_HEAD];
	uint8_t back[TLOG_HEAD];
	uint8_t page[PAGE];
	int err;

	t->used[k] = false; /* until its header is whole */
	err = t->io.erase(t->io.ctx, base(k), STORE_SECTOR);
	for (uint32_t off = 0; err == 0 && off < STORE_SECTOR; off += PAGE) {
		err = t->io.read(t->io.ctx, base(k) + off, page, sizeof(page));
		if (err == 0 && !blank(page, sizeof(page))) {
			err = -EIO;
		}
	}
	if (err) {
		return err;
	}
	put32(head, TLOG_MAGIC);
	head[4] = TLOG_VERSION;
	head[6] = 0xffU;
	head[7] = 0xffU;
	put32(&head[8], number);
	put32(&head[12], t->id);
	head[5] = check(&head[8], 8);
	err = t->io.write(t->io.ctx, base(k), head, sizeof(head));
	if (err == 0) {
		err = t->io.read(t->io.ctx, base(k), back, sizeof(back));
	}
	if (err == 0 && memcmp(head, back, sizeof(head)) != 0) {
		err = -EIO;
	}
	if (err) {
		return err;
	}
	t->used[k] = true;
	t->number[k] = number;
	t->current = k;
	t->slot = 0;
	return 0;
}

int tlog_start(struct tlog *t, uint32_t id)
{
	int err = 0;

	if (t->found) {
		return -EALREADY;
	}
	/* The whole tail erased at once: what the notes left there (the voice of
	 * notes delivered, not yet erased) goes now, not when the journal comes
	 * round to it weeks later */
	for (uint16_t k = 1; k < TLOG_SECTORS && err == 0; k++) {
		err = t->io.erase(t->io.ctx, base(k), STORE_SECTOR);
	}
	if (err) {
		return err;
	}
	t->id = id;
	err = claim(t, 0, 0);
	t->found = err == 0;
	return err;
}

int tlog_append(struct tlog *t, const struct tlog_record *r)
{
	_Alignas(4) uint8_t rec[TLOG_RECORD];
	int err;

	if (!t->found) {
		return -ENODEV;
	}
	if (t->slot >= TLOG_PER_SECTOR) {
		const uint16_t next = (uint16_t)((t->current + 1U) % TLOG_SECTORS);

		err = claim(t, next, t->number[t->current] + 1U);
		if (err) {
			return err; /* tried again at the next record */
		}
	}
	put32(rec, r->time_s);
	rec[4] = (uint8_t)(uint16_t)r->temp_cc;
	rec[5] = (uint8_t)((uint16_t)r->temp_cc >> 8);
	rec[6] = (uint8_t)r->cell;
	rec[7] = (uint8_t)(r->cell >> 8);
	err = t->io.write(t->io.ctx, slot_addr(t->current, t->slot), rec, sizeof(rec));
	t->slot++; /* written or torn, the slot is spent: a byte programs once */
	return err;
}

uint32_t tlog_next(const struct tlog *t)
{
	return t->found ? t->number[t->current] * TLOG_PER_SECTOR + t->slot : 0U;
}

static int sector_of(const struct tlog *t, uint32_t number)
{
	for (uint16_t k = 0; k < TLOG_SECTORS; k++) {
		if (t->used[k] && t->number[k] == number) {
			return k;
		}
	}
	return -1;
}

size_t tlog_read(struct tlog *t, uint32_t from, struct tlog_record *out, size_t max,
		 uint32_t *first)
{
	const uint32_t end = tlog_next(t);
	uint32_t oldest = t->number[t->current];
	size_t n = 0;

	*first = 0;
	if (!t->found) {
		return 0;
	}
	for (uint16_t k = 0; k < TLOG_SECTORS; k++) {
		if (t->used[k] && t->number[k] < oldest) {
			oldest = t->number[k];
		}
	}
	if (from < oldest * TLOG_PER_SECTOR) {
		from = oldest * TLOG_PER_SECTOR; /* the older ones are gone */
	}
	for (uint32_t index = from; index < end && n < max; index++) {
		const int k = sector_of(t, index / TLOG_PER_SECTOR);
		uint8_t rec[TLOG_RECORD];
		struct tlog_record r;

		if (k < 0) {
			/* A sector missing (its header lost): the run stops there, or
			 * starts after it */
			if (n > 0) {
				break;
			}
			index = (index / TLOG_PER_SECTOR + 1U) * TLOG_PER_SECTOR - 1U;
			continue;
		}
		if (t->io.read(t->io.ctx, slot_addr((uint16_t)k, index % TLOG_PER_SECTOR), rec,
			       sizeof(rec)) != 0) {
			break;
		}
		decode(rec, &r);
		if (!whole(&r)) {
			if (n > 0) {
				break; /* consecutive indexes only */
			}
			continue;
		}
		if (n == 0) {
			*first = index;
		}
		out[n++] = r;
	}
	return n;
}

int tlog_wipe(struct tlog *t, uint32_t id)
{
	int err = 0;

	if (!t->found) {
		return -ENODEV;
	}
	/* No record goes on until the new journal has its header */
	t->found = false;
	for (uint16_t k = 0; k < TLOG_SECTORS && err == 0; k++) {
		t->used[k] = false;
		err = t->io.erase(t->io.ctx, base(k), STORE_SECTOR);
	}
	if (err) {
		return err; /* the reset starts again at the next boot */
	}
	t->id = id;
	err = claim(t, 0, 0);
	t->found = err == 0;
	return err;
}

int64_t tlog_next_mark(int64_t now_ms, uint32_t period_s)
{
	const int64_t period_ms = (int64_t)period_s * 1000;
	int64_t into = now_ms % period_ms;

	if (into < 0) {
		into += period_ms; /* before 1970: never from the phone, but exact */
	}
	return now_ms - into + period_ms;
}

bool tlog_early(int64_t now_ms, int64_t mark_ms)
{
	return now_ms < mark_ms;
}
