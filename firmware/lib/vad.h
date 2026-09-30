/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Voice activity, to end a take when the wearer stops talking (lot E1, asked
 * for on 2026-09-24: three seconds without speech). Pure logic on the PCM frames of
 * the microphone, before the encoder: no Zephyr, no floating point, tested on
 * the PC (firmware/tests/host/test_vad.c) and tuned on the ten sentences of K1.
 *
 * Each frame gets a level, the log2 of its mean square in Q12. The noise
 * floor follows the level down at once and up slowly, 1 dB per second: a
 * word is too short to lift it, a fan that starts is not. A frame at least
 * 9 dB over the floor, and over an absolute floor, is loud; four loud frames
 * in a row are speech: a click, a step, a key of a keyboard last 10 to 20 ms,
 * a syllable more than 50. Once some speech has been heard (100 ms of it),
 * the take ends after 3 s without any; before any speech, after 5 s. What
 * comes after the last word, but for half a second, need not be kept.
 */

#ifndef CB91AI_VAD_H
#define CB91AI_VAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VAD_FRAME_MS        10U
#define VAD_THRESHOLD_DB    9U    /* speech: this far over the noise floor */
#define VAD_RISE_DB_PER_S   1U    /* how fast the floor may rise */
#define VAD_FLOOR_DBFS      55U   /* speech is louder than -55 dBFS whatever the floor:
                                   * a quiet room is about -60 dBFS after the 50 Hz
                                   * high-pass, a voice at 30 cm about -33 (K1) */
#define VAD_RUN_FRAMES      4U    /* loud frames in a row that make speech */
#define VAD_SPEECH_MIN_MS   100U  /* heard at least this much before a take counts */
#define VAD_SILENCE_STOP_MS 3000U /* then this long without speech ends it */
#define VAD_FIRST_WORD_MS   5000U /* nothing heard this long from the start: it ends */
#define VAD_TAIL_MS         500U  /* kept after the last word */

enum vad_verdict {
	VAD_GO_ON,
	VAD_STOP_SILENCE, /* speech, then VAD_SILENCE_STOP_MS without */
	VAD_STOP_NOTHING, /* no speech in VAD_FIRST_WORD_MS */
};

struct vad {
	uint32_t frames;       /* frames seen */
	int32_t noise_q12;     /* the noise floor, log2 of a mean square, Q12 */
	int32_t level_q12;     /* the level of the last frame */
	uint32_t speech_frames;
	uint32_t last_speech;  /* frame index after the last speech frame, 0: none */
	uint32_t run;          /* loud frames in a row */
	bool speech;           /* the last frame was speech */
	bool deaf;             /* no frame is speech meanwhile (vad_deafen()) */
	int32_t floor_q12;     /* the absolute floor of speech (vad_set_gain_db()) */
};

void vad_init(struct vad *v);

/* The microphone's gain around its default, in dB: the absolute floor
 * (VAD_FLOOR_DBFS, set at 0 dB on the voices of K1) moves with it, so that a
 * voice is judged the same at any gain. The watch lowered its gain by 6 dB in
 * 0.2.1+31 (a voice clipped when spoken close to the watch). */
void vad_set_gain_db(struct vad *v, int gain_db);

/* While deaf, no frame is speech; the floor still follows. The recorder deafens
 * the detector until ALARM is released after the latch: through the case, the
 * click of the release is as loud as a voice, 100 to 150 ms of it (seen at the
 * wrist on 2026-09-24), and would count as a first word. */
void vad_deafen(struct vad *v, bool deaf);

/* One frame of 16-bit PCM, VAD_FRAME_MS long (160 samples at 16 kHz) */
enum vad_verdict vad_frame(struct vad *v, const int16_t *pcm, size_t n);

/* The same in two steps: the level of a frame (log2 of its mean square,
 * Q12), then the frame judged from its level. The recorder measures a frame
 * as it comes and judges it 320 ms later, once it knows whether a button
 * was heard then (watch/src/clicks.h). */
int32_t vad_level(const int16_t *pcm, size_t n);
enum vad_verdict vad_step(struct vad *v, int32_t level);

/* The frames worth keeping from the start: up to the last word and its
 * tail, all of them while no silence has ended the take */
uint32_t vad_keep_frames(const struct vad *v);

/* log2(x) in Q12, x > 0: for the levels, and for the tests */
int32_t vad_log2_q12(uint64_t x);

#endif /* CB91AI_VAD_H */
