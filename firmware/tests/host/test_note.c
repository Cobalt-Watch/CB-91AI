/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of the voice note container v2 (firmware/lib/note.c, lot E1):
 * the CRC32 against its reference value, its combination against the CRC of
 * the whole, the header byte for byte and its refusals, and the vectors of
 * firmware/tests/vectors/note_v2.txt (path in argv[1]), which the PC harness
 * reads too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "note.h"

static uint8_t big[250000];

static void fill(uint8_t *p, size_t n, uint32_t seed)
{
	for (size_t i = 0; i < n; i++) {
		seed = seed * 1103515245U + 12345U;
		p[i] = (uint8_t)(seed >> 16);
	}
}

static void crc_reference(void)
{
	const char *check = "123456789";

	CHECK_EQ(note_crc32(0, check, 9), 0xcbf43926U);
	/* In pieces as at once */
	CHECK_EQ(note_crc32(note_crc32(0, check, 4), check + 4, 5), 0xcbf43926U);
	CHECK_EQ(note_crc32(0, check, 0), 0);
}

static void crc_combined(void)
{
	static const size_t lengths[] = { 0, 1, 3, 64, 1000, 4096, 65536, 240000 };

	fill(big, sizeof(big), 7);
	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		const size_t a = 64, b = lengths[i];
		const uint32_t whole = note_crc32(0, big, a + b);
		const uint32_t crc_a = note_crc32(0, big, a);
		const uint32_t crc_b = note_crc32(0, big + a, b);

		CHECK_EQ(note_crc32_combine(crc_a, crc_b, b), whole);
	}
}

static struct note_header sample(void)
{
	const struct note_header h = {
		.id = 42,
		.codec = NOTE_CODEC_LC3,
		.channels = 1,
		.rate_hz = 16000,
		.frame_us = 10000,
		.frame_bytes = 20,
		.bitrate = 16000,
		.utc_ms = 1790257800123LL,
		.tz_minutes = 120,
		.time_approx = true,
		.end = NOTE_END_SILENCE,
		.duration_ms = 7230,
		.data_size = 14460,
		.data_crc = 0x12345678U,
		.gain_db = -3,
		.high_pass_hz = 50,
		.fw = { 0, 2, 2 },
		.state = NOTE_STATE_COMPLETE,
	};

	return h;
}

static void header_both_ways(void)
{
	const struct note_header h = sample();
	struct note_header back;
	uint8_t raw[NOTE_HEADER_SIZE];

	note_header_put(&h, raw);
	/* Magic, version, length, codec, the id little-endian */
	CHECK_EQ(raw[0], 'C');
	CHECK_EQ(raw[1], 'B');
	CHECK_EQ(raw[2], 'N');
	CHECK_EQ(raw[3], '2');
	CHECK_EQ(raw[4], 1);
	CHECK_EQ(raw[5], 64);
	CHECK_EQ(raw[6], NOTE_CODEC_LC3);
	CHECK_EQ(raw[8], 42);
	CHECK_EQ(raw[34], 0x11); /* approximate time, ended by silence */
	CHECK_EQ(raw[48], 0xfd);
	CHECK_EQ(raw[60], 0xff);
	CHECK_EQ(raw[63], NOTE_STATE_COMPLETE);
	CHECK(note_header_get(raw, &back));
	CHECK_EQ(back.id, 42);
	CHECK_EQ(back.utc_ms, 1790257800123LL);
	CHECK_EQ(back.tz_minutes, 120);
	CHECK(back.time_approx);
	CHECK_EQ(back.end, NOTE_END_SILENCE);
	CHECK_EQ(back.duration_ms, 7230);
	CHECK_EQ(back.data_size, 14460);
	CHECK_EQ(back.data_crc, 0x12345678U);
	CHECK_EQ(back.gain_db, -3);
	CHECK_EQ(back.fw[2], 2);
	/* The state is outside the CRC: delivered, still a header */
	raw[NOTE_STATE_AT] = NOTE_STATE_DELIVERED;
	CHECK(note_header_get(raw, &back));
	CHECK_EQ(back.state, NOTE_STATE_DELIVERED);
}

static void header_refusals(void)
{
	const struct note_header h = sample();
	struct note_header back;
	uint8_t raw[NOTE_HEADER_SIZE];
	uint8_t blank[NOTE_HEADER_SIZE];
	struct note_header zero = h;

	/* Any byte of the CRC area changed */
	for (size_t i = 0; i < 60; i++) {
		note_header_put(&h, raw);
		raw[i] ^= 0x10;
		CHECK(!note_header_get(raw, &back));
	}
	/* An erased page, and an id of 0 */
	memset(blank, 0xff, sizeof(blank));
	CHECK(!note_header_get(blank, &back));
	zero.id = 0;
	note_header_put(&zero, raw);
	CHECK(!note_header_get(raw, &back));
}

static void offer_crc_covers_header_and_frames(void)
{
	struct note_header h = sample();
	uint8_t raw[NOTE_HEADER_SIZE];
	uint8_t sent[NOTE_HEADER_SIZE];
	uint32_t whole;

	fill(big, h.data_size, 3);
	h.data_crc = note_crc32(0, big, h.data_size);
	/* As the phone checks it: the header as sent, state "complete", then the frames */
	h.state = NOTE_STATE_DELIVERED; /* whatever the flash says now */
	note_header_put(&h, raw);
	memcpy(sent, raw, sizeof(sent));
	sent[NOTE_STATE_AT] = NOTE_STATE_COMPLETE;
	whole = note_crc32(note_crc32(0, sent, sizeof(sent)), big, h.data_size);
	CHECK_EQ(note_offer_crc(raw, h.data_crc, h.data_size), whole);
	/* A flag of a later version, unknown here, goes out as it is: covered */
	raw[35] |= 0x02;
	sent[35] |= 0x02;
	whole = note_crc32(note_crc32(0, sent, sizeof(sent)), big, h.data_size);
	CHECK_EQ(note_offer_crc(raw, h.data_crc, h.data_size), whole);
}

static const char *vectors_path;

/* The bytes of vector `kind`, from its hex: how many */
static size_t vector(const char *kind, uint8_t *out, size_t room)
{
	FILE *f = fopen(vectors_path, "r");
	char line[1024];
	size_t n = 0;

	CHECK(f != NULL);
	if (f == NULL) {
		return 0;
	}
	while (n == 0 && fgets(line, sizeof(line), f) != NULL) {
		const size_t k = strlen(kind);

		if (strncmp(line, kind, k) != 0 || line[k] != ' ') {
			continue;
		}
		for (const char *p = &line[k + 1]; n < room && strchr(" \r\n", p[0]) == NULL;
		     p += 2) {
			const char pair[3] = { p[0], p[1], 0 };

			out[n++] = (uint8_t)strtoul(pair, NULL, 16);
		}
	}
	fclose(f);
	return n;
}

static void the_shared_vectors(void)
{
	const struct note_header h = sample();
	uint8_t want[NOTE_HEADER_SIZE];
	uint8_t raw[NOTE_HEADER_SIZE];
	uint8_t note[256];
	struct note_header back;
	size_t n;

	/* The header of the sample, byte for byte as the document says */
	CHECK_EQ(vector("HEADER", want, sizeof(want)), NOTE_HEADER_SIZE);
	note_header_put(&h, raw);
	CHECK(memcmp(raw, want, sizeof(raw)) == 0);
	CHECK(note_header_get(want, &back));
	CHECK_EQ(back.utc_ms, 1790257800123LL);
	CHECK_EQ(back.gain_db, -3);
	/* A whole note: its header read, its frames and the CRC of the offer */
	n = vector("NOTE", note, sizeof(note));
	CHECK_EQ(n, 124);
	CHECK(note_header_get(note, &back));
	CHECK_EQ(back.id, 7);
	CHECK_EQ(back.data_size, n - NOTE_HEADER_SIZE);
	CHECK_EQ(back.data_crc, note_crc32(0, &note[NOTE_HEADER_SIZE], n - NOTE_HEADER_SIZE));
	CHECK_EQ(back.end, NOTE_END_PRESS);
	CHECK_EQ(note_offer_crc(note, back.data_crc, back.data_size), 1468073345U);
	CHECK_EQ(note_crc32(0, note, n), 1468073345U);
}

int main(int argc, char **argv)
{
	vectors_path = argc > 1 ? argv[1] : "../vectors/note_v2.txt";
	RUN(crc_reference);
	RUN(crc_combined);
	RUN(header_both_ways);
	RUN(header_refusals);
	RUN(offer_crc_covers_header_and_frames);
	RUN(the_shared_vectors);
	return harness_report("note");
}
