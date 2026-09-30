/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The voice note container v2, see note.h. Pure C: the host tests build this
 * file as it is.
 */

#include <string.h>

#include "note.h"

#define CRC_POLY 0xedb88320U /* reflected 0x04c11db7 */

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
	return get16(p) | (uint32_t)get16(p + 2) << 16;
}

/* Four bits at a time: a table of 16 words, 64 bytes of flash */
uint32_t note_crc32(uint32_t crc, const void *data, size_t len)
{
	static const uint32_t nibble[16] = {
		0x00000000U, 0x1db71064U, 0x3b6e20c8U, 0x26d930acU,
		0x76dc4190U, 0x6b6b51f4U, 0x4db26158U, 0x5005713cU,
		0xedb88320U, 0xf00f9344U, 0xd6d6a3e8U, 0xcb61b38cU,
		0x9b64c2b0U, 0x86d3d2d4U, 0xa00ae278U, 0xbdbdf21cU,
	};
	const uint8_t *p = data;

	crc = ~crc;
	for (size_t i = 0; i < len; i++) {
		crc ^= p[i];
		crc = (crc >> 4) ^ nibble[crc & 0xfU];
		crc = (crc >> 4) ^ nibble[crc & 0xfU];
	}
	return ~crc;
}

/* zlib's crc32_combine, by squaring the operator of a zero bit over GF(2) */
static uint32_t gf2_times(const uint32_t *mat, uint32_t vec)
{
	uint32_t sum = 0;

	for (unsigned int i = 0; vec != 0; i++, vec >>= 1) {
		if (vec & 1U) {
			sum ^= mat[i];
		}
	}
	return sum;
}

static void gf2_square(uint32_t *square, const uint32_t *mat)
{
	for (unsigned int n = 0; n < 32; n++) {
		square[n] = gf2_times(mat, mat[n]);
	}
}

uint32_t note_crc32_combine(uint32_t crc_a, uint32_t crc_b, size_t len_b)
{
	uint32_t even[32];
	uint32_t odd[32];
	uint32_t row = 1;

	if (len_b == 0) {
		return crc_a;
	}
	odd[0] = CRC_POLY; /* the operator of one zero bit */
	for (unsigned int n = 1; n < 32; n++) {
		odd[n] = row;
		row <<= 1;
	}
	gf2_square(even, odd); /* two zero bits */
	gf2_square(odd, even); /* four */
	do {
		/* One zero byte, then each bit of the length */
		gf2_square(even, odd);
		if (len_b & 1U) {
			crc_a = gf2_times(even, crc_a);
		}
		len_b >>= 1;
		if (len_b == 0) {
			break;
		}
		gf2_square(odd, even);
		if (len_b & 1U) {
			crc_a = gf2_times(odd, crc_a);
		}
		len_b >>= 1;
	} while (len_b != 0);
	return crc_a ^ crc_b;
}

void note_header_put(const struct note_header *h, uint8_t out[NOTE_HEADER_SIZE])
{
	memset(out, 0, NOTE_HEADER_SIZE);
	put32(&out[0], NOTE_MAGIC);
	out[4] = NOTE_VERSION;
	out[5] = NOTE_HEADER_SIZE;
	out[6] = h->codec;
	out[7] = h->channels;
	put32(&out[8], h->id);
	put32(&out[12], h->rate_hz);
	put16(&out[16], h->frame_us);
	put16(&out[18], h->frame_bytes);
	put32(&out[20], h->bitrate);
	put32(&out[24], (uint32_t)(uint64_t)h->utc_ms);
	put32(&out[28], (uint32_t)((uint64_t)h->utc_ms >> 32));
	put16(&out[32], (uint16_t)h->tz_minutes);
	put16(&out[34], (uint16_t)((h->time_approx ? 1U : 0U) | (h->end & 0xfU) << 4));
	put32(&out[36], h->duration_ms);
	put32(&out[40], h->data_size);
	put32(&out[44], h->data_crc);
	out[48] = (uint8_t)h->gain_db;
	out[49] = h->high_pass_hz;
	out[50] = h->fw[0];
	out[51] = h->fw[1];
	out[52] = h->fw[2];
	put32(&out[56], note_crc32(0, out, 56));
	out[60] = 0xffU;
	out[61] = 0xffU;
	out[62] = 0xffU;
	out[NOTE_STATE_AT] = h->state;
}

bool note_header_get(const uint8_t in[NOTE_HEADER_SIZE], struct note_header *h)
{
	uint16_t flags;

	if (get32(&in[0]) != NOTE_MAGIC || in[4] != NOTE_VERSION || in[5] != NOTE_HEADER_SIZE ||
	    get32(&in[56]) != note_crc32(0, in, 56)) {
		return false;
	}
	h->codec = in[6];
	h->channels = in[7];
	h->id = get32(&in[8]);
	h->rate_hz = get32(&in[12]);
	h->frame_us = get16(&in[16]);
	h->frame_bytes = get16(&in[18]);
	h->bitrate = get32(&in[20]);
	h->utc_ms = (int64_t)((uint64_t)get32(&in[24]) | (uint64_t)get32(&in[28]) << 32);
	h->tz_minutes = (int16_t)get16(&in[32]);
	flags = get16(&in[34]);
	h->time_approx = (flags & 1U) != 0;
	h->end = (uint8_t)((flags >> 4) & 0xfU);
	h->duration_ms = get32(&in[36]);
	h->data_size = get32(&in[40]);
	h->data_crc = get32(&in[44]);
	h->gain_db = (int8_t)in[48];
	h->high_pass_hz = in[49];
	h->fw[0] = in[50];
	h->fw[1] = in[51];
	h->fw[2] = in[52];
	h->state = in[NOTE_STATE_AT];
	return h->id != 0;
}

uint32_t note_offer_crc(const uint8_t header[NOTE_HEADER_SIZE], uint32_t data_crc,
			uint32_t data_size)
{
	uint8_t head[NOTE_HEADER_SIZE];

	memcpy(head, header, sizeof(head));
	head[NOTE_STATE_AT] = NOTE_STATE_COMPLETE;
	return note_crc32_combine(note_crc32(0, head, sizeof(head)), data_crc, data_size);
}
