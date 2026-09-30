/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The audio thread, see recorder.h.
 *
 * mic_stream() hands over the samples 100 ms at a time, high-passed at 50 Hz,
 * in this thread. The driver keeps two of its four blocks (the one it fills,
 * the next), so a block has about 200 ms to go through before the driver runs
 * out and stops. Each one is ten frames of 10 ms: the voice detector sees
 * them, LC3 encodes them (20 bytes each, 2.5 ms of CPU apiece at most, K2),
 * and the 200 bytes go to the page of the store, which writes the flash a page
 * at a time. Nothing is erased meanwhile: the sectors ahead were erased
 * before the microphone started.
 *
 * Until the take is latched ("rEC", half a second of ALARM held), its frames
 * wait in RAM: a click drops it, and it would otherwise have written the head
 * sector only for it to be erased again, a cycle of wear per click. A take
 * never latched never becomes a note.
 *
 * The buttons are heard through the case as loud as a voice (at the wrist,
 * 2026-09-24). The loop tells this thread when each edge came, in ms of
 * uptime, and clicks.c maps it onto the frames: the voice detector is deaf
 * until ALARM is released after the latch and 200 ms more (1.75 s at most, for
 * a wearer who holds it while talking), and around any edge during the take;
 * it judges each frame 320 ms after it came, once the loop has told of any
 * button heard then. A note stopped by a press ends 300 ms before the contact,
 * where the finger is first heard (take.h).
 *
 * Priority 2: under the event loop (0, Zephyr's default), over the updates of
 * MCUmgr (3). The loop only runs short handlers; an upload writes the internal
 * flash, and it is refused during a take anyway (update.c).
 */

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <lc3.h>

#include "clicks.h"
#include "events.h"
#include "mic.h"
#include "note.h"
#include "notes.h"
#include "recorder.h"
#include "vad.h"

LOG_MODULE_REGISTER(watch_recorder, LOG_LEVEL_INF);

#define FRAME_US      10000
#define FRAME_SAMPLES (MIC_SAMPLE_RATE_HZ / 100)  /* 160 */
#define BITRATE       16000                       /* LC3, decided on 2026-09-23 (K3) */
#define FRAME_BYTES   (BITRATE / 8 / 100)         /* 20 */
#define BLOCK_FRAMES  10                          /* the 100 ms blocks of mic_stream() */
/* Frames held in RAM until the take is latched: a click is decided 300 ms after
 * its release, a triple click in about 1.2 s */
#define HOLD_BYTES    (15 * BLOCK_FRAMES * FRAME_BYTES)
/* Sectors kept erased ahead of the notes: the longest take */
#define PREPARE_SECTORS                                                                            \
	DIV_ROUND_UP(NOTE_HEADER_SIZE + CONFIG_CB91AI_WATCH_NOTE_MAX_S * 100U * FRAME_BYTES,      \
		     STORE_SECTOR)
/* No erase right after boot (the cell; and an image in test is still
 * to be confirmed): the first look at the flash waits this long, unless a take
 * or a delivered note comes first */
#define BOOT_REST_S   30
/* The detector deaf until the release of ALARM (and its click, clicks.h), 175
 * frames at most (the audio starts about 250 ms after the press) */
#define DEAF_AT_MOST       175U
/* Each frame judged this many frames after it came. A frame is judged with the
 * block that brings the frame VAD_LAG after it, and the loop tells of an edge
 * a few ms after it came: a press is heard from CLICKS_PRESS_BEFORE frames
 * before its contact, all of them must still wait when it is told */
#define VAD_LAG            32U
#define VAD_RING           (VAD_LAG + 1U)
/* The latest edges of the buttons, from the loop: a press and its release,
 * with their bounces, between two blocks */
#define EDGES              8U
BUILD_ASSERT(VAD_LAG >= CLICKS_PRESS_BEFORE + 2U, "a press told late would be heard");
/* A take reported as running holds an update off this long at most, should
 * the thread never report it */
#define TAKE_BOUND_MS (CONFIG_CB91AI_WATCH_NOTE_MAX_S * 1000U + 60000U)

BUILD_ASSERT(FRAME_SAMPLES * 100 == MIC_SAMPLE_RATE_HZ, "frames of 10 ms");
BUILD_ASSERT(PREPARE_SECTORS < STORE_SECTORS, "the longest take fits the flash");

static K_SEM_DEFINE(wake, 0, 1);
/* Take numbers: the loop moves start, keep, stop and drop, this thread done */
static atomic_t start_seq;
static atomic_t done_seq;
static atomic_t keep_seq;
static atomic_t listen_seq;
static atomic_t listen_at; /* uptime of that release, ms */
static atomic_t stop_seq;
static atomic_t stop_at;   /* uptime of the contact of that press, ms */
/* Edges of the buttons during a take: the loop writes, this thread reads */
static atomic_t edge_at[EDGES];
static atomic_t edge_down[EDGES];
static atomic_t edges_given;
static atomic_t drop_seq;
static atomic_t prepare_asked;
static atomic_t wipe_asked;
static atomic_t recorded_ms;
static atomic_t started_at; /* uptime of the last start, ms */
/* What the loop asked last, and how the last take went */
static struct k_spinlock lock;
static struct recorder_take asked;
static uint32_t result_seq;
static uint8_t result_end;
static uint32_t result_id;

static lc3_encoder_mem_16k_t encoder_mem;
static uint8_t held[HOLD_BYTES];
static size_t held_len;

struct take {
	uint32_t seq;
	struct vad vad;
	struct clicks clicks; /* where the buttons were heard */
	int32_t level[VAD_RING]; /* the levels of the frames still to judge */
	lc3_encoder_t encoder;
	uint32_t captured;    /* frames encoded, from the microphone */
	uint32_t frames;      /* frames written to the store */
	uint32_t edges_read;  /* edges of the loop taken into the clicks */
	uint32_t stop_frame;  /* the frame of the press that stopped it */
	bool listened;        /* the release of ALARM mapped */
	bool until_silence;
	bool latched;         /* "rEC": its frames go to the flash */
	bool ended;
	uint8_t end;          /* NOTE_END_*, or RECORDER_NOTHING, once `ended` */
	bool dropped;
};

static bool dropped(uint32_t seq)
{
	return (uint32_t)atomic_get(&drop_seq) == seq;
}

static void end_take(struct take *t, uint8_t end)
{
	t->ended = true;
	t->end = end;
}

/* Frames to the store: false once it takes no more (the room ahead is full,
 * or a write failed, which the finish tells) */
static bool store_frames(struct take *t, const uint8_t *data, size_t bytes)
{
	const size_t taken = notes_append(data, bytes);

	t->frames += (uint32_t)(taken / FRAME_BYTES);
	return taken == bytes;
}

static bool flush_held(struct take *t)
{
	const bool ok = held_len == 0 || store_frames(t, held, held_len);

	held_len = 0;
	return ok;
}

/* The edges the loop gave since the last block, and the release of ALARM
 * after the latch: the frames of their clicks made deaf */
static void hear_clicks(struct take *t)
{
	const uint32_t given = (uint32_t)atomic_get(&edges_given);

	if (given - t->edges_read > EDGES) {
		t->edges_read = given - EDGES; /* the oldest are gone: bounces */
	}
	for (; t->edges_read != given; t->edges_read++) {
		const uint32_t i = t->edges_read % EDGES;

		(void)clicks_edge(&t->clicks, (uint32_t)atomic_get(&edge_at[i]),
				  atomic_get(&edge_down[i]) != 0);
	}
	if (!t->listened && (uint32_t)atomic_get(&listen_seq) == t->seq) {
		t->listened = clicks_listen(&t->clicks, (uint32_t)atomic_get(&listen_at));
	}
}

/* From mic_stream(), every 100 ms: 1 stops the capture */
static int on_block(const int16_t *samples, size_t count, void *ctx)
{
	struct take *t = ctx;
	const uint32_t came = k_uptime_get_32();
	uint8_t out[BLOCK_FRAMES * FRAME_BYTES];
	size_t frames = MIN(count / FRAME_SAMPLES, (size_t)BLOCK_FRAMES);
	size_t bytes;

	if (dropped(t->seq)) {
		t->dropped = true;
		return 1;
	}
	/* When these frames were heard, then the buttons heard until now */
	clicks_block(&t->clicks, came, t->captured + (uint32_t)frames);
	hear_clicks(t);
	for (size_t i = 0; i < frames; i++) {
		const int16_t *pcm = &samples[i * FRAME_SAMPLES];
		const int32_t level = vad_level(pcm, FRAME_SAMPLES);

		if (lc3_encode(t->encoder, LC3_PCM_FORMAT_S16, pcm, 1, FRAME_BYTES,
			       &out[i * FRAME_BYTES]) != 0) {
			/* Never with these settings: the frames before it are kept,
			 * and an end already decided stays */
			LOG_ERR("LC3 refused a frame");
			if (!t->ended) {
				end_take(t, NOTE_END_FAULT);
			}
			frames = i;
			break;
		}
		/* Measured now, judged VAD_LAG frames later: by then the loop has
		 * told of any button heard around it */
		t->level[t->captured % VAD_RING] = level;
		t->captured++;
		if (t->captured > VAD_LAG && !t->ended) {
			const uint32_t j = t->captured - 1U - VAD_LAG;
			enum vad_verdict verdict;

			vad_deafen(&t->vad, clicks_deaf(&t->clicks, j));
			verdict = vad_step(&t->vad, t->level[j % VAD_RING]);
			if (t->until_silence && verdict != VAD_GO_ON) {
				/* The rest of the block goes to the store all the
				 * same: what the note keeps is decided at its end */
				end_take(t, verdict == VAD_STOP_SILENCE ? NOTE_END_SILENCE
									: RECORDER_NOTHING);
			}
		}
	}
	bytes = frames * FRAME_BYTES;
	if (!t->latched && (uint32_t)atomic_get(&keep_seq) == t->seq) {
		t->latched = true;
	}
	if (bytes > 0) {
		if (!t->latched && held_len + bytes <= sizeof(held)) {
			memcpy(&held[held_len], out, bytes);
			held_len += bytes;
		} else if (!flush_held(t) || !store_frames(t, out, bytes)) {
			if (!t->ended) {
				end_take(t, NOTE_END_FULL); /* an end already decided stays */
			}
			return 1;
		}
	}
	if (t->ended) {
		return 1;
	}
	if ((uint32_t)atomic_get(&stop_seq) == t->seq) {
		/* The note ends where the finger was first heard: the frame of
		 * the contact (take.h) */
		(void)clicks_frame(&t->clicks, (uint32_t)atomic_get(&stop_at), &t->stop_frame);
		end_take(t, NOTE_END_PRESS);
		return 1;
	}
	return 0;
}

/* The take is over: its result, then the loop is told */
static void report(uint32_t seq, uint8_t end, uint32_t id)
{
	const k_spinlock_key_t key = k_spin_lock(&lock);

	result_seq = seq;
	result_end = end;
	result_id = id;
	k_spin_unlock(&lock, key);
	atomic_set(&done_seq, (atomic_val_t)seq);
	/* Lost (queue full): the loop finds the take over at its next wake-up */
	(void)evt_post(EVT_AUDIO_STOPPED, end, (uint16_t)seq, id);
}

static void run_take(uint32_t seq, const struct recorder_take *p)
{
	struct take t = {
		.seq = seq,
		.edges_read = (uint32_t)atomic_get(&edges_given),
		.stop_frame = TAKE_STOP_UNKNOWN,
		.until_silence = p->until_silence,
		.latched = p->latched,
	};
	struct note_header h = {
		.codec = NOTE_CODEC_LC3,
		.channels = 1,
		.rate_hz = MIC_SAMPLE_RATE_HZ,
		.frame_us = FRAME_US,
		.frame_bytes = FRAME_BYTES,
		.bitrate = BITRATE,
		.utc_ms = p->utc_ms,
		.tz_minutes = p->tz_minutes,
		.time_approx = p->time_approx,
		.gain_db = (int8_t)mic_gain_db(),
		.high_pass_hz = CONFIG_CB91AI_MIC_HIGH_PASS_HZ,
		.fw = { APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_PATCHLEVEL },
	};
	uint32_t keep;
	uint8_t code;
	int ret = 1;

	/* The room first, before the microphone: what the last take left to
	 * erase, or the boot (never an erase during the take itself) */
	while (ret > 0 && !dropped(seq)) {
		ret = notes_prepare(PREPARE_SECTORS);
	}
	if (dropped(seq)) {
		report(seq, RECORDER_DROPPED, 0); /* dropped before it began */
		return;
	}
	ret = notes_begin();
	if (ret) {
		LOG_WRN("take %u: no room in the flash (%d)", seq, ret);
		report(seq, ret == -ENOSPC ? RECORDER_FULL : RECORDER_FAILED, 0);
		return;
	}
	vad_init(&t.vad);
	vad_set_gain_db(&t.vad, mic_gain_db());
	clicks_init(&t.clicks, DEAF_AT_MOST);
	t.encoder = lc3_setup_encoder(FRAME_US, MIC_SAMPLE_RATE_HZ, 0, &encoder_mem);
	held_len = 0;
	ret = t.encoder != NULL ? mic_stream((size_t)p->max_ms * (MIC_SAMPLE_RATE_HZ / 1000U), true,
					     on_block, &t)
				: -EINVAL;
	if (!t.ended && !t.dropped) {
		/* The whole length ran, or the microphone failed: what it gave
		 * before is kept, a dictation is not lost to a glitch */
		end_take(&t, ret >= 0 ? NOTE_END_LONGEST : NOTE_END_FAULT);
	}
	if (!t.latched && (uint32_t)atomic_get(&keep_seq) == seq) {
		t.latched = true; /* latched after its last block */
	}
	/* What waits in RAM goes to the note of a latched take that ended as a
	 * note does; the room ending meanwhile only shortens it */
	if (t.latched && !t.dropped && t.end < RECORDER_DROPPED) {
		(void)flush_held(&t);
	}
	held_len = 0;
	{
		const struct take_ended ended = {
			.dropped = t.dropped,
			.latched = t.latched,
			.end = t.end,
			.frames = t.frames,
			.vad_keep = vad_keep_frames(&t.vad),
			.stop_frame = t.stop_frame,
		};

		keep = take_keep(&ended, &code);
	}
	if (keep == 0) {
		notes_abandon();
		LOG_INF("take %u: nothing kept (%u) after %u frames", seq, code, t.frames);
		report(seq, code, 0);
		return;
	}
	h.end = code;
	h.duration_ms = keep * (FRAME_US / 1000U);
	ret = notes_finish(&h, keep * FRAME_BYTES);
	if (ret) {
		LOG_ERR("take %u: note not written (%d)", seq, ret);
		report(seq, RECORDER_FAILED, 0);
		return;
	}
	atomic_add(&recorded_ms, (atomic_val_t)h.duration_ms);
	LOG_INF("take %u: note %u, %u ms kept of %u, end %u", seq, h.id, h.duration_ms,
		t.frames * (FRAME_US / 1000U), h.end);
	report(seq, h.end, h.id);
}

/* At rest, a step at a time, as long as no take is asked: the room of the next
 * take, then what is left anywhere else (the voice of a delivered note) */
static void rest(void)
{
	int ret = 1;

	atomic_set(&prepare_asked, 0);
	while (ret > 0 && !recorder_busy() && !atomic_get(&wipe_asked)) {
		ret = notes_prepare(PREPARE_SECTORS);
	}
	if (ret == 0) {
		ret = 1;
		while (ret > 0 && !recorder_busy() && !atomic_get(&wipe_asked)) {
			ret = notes_tidy();
		}
	}
	if (ret < 0) {
		LOG_ERR("storage flash not prepared (%d)", ret);
	}
}

static void audio_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* The notes of the flash: reads only; the loop may call the phone */
	(void)notes_init();
	(void)evt_post(EVT_NOTES_READY, 0, 0, 0);
	(void)k_sem_take(&wake, K_SECONDS(BOOT_REST_S));
	atomic_set(&prepare_asked, 1);
	for (;;) {
		if (recorder_busy()) {
			/* The latest take asked: an older one that never ran is over too */
			const k_spinlock_key_t key = k_spin_lock(&lock);
			const uint32_t seq = (uint32_t)atomic_get(&start_seq);
			const struct recorder_take p = asked;

			k_spin_unlock(&lock, key);
			run_take(seq, &p);
			atomic_set(&prepare_asked, 1);
		} else if (atomic_cas(&wipe_asked, 1, 0)) {
			/* The reset: the loop finishes it once told, done or not */
			const int err = notes_wipe();

			(void)evt_post(EVT_NOTES_WIPED, err == 0 ? 1U : 0U, 0, 0);
		} else if (atomic_get(&prepare_asked)) {
			rest();
		} else {
			k_sem_take(&wake, K_FOREVER);
		}
	}
}

/* 3.2 KB for the LC3 encoder (measured, K2), the frames of a block, the store
 * and the flash driver under it */
K_THREAD_DEFINE(audio, 6144, audio_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(2), K_FP_REGS, 0);

uint32_t recorder_start(const struct recorder_take *take)
{
	const k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t seq;

	asked = *take;
	atomic_set(&started_at, (atomic_val_t)k_uptime_get_32());
	seq = (uint32_t)atomic_inc(&start_seq) + 1U;
	k_spin_unlock(&lock, key);
	k_sem_give(&wake);
	return seq;
}

void recorder_keep(uint32_t take)
{
	atomic_set(&keep_seq, (atomic_val_t)take);
}

void recorder_listen(uint32_t take, uint32_t at_ms)
{
	/* The time first: the thread reads it once it sees the take */
	atomic_set(&listen_at, (atomic_val_t)at_ms);
	atomic_set(&listen_seq, (atomic_val_t)take);
}

void recorder_stop(uint32_t take, uint32_t at_ms)
{
	atomic_set(&stop_at, (atomic_val_t)at_ms);
	atomic_set(&stop_seq, (atomic_val_t)take);
}

void recorder_heard(uint32_t at_ms, bool down)
{
	/* From the loop only; the thread reads up to the count */
	const uint32_t i = (uint32_t)atomic_get(&edges_given) % EDGES;

	atomic_set(&edge_at[i], (atomic_val_t)at_ms);
	atomic_set(&edge_down[i], down ? 1 : 0);
	atomic_inc(&edges_given);
}

void recorder_drop(uint32_t take)
{
	atomic_set(&drop_seq, (atomic_val_t)take);
}

bool recorder_busy(void)
{
	return atomic_get(&start_seq) != atomic_get(&done_seq);
}

bool recorder_holds_updates(void)
{
	return recorder_busy() &&
	       k_uptime_get_32() - (uint32_t)atomic_get(&started_at) < TAKE_BOUND_MS;
}

uint32_t recorder_result(uint8_t *end, uint32_t *note_id)
{
	const k_spinlock_key_t key = k_spin_lock(&lock);
	const uint32_t seq = result_seq;

	*end = result_end;
	*note_id = result_id;
	k_spin_unlock(&lock, key);
	return seq;
}

void recorder_prepare(void)
{
	atomic_set(&prepare_asked, 1);
	k_sem_give(&wake);
}

void recorder_wipe(void)
{
	atomic_set(&wipe_asked, 1);
	k_sem_give(&wake);
}

bool recorder_flash_full(void)
{
	return notes_room() < PREPARE_SECTORS;
}

uint32_t recorder_recorded_s(void)
{
	return (uint32_t)atomic_get(&recorded_ms) / 1000U;
}
