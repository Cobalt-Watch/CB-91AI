/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The voice notes in the storage flash, see store.h. Pure C: the host tests
 * build this file as it is.
 */

#include <errno.h>
#include <string.h>

#include "store.h"

#define LESSER(a, b) ((a) < (b) ? (a) : (b))

/* The bytes of the ring: the whole flash, or all but the journal's tail */
static uint32_t ring_size(const struct store *s)
{
	return (uint32_t)s->ring * STORE_SECTOR;
}

static uint32_t addr_of(const struct store *s, uint16_t first, uint32_t offset)
{
	return ((uint32_t)first * STORE_SECTOR + offset) % ring_size(s);
}

static uint16_t sectors_for(uint32_t bytes)
{
	return (uint16_t)((bytes + STORE_SECTOR - 1U) / STORE_SECTOR);
}

static uint16_t sector_after(const struct store *s, uint16_t sector, uint32_t k)
{
	return (uint16_t)((sector + k) % s->ring);
}

static bool all_blank(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (p[i] != 0xffU) {
			return false;
		}
	}
	return true;
}

/* Bytes of a note, across the end of the ring when it wraps */
static int read_at(struct store *s, uint16_t first, uint32_t offset, uint8_t *buf, size_t len)
{
	const uint32_t addr = addr_of(s, first, offset);
	const size_t head = LESSER(len, (size_t)(ring_size(s) - addr));
	int err = s->io.read(s->io.ctx, addr, buf, head);

	if (err == 0 && head < len) {
		err = s->io.read(s->io.ctx, 0, buf + head, len - head);
	}
	return err;
}

/* Sorted by id; false for an id seen already */
static bool insert(struct store *s, bool *delivered, const struct store_note *n, bool was_delivered)
{
	uint16_t at = s->count;

	if (s->count >= STORE_MAX_NOTES) {
		return false;
	}
	while (at > 0 && s->notes[at - 1U].id > n->id) {
		at--;
	}
	if (at > 0 && s->notes[at - 1U].id == n->id) {
		return false;
	}
	for (uint16_t i = s->count; i > at; i--) {
		s->notes[i] = s->notes[i - 1U];
		delivered[i] = delivered[i - 1U];
	}
	s->notes[at] = *n;
	delivered[at] = was_delivered;
	s->count++;
	return true;
}

int store_mount(struct store *s, const struct store_io *io, bool tail_taken)
{
	static bool delivered[STORE_MAX_NOTES];
	static bool claimed[STORE_SECTORS];
	uint8_t head[NOTE_HEADER_SIZE];
	struct note_header h;
	uint16_t kept = 0;

	memset(s, 0, sizeof(*s));
	s->io = *io;
	s->ring = tail_taken ? STORE_TAIL_FIRST : STORE_SECTORS;
	memset(claimed, 0, sizeof(claimed));
	/* The first 64 bytes of every sector of the ring, 16 KB in all: a header,
	 * blank, or anything else; the journal's tail is left to it */
	for (uint16_t sec = 0; sec < STORE_SECTORS; sec++) {
		int err;

		if (sec >= s->ring) {
			s->sector[sec] = STORE_FOREIGN;
			continue;
		}
		err = io->read(io->ctx, (uint32_t)sec * STORE_SECTOR, head, sizeof(head));
		if (err) {
			return err;
		}
		if (note_header_get(head, &h) && h.data_size <= ring_size(s) - NOTE_HEADER_SIZE) {
			const struct store_note n = {
				.id = h.id,
				.first = sec,
				.sectors = sectors_for(NOTE_HEADER_SIZE + h.data_size),
				.size = NOTE_HEADER_SIZE + h.data_size,
				.crc = note_offer_crc(head, h.data_crc, h.data_size),
			};

			(void)insert(s, delivered, &n, h.state != NOTE_STATE_COMPLETE);
			s->sector[sec] = STORE_DIRTY; /* until its note claims it */
		} else {
			s->sector[sec] = all_blank(head, sizeof(head)) ? STORE_UNKNOWN : STORE_DIRTY;
		}
	}
	/* The newest note, delivered or not, gives the last id and the head; once
	 * delivered its first sector stays, for that id */
	if (s->count > 0) {
		const struct store_note *newest = &s->notes[s->count - 1U];

		s->last_id = newest->id;
		s->head = sector_after(s, newest->first, newest->sectors);
		if (delivered[s->count - 1U]) {
			s->sector[newest->first] = STORE_KEEP;
			claimed[newest->first] = true;
		}
	}
	/* The notes waiting claim their sectors, newest first: a sector is
	 * written again only once the note that held it was let go, so of two
	 * notes over the same sector the older one is stale (its delivered mark
	 * lost) and is left to erase */
	for (uint16_t i = s->count; i-- > 0;) {
		const struct store_note *n = &s->notes[i];
		bool clash = false;

		if (delivered[i]) {
			continue;
		}
		for (uint16_t k = 0; k < n->sectors; k++) {
			clash |= claimed[sector_after(s, n->first, k)];
		}
		if (clash) {
			delivered[i] = true; /* dropped all the same */
			continue;
		}
		for (uint16_t k = 0; k < n->sectors; k++) {
			claimed[sector_after(s, n->first, k)] = true;
			s->sector[sector_after(s, n->first, k)] = STORE_NOTE;
		}
	}
	for (uint16_t i = 0; i < s->count; i++) {
		if (!delivered[i]) {
			s->notes[kept++] = s->notes[i];
		}
	}
	s->count = kept;
	return 0;
}

static bool holds_note(uint8_t state)
{
	return state == STORE_NOTE || state == STORE_KEEP;
}

/* The sector, or its whole block when no note lies there: 20 ms either way on
 * the ZD25WQ80C of the V1 (measured) */
static int erase_around(struct store *s, uint16_t sec)
{
	const uint16_t block = (uint16_t)(sec - sec % STORE_BLOCK);
	/* A whole block only within the ring: the last one of the notes runs
	 * into the journal's tail */
	bool whole = block + STORE_BLOCK <= s->ring;
	uint16_t first;
	uint16_t count;
	int err;

	for (uint16_t k = 0; k < STORE_BLOCK; k++) {
		whole = whole && !holds_note(s->sector[block + k]);
	}
	first = whole ? block : sec;
	count = whole ? STORE_BLOCK : 1U;
	err = s->io.erase(s->io.ctx, (uint32_t)first * STORE_SECTOR, (size_t)count * STORE_SECTOR);
	if (err) {
		return err;
	}
	memset(&s->sector[first], STORE_BLANK, count);
	return 0;
}

/* One step on a sector: unknown, read through (blank after all, or dirty);
 * dirty, erased. 1: a step was taken; 0: nothing to do there. */
static int clean(struct store *s, uint16_t sec)
{
	uint8_t page[STORE_PAGE];

	if (s->sector[sec] == STORE_UNKNOWN) {
		/* Its first bytes are blank: the rest too? A take cut before its
		 * header, or an erase cut short, left bits further on */
		for (uint32_t off = 0; off < STORE_SECTOR; off += STORE_PAGE) {
			const int err = s->io.read(s->io.ctx, (uint32_t)sec * STORE_SECTOR + off, page,
						  sizeof(page));

			if (err) {
				return err;
			}
			if (!all_blank(page, sizeof(page))) {
				s->sector[sec] = STORE_DIRTY;
				break;
			}
		}
		if (s->sector[sec] == STORE_UNKNOWN) {
			s->sector[sec] = STORE_BLANK;
			return 1;
		}
	}
	if (s->sector[sec] == STORE_DIRTY) {
		const int err = erase_around(s, sec);

		return err ? err : 1;
	}
	return 0;
}

int store_prepare(struct store *s, uint16_t want)
{
	if (s->writing) {
		return 0; /* never an erase during a take */
	}
	for (uint32_t k = 0; k < want && k < s->ring; k++) {
		const uint16_t sec = sector_after(s, s->head, k);
		int ret;

		if (holds_note(s->sector[sec])) {
			return 0; /* a note waits there: the ring is full up to it */
		}
		ret = clean(s, sec);
		if (ret != 0) {
			return ret;
		}
	}
	return 0;
}

int store_tidy(struct store *s)
{
	if (s->writing) {
		return 0;
	}
	for (uint16_t sec = 0; sec < s->ring; sec++) {
		const int ret = clean(s, sec);

		if (ret != 0) {
			return ret;
		}
	}
	return 0;
}

uint16_t store_blank_ahead(const struct store *s)
{
	uint16_t n = 0;

	while (n < s->ring && s->sector[sector_after(s, s->head, n)] == STORE_BLANK) {
		n++;
	}
	return n;
}

uint16_t store_room(const struct store *s)
{
	uint16_t n = 0;

	while (n < s->ring && !holds_note(s->sector[sector_after(s, s->head, n)])) {
		n++;
	}
	return n;
}

int store_begin(struct store *s)
{
	const uint16_t blank = store_blank_ahead(s);

	if (s->writing) {
		return -EBUSY;
	}
	if (s->count >= STORE_MAX_NOTES || blank == 0) {
		return -ENOSPC;
	}
	s->writing = true;
	s->failed = false;
	s->w_first = s->head;
	s->w_room = (uint32_t)blank * STORE_SECTOR - NOTE_HEADER_SIZE;
	s->w_len = 0;
	s->w_page_at = NOTE_HEADER_SIZE; /* the header comes last, before the frames */
	s->fill = 0;
	memset(s->page, 0xff, sizeof(s->page));
	return 0;
}

/* The page in RAM to the flash: up to its end, or the last one, padded with
 * 0xff to a whole word (0xff programs nothing) */
static int flush(struct store *s, bool last)
{
	const uint16_t len = last ? (uint16_t)((s->fill + 3U) & ~3U) : s->fill;
	int err = 0;

	if (s->fill == 0) {
		return 0;
	}
	err = s->io.write(s->io.ctx, addr_of(s, s->w_first, s->w_page_at), s->page, len);
	s->w_page_at += s->fill;
	s->fill = 0;
	memset(s->page, 0xff, sizeof(s->page));
	if (err) {
		s->failed = true;
	}
	return err;
}

size_t store_append(struct store *s, const uint8_t *data, size_t len)
{
	size_t done = 0;

	if (!s->writing || s->failed) {
		return 0;
	}
	len = LESSER(len, (size_t)(s->w_room - s->w_len));
	while (done < len) {
		const size_t space = STORE_PAGE - (s->w_page_at + s->fill) % STORE_PAGE;
		const size_t n = LESSER(space, len - done);

		memcpy(&s->page[s->fill], data + done, n);
		s->fill = (uint16_t)(s->fill + n);
		s->w_len += (uint32_t)n;
		done += n;
		if ((s->w_page_at + s->fill) % STORE_PAGE == 0 && flush(s, false) != 0) {
			break;
		}
	}
	return done;
}

void store_abandon(struct store *s)
{
	if (!s->writing) {
		return;
	}
	/* Whatever reached the flash will be erased; nothing did while the first
	 * page is still in RAM */
	if (s->w_page_at > NOTE_HEADER_SIZE || s->failed) {
		const uint16_t dirty = sectors_for(s->w_page_at + s->fill);

		for (uint16_t k = 0; k < dirty; k++) {
			s->sector[sector_after(s, s->w_first, k)] = STORE_DIRTY;
		}
	}
	s->writing = false;
}

int store_finish(struct store *s, struct note_header *h, uint32_t keep)
{
	uint8_t buf[STORE_PAGE];
	_Alignas(4) uint8_t head[NOTE_HEADER_SIZE];
	uint8_t back[NOTE_HEADER_SIZE];
	uint32_t crc = 0;
	uint16_t sectors;
	uint16_t written;
	int err;

	if (!s->writing) {
		return -EINVAL;
	}
	err = flush(s, true);
	if (err || s->failed) {
		store_abandon(s);
		return err ? err : -EIO;
	}
	keep = LESSER(keep, s->w_len);
	if (keep == 0) {
		store_abandon(s);
		return -ENODATA;
	}
	/* The CRC of the frames as the flash holds them: a bad write shows here,
	 * not at the phone */
	for (uint32_t off = 0; off < keep;) {
		const size_t n = LESSER(sizeof(buf), (size_t)(keep - off));

		err = read_at(s, s->w_first, NOTE_HEADER_SIZE + off, buf, n);
		if (err) {
			store_abandon(s);
			return err;
		}
		crc = note_crc32(crc, buf, n);
		off += (uint32_t)n;
	}
	h->id = s->last_id + 1U;
	h->data_size = keep;
	h->data_crc = crc;
	h->state = NOTE_STATE_COMPLETE;
	note_header_put(h, head);
	/* Last of all: the header, which makes the note */
	err = s->io.write(s->io.ctx, addr_of(s, s->w_first, 0), head, sizeof(head));
	if (err == 0) {
		err = s->io.read(s->io.ctx, addr_of(s, s->w_first, 0), back, sizeof(back));
	}
	if (err == 0 && memcmp(head, back, sizeof(head)) != 0) {
		err = -EIO;
	}
	if (err) {
		/* Its id may be whole in the flash all the same, but the head has
		 * not moved: its sector is erased before the next take */
		s->failed = true;
		store_abandon(s);
		return err;
	}
	/* Its sectors; those written past what is kept are erased later; the kept
	 * header of the last delivered note is not needed any more */
	sectors = sectors_for(NOTE_HEADER_SIZE + keep);
	written = sectors_for(s->w_page_at);
	for (uint16_t sec = 0; sec < s->ring; sec++) {
		if (s->sector[sec] == STORE_KEEP) {
			s->sector[sec] = STORE_DIRTY;
		}
	}
	/* written >= sectors: what is kept was written */
	for (uint16_t k = 0; k < written; k++) {
		s->sector[sector_after(s, s->w_first, k)] = k < sectors ? STORE_NOTE : STORE_DIRTY;
	}
	s->notes[s->count++] = (struct store_note){
		.id = h->id,
		.first = s->w_first,
		.sectors = sectors,
		.size = NOTE_HEADER_SIZE + keep,
		.crc = note_offer_crc(back, crc, keep),
	};
	s->last_id = h->id;
	s->head = sector_after(s, s->w_first, sectors);
	s->writing = false;
	return 0;
}

bool store_next(const struct store *s, uint32_t after, struct store_note *out)
{
	for (uint16_t i = 0; i < s->count; i++) {
		if (s->notes[i].id > after) {
			*out = s->notes[i];
			return true;
		}
	}
	return false;
}

static int find(const struct store *s, uint32_t id)
{
	for (uint16_t i = 0; i < s->count; i++) {
		if (s->notes[i].id == id) {
			return i;
		}
	}
	return -1;
}

size_t store_read(struct store *s, uint32_t id, uint32_t offset, uint8_t *buf, size_t len)
{
	const int i = find(s, id);

	if (i < 0 || offset >= s->notes[i].size) {
		return 0;
	}
	len = LESSER(len, (size_t)(s->notes[i].size - offset));
	return read_at(s, s->notes[i].first, offset, buf, len) == 0 ? len : 0U;
}

int store_delivered(struct store *s, uint32_t id)
{
	/* Only the state byte programs; 0xff leaves the others as they are. In
	 * RAM, as the DMA of the flash wants it */
	_Alignas(4) uint8_t mark[4] = { 0xffU, 0xffU, 0xffU, NOTE_STATE_DELIVERED };
	const int i = find(s, id);
	struct store_note n;
	int err;

	if (i < 0) {
		return -ENOENT;
	}
	n = s->notes[i];
	err = s->io.write(s->io.ctx, addr_of(s, n.first, 60), mark, sizeof(mark));
	if (err) {
		return err;
	}
	for (uint16_t k = 0; k < n.sectors; k++) {
		s->sector[sector_after(s, n.first, k)] = STORE_DIRTY;
	}
	if (n.id == s->last_id) {
		s->sector[n.first] = STORE_KEEP;
	}
	for (uint16_t j = (uint16_t)i; j + 1U < s->count; j++) {
		s->notes[j] = s->notes[j + 1U];
	}
	s->count--;
	return 0;
}

uint16_t store_count(const struct store *s)
{
	return s->count;
}

uint32_t store_bytes(const struct store *s)
{
	uint32_t total = 0;

	for (uint16_t i = 0; i < s->count; i++) {
		total += s->notes[i].size;
	}
	return total;
}

static int erase_one(struct store *s, uint16_t first, uint16_t count)
{
	for (uint16_t k = 0; k < count; k++) {
		if (s->sector[first + k] != STORE_BLANK) {
			return s->io.erase(s->io.ctx, (uint32_t)first * STORE_SECTOR,
					   (size_t)count * STORE_SECTOR);
		}
	}
	return 0; /* blank already */
}

int store_wipe(struct store *s)
{
	const uint32_t last = s->last_id;
	/* The head never holds the header of the newest note, which ends before it */
	const uint16_t keep = s->head;
	int err;

	if (s->writing) {
		return -EBUSY;
	}
	if (last != 0) {
		/* First the last id, in a header of no frames, delivered: until it
		 * is whole, the headers still in the flash hold that id */
		const struct note_header h = {
			.id = last,
			.codec = NOTE_CODEC_LC3,
			.channels = 1,
			.state = NOTE_STATE_DELIVERED,
		};
		_Alignas(4) uint8_t head[NOTE_HEADER_SIZE];
		uint8_t back[NOTE_HEADER_SIZE];

		err = erase_one(s, keep, 1);
		if (err) {
			return err;
		}
		s->sector[keep] = STORE_BLANK;
		note_header_put(&h, head);
		err = s->io.write(s->io.ctx, (uint32_t)keep * STORE_SECTOR, head, sizeof(head));
		/* Read back, as a note's header is: nothing else is erased on a
		 * header the flash did not take */
		if (err == 0) {
			err = s->io.read(s->io.ctx, (uint32_t)keep * STORE_SECTOR, back, sizeof(back));
		}
		if (err == 0 && memcmp(head, back, sizeof(head)) != 0) {
			err = -EIO;
		}
		if (err) {
			s->sector[keep] = STORE_DIRTY;
			return err;
		}
		s->sector[keep] = STORE_KEEP;
	}
	/* Then everything else of the ring: whole blocks, sector by sector around
	 * that header, and in the last block of the notes when it runs into the
	 * journal's tail (the journal wipes itself, tlog.h) */
	for (uint16_t block = 0; block < s->ring; block += STORE_BLOCK) {
		const uint16_t span = (uint16_t)LESSER((uint32_t)STORE_BLOCK, (uint32_t)(s->ring - block));
		const bool holds_keep = last != 0 && keep >= block && keep < block + span;

		for (uint16_t k = 0; k < span && (holds_keep || span < STORE_BLOCK); k++) {
			if (!holds_keep || block + k != keep) {
				err = erase_one(s, (uint16_t)(block + k), 1);
				if (err) {
					return err;
				}
				s->sector[block + k] = STORE_BLANK;
			}
		}
		if (!holds_keep && span == STORE_BLOCK) {
			err = erase_one(s, block, STORE_BLOCK);
			if (err) {
				return err;
			}
			memset(&s->sector[block], STORE_BLANK, STORE_BLOCK);
		}
	}
	/* As a mount would find it now */
	s->count = 0;
	s->last_id = last;
	s->head = last != 0 ? sector_after(s, keep, 1) : 0U;
	return 0;
}

bool store_tail_free(const struct store *s)
{
	if (s->ring != STORE_SECTORS || s->writing) {
		return false;
	}
	for (uint16_t sec = STORE_TAIL_FIRST; sec < STORE_SECTORS; sec++) {
		if (holds_note(s->sector[sec])) {
			return false; /* a note waits there, or the header of the last id */
		}
	}
	return true;
}

void store_tail_dirty(struct store *s)
{
	for (uint16_t sec = STORE_TAIL_FIRST; sec < s->ring; sec++) {
		if (!holds_note(s->sector[sec])) {
			s->sector[sec] = STORE_DIRTY;
		}
	}
}

void store_give_tail(struct store *s)
{
	/* No note lies in the tail (store_tail_free()), so none changes place:
	 * one that wrapped round the end of the flash would lie there */
	s->ring = STORE_TAIL_FIRST;
	memset(&s->sector[STORE_TAIL_FIRST], STORE_FOREIGN, STORE_TAIL);
	if (s->head >= s->ring) {
		s->head = 0;
	}
}
