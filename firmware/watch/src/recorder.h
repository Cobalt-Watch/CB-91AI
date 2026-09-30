/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The voice (lot E1): the audio thread
 * powers the microphone, finds the speech (lib/vad.c), encodes it in LC3 at
 * 16 kbit/s and writes it straight to the storage flash (notes.c), where each
 * take becomes a note once its header is written. At rest it erases ahead of
 * the notes, never during a take.
 *
 * The event loop drives it: a take starts on the press of ALARM (record then
 * cancel, EF-10), a press ends it and keeps the note, a click drops it. It
 * also ends by itself: 3 s after the last word, nothing said in its first 5 s
 * (not kept: a press in a pocket), its longest length, or the flash full. Its
 * end posts EVT_AUDIO_STOPPED, and recorder_result() tells how it went.
 */

#ifndef CB91AI_WATCH_RECORDER_H
#define CB91AI_WATCH_RECORDER_H

#include <stdbool.h>
#include <stdint.h>

#include "take.h" /* RECORDER_*: how a take that left no note ended */

struct recorder_take {
	int64_t utc_ms;      /* the press; 0: the watch has no time */
	int16_t tz_minutes;
	bool time_approx;
	uint32_t max_ms;     /* the longest take */
	bool until_silence;  /* ends 3 s after the last word, and without any in 5 s */
	bool latched;        /* kept from the start (the harness), no recorder_keep() */
};

/* Start a take: its number, never 0. Its frames wait in RAM until it is
 * latched: a click that drops it writes nothing to the flash. */
uint32_t recorder_start(const struct recorder_take *take);

/* Take `take` is latched ("rEC"): its frames go to the flash */
void recorder_keep(uint32_t take);

/* ALARM was released after the latch, at `at_ms` of uptime (buttons.h): the
 * voice detector hears from the end of its click on (it is deaf until then,
 * 1.75 s of frames at most) */
void recorder_listen(uint32_t take, uint32_t at_ms);

/* A press ended take `take`, its contact at `at_ms` of uptime: its note is
 * kept, up to where the finger is first heard (take.h) */
void recorder_stop(uint32_t take, uint32_t at_ms);

/* A button moved during the take under way, pressed (`down`) or released, at
 * `at_ms` of uptime: the voice detector does not hear its click (clicks.h) */
void recorder_heard(uint32_t at_ms, bool down);

/* Take `take` is dropped */
void recorder_drop(uint32_t take);

/* A take was asked and is not over yet */
bool recorder_busy(void);

/* The same, for the updates it holds off (update.c): no longer than the
 * longest take and a minute, should the thread never report its end */
bool recorder_holds_updates(void);

/* The last take over: its number (0: none yet), how it ended (NOTE_END_* when
 * a note was kept, RECORDER_* otherwise) and the id of its note, 0 if none */
uint32_t recorder_result(uint8_t *end, uint32_t *note_id);

/* Look ahead of the notes again, at rest: some were delivered */
void recorder_prepare(void);

/* The watch reset (lot S3): every note erased, but the last id, once the take
 * under way (if any) is over. EVT_NOTES_WIPED then, `arg` 1 if erased, 0 if
 * the flash refused. */
void recorder_wipe(void);

/* Less room in the flash than the longest take: the notes should go now */
bool recorder_flash_full(void);

/* Seconds of voice kept since boot (EF-73) */
uint32_t recorder_recorded_s(void);

#endif /* CB91AI_WATCH_RECORDER_H */
