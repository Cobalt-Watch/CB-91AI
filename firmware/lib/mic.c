/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * PDM microphone (U119) check: the microphone is powered through P1.01, the
 * nRF52840 PDM peripheral is run through the Zephyr dmic API at 16 kHz mono,
 * and the level of a short capture is logged. A dead microphone shows as a
 * flat signal (RMS of a few LSB, or a saturated constant).
 */

#include <errno.h>
#include <math.h>
#include <string.h>
#include <hal/nrf_pdm.h>
#include <zephyr/kernel.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "mic.h"

LOG_MODULE_REGISTER(cb91ai_mic, LOG_LEVEL_INF);

#define SAMPLE_RATE_HZ   MIC_SAMPLE_RATE_HZ
#define SAMPLE_BITS      16
#define BLOCK_SAMPLES    (SAMPLE_RATE_HZ / 10) /* 100 ms per block */
#define BLOCK_SIZE       (BLOCK_SAMPLES * sizeof(int16_t))
#define BLOCK_COUNT      4
#define BLOCKS_TO_READ   5
/* The first blocks carry the start-up of the decimation filter */
#define BLOCKS_TO_SKIP   2
#define READ_TIMEOUT_MS  500

K_MEM_SLAB_DEFINE_STATIC(mic_slab, BLOCK_SIZE, BLOCK_COUNT, 4);

static const struct device *const pdm = DEVICE_DT_GET(DT_NODELABEL(pdm0));
static const struct device *const mic_pwr = DEVICE_DT_GET(DT_NODELABEL(mic_pwr));

static int gain_db = CONFIG_CB91AI_MIC_GAIN_DB;

/*
 * High-pass of the capture path, first order: y[n] = x[n] - x[n-1] + a y[n-1],
 * with a = 1 - 2 pi fc / fs in Q16 and the output kept in Q16, so that no
 * offset builds up from the rounding. It starts on the first sample instead of
 * on zero: the PDM output sits hundreds of LSB away from it at power-up.
 */
#define HIGH_PASS_A_Q16 (65536 - CONFIG_CB91AI_MIC_HIGH_PASS_HZ * 411775LL / SAMPLE_RATE_HZ)

struct high_pass {
	int32_t x_prev;
	int64_t y_q16;
	bool primed;
};

static void high_pass_run(struct high_pass *hp, int16_t *samples, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		int32_t x = samples[i];

		if (!hp->primed) {
			hp->x_prev = x;
			hp->primed = true;
		}
		hp->y_q16 = (int64_t)(x - hp->x_prev) * 65536 + ((hp->y_q16 * HIGH_PASS_A_Q16) >> 16);
		hp->x_prev = x;
		samples[i] = (int16_t)CLAMP((hp->y_q16 + 32768) >> 16, INT16_MIN, INT16_MAX);
	}
}

int mic_set_gain_db(int db)
{
	if (db < -20 || db > 20) {
		return -EINVAL;
	}
	gain_db = db;
	return 0;
}

int mic_gain_db(void)
{
	return gain_db;
}

static void log_level(const int16_t *samples, size_t count, int block)
{
	int64_t sum = 0, sum_sq = 0;
	int16_t peak = 0;

	for (size_t i = 0; i < count; i++) {
		int32_t s = samples[i];

		sum += s;
		sum_sq += s * s;
		if (s > peak) {
			peak = s;
		} else if (-s > peak) {
			peak = -s;
		}
	}
	int32_t mean = sum / (int64_t)count;
	double variance = (double)sum_sq / count - (double)mean * mean;
	int32_t ac_rms = variance > 0 ? (int32_t)sqrt(variance) : 0;

	LOG_INF("mic block %d: %u samples, ac rms %d, peak %d, dc offset %d", block,
		(unsigned int)count, ac_rms, peak, mean);
}

static void mic_power_off(void)
{
	if (device_is_ready(mic_pwr)) {
		regulator_disable(mic_pwr);
	}
}

/* Power the microphone and start the PDM stream; mic_stop() undoes both */
static int mic_start(void)
{
	struct pcm_stream_cfg stream = {
		.pcm_width = SAMPLE_BITS,
		.mem_slab = &mic_slab,
	};
	struct dmic_cfg cfg = {
		.io = {
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3500000,
			.min_pdm_clk_dc = 40,
			.max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = {
			.req_num_streams = 1,
			.req_num_chan = 1,
			.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT),
		},
	};
	int ret;

	if (!device_is_ready(pdm)) {
		LOG_ERR("PDM device not ready");
		return -ENODEV;
	}
	if (device_is_ready(mic_pwr)) {
		ret = regulator_enable(mic_pwr);
		if (ret && ret != -EALREADY) {
			LOG_ERR("microphone supply failed (%d)", ret);
			return ret;
		}
	} else {
		LOG_WRN("microphone supply regulator not ready, trying anyway");
	}
	k_msleep(50); /* supply settling and microphone wake-up */

	cfg.streams[0].pcm_rate = SAMPLE_RATE_HZ;
	cfg.streams[0].block_size = BLOCK_SIZE;
	ret = dmic_configure(pdm, &cfg);
	if (ret) {
		LOG_ERR("dmic_configure failed (%d)", ret);
		mic_power_off();
		return ret;
	}
	/* The driver sets the default gain each time it is configured. The gain
	 * registers count half decibels around NRF_PDM_GAIN_DEFAULT (nrfx MDK).
	 */
	nrf_pdm_gain_set((NRF_PDM_Type *)DT_REG_ADDR(DT_NODELABEL(pdm0)),
			 (nrf_pdm_gain_t)(NRF_PDM_GAIN_DEFAULT + 2 * gain_db),
			 (nrf_pdm_gain_t)(NRF_PDM_GAIN_DEFAULT + 2 * gain_db));
	ret = dmic_trigger(pdm, DMIC_TRIGGER_START);
	if (ret) {
		LOG_ERR("dmic start failed (%d)", ret);
		mic_power_off();
	}
	return ret;
}

static void mic_stop(void)
{
	dmic_trigger(pdm, DMIC_TRIGGER_STOP);
	mic_power_off();
}

int mic_test(void)
{
	int ret = mic_start();

	if (ret) {
		return ret;
	}
	for (int block = 0; block < BLOCKS_TO_READ; block++) {
		void *buffer;
		uint32_t size;

		ret = dmic_read(pdm, 0, &buffer, &size, READ_TIMEOUT_MS);
		if (ret) {
			LOG_ERR("dmic_read failed (%d)", ret);
			break;
		}
		if (block >= BLOCKS_TO_SKIP) {
			log_level(buffer, size / sizeof(int16_t), block);
		}
		k_mem_slab_free(&mic_slab, buffer);
	}
	mic_stop();
	return ret;
}

int mic_stream(size_t count, bool high_pass, mic_sink_t sink, void *ctx)
{
	struct high_pass hp = {0};
	size_t done = 0;
	int ret = mic_start();

	if (ret) {
		return ret;
	}
	for (int block = 0; done < count; block++) {
		void *buffer;
		uint32_t size;

		ret = dmic_read(pdm, 0, &buffer, &size, READ_TIMEOUT_MS);
		if (ret) {
			LOG_ERR("dmic_read failed (%d)", ret);
			break;
		}
		if (block >= BLOCKS_TO_SKIP) {
			size_t n = MIN(size / sizeof(int16_t), count - done);

			/* The block is ours until it goes back to the slab */
			if (high_pass) {
				high_pass_run(&hp, buffer, n);
			}
			ret = sink(buffer, n, ctx);
			done += n;
		}
		k_mem_slab_free(&mic_slab, buffer);
		if (ret) {
			break;
		}
	}
	mic_stop();
	return ret ? ret : (int)done;
}

struct capture_to_ram {
	int16_t *samples;
	size_t done;
};

static int copy_to_ram(const int16_t *samples, size_t count, void *ctx)
{
	struct capture_to_ram *ram = ctx;

	memcpy(&ram->samples[ram->done], samples, count * sizeof(int16_t));
	ram->done += count;
	return 0;
}

int mic_capture(int16_t *samples, size_t count, bool high_pass)
{
	struct capture_to_ram ram = { .samples = samples };

	return mic_stream(count, high_pass, copy_to_ram, &ram);
}
