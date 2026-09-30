/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The voice note container v2 (lot E1):
 * a header of 64 bytes, little-endian, then the frames of the codec. Pure C:
 * the header written and read byte for byte, the CRC32 of IEEE 802.3 (the one
 * of zlib, Python's zlib.crc32 and Zephyr's crc32_ieee), and its combination,
 * which gives the CRC of header and frames from the CRC of each. The host
 * tests check it (firmware/tests/host/test_note.c), and the harness of the PC
 * reads the same bytes (firmware/tools/cobalt_link.py).
 */

#ifndef CB91AI_NOTE_H
#define CB91AI_NOTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NOTE_HEADER_SIZE 64U
#define NOTE_MAGIC       0x324e4243U /* "CBN2" */
#define NOTE_VERSION     1U
#define NOTE_STATE_AT    63U         /* the state byte, outside the CRC */

#define NOTE_CODEC_LC3   1U
#define NOTE_CODEC_ADPCM 2U

/* How a take ended (bits 4 to 7 of the flags) */
#define NOTE_END_PRESS   0U
#define NOTE_END_SILENCE 1U
#define NOTE_END_LONGEST 2U
#define NOTE_END_FULL    3U
#define NOTE_END_FAULT   4U /* the microphone failed: what came before is kept */

#define NOTE_STATE_COMPLETE  0xffU
#define NOTE_STATE_DELIVERED 0x00U

struct note_header {
	uint32_t id;           /* grows with age, never 0 */
	uint8_t codec;
	uint8_t channels;
	uint32_t rate_hz;
	uint16_t frame_us;
	uint16_t frame_bytes;
	uint32_t bitrate;
	int64_t utc_ms;        /* start of the take; 0: the watch had no time */
	int16_t tz_minutes;
	bool time_approx;
	uint8_t end;           /* NOTE_END_* */
	uint32_t duration_ms;
	uint32_t data_size;    /* bytes of frames after the header */
	uint32_t data_crc;
	int8_t gain_db;
	uint8_t high_pass_hz;
	uint8_t fw[3];         /* major, minor, revision */
	uint8_t state;         /* NOTE_STATE_* */
};

/* The 64 bytes of the header, the state byte included */
void note_header_put(const struct note_header *h, uint8_t out[NOTE_HEADER_SIZE]);

/* False unless magic, version, length and CRC hold */
bool note_header_get(const uint8_t in[NOTE_HEADER_SIZE], struct note_header *h);

/* CRC32 of IEEE 802.3, continued from `crc` (0 to start): crc32(0, "123456789")
 * is 0xcbf43926 */
uint32_t note_crc32(uint32_t crc, const void *data, size_t len);

/* The CRC32 of A then B, from the CRC32 of A, that of B and the length of B */
uint32_t note_crc32_combine(uint32_t crc_a, uint32_t crc_b, size_t len_b);

/* The CRC32 of the note as NOTE_OFFER announces it: its header as the flash
 * holds it, byte for byte, in the state "complete", then its frames, from their
 * CRC and size. On the raw bytes rather than on a header written again: a bit
 * this version does not know (a flag of a later one) goes out as it is. */
uint32_t note_offer_crc(const uint8_t header[NOTE_HEADER_SIZE], uint32_t data_crc,
			uint32_t data_size);

#endif /* CB91AI_NOTE_H */
