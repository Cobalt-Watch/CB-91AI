/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The voice notes in the storage flash (lot E1):
 * 256 sectors of 4 KB in a ring, a note on consecutive sectors
 * from the start of one, its header of 64 bytes (lib/note.h) written last.
 * Pure logic over three functions of the flash, so that the host tests run it
 * on a simulated NOR flash, power cuts included (tests/host/test_store.c).
 *
 * What a sector holds, as the mount finds it and the store keeps track:
 * - a note not yet delivered;
 * - blank, checked erased, ready for a take;
 * - unknown: its first 64 bytes are blank, the rest unchecked (a take cut
 *   before its header, an erase cut short);
 * - dirty, to erase: a take cut or dropped, a delivered note, anything else
 *   (the takes of the self-test's K1, for one);
 * - kept: the first sector of the newest note once delivered, whose header
 *   holds the last id given, so that an id is never given twice.
 *
 * Nothing is erased while a take is written: store_prepare(), at rest,
 * checks and erases the sectors ahead of the head. Not thread-safe: the
 * Zephyr glue holds a lock around each call.
 *
 * The last STORE_TAIL sectors go to the journal of the temperature (lot T1,
 * tlog.h) once no note is there: the ring of the notes then ends before them
 * (the Cobalt Link specification), and the store never reads, writes
 * nor erases them again.
 */

#ifndef CB91AI_WATCH_STORE_H
#define CB91AI_WATCH_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "note.h"

#define STORE_SECTOR    4096U
#define STORE_SECTORS   256U
#define STORE_SIZE      (STORE_SECTOR * STORE_SECTORS)
#define STORE_PAGE      256U
#define STORE_BLOCK     16U /* sectors of a 64 KB block, erased as fast as one sector */
#define STORE_MAX_NOTES STORE_SECTORS /* one per sector at most */
#define STORE_TAIL      8U  /* the journal's, at the end of the flash (lot T1) */
#define STORE_TAIL_FIRST (STORE_SECTORS - STORE_TAIL)

enum store_sector_state {
	STORE_UNKNOWN,
	STORE_BLANK,
	STORE_DIRTY,
	STORE_NOTE,
	STORE_KEEP,
	STORE_FOREIGN, /* the journal's tail: not the store's any more */
};

/* The flash: writes of whole words at aligned addresses, from word-aligned RAM
 * (the QSPI peripheral reads them by DMA), erases of whole sectors or blocks */
struct store_io {
	void *ctx;
	int (*read)(void *ctx, uint32_t addr, void *buf, size_t len);
	int (*write)(void *ctx, uint32_t addr, const void *buf, size_t len);
	int (*erase)(void *ctx, uint32_t addr, size_t len);
};

/* A note waiting to be sent: its bytes are its header then its frames */
struct store_note {
	uint32_t id;
	uint16_t first;   /* first sector */
	uint16_t sectors;
	uint32_t size;    /* header and frames */
	uint32_t crc;     /* as NOTE_OFFER announces it */
};

struct store {
	struct store_io io;
	uint16_t ring;     /* sectors of the ring: all, or STORE_TAIL_FIRST once the journal has the tail */
	uint8_t sector[STORE_SECTORS]; /* enum store_sector_state */
	struct store_note notes[STORE_MAX_NOTES]; /* waiting, oldest first */
	uint16_t count;
	uint32_t last_id;  /* the last id given */
	uint16_t head;     /* where the next note starts */
	/* The take being written */
	bool writing;
	bool failed;       /* a write of it failed: it cannot be finished */
	uint16_t w_first;
	uint32_t w_room;   /* bytes of frames the blank sectors ahead can take */
	uint32_t w_len;    /* bytes of frames given */
	uint32_t w_page_at; /* offset in the note of page[0] */
	uint16_t fill;     /* bytes waiting in page[] */
	_Alignas(4) uint8_t page[STORE_PAGE];
};

/* Read what the flash holds: the notes, the dirty sectors, the head. With
 * `tail_taken` (the journal found its headers there), the ring ends before the
 * tail. */
int store_mount(struct store *s, const struct store_io *io, bool tail_taken);

/* The tail can go to the journal: the ring is still whole, no take is being
 * written, and neither a note waiting nor the header of the last id lies there.
 * Then, once the journal has written its first header there, under the same
 * lock, store_give_tail(): the ring ends before the tail from then on. */
bool store_tail_free(const struct store *s);
void store_give_tail(struct store *s);

/* The journal wrote in the tail but did not start (tlog_start() failed): its
 * sectors, still the store's, are erased at rest before a take goes there */
void store_tail_dirty(struct store *s);

/* At rest, before a take: one step (check a sector, or erase one, or its whole
 * block when no note lies there) towards `want` blank sectors ahead of the
 * head. More than 0: call again; 0: nothing to do (ready, or stopped by a
 * waiting note); less than 0: a flash error. */
int store_prepare(struct store *s, uint16_t want);

/* At rest, once prepared: one step of the same kind anywhere else in the
 * flash, so that no voice stays there once the phone has it (a delivered note,
 * a take cut short). Same returns; 0 when the whole flash is known clean. */
int store_tidy(struct store *s);

/* Blank sectors ready ahead of the head */
uint16_t store_blank_ahead(const struct store *s);

/* Sectors ahead of the head up to the first that holds a note: the room of
 * the next take once store_prepare() is done */
uint16_t store_room(const struct store *s);

/* A take begins: -ENOSPC if no blank sector is ready or the list is full */
int store_begin(struct store *s);

/* Frames of the take: how many bytes were taken (fewer once the room ahead is
 * full: the take must end, NOTE_END_FULL) */
size_t store_append(struct store *s, const uint8_t *data, size_t len);

/* The take ends and becomes a note, its frames cut at `keep` bytes (the
 * silence after the last word). The store fills id, data_size, data_crc and
 * state of `h`, read back from the flash; the rest is the caller's. */
int store_finish(struct store *s, struct note_header *h, uint32_t keep);

/* The take is dropped: what it wrote will be erased */
void store_abandon(struct store *s);

/* The oldest note waiting with an id above `after`: false if none */
bool store_next(const struct store *s, uint32_t after, struct store_note *out);

/* Bytes of note `id` from `offset`: how many were read, 0 past its end or on
 * an error */
size_t store_read(struct store *s, uint32_t id, uint32_t offset, uint8_t *buf, size_t len);

/* The phone has note `id`: marked delivered in its header, its sectors to be
 * erased at rest */
int store_delivered(struct store *s, uint32_t id);

/* Notes waiting, and their bytes */
uint16_t store_count(const struct store *s);
uint32_t store_bytes(const struct store *s);

/* The watch reset (lot S3): every note erased, delivered or not, but the last
 * id, written first into a header with no frames, so that an id is never given
 * twice, not even to a phone paired again. A cut at any point loses no id. */
int store_wipe(struct store *s);

#endif /* CB91AI_WATCH_STORE_H */
