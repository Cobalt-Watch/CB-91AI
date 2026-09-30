/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the voice notes in the storage flash (firmware/watch/src/
 * store.c, lot E1), on a simulated NOR flash of 1 MB: programming only clears
 * bits, an erase sets a whole 4 KB sector, writes are whole aligned words
 * within a page, as the QSPI driver of the watch wants them. Power cuts stop
 * the programming at any byte, and a remount plays the reboot.
 */

#include <errno.h>
#include <string.h>

#include "harness.h"
#include "note.h"
#include "store.h"

static uint8_t flash[STORE_SIZE];
static long budget = -1; /* bytes that may still be programmed; -1: no cut */
static unsigned int erases;

/* A rule of the flash broken: a failure, and the access refused */
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
	memcpy(buf, &flash[addr], len);
	return 0;
}

static int f_write(void *ctx, uint32_t addr, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	(void)ctx;
	FLASH_RULE(addr % 4 == 0 && len % 4 == 0 && len > 0 && addr < STORE_SIZE);
	FLASH_RULE(addr / STORE_PAGE == (addr + len - 1) / STORE_PAGE);
	/* The QSPI peripheral reads the source by DMA, a word at a time */
	FLASH_RULE((uintptr_t)buf % 4 == 0);
	for (size_t i = 0; i < len; i++) {
		if (budget == 0) {
			return -EIO; /* the power went */
		}
		/* A byte is programmed once between two erases (0xff programs nothing) */
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
	FLASH_RULE(addr % STORE_SECTOR == 0 && len % STORE_SECTOR == 0 && len > 0);
	FLASH_RULE(addr < STORE_SIZE && len <= STORE_SIZE - addr);
	memset(&flash[addr], 0xff, len);
	erases++;
	return 0;
}

static const struct store_io io = { NULL, f_read, f_write, f_erase };
static struct store st;

static uint8_t byte_of(uint32_t seed, uint32_t i)
{
	return (uint8_t)((seed * 2654435761U + i * 40503U) >> 13);
}

static void blank_flash(void)
{
	memset(flash, 0xff, sizeof(flash));
	budget = -1;
	erases = 0;
}

/* At rest, as the audio thread does: the room of the next take, then the rest */
static void ready(struct store *s)
{
	int ret;

	while ((ret = store_prepare(s, 61)) > 0) {
	}
	CHECK_EQ(ret, 0);
	while ((ret = store_tidy(s)) > 0) {
	}
	CHECK_EQ(ret, 0);
}

/* A take of `n` bytes of frames, fed as the recorder does (ten frames of 20
 * bytes at a time), kept up to `keep`: its id, 0 if it failed */
static uint32_t take(struct store *s, uint32_t seed, uint32_t n, uint32_t keep)
{
	struct note_header h = { .codec = NOTE_CODEC_LC3, .channels = 1, .rate_hz = 16000,
				 .frame_us = 10000, .frame_bytes = 20, .bitrate = 16000 };
	uint8_t chunk[200];

	ready(s);
	if (store_begin(s) != 0) {
		return 0;
	}
	for (uint32_t at = 0; at < n; at += sizeof(chunk)) {
		const uint32_t len = n - at < sizeof(chunk) ? n - at : sizeof(chunk);

		for (uint32_t i = 0; i < len; i++) {
			chunk[i] = byte_of(seed, at + i);
		}
		if (store_append(s, chunk, len) < len) {
			break;
		}
	}
	h.duration_ms = keep / 2;
	return store_finish(s, &h, keep) == 0 ? h.id : 0;
}

/* The note reads back whole: its header, its frames, the CRC of the offer */
static void check_note(struct store *s, uint32_t id, uint32_t seed, uint32_t size)
{
	static uint8_t all[NOTE_HEADER_SIZE + 300000];
	struct store_note n = { 0 };
	struct note_header h;
	uint32_t got = 0;
	bool found = store_next(s, id - 1U, &n) && n.id == id;

	CHECK(found);
	if (!found) {
		return;
	}
	CHECK_EQ(n.size, NOTE_HEADER_SIZE + size);
	while (got < n.size) {
		const size_t r = store_read(s, id, got, &all[got], 243);

		CHECK(r > 0);
		if (r == 0) {
			return;
		}
		got += (uint32_t)r;
	}
	CHECK_EQ(store_read(s, id, n.size, all, 243), 0);
	CHECK(note_header_get(all, &h));
	CHECK_EQ(h.id, id);
	CHECK_EQ(h.data_size, size);
	CHECK_EQ(h.state, NOTE_STATE_COMPLETE);
	CHECK_EQ(note_crc32(0, all, n.size), n.crc);
	for (uint32_t i = 0; i < size; i++) {
		if (all[NOTE_HEADER_SIZE + i] != byte_of(seed, i)) {
			CHECK(false);
			return;
		}
	}
}

static void a_blank_flash(void)
{
	blank_flash();
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(store_count(&st), 0);
	CHECK_EQ(st.head, 0);
	/* Unknown until checked: nothing ready yet */
	CHECK_EQ(store_blank_ahead(&st), 0);
	CHECK_EQ(store_begin(&st), -ENOSPC);
	CHECK_EQ(store_prepare(&st, 3), 1);
	CHECK_EQ(store_prepare(&st, 3), 1);
	CHECK_EQ(store_prepare(&st, 3), 1);
	CHECK_EQ(store_prepare(&st, 3), 0);
	CHECK_EQ(store_blank_ahead(&st), 3);
	CHECK_EQ(erases, 0); /* blank indeed: checked, not erased */
	CHECK_EQ(store_room(&st), STORE_SECTORS);
	/* The rest, at rest: read through, blank, nothing erased */
	ready(&st);
	CHECK_EQ(store_blank_ahead(&st), STORE_SECTORS);
	CHECK_EQ(erases, 0);
}

static void a_note_after_a_reboot(void)
{
	blank_flash();
	store_mount(&st, &io, false);
	CHECK_EQ(take(&st, 5, 10000, 10000), 1);
	check_note(&st, 1, 5, 10000);
	CHECK_EQ(store_bytes(&st), NOTE_HEADER_SIZE + 10000);
	/* Rebooted: the same note, the head after it */
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(store_count(&st), 1);
	check_note(&st, 1, 5, 10000);
	CHECK_EQ(st.head, 3);
	CHECK_EQ(st.last_id, 1);
	CHECK_EQ(store_room(&st), STORE_SECTORS - 3);
}

static void the_silence_is_cut(void)
{
	blank_flash();
	store_mount(&st, &io, false);
	/* 20000 bytes written, 5000 kept: two sectors of note, three to erase */
	CHECK_EQ(take(&st, 9, 20000, 5000), 1);
	check_note(&st, 1, 9, 5000);
	CHECK_EQ(st.head, 2);
	CHECK_EQ(st.sector[2], STORE_DIRTY);
	CHECK_EQ(st.sector[4], STORE_DIRTY);
	erases = 0;
	ready(&st);
	CHECK_EQ(erases, 3);
	CHECK_EQ(take(&st, 10, 3000, 3000), 2);
	check_note(&st, 2, 10, 3000);
	CHECK_EQ(store_mount(&st, &io, false), 0);
	check_note(&st, 1, 9, 5000);
	check_note(&st, 2, 10, 3000);
}

static void a_dropped_take(void)
{
	struct note_header h = { 0 };
	uint8_t chunk[200];

	blank_flash();
	store_mount(&st, &io, false);
	ready(&st);
	memset(chunk, 0x5a, sizeof(chunk));
	/* A click: 100 bytes never left RAM, nothing to erase */
	CHECK_EQ(store_begin(&st), 0);
	CHECK_EQ(store_append(&st, chunk, 100), 100);
	store_abandon(&st);
	CHECK_EQ(st.sector[0], STORE_BLANK);
	/* Longer: what it wrote is erased before the next take, at the same place */
	CHECK_EQ(store_begin(&st), 0);
	for (int i = 0; i < 35; i++) {
		store_append(&st, chunk, sizeof(chunk));
	}
	store_abandon(&st);
	CHECK_EQ(st.sector[0], STORE_DIRTY);
	CHECK_EQ(st.sector[1], STORE_DIRTY);
	erases = 0;
	CHECK_EQ(take(&st, 3, 4000, 4000), 1);
	CHECK_EQ(erases, 1); /* no note in their block: erased whole, at once */
	CHECK_EQ(st.notes[0].first, 0);
	check_note(&st, 1, 3, 4000);
	/* Nothing to finish once dropped */
	CHECK_EQ(store_finish(&st, &h, 10), -EINVAL);
}

static void delivered_notes_and_ids(void)
{
	struct store_note n;

	blank_flash();
	store_mount(&st, &io, false);
	CHECK_EQ(take(&st, 1, 5000, 5000), 1);
	CHECK_EQ(take(&st, 2, 5000, 5000), 2);
	CHECK_EQ(take(&st, 3, 5000, 5000), 3);
	/* The middle one received: oldest first, without it */
	CHECK_EQ(store_delivered(&st, 2), 0);
	CHECK_EQ(store_delivered(&st, 2), -ENOENT);
	CHECK(store_next(&st, 0, &n) && n.id == 1);
	CHECK(store_next(&st, 1, &n) && n.id == 3);
	CHECK(!store_next(&st, 3, &n));
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(store_count(&st), 2);
	check_note(&st, 1, 1, 5000);
	check_note(&st, 3, 3, 5000);
	/* A cut before the erase at rest: the delivered one is still to erase */
	CHECK_EQ(st.sector[2], STORE_DIRTY);
	CHECK_EQ(st.sector[3], STORE_DIRTY);
	/* All received: none waits, but the next id follows, after a reboot too */
	CHECK_EQ(store_delivered(&st, 1), 0);
	CHECK_EQ(store_delivered(&st, 3), 0);
	CHECK_EQ(store_count(&st), 0);
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(store_count(&st), 0);
	CHECK_EQ(st.last_id, 3);
	CHECK_EQ(st.sector[4], STORE_KEEP); /* the header of note 3 */
	CHECK_EQ(take(&st, 4, 5000, 5000), 4);
	check_note(&st, 4, 4, 5000);
	/* The kept header is not needed any more: to erase */
	for (uint16_t sec = 0; sec < STORE_SECTORS; sec++) {
		CHECK(st.sector[sec] != STORE_KEEP);
	}
}

static void ids_outlive_a_clean_ring(void)
{
	blank_flash();
	store_mount(&st, &io, false);
	CHECK_EQ(take(&st, 1, 5000, 5000), 1);
	CHECK_EQ(take(&st, 2, 5000, 5000), 2);
	CHECK_EQ(store_delivered(&st, 1), 0);
	CHECK_EQ(store_delivered(&st, 2), 0);
	/* Everything erased ahead, round to the header of note 2 */
	while (store_prepare(&st, STORE_SECTORS) > 0) {
	}
	CHECK_EQ(st.sector[2], STORE_KEEP);
	CHECK_EQ(store_blank_ahead(&st), STORE_SECTORS - 2);
	CHECK_EQ(store_room(&st), STORE_SECTORS - 2);
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(store_count(&st), 0);
	CHECK_EQ(st.last_id, 2);
	CHECK_EQ(take(&st, 3, 5000, 5000), 3);
	check_note(&st, 3, 3, 5000);
}

/* Once the phone has a note, its voice leaves the flash at rest, but for the
 * first sector of the newest, which keeps the last id */
static void delivered_voice_is_erased(void)
{
	bool clean = true;

	blank_flash();
	store_mount(&st, &io, false);
	CHECK_EQ(take(&st, 1, 10000, 10000), 1); /* sectors 0 to 2 */
	CHECK_EQ(take(&st, 2, 10000, 10000), 2); /* 3 to 5 */
	CHECK_EQ(store_delivered(&st, 1), 0);
	CHECK_EQ(store_delivered(&st, 2), 0);
	ready(&st);
	for (uint32_t i = 0; i < 6U * STORE_SECTOR; i++) {
		if (i / STORE_SECTOR != 3 && flash[i] != 0xff) {
			clean = false;
		}
	}
	CHECK(clean);
	CHECK_EQ(st.sector[3], STORE_KEEP);
	CHECK_EQ(st.sector[4], STORE_BLANK);
	/* The next note lets that sector go too */
	CHECK_EQ(take(&st, 3, 1000, 1000), 3);
	ready(&st);
	CHECK_EQ(st.sector[3], STORE_BLANK);
	CHECK_EQ(flash[3 * STORE_SECTOR], 0xff);
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(st.last_id, 3);
}

static bool voice_left(uint16_t except)
{
	for (uint32_t i = 0; i < STORE_SIZE; i++) {
		const bool header = i / STORE_SECTOR == except && i % STORE_SECTOR < NOTE_HEADER_SIZE;

		if (!header && flash[i] != 0xff) {
			return true;
		}
	}
	return false;
}

/* The reset: nothing of the voice left, the ids go on */
static void a_reset_wipes_all_but_the_last_id(void)
{
	uint16_t keep;

	blank_flash();
	store_mount(&st, &io, false);
	CHECK_EQ(take(&st, 1, 30000, 30000), 1);
	CHECK_EQ(take(&st, 2, 30000, 30000), 2);
	CHECK_EQ(store_delivered(&st, 1), 0);
	CHECK_EQ(take(&st, 3, 30000, 30000), 3); /* waiting, undelivered */
	keep = st.head;
	CHECK_EQ(store_wipe(&st), 0);
	CHECK_EQ(store_count(&st), 0);
	CHECK_EQ(st.last_id, 3);
	CHECK(!voice_left(keep));
	/* After a reboot too; and the next note is number 4 */
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(store_count(&st), 0);
	CHECK_EQ(st.last_id, 3);
	CHECK_EQ(take(&st, 4, 3000, 3000), 4);
	check_note(&st, 4, 4, 3000);
	/* A blank store: all erased, nothing written */
	blank_flash();
	store_mount(&st, &io, false);
	CHECK_EQ(store_wipe(&st), 0);
	CHECK_EQ(st.last_id, 0);
	CHECK(!voice_left(STORE_SECTORS));
	CHECK_EQ(take(&st, 5, 3000, 3000), 1);
}

static void a_reset_cut_short(void)
{
	/* The power goes at every point of the wipe: no id is ever lost */
	for (long cut = 0; cut < 200; cut += 20) {
		blank_flash();
		store_mount(&st, &io, false);
		CHECK_EQ(take(&st, 1, 9000, 9000), 1);
		CHECK_EQ(take(&st, 2, 9000, 9000), 2);
		CHECK_EQ(store_delivered(&st, 2), 0);
		budget = cut; /* bytes of the header that reach the flash */
		(void)store_wipe(&st);
		budget = -1;
		CHECK_EQ(store_mount(&st, &io, false), 0);
		CHECK_EQ(st.last_id, 2);
		CHECK_EQ(take(&st, 3, 3000, 3000), 3);
	}
}

static void the_ring_wraps(void)
{
	uint32_t id = 0;
	struct store_note n;

	blank_flash();
	store_mount(&st, &io, false);
	/* Notes of 200 KB, 49 sectors: the sixth crosses the end of the flash */
	for (uint32_t i = 1; i <= 7; i++) {
		if (store_count(&st) >= 3) {
			CHECK(store_next(&st, 0, &n));
			CHECK_EQ(store_delivered(&st, n.id), 0);
		}
		id = take(&st, 100 + i, 200000, 200000);
		CHECK_EQ(id, i);
	}
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(store_count(&st), 3);
	for (uint32_t i = 5; i <= 7; i++) {
		check_note(&st, i, 100 + i, 200000);
	}
}

static void the_flash_full(void)
{
	uint32_t id;
	uint32_t n = 0;
	struct store_note first;

	blank_flash();
	store_mount(&st, &io, false);
	/* Notes of 60 KB, none received: the last one gets what is left */
	while ((id = take(&st, 7, 60000, 60000)) != 0 && n < 30) {
		n++;
		if (st.notes[st.count - 1].size < NOTE_HEADER_SIZE + 60000) {
			break;
		}
	}
	CHECK(n >= 16);
	CHECK_EQ(store_blank_ahead(&st), 0);
	CHECK_EQ(store_room(&st), 0);
	CHECK_EQ(store_begin(&st), -ENOSPC);
	/* Every note whole, the last one cut at the end of the room */
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(store_count(&st), n);
	for (uint16_t k = 0; k < store_count(&st); k++) {
		check_note(&st, st.notes[k].id, 7, st.notes[k].size - NOTE_HEADER_SIZE);
	}
	ready(&st);
	CHECK_EQ(store_begin(&st), -ENOSPC);
	/* The phone takes the oldest: room again */
	CHECK(store_next(&st, 0, &first));
	CHECK_EQ(store_delivered(&st, first.id), 0);
	CHECK(take(&st, 8, 20000, 20000) != 0);
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(store_count(&st), n);
}

static void power_cuts(void)
{
	uint32_t id;
	struct note_header h = { .codec = NOTE_CODEC_LC3 };
	uint8_t chunk[200];

	/* Cut while the frames are written: no note, the sectors erased later */
	blank_flash();
	store_mount(&st, &io, false);
	CHECK_EQ(take(&st, 1, 3000, 3000), 1);
	ready(&st);
	CHECK_EQ(store_begin(&st), 0);
	memset(chunk, 0x33, sizeof(chunk));
	budget = 5000;
	for (int i = 0; i < 40; i++) {
		store_append(&st, chunk, sizeof(chunk));
	}
	CHECK(st.failed);
	CHECK(store_finish(&st, &h, 8000) != 0);
	budget = -1;
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(store_count(&st), 1);
	check_note(&st, 1, 1, 3000);
	id = take(&st, 2, 3000, 3000);
	CHECK_EQ(id, 2);
	check_note(&st, 2, 2, 3000);
	/* Cut in the middle of the header: not a note */
	ready(&st);
	CHECK_EQ(store_begin(&st), 0);
	CHECK_EQ(store_append(&st, chunk, sizeof(chunk)), sizeof(chunk));
	budget = ((st.fill + 3) & ~3) + 20; /* the last page, then 20 bytes of the header */
	CHECK(store_finish(&st, &h, sizeof(chunk)) != 0);
	budget = -1;
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(store_count(&st), 2);
	CHECK_EQ(st.last_id, 2);
	CHECK_EQ(take(&st, 3, 1000, 1000), 3);
	check_note(&st, 3, 3, 1000);
}

/* Note 1 went to the phone but its mark never reached the flash, and note 2
 * was written over its sector 1: note 2 is the one to keep */
static void a_stale_note_gives_way(void)
{
	struct note_header h = { .codec = NOTE_CODEC_LC3, .id = 2, .data_size = 1000 };
	_Alignas(4) uint8_t img[5 * STORE_PAGE]; /* the header, then the frames */

	blank_flash();
	store_mount(&st, &io, false);
	CHECK_EQ(take(&st, 1, 10000, 10000), 1);
	memset(img, 0xff, sizeof(img));
	for (uint32_t i = 0; i < h.data_size; i++) {
		img[NOTE_HEADER_SIZE + i] = byte_of(77, i);
	}
	h.data_crc = note_crc32(0, &img[NOTE_HEADER_SIZE], h.data_size);
	h.state = NOTE_STATE_COMPLETE;
	f_erase(NULL, STORE_SECTOR, STORE_SECTOR);
	f_write(NULL, STORE_SECTOR + NOTE_HEADER_SIZE, &img[NOTE_HEADER_SIZE],
		STORE_PAGE - NOTE_HEADER_SIZE);
	for (uint32_t at = STORE_PAGE; at < sizeof(img); at += STORE_PAGE) {
		f_write(NULL, STORE_SECTOR + at, &img[at], STORE_PAGE);
	}
	note_header_put(&h, img);
	f_write(NULL, STORE_SECTOR, img, NOTE_HEADER_SIZE);
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(store_count(&st), 1);
	check_note(&st, 2, 77, 1000);
	CHECK_EQ(st.last_id, 2);
	CHECK_EQ(st.head, 2);
	CHECK_EQ(st.sector[0], STORE_DIRTY);
	CHECK_EQ(st.sector[2], STORE_DIRTY);
	CHECK_EQ(take(&st, 3, 3000, 3000), 3);
	check_note(&st, 3, 3, 3000);
}

static void leftovers_are_erased(void)
{
	blank_flash();
	/* The takes of the self-test's K1, at 16 and 64 */
	for (uint32_t i = 0; i < 300; i++) {
		flash[16 * STORE_SECTOR + i] = (uint8_t)i;
		flash[64 * STORE_SECTOR + i] = (uint8_t)i;
	}
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(store_count(&st), 0);
	CHECK_EQ(st.sector[0], STORE_UNKNOWN);
	CHECK_EQ(st.sector[16], STORE_DIRTY);
	erases = 0;
	CHECK_EQ(take(&st, 4, 6000, 6000), 1);
	check_note(&st, 1, 4, 6000);
	CHECK_EQ(erases, 2); /* blocks 1 and 4, whole; the rest read through, blank */
	/* An erase cut short in sector 2: its first bytes blank, a bit left
	 * further on. Beside note 1, it is erased alone. */
	flash[2 * STORE_SECTOR + 1000] = 0x12;
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(st.sector[2], STORE_UNKNOWN);
	erases = 0;
	CHECK_EQ(take(&st, 5, 3000, 3000), 2);
	CHECK_EQ(erases, 1);
	check_note(&st, 1, 4, 6000);
	check_note(&st, 2, 5, 3000);
}

static void a_hundred_notes(void)
{
	static uint32_t sizes[101];
	uint32_t next = 1;

	blank_flash();
	store_mount(&st, &io, false);
	for (uint32_t i = 1; i <= 100; i++) {
		struct store_note n;

		sizes[i] = 2000U + (i * 7919U) % 38000U;
		if (store_count(&st) > 4) {
			CHECK(store_next(&st, 0, &n));
			CHECK_EQ(store_delivered(&st, n.id), 0);
		}
		CHECK_EQ(take(&st, 1000 + i, sizes[i], sizes[i]), i);
		if (i % 7 == 0) {
			CHECK_EQ(store_mount(&st, &io, false), 0);
		}
	}
	/* Every note waiting reads back whole, oldest first */
	for (uint16_t k = 0; k < store_count(&st); k++) {
		const uint32_t id = st.notes[k].id;

		CHECK(id >= next);
		next = id + 1;
		check_note(&st, id, 1000 + id, sizes[id]);
	}
	CHECK_EQ(st.last_id, 100);
}

/* ---- The journal's tail (lot T1) ------------------------------------------ */

/* The journal's headers in the tail, as it writes them; intact: nothing of the
 * store read them as notes, wrote or erased there */
static void journal_marks(void)
{
	for (uint16_t k = 0; k < STORE_TAIL; k++) {
		memset(&flash[(size_t)(STORE_TAIL_FIRST + k) * STORE_SECTOR], 0x5a, 16);
	}
}

static bool journal_intact(void)
{
	for (uint16_t k = 0; k < STORE_TAIL; k++) {
		const uint8_t *p = &flash[(size_t)(STORE_TAIL_FIRST + k) * STORE_SECTOR];

		for (size_t i = 0; i < STORE_SECTOR; i++) {
			if (p[i] != (i < 16 ? 0x5a : 0xff)) {
				return false;
			}
		}
	}
	return true;
}

/* A blank flash: the tail goes to the journal at once, the store never
 * touches it again, and knows it at the next boot */
static void the_tail_goes_to_the_journal(void)
{
	blank_flash();
	CHECK_EQ(store_mount(&st, &io, false), 0);
	CHECK_EQ(st.ring, STORE_SECTORS);
	/* Not while a take is written */
	CHECK_EQ(store_prepare(&st, 1), 1);
	CHECK_EQ(store_begin(&st), 0);
	CHECK(!store_tail_free(&st));
	store_abandon(&st);
	CHECK(store_tail_free(&st));
	journal_marks();
	store_give_tail(&st);
	CHECK_EQ(st.ring, STORE_TAIL_FIRST);
	CHECK(!store_tail_free(&st)); /* given for good */
	ready(&st);
	CHECK_EQ(store_blank_ahead(&st), STORE_TAIL_FIRST);
	CHECK_EQ(store_room(&st), STORE_TAIL_FIRST);
	CHECK_EQ(take(&st, 9, 30000, 30000), 1);
	CHECK(journal_intact());
	/* Rebooted, the journal's headers found there */
	CHECK_EQ(store_mount(&st, &io, true), 0);
	CHECK_EQ(st.ring, STORE_TAIL_FIRST);
	CHECK_EQ(st.sector[STORE_TAIL_FIRST], STORE_FOREIGN);
	CHECK_EQ(st.sector[STORE_SECTORS - 1U], STORE_FOREIGN);
	CHECK(!store_tail_free(&st));
	check_note(&st, 1, 9, 30000);
	ready(&st);
	CHECK(journal_intact());
}

/* The ring goes round before the tail: a note over its end goes on at sector
 * 0, read back the same way after a reboot; the last block of the notes is
 * erased sector by sector */
static void the_ring_wraps_before_the_tail(void)
{
	blank_flash();
	store_mount(&st, &io, false);
	journal_marks();
	store_give_tail(&st);
	/* 15 sectors a note, each delivered but the last: the 17th starts at 240 */
	for (uint32_t i = 1; i <= 17; i++) {
		CHECK_EQ(take(&st, 200 + i, 60000, 60000), i);
		if (i < 17) {
			CHECK_EQ(store_delivered(&st, i), 0);
		}
	}
	CHECK_EQ(st.head, 7); /* 240 + 15, round the ring of 248 */
	check_note(&st, 17, 217, 60000);
	ready(&st);
	CHECK(journal_intact());
	CHECK_EQ(store_mount(&st, &io, true), 0);
	CHECK_EQ(store_count(&st), 1);
	CHECK_EQ(st.head, 7);
	check_note(&st, 17, 217, 60000);
	CHECK_EQ(store_delivered(&st, 17), 0);
	CHECK_EQ(take(&st, 300, 3000, 3000), 18);
	ready(&st);
	CHECK(journal_intact());
}

/* The tail waits while a note or the header of the last id lies there; then
 * the head, if there, starts again at sector 0 */
static void the_tail_waits_for_its_notes(void)
{
	blank_flash();
	store_mount(&st, &io, false);
	/* Five notes of 50 sectors: the fifth runs into the tail, waiting */
	for (uint32_t i = 1; i <= 5; i++) {
		CHECK_EQ(take(&st, 400 + i, 204736, 204736), i);
	}
	CHECK_EQ(st.head, 250);
	CHECK(!store_tail_free(&st));
	for (uint32_t i = 1; i <= 5; i++) {
		CHECK_EQ(store_delivered(&st, i), 0);
	}
	CHECK(store_tail_free(&st)); /* the last id is kept at 200 */
	/* Small notes in the tail instead, each delivered: the last id there */
	for (uint32_t i = 6; i <= 8; i++) {
		CHECK_EQ(take(&st, 400 + i, 6000, 6000), i);
		CHECK_EQ(store_delivered(&st, i), 0);
		CHECK(!store_tail_free(&st));
	}
	CHECK_EQ(st.head, 0);
	/* A new note at 0 holds the last id now, waiting: the tail is free */
	CHECK_EQ(take(&st, 409, 6000, 6000), 9);
	CHECK(store_tail_free(&st));
	store_give_tail(&st);
	CHECK_EQ(st.head, 2);
	CHECK_EQ(take(&st, 410, 6000, 6000), 10);
	/* The ids go on across the reboot, the old headers of the tail unread */
	journal_marks();
	CHECK_EQ(store_mount(&st, &io, true), 0);
	CHECK_EQ(st.last_id, 10);
	check_note(&st, 10, 410, 6000);
	/* A delivered note ending in the tail: free, and the head comes back */
	blank_flash();
	store_mount(&st, &io, false);
	for (uint32_t i = 1; i <= 5; i++) {
		CHECK_EQ(take(&st, 500 + i, 204736, 204736), i);
		CHECK_EQ(store_delivered(&st, i), 0);
	}
	CHECK_EQ(st.head, 250);
	CHECK(store_tail_free(&st));
	store_give_tail(&st);
	CHECK_EQ(st.head, 0);
	CHECK_EQ(take(&st, 506, 6000, 6000), 6);
}

/* The journal failed to start: the tail, still the store's, is erased at rest
 * before a take can go there */
static void a_journal_that_did_not_start(void)
{
	blank_flash();
	store_mount(&st, &io, false);
	ready(&st);
	CHECK(store_tail_free(&st));
	memset(&flash[(size_t)STORE_TAIL_FIRST * STORE_SECTOR], 0x00, 8); /* half a header */
	store_tail_dirty(&st);
	CHECK_EQ(st.ring, STORE_SECTORS);
	CHECK_EQ(st.sector[STORE_TAIL_FIRST], STORE_DIRTY);
	CHECK_EQ(st.sector[STORE_SECTORS - 1U], STORE_DIRTY);
	ready(&st);
	CHECK_EQ(st.sector[STORE_TAIL_FIRST], STORE_BLANK);
	CHECK_EQ(flash[(size_t)STORE_TAIL_FIRST * STORE_SECTOR], 0xff);
	CHECK(store_tail_free(&st));
}

/* The reset wipes the ring, not the tail, and keeps the last id, also when
 * its header falls in the last block of the notes */
static void a_reset_leaves_the_tail(void)
{
	blank_flash();
	store_mount(&st, &io, false);
	journal_marks();
	store_give_tail(&st);
	/* Five notes of 49 sectors, waiting: the head at 245 */
	for (uint32_t i = 1; i <= 5; i++) {
		CHECK_EQ(take(&st, 600 + i, 200000, 200000), i);
	}
	CHECK_EQ(st.head, 245);
	CHECK_EQ(store_wipe(&st), 0);
	CHECK_EQ(store_count(&st), 0);
	CHECK_EQ(st.last_id, 5);
	CHECK(journal_intact());
	CHECK_EQ(store_mount(&st, &io, true), 0);
	CHECK_EQ(store_count(&st), 0);
	CHECK_EQ(st.last_id, 5);
	CHECK_EQ(st.head, 246);
	CHECK_EQ(take(&st, 606, 3000, 3000), 6);
	CHECK(journal_intact());
}

int main(void)
{
	RUN(a_blank_flash);
	RUN(a_note_after_a_reboot);
	RUN(the_silence_is_cut);
	RUN(a_dropped_take);
	RUN(delivered_notes_and_ids);
	RUN(ids_outlive_a_clean_ring);
	RUN(delivered_voice_is_erased);
	RUN(a_reset_wipes_all_but_the_last_id);
	RUN(a_reset_cut_short);
	RUN(the_ring_wraps);
	RUN(the_flash_full);
	RUN(power_cuts);
	RUN(a_stale_note_gives_way);
	RUN(leftovers_are_erased);
	RUN(a_hundred_notes);
	RUN(the_tail_goes_to_the_journal);
	RUN(the_ring_wraps_before_the_tail);
	RUN(the_tail_waits_for_its_notes);
	RUN(a_journal_that_did_not_start);
	RUN(a_reset_leaves_the_tail);
	return harness_report("store");
}
