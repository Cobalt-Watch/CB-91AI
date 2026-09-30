/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the voice activity that ends a take after 3 s without speech
 * (firmware/lib/vad.c, lot E1), on made-up signals: a noise floor, bursts of
 * a voiced tone, a click, a noise that rises. The real voices of K1 go through
 * `test_vad file.wav...` (the same code, on WAV files, not in the suite).
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "vad.h"

#define RATE  16000
#define FRAME 160
#define PI    3.14159265358979

static uint32_t seed = 12345;

/* White noise of about `dbfs` rms */
static void noise(int16_t *out, double dbfs)
{
	const double rms = 32768.0 * pow(10.0, dbfs / 20.0);

	for (int i = 0; i < FRAME; i++) {
		seed = seed * 1664525U + 1013904223U;
		/* Uniform in [-1, 1): rms 1/sqrt(3) */
		out[i] = (int16_t)(((double)(int32_t)seed / 2147483648.0) * rms * 1.7320508);
	}
}

/* A voiced tone, 200 Hz and three harmonics, of about `dbfs` rms, over noise */
static void voice(int16_t *out, double dbfs, double noise_dbfs, uint32_t frame)
{
	const double rms = 32768.0 * pow(10.0, dbfs / 20.0);

	noise(out, noise_dbfs);
	for (int i = 0; i < FRAME; i++) {
		const double t = (double)(frame * FRAME + (uint32_t)i) / RATE;
		double s = 0;

		for (int h = 1; h <= 4; h++) {
			s += sin(2 * PI * 200.0 * h * t) / h;
		}
		out[i] = (int16_t)(out[i] + s * rms * 0.93);
	}
}

static void log2_is_close(void)
{
	CHECK_EQ(vad_log2_q12(1), 0);
	CHECK_EQ(vad_log2_q12(2), 4096);
	CHECK_EQ(vad_log2_q12((uint64_t)1 << 30), 30 * 4096);
	/* log2(3) = 1.58496: within 0.01 */
	CHECK(abs(vad_log2_q12(3) - 6492) < 41);
	CHECK(abs(vad_log2_q12(1000000) - (int32_t)(19.93157 * 4096)) < 41);
}

static void nothing_said(void)
{
	struct vad v;
	int16_t pcm[FRAME];
	enum vad_verdict verdict = VAD_GO_ON;
	uint32_t f = 0;

	vad_init(&v);
	while (verdict == VAD_GO_ON && f < 1000) {
		noise(pcm, -62);
		verdict = vad_frame(&v, pcm, FRAME);
		f++;
	}
	CHECK_EQ(verdict, VAD_STOP_NOTHING);
	CHECK_EQ(f, VAD_FIRST_WORD_MS / VAD_FRAME_MS);
	CHECK_EQ(vad_keep_frames(&v), f);
}

/* Noise, speech from 0.5 s to 2 s, a gap of `gap_ms`, speech again 1.5 s,
 * then noise: the frame where it stops, 0 if not within 12 s */
static uint32_t two_sentences(uint32_t gap_ms, uint32_t *second_end, uint32_t *keep)
{
	struct vad v;
	int16_t pcm[FRAME];
	const uint32_t s1 = 50, e1 = 200, s2 = e1 + gap_ms / 10U, e2 = s2 + 150;

	vad_init(&v);
	*second_end = e2;
	for (uint32_t f = 0; f < 1200; f++) {
		if ((f >= s1 && f < e1) || (f >= s2 && f < e2)) {
			voice(pcm, -33, -60, f);
		} else {
			noise(pcm, -60);
		}
		if (vad_frame(&v, pcm, FRAME) != VAD_GO_ON) {
			*keep = vad_keep_frames(&v);
			return f + 1;
		}
	}
	return 0;
}

static void three_seconds_of_silence_end_it(void)
{
	uint32_t end, stop, keep = 0;

	/* A pause of 2.9 s between two sentences: it goes on */
	stop = two_sentences(2900, &end, &keep);
	CHECK(stop > end);
	/* ... and ends 3 s after the last word, keeping half a second after it */
	CHECK(stop >= end + 290 && stop <= end + 310);
	CHECK(keep >= end && keep <= end + 60);
	/* A pause of 3.1 s: it ends in the pause */
	stop = two_sentences(3100, &end, &keep);
	CHECK(stop > 200 && stop < 200 + 320);
}

static void a_click_is_not_speech(void)
{
	struct vad v;
	int16_t pcm[FRAME];
	enum vad_verdict verdict = VAD_GO_ON;
	uint32_t f = 0;

	vad_init(&v);
	while (verdict == VAD_GO_ON && f < 1000) {
		if (f == 20 || f == 21) {
			voice(pcm, -20, -60, f); /* 20 ms loud: the button */
		} else {
			noise(pcm, -60);
		}
		verdict = vad_frame(&v, pcm, FRAME);
		f++;
	}
	CHECK_EQ(verdict, VAD_STOP_NOTHING);
}

/* The release of ALARM through the case, as heard on 2026-09-24: 150 ms as loud
 * as a voice. Deaf until then, the take with nothing said still ends empty; a
 * word said after it still counts. */
static void the_release_of_the_button(void)
{
	struct vad v;
	int16_t pcm[FRAME];
	enum vad_verdict verdict = VAD_GO_ON;
	uint32_t f = 0;

	vad_init(&v);
	while (verdict == VAD_GO_ON && f < 1000) {
		vad_deafen(&v, f < 70);
		if (f >= 50 && f < 65) {
			voice(pcm, -25, -65, f); /* the release, 150 ms */
		} else {
			noise(pcm, -65);
		}
		verdict = vad_frame(&v, pcm, FRAME);
		f++;
	}
	CHECK_EQ(verdict, VAD_STOP_NOTHING);
	CHECK_EQ(v.speech_frames, 0);
	/* The same, heard: it was a first word */
	vad_init(&v);
	verdict = VAD_GO_ON;
	for (f = 0; verdict == VAD_GO_ON && f < 1000; f++) {
		if (f >= 50 && f < 65) {
			voice(pcm, -25, -65, f);
		} else {
			noise(pcm, -65);
		}
		verdict = vad_frame(&v, pcm, FRAME);
	}
	CHECK_EQ(verdict, VAD_STOP_SILENCE);
	/* Deaf through the release, then a word: it counts, and the floor
	 * followed meanwhile */
	vad_init(&v);
	verdict = VAD_GO_ON;
	for (f = 0; verdict == VAD_GO_ON && f < 1000; f++) {
		vad_deafen(&v, f < 70);
		if ((f >= 50 && f < 65) || (f >= 120 && f < 160)) {
			voice(pcm, -25, -65, f);
		} else {
			noise(pcm, -65);
		}
		verdict = vad_frame(&v, pcm, FRAME);
	}
	CHECK_EQ(verdict, VAD_STOP_SILENCE);
	CHECK_EQ(v.last_speech, 160);
	CHECK_EQ(f, 160 + 300);
}

static void clicks_after_the_last_word(void)
{
	struct vad v;
	int16_t pcm[FRAME];
	uint32_t stop = 0;

	/* Speech to 2 s, then steps or keys: 20 ms loud every 300 ms. They do
	 * not hold the take: it ends 3 s after the last word. */
	vad_init(&v);
	for (uint32_t f = 0; f < 1200 && stop == 0; f++) {
		if (f >= 50 && f < 200) {
			voice(pcm, -33, -60, f);
		} else if (f > 200 && f % 30 < 2) {
			voice(pcm, -25, -60, f);
		} else {
			noise(pcm, -60);
		}
		if (vad_frame(&v, pcm, FRAME) != VAD_GO_ON) {
			stop = f + 1;
		}
	}
	CHECK(stop >= 200 + 290 && stop <= 200 + 310);
}

static void a_noise_that_rises(void)
{
	struct vad v;
	int16_t pcm[FRAME];
	uint32_t stop = 0;

	vad_init(&v);
	/* Speech to 2 s, then the noise steps up 10 dB, just over the threshold:
	 * it flickers around it, rarely four loud frames in a row, while the
	 * floor catches up at 1 dB a second. The take ends 3 s after the last
	 * word, a second later at most. */
	for (uint32_t f = 0; f < 1500 && stop == 0; f++) {
		if (f >= 50 && f < 200) {
			voice(pcm, -33, -60, f);
		} else {
			noise(pcm, f < 200 ? -60 : -50);
		}
		if (vad_frame(&v, pcm, FRAME) != VAD_GO_ON) {
			stop = f + 1;
		}
	}
	CHECK(stop >= 200 + 290);
	CHECK(stop < 200 + 300 + 100);
}

static void dead_frames_at_the_start(void)
{
	struct vad v;
	int16_t pcm[FRAME];
	uint32_t f;

	/* The microphone settling: zeros, then a quiet room. No speech seen. */
	vad_init(&v);
	memset(pcm, 0, sizeof(pcm));
	for (f = 0; f < 3; f++) {
		(void)vad_frame(&v, pcm, FRAME);
	}
	for (; f < 300; f++) {
		noise(pcm, -65);
		(void)vad_frame(&v, pcm, FRAME);
		CHECK(!v.speech);
	}
	CHECK_EQ(v.speech_frames, 0);
}

/* The recorder measures a frame at once and judges it later (vad_level(),
 * vad_step()): the same verdicts, frame for frame, as vad_frame() */
static void measured_then_judged(void)
{
	struct vad whole, split;
	int16_t pcm[FRAME];
	uint32_t same = 0;

	vad_init(&whole);
	vad_init(&split);
	for (uint32_t f = 0; f < 700; f++) {
		if (f >= 100 && f < 250) {
			voice(pcm, -30, -60, f);
		} else {
			noise(pcm, -60);
		}
		same += vad_frame(&whole, pcm, FRAME) == vad_step(&split, vad_level(pcm, FRAME));
		same += whole.speech == split.speech && whole.noise_q12 == split.noise_q12;
	}
	CHECK_EQ(same, 1400);
	CHECK_EQ(vad_keep_frames(&whole), vad_keep_frames(&split));
	CHECK(whole.speech_frames > 100);
}

/* The same room and voice through a microphone 6 dB lower, the gain known:
 * judged the same; the gain unknown, a faint voice falls under the floor */
static uint32_t soft_voice(int voice_dbfs, int noise_dbfs, int gain_db)
{
	struct vad v;
	int16_t pcm[FRAME];

	vad_init(&v);
	vad_set_gain_db(&v, gain_db);
	for (uint32_t f = 0; f < 300; f++) {
		if (f >= 50 && f < 200) {
			voice(pcm, voice_dbfs, noise_dbfs, f);
		} else {
			noise(pcm, noise_dbfs);
		}
		(void)vad_frame(&v, pcm, FRAME);
	}
	return v.speech_frames;
}

static void the_gain_moves_the_floor(void)
{
	/* A faint voice at -50 dBFS in a room at -66, over the floor of -55 */
	const uint32_t at_0db = soft_voice(-50, -66, 0);

	CHECK(at_0db > 100);
	/* All 6 dB lower: the same verdicts once the detector knows the gain */
	CHECK(abs((int)soft_voice(-56, -72, -6) - (int)at_0db) <= 4);
	/* Not told: the voice is under the floor of -55 */
	CHECK_EQ(soft_voice(-56, -72, 0), 0);
	/* The room alone stays silent at the lower floor */
	CHECK_EQ(soft_voice(-72, -72, -6), 0);
}

/* ---- The voices of K1, off the suite: test_vad file.wav... ------------------- */

static int wav_verdicts(int argc, char **argv)
{
	/* test_vad --gain -6 file.wav...: the files attenuated by that much, the
	 * detector told the gain, as the watch with its microphone lower */
	int gain_db = 0;
	int from = 1;

	if (argc > 2 && strcmp(argv[1], "--gain") == 0) {
		gain_db = atoi(argv[2]);
		from = 3;
	}
	for (int a = from; a < argc; a++) {
		FILE *fp = fopen(argv[a], "rb");
		unsigned char head[44];
		struct vad v;
		int16_t pcm[FRAME];
		uint32_t f = 0, stop = 0, keep = 0;
		enum vad_verdict kind = VAD_GO_ON;

		if (fp == NULL || fread(head, 1, sizeof(head), fp) != sizeof(head)) {
			printf("%s: cannot read\n", argv[a]);
			if (fp != NULL) {
				fclose(fp);
			}
			continue;
		}
		if ((head[24] | head[25] << 8 | head[26] << 16) != RATE || head[22] != 1) {
			printf("%s: not 16 kHz mono\n", argv[a]);
		}
		vad_init(&v);
		vad_set_gain_db(&v, gain_db);
		while (fread(pcm, sizeof(int16_t), FRAME, fp) == FRAME) {
			enum vad_verdict verdict;

			if (gain_db != 0) {
				/* 10^(gain/20) in Q15, applied to each sample */
				const int32_t g = (int32_t)(32768.0 * pow(10.0, gain_db / 20.0));

				for (int i = 0; i < FRAME; i++) {
					pcm[i] = (int16_t)(((int32_t)pcm[i] * g) >> 15);
				}
			}
			verdict = vad_frame(&v, pcm, FRAME);

			f++;
			if (verdict != VAD_GO_ON && stop == 0) {
				stop = f;
				kind = verdict;
				keep = vad_keep_frames(&v);
			}
		}
		fclose(fp);
		printf("%s: %u ms, speech %u ms, last word to %u ms, %s at %u ms, keep %u ms\n",
		       argv[a], f * 10U, v.speech_frames * 10U, v.last_speech * 10U,
		       stop == 0 ? "no stop" : kind == VAD_STOP_SILENCE ? "silence stop" : "no speech",
		       stop * 10U, (stop == 0 ? vad_keep_frames(&v) : keep) * 10U);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		return wav_verdicts(argc, argv);
	}
	RUN(log2_is_close);
	RUN(the_gain_moves_the_floor);
	RUN(nothing_said);
	RUN(three_seconds_of_silence_end_it);
	RUN(a_click_is_not_speech);
	RUN(the_release_of_the_button);
	RUN(clicks_after_the_last_word);
	RUN(a_noise_that_rises);
	RUN(dead_frames_at_the_start);
	RUN(measured_then_judged);
	return harness_report("vad");
}
