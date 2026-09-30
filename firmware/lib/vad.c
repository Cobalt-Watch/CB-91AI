/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Voice activity, see vad.h. Pure C: the host tests build this file as it is.
 */

#include "vad.h"

/* dB to log2 of a mean square, Q12: 4096 / 3.0103 per dB */
#define DB_Q12(db)        ((int32_t)((db) * 1361))
#define FRAMES(ms)        ((ms) / VAD_FRAME_MS)
/* 0 dBFS: the mean square of a full-scale square wave, 32768^2, log2 = 30 */
#define FULL_SCALE_Q12    (30 * 4096)
#define THRESHOLD_Q12     DB_Q12(VAD_THRESHOLD_DB)
#define RISE_Q12          ((DB_Q12(VAD_RISE_DB_PER_S) * (int32_t)VAD_FRAME_MS + 500) / 1000)
#define SPEECH_FLOOR_Q12  (FULL_SCALE_Q12 - DB_Q12(VAD_FLOOR_DBFS))
/* The floor never goes below -80 dBFS: a dead frame (the microphone settling,
 * a glitch) must not make the whole background look like speech */
#define NOISE_MIN_Q12     (FULL_SCALE_Q12 - DB_Q12(80))
#define SETTLE_FRAMES     5U /* the first 50 ms only seed the floor */

int32_t vad_log2_q12(uint64_t x)
{
	int32_t bits = 0;
	uint32_t frac;
	uint32_t bend;

	if (x == 0) {
		return 0;
	}
	while ((x >> bits) > 1U) {
		bits++;
	}
	/* The 12 bits under the leading one, f in [0, 1), then log2(1 + f) as
	 * f + 0.3466 f (1 - f): within 0.01 of a log2 unit, 0.03 dB */
	frac = bits >= 12 ? (uint32_t)(x >> (bits - 12)) & 0xfffU
			  : (uint32_t)(x << (12 - bits)) & 0xfffU;
	bend = (frac * (4096U - frac)) >> 12;
	return bits * 4096 + (int32_t)frac + (int32_t)((bend * 1420U) >> 12);
}

void vad_init(struct vad *v)
{
	const struct vad clear = { 0 };

	*v = clear;
	v->floor_q12 = SPEECH_FLOOR_Q12;
}

void vad_set_gain_db(struct vad *v, int gain_db)
{
	v->floor_q12 = SPEECH_FLOOR_Q12 + DB_Q12(gain_db);
}

void vad_deafen(struct vad *v, bool deaf)
{
	v->deaf = deaf;
}

int32_t vad_level(const int16_t *pcm, size_t n)
{
	uint64_t sum = 0;

	if (n == 0) {
		return 0;
	}
	for (size_t i = 0; i < n; i++) {
		sum += (uint64_t)((int32_t)pcm[i] * (int32_t)pcm[i]);
	}
	return vad_log2_q12(sum / n);
}

static bool heard(const struct vad *v)
{
	return v->speech_frames >= FRAMES(VAD_SPEECH_MIN_MS);
}

enum vad_verdict vad_frame(struct vad *v, const int16_t *pcm, size_t n)
{
	return vad_step(v, vad_level(pcm, n));
}

enum vad_verdict vad_step(struct vad *v, int32_t level)
{
	v->level_q12 = level;
	v->frames++;
	if (v->frames <= SETTLE_FRAMES) {
		/* Seed the floor with the quietest of the first frames */
		if (v->frames == 1U || level < v->noise_q12) {
			v->noise_q12 = level < NOISE_MIN_Q12 ? NOISE_MIN_Q12 : level;
		}
		v->speech = false;
		return VAD_GO_ON;
	}
	/* Loud, and long enough not to be a click: speech, unless deaf */
	if (!v->deaf && level >= v->noise_q12 + THRESHOLD_Q12 && level >= v->floor_q12) {
		v->run++;
	} else {
		v->run = 0;
	}
	v->speech = v->run >= VAD_RUN_FRAMES;
	if (v->speech) {
		/* The run counts from its first frame */
		v->speech_frames += v->run == VAD_RUN_FRAMES ? VAD_RUN_FRAMES : 1U;
		v->last_speech = v->frames;
	}
	/* The floor: down in a few frames, up by 1 dB a second, never below -80 dBFS */
	if (level < v->noise_q12) {
		v->noise_q12 -= (v->noise_q12 - level + 3) / 4;
	} else {
		const int32_t up = level - v->noise_q12;

		v->noise_q12 += up < RISE_Q12 ? up : RISE_Q12;
	}
	if (v->noise_q12 < NOISE_MIN_Q12) {
		v->noise_q12 = NOISE_MIN_Q12;
	}
	if (heard(v)) {
		return v->frames - v->last_speech >= FRAMES(VAD_SILENCE_STOP_MS) ? VAD_STOP_SILENCE
										 : VAD_GO_ON;
	}
	return v->frames >= FRAMES(VAD_FIRST_WORD_MS) ? VAD_STOP_NOTHING : VAD_GO_ON;
}

uint32_t vad_keep_frames(const struct vad *v)
{
	uint32_t keep;

	if (!heard(v)) {
		return v->frames;
	}
	keep = v->last_speech + FRAMES(VAD_TAIL_MS);
	return keep < v->frames ? keep : v->frames;
}
