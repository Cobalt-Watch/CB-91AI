/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CB91AI_MIC_H
#define CB91AI_MIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIC_SAMPLE_RATE_HZ 16000

/*
 * Power the PDM microphone, capture a short burst at 16 kHz and log its
 * level (RMS, peak, DC offset). Returns 0 when samples were received.
 */
int mic_test(void);

/*
 * Power the PDM microphone and record `count` samples at 16 kHz, 16 bits,
 * mono, once the decimation filter has settled. Blocks for the duration of
 * the capture. Returns the number of samples written, or a negative error.
 *
 * `high_pass` runs the samples through the high-pass of the capture path
 * (CONFIG_CB91AI_MIC_HIGH_PASS_HZ): the output of this microphone drifts for
 * more than three seconds after power-up, a thump as loud as the voice in the
 * first half second of a note. Without it, the samples are those of the PDM
 * peripheral, untouched.
 */
int mic_capture(int16_t *samples, size_t count, bool high_pass);

/*
 * Receives the samples of mic_stream() block by block, in the caller's thread:
 * 100 ms each (1600 samples), the last one shorter. A non-zero return stops the
 * capture and is returned by mic_stream().
 */
typedef int (*mic_sink_t)(const int16_t *samples, size_t count, void *ctx);

/*
 * Like mic_capture(), but each block goes to `sink` as soon as the PDM
 * peripheral has filled it, instead of into one buffer: the way to record for
 * longer than the RAM holds, straight to the storage flash (lot E1). The
 * driver keeps sampling meanwhile into its other blocks, two of the four at a
 * time (the one it fills, the next): the sink has about 200 ms per block to
 * return, or the driver runs out of blocks and stops, and mic_stream() returns
 * an error. Returns the number of samples given to the sink, or a negative
 * error.
 */
int mic_stream(size_t count, bool high_pass, mic_sink_t sink, void *ctx);

/*
 * Gain of the PDM peripheral around its default, in dB, from -20 to +20 (the
 * hardware moves in steps of 0.5 dB). Used from the next capture on. Starts
 * at CONFIG_CB91AI_MIC_GAIN_DB.
 */
int mic_set_gain_db(int db);
int mic_gain_db(void);

#endif /* CB91AI_MIC_H */
