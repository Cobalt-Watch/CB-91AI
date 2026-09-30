/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Codec bench (lot K2): what a speech codec costs on this board, measured
 * rather than guessed. A couple of seconds of 16 kHz speech are held in RAM, either a
 * synthetic signal that every board reproduces bit for bit, or a capture from
 * the watch microphone, and encoded frame by frame. The DWT cycle counter gives
 * the CPU cycles of each frame; the encoder runs in a thread of its own, so
 * that the high-water mark of that stack is the scratch memory of the codec
 * and nothing else (results are printed from the shell thread).
 *
 * The counter keeps running during the interrupts and the higher priority
 * threads that preempt the encoder (advertising, the main loop once a second):
 * minimum and median are the cost of the codec, mean and maximum include that
 * noise. Interrupts stay enabled on purpose: the BLE controller asserts when
 * its interrupts are held off for the length of a frame.
 *
 * Code size does not come from here but from the link map of the build.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <cmsis_core.h>
#include <soc.h>
#include <SEGGER_RTT.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_rtt.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "codec_bench.h"
#include "mic.h"

#if defined(CONFIG_LIBLC3)
#include <lc3.h>
#endif
#if defined(CONFIG_CB91AI_OPUS)
#include <opus.h>
#endif

#define RATE_HZ        MIC_SAMPLE_RATE_HZ
#define PCM_SAMPLES    (CONFIG_CB91AI_CODEC_BENCH_MS * (RATE_HZ / 1000))
/* Shortest frame of the bench: LC3 at 7.5 ms, 120 samples */
#define MAX_FRAMES     (PCM_SAMPLES / 120)
#define CPU_HZ         64000000ULL
/* Below the main loop and the Bluetooth host, above the shell and the logs */
#define BENCH_PRIORITY K_PRIO_PREEMPT(12)
#define MAX_PACKET     400

enum bench_codec {
	BENCH_ADPCM,
	BENCH_LC3,
	BENCH_OPUS,
};

struct bench_job {
	enum bench_codec codec;
	int bitrate;
	int frame_us;
	int complexity; /* Opus only */
	int max_band;   /* Opus only: OPUS_BANDWIDTH_*, 0 to let the encoder choose */
	int band;       /* Opus only: bandwidth of the last frame */
	bool roundtrip; /* decode each frame again and hand it to the shell thread */
	int err;
	uint32_t frames;
	uint32_t bytes;       /* encoded output */
	uint32_t crc;         /* of the encoded output: same signal, same build, same value */
	uint32_t state_bytes; /* encoder state, static */
	uint32_t cycles[MAX_FRAMES];
};

static int16_t pcm[PCM_SAMPLES];
static size_t pcm_count;
static const char *pcm_source = "empty";

static struct bench_job job;
static struct k_thread bench_thread;
static K_THREAD_STACK_DEFINE(bench_stack, CONFIG_CB91AI_CODEC_BENCH_STACK_SIZE);
static K_MUTEX_DEFINE(bench_lock);

/* --- Test signal ---------------------------------------------------------- */

static int16_t sine_table[256];

static void sine_table_init(void)
{
	/* Bhaskara's approximation: good to 0.2 %, plenty for a test signal, and
	 * no libm in the image because of the bench.
	 */
	for (int i = 0; i < 128; i++) {
		int64_t p = i * 256; /* 0 to 32768 for 0 to pi */
		int64_t a = p * (32768 - p);
		int64_t v = (16 * a * 32767) / (5LL * 32768 * 32768 - 4 * a);

		sine_table[i] = (int16_t)v;
		sine_table[i + 128] = (int16_t)-v;
	}
}

static uint32_t xorshift32(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

/*
 * Speech-like signal, integer only and the same on every board: four
 * syllables a second, each a closure, sometimes a burst of hissing noise, then
 * a vowel made of the harmonics of a moving pitch weighted by three formants;
 * one syllable in eight is a pause, and a noise floor runs under it all, as
 * from a real microphone (digital silence would let the codecs off lightly).
 */
static void fill_synthetic(void)
{
	static const uint16_t vowels[][3] = {
		{700, 1200, 2600}, {300, 2300, 3000}, {320, 800, 2500},
		{500, 1900, 2600}, {500, 900, 2500},
	};
	static const uint16_t formant_gain[3] = {256, 154, 77};
	const int32_t bw2 = 90 * 90;
	uint16_t weight[40];
	uint32_t harmonics = 0;
	uint32_t phase = 0, step = 0;
	uint32_t rng = 0x2545F491;
	int32_t noise_prev = 0;

	if (sine_table[64] == 0) {
		sine_table_init();
	}
	for (size_t n = 0; n < PCM_SAMPLES; n++) {
		uint32_t ms = n / (RATE_HZ / 1000);
		uint32_t syllable = ms / 250, at = ms % 250;
		bool hiss = (syllable % 3) == 2;
		uint32_t voiced_from = hiss ? 90 : 30;
		int32_t noise = (int16_t)(xorshift32(&rng) >> 16);
		int32_t sample = noise >> 11; /* noise floor, +/-16 */

		if (n % 160 == 0) {
			/* Pitch: 120 Hz +/-30 Hz on a 1.3 s triangle, in 1/256 Hz */
			uint32_t tri = n % 20800;
			uint32_t f0_q8;
			const uint16_t *formants = vowels[syllable % ARRAY_SIZE(vowels)];

			tri = tri < 10400 ? tri : 20800 - tri;
			f0_q8 = (90 * 256) + (uint32_t)((60ULL * 256 * tri) / 10400);
			step = (uint32_t)(((uint64_t)f0_q8 << 24) / RATE_HZ);
			harmonics = MIN(ARRAY_SIZE(weight), (3800U * 256) / f0_q8);
			for (uint32_t k = 1; k <= harmonics; k++) {
				int32_t f = (int32_t)((k * f0_q8) >> 8);
				int32_t w = 0;

				for (int i = 0; i < 3; i++) {
					int32_t d = f - formants[i];

					w += (formant_gain[i] * bw2) / (d * d + bw2);
				}
				/* Tilt of the glottal source */
				weight[k - 1] = (uint16_t)((w * 4) / (int32_t)(k + 3));
			}
		}
		if ((syllable % 8) != 5) {
			if (hiss && at >= 30 && at < 90) {
				/* First difference: a hiss lives in the high frequencies */
				sample += (noise - noise_prev) >> 4;
			} else if (at >= voiced_from && at < 230) {
				int32_t env = 256;
				int32_t acc = 0;

				if (at < voiced_from + 20) {
					env = (int32_t)(at - voiced_from) * 256 / 20;
				} else if (at >= 190) {
					env = (int32_t)(230 - at) * 256 / 40;
				}
				for (uint32_t k = 1; k <= harmonics; k++) {
					acc += (sine_table[(k * phase) >> 24] * weight[k - 1]) >> 8;
				}
				sample += (acc * env) >> 9;
			}
		}
		noise_prev = noise;
		phase += step;
		pcm[n] = (int16_t)CLAMP(sample, INT16_MIN, INT16_MAX);
	}
	pcm_count = PCM_SAMPLES;
	pcm_source = "synthetic";
}

static uint32_t isqrt64(uint64_t v)
{
	uint64_t r = 0, bit = 1ULL << 62;

	while (bit > v) {
		bit >>= 2;
	}
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return (uint32_t)r;
}

static void print_level(const struct shell *sh)
{
	int64_t sum = 0;
	uint64_t sum_sq = 0;
	int32_t peak = 0, mean;

	if (pcm_count == 0) {
		shell_print(sh, "bench buffer empty: cb91ai codec signal | record");
		return;
	}
	for (size_t i = 0; i < pcm_count; i++) {
		sum += pcm[i];
	}
	mean = (int32_t)(sum / (int64_t)pcm_count);
	for (size_t i = 0; i < pcm_count; i++) {
		int32_t s = pcm[i] - mean;

		sum_sq += (uint64_t)((int64_t)s * s);
		peak = MAX(peak, abs(s));
	}
	shell_print(sh, "bench buffer: %s, %u samples (%u ms), ac rms %u, peak %d, dc %d",
		    pcm_source, (unsigned int)pcm_count,
		    (unsigned int)(pcm_count / (RATE_HZ / 1000)),
		    isqrt64(sum_sq / pcm_count), peak, mean);
}

/* --- Encoders ------------------------------------------------------------- */

static inline uint32_t cycles_now(void)
{
	return DWT->CYCCNT;
}

static void cycles_enable(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/*
 * Outside the timed section. The CRC makes the output of an encoder something
 * the program depends on: without it the compiler deleted the whole ADPCM loop,
 * whose result nothing read, and the bench timed an empty frame.
 */
static void frame_done(struct bench_job *j, uint32_t cycles, const uint8_t *out, size_t len)
{
	j->cycles[j->frames++] = cycles;
	j->bytes += len;
	j->crc = crc32_ieee_update(j->crc, out, len);
}

/* IMA ADPCM, 4 bits a sample: the codec of the V1 prototype, as a reference */
struct adpcm_state {
	int32_t predicted; /* also what a decoder rebuilds: its output sample */
	int index;
};

static inline uint8_t adpcm_encode_sample(struct adpcm_state *state, int16_t sample)
{
	static const int16_t steps[89] = {
		7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
		50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
		253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
		1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
		3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
		11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
		32767,
	};
	static const int8_t index_moves[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
	int32_t step = steps[state->index];
	int32_t diff = sample - state->predicted;
	int32_t delta = step >> 3;
	uint8_t code = 0;

	if (diff < 0) {
		code = 8;
		diff = -diff;
	}
	if (diff >= step) {
		code |= 4;
		diff -= step;
		delta += step;
	}
	if (diff >= step >> 1) {
		code |= 2;
		diff -= step >> 1;
		delta += step >> 1;
	}
	if (diff >= step >> 2) {
		code |= 1;
		delta += step >> 2;
	}
	state->predicted = CLAMP(state->predicted + ((code & 8) ? -delta : delta), INT16_MIN,
				 INT16_MAX);
	state->index = CLAMP(state->index + index_moves[code & 7], 0, 88);
	return code;
}

static int run_adpcm(struct bench_job *j)
{
	const size_t frame = (size_t)j->frame_us * RATE_HZ / 1000000;
	uint8_t out[MAX_PACKET];
	struct adpcm_state state = {0};

	if (frame == 0 || frame / 2 > sizeof(out)) {
		return -EINVAL;
	}
	for (size_t at = 0; at + frame <= pcm_count && j->frames < MAX_FRAMES; at += frame) {
		uint32_t start = cycles_now();

		for (size_t i = 0; i < frame; i++) {
			uint8_t code = adpcm_encode_sample(&state, pcm[at + i]);

			if (i & 1) {
				out[i / 2] |= (uint8_t)(code << 4);
			} else {
				out[i / 2] = code;
			}
		}
		frame_done(j, cycles_now() - start, out, frame / 2);
	}
	j->state_bytes = sizeof(state);
	return 0;
}

/*
 * Round trip (first step of K1 and K3). To judge a codec by ear and by
 * transcription, its decoded output has to reach the PC: `codec roundtrip`
 * encodes and decodes the speech buffer frame by frame and prints the result,
 * `codec dump` prints the buffer itself, both as base64 lines between PCM-BEGIN
 * and PCM-END (firmware/tools/pcm_from_log.py turns a log into WAV files). The
 * buffer is never overwritten, so one capture goes through every codec and bit
 * rate. The codec runs in the bench thread, as in the measurements; the shell
 * thread prints, one frame at a time, because only the thread of a shell may
 * write to it while one of its commands runs.
 */
#define RT_FRAME_MAX 320 /* samples: 20 ms */

static int16_t rt_frame[RT_FRAME_MAX];
static size_t rt_frame_len;
static K_SEM_DEFINE(rt_ready, 0, 1);
static K_SEM_DEFINE(rt_taken, 0, 1);

/* Bench thread: hand rt_frame to the shell thread and wait until it is printed */
static void rt_emit(size_t samples)
{
	rt_frame_len = samples;
	k_sem_give(&rt_ready);
	k_sem_take(&rt_taken, K_FOREVER);
}

static int roundtrip_adpcm(struct bench_job *j)
{
	const size_t frame = (size_t)j->frame_us * RATE_HZ / 1000000;
	struct adpcm_state state = {0};

	if (frame == 0 || frame > RT_FRAME_MAX) {
		return -EINVAL;
	}
	for (size_t at = 0; at + frame <= pcm_count; at += frame) {
		for (size_t i = 0; i < frame; i++) {
			adpcm_encode_sample(&state, pcm[at + i]);
			rt_frame[i] = (int16_t)state.predicted;
		}
		j->frames++;
		rt_emit(frame);
	}
	return 0;
}

#if defined(CONFIG_LIBLC3)
static lc3_encoder_mem_16k_t lc3_state;

static int run_lc3(struct bench_job *j)
{
	uint8_t out[MAX_PACKET];
	int samples = lc3_frame_samples(j->frame_us, RATE_HZ);
	int bytes = lc3_frame_bytes(j->frame_us, j->bitrate);
	lc3_encoder_t encoder;

	if (samples <= 0 || bytes <= 0 || bytes > (int)sizeof(out)) {
		return -EINVAL;
	}
	encoder = lc3_setup_encoder(j->frame_us, RATE_HZ, 0, &lc3_state);
	if (encoder == NULL) {
		return -EINVAL;
	}
	j->state_bytes = lc3_encoder_size(j->frame_us, RATE_HZ);
	for (size_t at = 0; at + samples <= pcm_count && j->frames < MAX_FRAMES; at += samples) {
		uint32_t start = cycles_now();
		int ret = lc3_encode(encoder, LC3_PCM_FORMAT_S16, &pcm[at], 1, bytes, out);
		uint32_t cycles = cycles_now() - start;

		if (ret < 0) {
			return -EIO;
		}
		frame_done(j, cycles, out, bytes);
	}
	return 0;
}

static lc3_decoder_mem_16k_t lc3_decoder_state;

static int roundtrip_lc3(struct bench_job *j)
{
	uint8_t packet[MAX_PACKET];
	int samples = lc3_frame_samples(j->frame_us, RATE_HZ);
	int bytes = lc3_frame_bytes(j->frame_us, j->bitrate);
	lc3_encoder_t encoder;
	lc3_decoder_t decoder;

	if (samples <= 0 || samples > RT_FRAME_MAX || bytes <= 0 || bytes > (int)sizeof(packet)) {
		return -EINVAL;
	}
	encoder = lc3_setup_encoder(j->frame_us, RATE_HZ, 0, &lc3_state);
	decoder = lc3_setup_decoder(j->frame_us, RATE_HZ, 0, &lc3_decoder_state);
	if (encoder == NULL || decoder == NULL) {
		return -EINVAL;
	}
	for (size_t at = 0; at + samples <= pcm_count; at += samples) {
		if (lc3_encode(encoder, LC3_PCM_FORMAT_S16, &pcm[at], 1, bytes, packet) < 0 ||
		    lc3_decode(decoder, packet, bytes, LC3_PCM_FORMAT_S16, rt_frame, 1) < 0) {
			return -EIO;
		}
		j->frames++;
		j->bytes += bytes;
		rt_emit(samples);
	}
	return 0;
}
#endif /* CONFIG_LIBLC3 */

#if defined(CONFIG_CB91AI_OPUS)
static uint8_t opus_state[CONFIG_CB91AI_CODEC_BENCH_OPUS_STATE_SIZE] __aligned(8);

static int run_opus(struct bench_job *j)
{
	uint8_t out[MAX_PACKET];
	OpusEncoder *encoder = (OpusEncoder *)opus_state;
	int samples = (int)((int64_t)j->frame_us * RATE_HZ / 1000000);

	j->state_bytes = opus_encoder_get_size(1);
	if (j->state_bytes > sizeof(opus_state)) {
		return -ENOMEM;
	}
	if (opus_encoder_init(encoder, RATE_HZ, 1, OPUS_APPLICATION_VOIP) != OPUS_OK ||
	    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(j->bitrate)) != OPUS_OK ||
	    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(j->complexity)) != OPUS_OK ||
	    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) != OPUS_OK ||
	    (j->max_band &&
	     opus_encoder_ctl(encoder, OPUS_SET_MAX_BANDWIDTH(j->max_band)) != OPUS_OK)) {
		return -EINVAL;
	}
	for (size_t at = 0; at + samples <= pcm_count && j->frames < MAX_FRAMES; at += samples) {
		uint32_t start = cycles_now();
		int ret = opus_encode(encoder, &pcm[at], samples, out, sizeof(out));
		uint32_t cycles = cycles_now() - start;

		if (ret < 0) {
			return -EIO;
		}
		frame_done(j, cycles, out, ret);
	}
	/* The encoder picks the audio bandwidth from the bit rate: say which */
	opus_encoder_ctl(encoder, OPUS_GET_BANDWIDTH(&j->band));
	return 0;
}

static const char *opus_band_name(int band)
{
	switch (band) {
	case OPUS_BANDWIDTH_NARROWBAND:
		return "nb";
	case OPUS_BANDWIDTH_MEDIUMBAND:
		return "mb";
	case OPUS_BANDWIDTH_WIDEBAND:
		return "wb";
	default:
		return "?";
	}
}

static int opus_band_value(const char *name)
{
	if (strcmp(name, "nb") == 0) {
		return OPUS_BANDWIDTH_NARROWBAND;
	}
	if (strcmp(name, "mb") == 0) {
		return OPUS_BANDWIDTH_MEDIUMBAND;
	}
	return strcmp(name, "wb") == 0 ? OPUS_BANDWIDTH_WIDEBAND : 0;
}
#endif /* CONFIG_CB91AI_OPUS */

static void bench_entry(void *p1, void *p2, void *p3)
{
	struct bench_job *j = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	if (j->roundtrip) {
		j->err = -ENOTSUP;
		if (j->codec == BENCH_ADPCM) {
			j->err = roundtrip_adpcm(j);
		}
#if defined(CONFIG_LIBLC3)
		if (j->codec == BENCH_LC3) {
			j->err = roundtrip_lc3(j);
		}
#endif
		/* An empty frame ends the stream, whatever happened */
		rt_frame_len = 0;
		k_sem_give(&rt_ready);
		return;
	}
	switch (j->codec) {
	case BENCH_ADPCM:
		j->err = run_adpcm(j);
		break;
#if defined(CONFIG_LIBLC3)
	case BENCH_LC3:
		j->err = run_lc3(j);
		break;
#endif
#if defined(CONFIG_CB91AI_OPUS)
	case BENCH_OPUS:
		j->err = run_opus(j);
		break;
#endif
	default:
		j->err = -ENOTSUP;
		break;
	}
}

/* --- Report --------------------------------------------------------------- */

static int compare_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return (x > y) - (x < y);
}

static void print_header(const struct shell *sh)
{
	shell_print(sh, "codec  bit/s frame_us cpx band frames   cyc_min   cyc_med  cyc_mean   cyc_max "
			"cpu_med%% cpu_mean%% out_bit/s state_B stack_B      crc");
}

static const char *band_name(const struct bench_job *j)
{
#if defined(CONFIG_CB91AI_OPUS)
	if (j->codec == BENCH_OPUS) {
		return opus_band_name(j->band);
	}
#endif
	return "-";
}

static int bench_run(const struct shell *sh, enum bench_codec codec, int bitrate, int frame_us,
		     int complexity, int max_band)
{
	static const char *const names[] = {"adpcm", "lc3", "opus"};
	uint64_t sum = 0;
	uint32_t median, mean, cpu_median, cpu_mean, out_rate;
	size_t unused = 0;

	if (pcm_count == 0) {
		fill_synthetic();
	}
	if (k_mutex_lock(&bench_lock, K_NO_WAIT)) {
		shell_print(sh, "bench busy");
		return -EBUSY;
	}
	memset(&job, 0, sizeof(job));
	job.codec = codec;
	job.bitrate = bitrate;
	job.frame_us = frame_us;
	job.complexity = complexity;
	job.max_band = max_band;
	cycles_enable();
	k_thread_create(&bench_thread, bench_stack, K_THREAD_STACK_SIZEOF(bench_stack), bench_entry,
			&job, NULL, NULL, BENCH_PRIORITY, IS_ENABLED(CONFIG_FPU_SHARING) ? K_FP_REGS : 0,
			K_NO_WAIT);
	k_thread_join(&bench_thread, K_FOREVER);
	k_thread_stack_space_get(&bench_thread, &unused);

	if (job.err || job.frames == 0) {
		shell_print(sh, "%s %d bit/s: failed (%d), %u frames, encoder state %u B", names[codec],
			    bitrate, job.err, job.frames, job.state_bytes);
		k_mutex_unlock(&bench_lock);
		return job.err ? job.err : -ENODATA;
	}
	for (uint32_t i = 0; i < job.frames; i++) {
		sum += job.cycles[i];
	}
	qsort(job.cycles, job.frames, sizeof(job.cycles[0]), compare_u32);
	median = job.cycles[job.frames / 2];
	mean = (uint32_t)(sum / job.frames);
	/* Load in hundredths of a percent of a 64 MHz core */
	cpu_median = (uint32_t)((uint64_t)median * 10000 * 1000000 / (CPU_HZ * (uint64_t)frame_us));
	cpu_mean = (uint32_t)((uint64_t)mean * 10000 * 1000000 / (CPU_HZ * (uint64_t)frame_us));
	out_rate = (uint32_t)((uint64_t)job.bytes * 8 * 1000000 / ((uint64_t)job.frames * frame_us));
	shell_print(sh,
		    "%-5s %6d %8d %3d %4s %6u %9u %9u %9u %9u %5u.%02u %6u.%02u %9u %7u %7u %08x",
		    names[codec], bitrate, frame_us, complexity, band_name(&job), job.frames,
		    job.cycles[0], median, mean, job.cycles[job.frames - 1], cpu_median / 100,
		    cpu_median % 100, cpu_mean / 100, cpu_mean % 100, out_rate, job.state_bytes,
		    (unsigned int)(bench_thread.stack_info.size - unused), job.crc);
	k_mutex_unlock(&bench_lock);
	return 0;
}

static void print_context(const struct shell *sh)
{
	print_level(sh);
	shell_print(sh, "core 64 MHz, flash cache %s, fpu %s, bench stack %u B, buffer %u ms",
		    (NRF_NVMC->ICACHECNF & NVMC_ICACHECNF_CACHEEN_Msk) ? "on" : "off",
		    IS_ENABLED(CONFIG_FPU) ? "on" : "off",
		    (unsigned int)K_THREAD_STACK_SIZEOF(bench_stack), CONFIG_CB91AI_CODEC_BENCH_MS);
}

/* --- Speech out to the PC --------------------------------------------------- */

#define DUMP_LINE_BYTES 120
/* Half of the RTT up buffer. The RTT shell backend gives up after a few
 * milliseconds of a full buffer and drops what the probe has not read: pace
 * the dump on the fill level instead. PCM-END carries a CRC to be sure.
 */
#define RTT_HIGH_WATER  (CONFIG_SEGGER_RTT_BUFFER_SIZE_UP / 2)

struct pcm_dump {
	const struct shell *sh;
	bool paced;
	uint8_t raw[DUMP_LINE_BYTES];
	size_t fill;
	uint32_t samples;
	uint32_t crc;
};

static void dump_begin(struct pcm_dump *dump, const struct shell *sh, const char *name,
		       size_t samples, int delay)
{
	memset(dump, 0, sizeof(*dump));
	dump->sh = sh;
#if defined(CONFIG_SHELL_BACKEND_RTT)
	dump->paced = sh == shell_backend_rtt_get_ptr();
#endif
	shell_print(sh, "PCM-BEGIN %s %d %u %d", name, RATE_HZ, (unsigned int)samples, delay);
}

static void dump_flush(struct pcm_dump *dump)
{
	/* Static: the shell stacks are small, and bench_lock is held */
	static char text[4 * ((DUMP_LINE_BYTES + 2) / 3) + 1];
	size_t len;

	if (dump->fill == 0) {
		return;
	}
	for (int i = 0; dump->paced && i < 2000 &&
			SEGGER_RTT_GetBytesInBuffer(CONFIG_SHELL_BACKEND_RTT_BUFFER) > RTT_HIGH_WATER;
	     i++) {
		k_msleep(1);
	}
	if (base64_encode(text, sizeof(text), &len, dump->raw, dump->fill) == 0) {
		shell_print(dump->sh, "~%s", text);
	}
	dump->fill = 0;
}

static void dump_write(struct pcm_dump *dump, const int16_t *samples, size_t count)
{
	const uint8_t *bytes = (const uint8_t *)samples;
	size_t len = count * sizeof(int16_t);

	dump->samples += count;
	dump->crc = crc32_ieee_update(dump->crc, bytes, len);
	while (len) {
		size_t n = MIN(len, sizeof(dump->raw) - dump->fill);

		memcpy(&dump->raw[dump->fill], bytes, n);
		dump->fill += n;
		bytes += n;
		len -= n;
		if (dump->fill == sizeof(dump->raw)) {
			dump_flush(dump);
		}
	}
}

static void dump_end(struct pcm_dump *dump)
{
	dump_flush(dump);
	shell_print(dump->sh, "PCM-END %u %08x", dump->samples, dump->crc);
}

static int bench_dump(const struct shell *sh)
{
	/* Static: 120 bytes more than a shell stack should carry */
	static struct pcm_dump dump;

	if (pcm_count == 0) {
		shell_print(sh, "bench buffer empty: cb91ai codec signal | record");
		return -ENODATA;
	}
	if (k_mutex_lock(&bench_lock, K_NO_WAIT)) {
		shell_print(sh, "bench busy");
		return -EBUSY;
	}
	dump_begin(&dump, sh, pcm_source, pcm_count, 0);
	dump_write(&dump, pcm, pcm_count);
	dump_end(&dump);
	k_mutex_unlock(&bench_lock);
	return 0;
}

static int bench_roundtrip(const struct shell *sh, enum bench_codec codec, int bitrate,
			   int frame_us)
{
	static struct pcm_dump dump;
	size_t frame = (size_t)((int64_t)frame_us * RATE_HZ / 1000000);
	char name[24];
	int delay = 0;

	if (pcm_count == 0) {
		shell_print(sh, "bench buffer empty: cb91ai codec signal | record");
		return -ENODATA;
	}
	if (frame == 0 || frame > RT_FRAME_MAX) {
		return -EINVAL;
	}
	if (k_mutex_lock(&bench_lock, K_NO_WAIT)) {
		shell_print(sh, "bench busy");
		return -EBUSY;
	}
#if defined(CONFIG_LIBLC3)
	if (codec == BENCH_LC3) {
		/* What the decoded signal lags behind the original, for who compares them */
		delay = lc3_delay_samples(frame_us, RATE_HZ);
	}
#endif
	snprintk(name, sizeof(name), "%s-%d", codec == BENCH_LC3 ? "lc3" : "adpcm", bitrate);
	memset(&job, 0, sizeof(job));
	job.codec = codec;
	job.bitrate = bitrate;
	job.frame_us = frame_us;
	job.roundtrip = true;
	k_sem_reset(&rt_ready);
	k_sem_reset(&rt_taken);
	dump_begin(&dump, sh, name, pcm_count / frame * frame, delay);
	k_thread_create(&bench_thread, bench_stack, K_THREAD_STACK_SIZEOF(bench_stack), bench_entry,
			&job, NULL, NULL, BENCH_PRIORITY, IS_ENABLED(CONFIG_FPU_SHARING) ? K_FP_REGS : 0,
			K_NO_WAIT);
	for (;;) {
		k_sem_take(&rt_ready, K_FOREVER);
		if (rt_frame_len == 0) {
			break;
		}
		dump_write(&dump, rt_frame, rt_frame_len);
		k_sem_give(&rt_taken);
	}
	k_thread_join(&bench_thread, K_FOREVER);
	dump_end(&dump);
	if (job.err) {
		shell_print(sh, "round trip failed (%d) after %u frames", job.err, job.frames);
	}
	k_mutex_unlock(&bench_lock);
	return job.err;
}

static int bench_all(const struct shell *sh)
{
	print_context(sh);
	print_header(sh);
	bench_run(sh, BENCH_ADPCM, 64000, 20000, 0, 0);
#if defined(CONFIG_LIBLC3)
	static const int lc3_rates[] = {16000, 24000, 32000};

	for (size_t i = 0; i < ARRAY_SIZE(lc3_rates); i++) {
		bench_run(sh, BENCH_LC3, lc3_rates[i], 10000, 0, 0);
	}
	bench_run(sh, BENCH_LC3, 24000, 7500, 0, 0);
#endif
#if defined(CONFIG_CB91AI_OPUS)
	static const int opus_rates[] = {12000, 16000, 24000};
	static const int opus_bands[] = {OPUS_BANDWIDTH_NARROWBAND, OPUS_BANDWIDTH_MEDIUMBAND};
	static const int opus_complexities[] = {2, 5, 10};

	/* What holds real time on this core: complexities 0 and 1 */
	for (int complexity = 0; complexity <= 1; complexity++) {
		for (size_t i = 0; i < ARRAY_SIZE(opus_rates); i++) {
			bench_run(sh, BENCH_OPUS, opus_rates[i], 20000, complexity, 0);
		}
	}
	/* Narrower audio bands, which SILK runs at 8 and 12 kHz */
	for (size_t b = 0; b < ARRAY_SIZE(opus_bands); b++) {
		bench_run(sh, BENCH_OPUS, 12000, 20000, 0, opus_bands[b]);
		bench_run(sh, BENCH_OPUS, 16000, 20000, 0, opus_bands[b]);
	}
	/* Longer packets, same 20 ms SILK frames inside */
	bench_run(sh, BENCH_OPUS, 16000, 60000, 0, 0);
	/* For the record: from 2 up, the delayed-decision quantizer */
	for (size_t c = 0; c < ARRAY_SIZE(opus_complexities); c++) {
		bench_run(sh, BENCH_OPUS, 16000, 20000, opus_complexities[c], 0);
	}
#endif
	return 0;
}

int codec_bench_cmd(const struct shell *sh, size_t argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "signal") == 0) {
		fill_synthetic();
		print_level(sh);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "record") == 0) {
		int ret;

		if (k_mutex_lock(&bench_lock, K_NO_WAIT)) {
			shell_print(sh, "bench busy");
			return -EBUSY;
		}
		bool raw = argc >= 3 && strcmp(argv[2], "raw") == 0;

		shell_print(sh, "recording %u ms from the microphone, gain %+d dB, %s: speak now",
			    CONFIG_CB91AI_CODEC_BENCH_MS, mic_gain_db(),
			    raw ? "raw" : "high-pass " STRINGIFY(CONFIG_CB91AI_MIC_HIGH_PASS_HZ) " Hz");
		ret = mic_capture(pcm, PCM_SAMPLES, !raw);
		pcm_count = ret > 0 ? (size_t)ret : 0;
		pcm_source = raw ? "microphone-raw" : "microphone";
		k_mutex_unlock(&bench_lock);
		if (ret < 0) {
			shell_print(sh, "capture failed (%d)", ret);
			return ret;
		}
		print_level(sh);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "level") == 0) {
		print_context(sh);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "all") == 0) {
		return bench_all(sh);
	}
	if (argc >= 2 && strcmp(argv[1], "dump") == 0) {
		return bench_dump(sh);
	}
	if (argc >= 3 && strcmp(argv[1], "roundtrip") == 0) {
		if (strcmp(argv[2], "adpcm") == 0) {
			return bench_roundtrip(sh, BENCH_ADPCM, 64000, 20000);
		}
		if (argc >= 4 && strcmp(argv[2], "lc3") == 0 && IS_ENABLED(CONFIG_LIBLC3)) {
			return bench_roundtrip(sh, BENCH_LC3, (int)strtol(argv[3], NULL, 10),
					       argc >= 5 ? (int)strtol(argv[4], NULL, 10) : 10000);
		}
	}
	if (argc >= 2 && strcmp(argv[1], "adpcm") == 0) {
		print_header(sh);
		return bench_run(sh, BENCH_ADPCM, 64000, 20000, 0, 0);
	}
	if (argc >= 3 && strcmp(argv[1], "lc3") == 0) {
		print_header(sh);
		return bench_run(sh, BENCH_LC3, (int)strtol(argv[2], NULL, 10),
				 argc >= 4 ? (int)strtol(argv[3], NULL, 10) : 10000, 0, 0);
	}
#if defined(CONFIG_CB91AI_OPUS)
	if (argc >= 3 && strcmp(argv[1], "opus") == 0) {
		print_header(sh);
		return bench_run(sh, BENCH_OPUS, (int)strtol(argv[2], NULL, 10),
				 argc >= 5 ? (int)strtol(argv[4], NULL, 10) * 1000 : 20000,
				 argc >= 4 ? (int)strtol(argv[3], NULL, 10) : 0,
				 argc >= 6 ? opus_band_value(argv[5]) : 0);
	}
#endif
	shell_print(sh, "usage: cb91ai codec signal | record [raw] | level | all | adpcm | "
			"lc3 <bit/s> [frame us] | opus <bit/s> [complexity] [frame ms] [nb|mb|wb] | "
			"dump | roundtrip adpcm | roundtrip lc3 <bit/s> [frame us]");
	return -EINVAL;
}
