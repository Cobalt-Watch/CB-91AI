/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The voice notes of the storage flash (lot E1): store.c on the QSPI flash,
 * under one lock. The audio thread writes them (recorder.c), the event loop
 * reads them for the session and marks them delivered (link.c). The lock is
 * held for one call of store.c at a time: 20 ms for an erase, a few hundred
 * for the read-back of a long note as it ends.
 *
 * And the journal of the temperature and the cell (lot T1, tlog.c) in the tail
 * of the same flash, under the same lock: the event loop writes and reads it.
 */

#ifndef CB91AI_WATCH_NOTES_H
#define CB91AI_WATCH_NOTES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "note.h"
#include "store.h"
#include "tlog.h"

/* Read the notes of the flash: from the audio thread, before anything else */
int notes_init(void);

/* The reset under way (lot S3): until it has erased them, no note is offered,
 * counted or read */
void notes_hide(bool hide);

/* The oldest note waiting with an id above `after`: false if none */
bool notes_next(uint32_t after, struct store_note *out);

/* Bytes of note `id` from `offset`: how many were read */
size_t notes_read(uint32_t id, uint32_t offset, uint8_t *buf, size_t len);

/* The phone has note `id`: its sectors are erased at rest */
int notes_delivered(uint32_t id);

/* Notes waiting, and their bytes */
uint16_t notes_count(void);
uint32_t notes_bytes(void);

/* The last id given, 0 before the first note: the notes recorded, ever */
uint32_t notes_last_id(void);

/* Sectors the next take can fill once prepared */
uint16_t notes_room(void);

/* For the audio thread: store.h, under the lock */
int notes_prepare(uint16_t want);
int notes_tidy(void);
int notes_wipe(void);
int notes_begin(void);
size_t notes_append(const uint8_t *data, size_t len);
int notes_finish(struct note_header *h, uint32_t keep);
void notes_abandon(void);

/* The journal (tlog.h), for the event loop. A record at its end: the first one
 * takes the tail once the notes have left it (store_tail_free()) and
 * `may_take_tail` (the image running is confirmed: an image older than the
 * journal, come back by a revert, would read the ring of the notes whole), the
 * journal named at random; -EAGAIN until then, the record dropped. */
int notes_journal_append(const struct tlog_record *r, bool may_take_tail);

/* Records from index `from` (tlog_read()) and the journal's name, together;
 * the index of the next record (0 without a journal). notes_wipe() erases it
 * too, under a new name: without a journal the tail is the notes', wiped with
 * them. */
size_t notes_journal_read(uint32_t from, struct tlog_record *out, size_t max, uint32_t *first,
			  uint32_t *id);
uint32_t notes_journal_next(void);

#endif /* CB91AI_WATCH_NOTES_H */
