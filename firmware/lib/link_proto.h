/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Messages of the Cobalt Link service, version 1 (lot D6): what the watch
 * writes on TX and reads from RX, byte for byte as the Cobalt Link specification
 * defines them. Pure C, no Zephyr: the host tests check it against
 * the vectors that the PC harness (tools/cobalt_link.py) checks too
 * (firmware/tests/vectors/link_v1.txt).
 *
 * One message per write or notification, the type first, its top bit the way
 * it goes (0 watch to phone, 1 phone to watch), little-endian fields. A later
 * version only appends fields: extra bytes are ignored.
 */

#ifndef CB91AI_LINK_PROTO_H
#define CB91AI_LINK_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINK_PROTO_VERSION 1
#define LINK_TO_WATCH      0x80 /* direction bit of the type */
#define LINK_SMALL_MAX     20   /* every message fits here, but NOTE_DATA and VALUE */

/* Watch to phone (TX, notifications) */
#define LINK_HELLO      0x01
#define LINK_NOTE_OFFER 0x02
#define LINK_NOTE_DATA  0x03
#define LINK_NOTE_END   0x04
#define LINK_VALUE      0x05
#define LINK_EVENT      0x06
#define LINK_BYE_WATCH  0x0f

/* Phone to watch (RX, writes) */
#define LINK_TIME        0x81
#define LINK_NOTE_ACCEPT 0x82
#define LINK_NOTE_ACK    0x83
#define LINK_RESULT      0x84
#define LINK_SET         0x85
#define LINK_GET         0x86
#define LINK_BYE_PHONE   0x8f

/* HELLO: why the watch is here */
enum link_reason {
	LINK_REASON_PHONE = 0,
	LINK_REASON_NOTE = 1,
	LINK_REASON_SYNC = 2,
	LINK_REASON_DAILY = 3,
	LINK_REASON_UPDATED = 4,
	LINK_REASON_PAIRED = 5,
};

/* HELLO: state bits */
#define LINK_STATE_TIME_SET    (1U << 0)
#define LINK_STATE_TIME_APPROX (1U << 1)
#define LINK_STATE_IMAGE_TEST  (1U << 2)
#define LINK_STATE_FLASH_FULL  (1U << 3)

/* HELLO: codec bits; the codec of a note is in its own header */
#define LINK_CODEC_LC3   (1U << 0)
#define LINK_CODEC_ADPCM (1U << 1)

/* NOTE_ACK verdicts: anything but RECEIVED keeps the note */
#define LINK_ACK_RECEIVED 0
#define LINK_ACK_DAMAGED  1
#define LINK_WINDOW_MAX_S 60

/* RESULT codes: the LED */
#define LINK_RESULT_SUCCESS 0
#define LINK_RESULT_FAILURE 1

/* BYE reasons */
#define LINK_BYE_DONE  0
#define LINK_BYE_IDLE  1
#define LINK_BYE_ERROR 2
#define LINK_BYE_USER  3

/* VALUE statuses */
#define LINK_VALUE_OK        0
#define LINK_VALUE_UNKNOWN   1
#define LINK_VALUE_REFUSED   2
#define LINK_VALUE_READ_ONLY 3

/* Keys of SET, GET, VALUE */
#define LINK_KEY_TIME_FORMAT 0x01 /* u8: 12 or 24 */
#define LINK_KEY_DISPLAY_S   0x02 /* u8: 1 to 120 s */
#define LINK_KEY_TX_POWER    0x03 /* i8: 0, 4 or 8 dBm */
#define LINK_KEY_HOLD        0x04 /* u8: 0 or 1, held session */
#define LINK_KEY_LIGHT       0x05 /* u8 effect, then u8 red, green, blue: the light of LIGHT */
#define LINK_KEY_NAME        0x06 /* 1 to LINK_NAME_MAX bytes of printable ASCII: the name */
#define LINK_KEY_COUNTERS    0x10 /* read only, struct link_counters */
#define LINK_KEY_STATUS      0x11 /* read only, text */
#define LINK_KEY_JOURNAL     0x12 /* SET u32: the session's cursor; GET: a page of the
                                   * journal from it (link_put_journal), lot T1 */
#define LINK_KEY_DEBUG_TRIAL 0x7A /* the trial of the calls (lot N1a, risk R11): SET u8
                                   * minutes, u8 profile; GET struct link_trial;
                                   * development builds only */
#define LINK_KEY_DEBUG_ACCEL 0x7B /* read only: what the accelerometer said and its
                                   * registers (lot D4, watch/src/wrist.h), development
                                   * builds only (the harness) */
#define LINK_KEY_DEBUG_WRIST 0x7C /* the tuning of the wrist (lot D4, watch/src/wrist.h),
                                   * development builds only (the harness) */
#define LINK_KEY_DEBUG_JOURNAL_S 0x7D /* u16: the journal's period, 1 to 600 s, until the
                                       * next boot; development builds only (the harness) */
#define LINK_KEY_DEBUG_DAILY_S 0x7E /* u32: the period of the daily call, 60 to 86400 s,
                                     * until the next boot; development builds only (the
                                     * harness) */
#define LINK_KEY_DEBUG_TAKE  0x7F /* u8: a take of this many seconds, 1 to 60, no voice
                                   * detector; development builds only (the harness) */

/* The name of the watch (LINK_KEY_NAME): what fits in the scan response next
 * to the UUID of SMP, 31 bytes in all (the Cobalt Link specification) */
#define LINK_NAME_MAX 11

/* Effects of LINK_KEY_LIGHT */
#define LINK_LIGHT_STEADY  0
#define LINK_LIGHT_BREATHE 1
#define LINK_LIGHT_BLINK   2
#define LINK_LIGHT_RAINBOW 3 /* the colour wheel: the colour given is not used */

struct link_hello {
	uint8_t version; /* LINK_PROTO_VERSION */
	uint8_t reason;
	uint8_t state;
	uint8_t fw_major;
	uint8_t fw_minor;
	uint16_t fw_revision;
	uint8_t notes;
	uint32_t note_bytes;
	uint16_t battery_mv;
	uint8_t codecs;
	uint32_t watch_id;
};

struct link_counters {
	uint32_t notes;
	uint32_t recorded_s;
	uint32_t connected_s;
	uint16_t boots;
	uint16_t watchdog_resets;
	uint16_t events_lost;
};

#define LINK_COUNTERS_SIZE 18

/* A record of the journal of the temperature and the cell (LINK_KEY_JOURNAL,
 * the Cobalt Link specification) */
struct link_journal_record {
	uint32_t time_s; /* UTC, seconds since 1970 */
	int16_t temp_cc; /* hundredths of a degree C */
	uint16_t cell;   /* bits 0 to 12 mV; bit 14 a take, bit 15 a radio link */
};

/* The trial of the calls (LINK_KEY_DEBUG_TRIAL): its state, then the last call a
 * phone answered during a trial, as the watch timed it */
struct link_trial {
	uint16_t minutes_left; /* 0: no trial */
	uint8_t profile;       /* LINK_TRIAL_PRODUCT or LINK_TRIAL_APPLE */
	uint32_t number;       /* the call since boot, 0: none */
	uint8_t reason;        /* of the call, as in HELLO */
	uint32_t connect_ms;   /* first advertisement to the connection */
	uint32_t read_ms;      /* connection to the first read, LINK_TRIAL_NONE: none yet */
	uint16_t interval;     /* of the link at its connection, 1.25 ms units */
	uint16_t latency;
	uint16_t timeout;      /* 10 ms units */
};

#define LINK_TRIAL_PRODUCT 0U /* 100 ms for 10 s, then 1 s */
#define LINK_TRIAL_APPLE   1U /* 20 ms for 30 s, then 152.5 ms */
#define LINK_TRIAL_NONE    0xFFFFFFFFU
#define LINK_TRIAL_SIZE    23U
#define LINK_TRIAL_MAX_MIN 240U

#define LINK_JOURNAL_HEAD   9U  /* u32 journal identifier, u32 first index, u8 count */
#define LINK_JOURNAL_RECORD 8U
#define LINK_JOURNAL_MAX    29U /* records of a page at most: the longest VALUE */

/* A message from the phone, as parsed */
struct link_msg {
	uint8_t type;
	union {
		struct {
			int64_t utc_ms;
			int16_t tz_minutes;
		} time;
		struct {
			uint32_t id;
			uint32_t offset;
		} accept;
		struct {
			uint32_t id;
			uint8_t verdict;
			uint8_t window_s; /* capped to LINK_WINDOW_MAX_S */
		} ack;
		struct {
			uint32_t id;
			uint8_t code;
		} result;
		struct {
			uint8_t key;
			const uint8_t *value; /* points into the parsed buffer */
			uint8_t len;
		} set;
		struct {
			uint8_t key;
		} get;
		struct {
			uint8_t reason;
		} bye;
	} u;
};

enum link_parse {
	LINK_PARSED,
	LINK_EMPTY,     /* nothing written */
	LINK_WRONG_WAY, /* a type the watch sends */
	LINK_UNKNOWN,   /* a type of a later version */
	LINK_SHORT,     /* fewer bytes than the fields of its type */
};

/* What the phone wrote on RX */
enum link_parse link_parse(const uint8_t *buf, size_t len, struct link_msg *out);

/*
 * What the watch sends on TX: each writer puts the message at the start of
 * `buf` and returns its size, or 0 when `room` is too small.
 */
size_t link_put_hello(uint8_t *buf, size_t room, const struct link_hello *m);
size_t link_put_note_offer(uint8_t *buf, size_t room, uint32_t id, uint32_t size, uint32_t crc32);
/* The type byte only: the note bytes follow it, as many as the MTU takes */
size_t link_put_note_data(uint8_t *buf, size_t room);
size_t link_put_note_end(uint8_t *buf, size_t room, uint32_t id);
size_t link_put_value(uint8_t *buf, size_t room, uint8_t key, uint8_t status, const void *value,
		      size_t len);
size_t link_put_event(uint8_t *buf, size_t room, uint8_t gesture, uint8_t button);
size_t link_put_bye(uint8_t *buf, size_t room, uint8_t reason);

/* The counters of EF-73, as the value of LINK_KEY_COUNTERS */
size_t link_put_counters(uint8_t *buf, size_t room, const struct link_counters *c);

/* The trial of the calls as the value of LINK_KEY_DEBUG_TRIAL: format 1, then
 * the fields of struct link_trial in order; 0 when it does not fit */
size_t link_put_trial(uint8_t *buf, size_t room, const struct link_trial *t);

/* A page of the journal as the value of LINK_KEY_JOURNAL: of the `n` records
 * from index `first`, as many as `room` takes (LINK_JOURNAL_MAX at most). The
 * bytes written, 0 when not even the head fits; *put, the records put. */
size_t link_put_journal(uint8_t *buf, size_t room, uint32_t id, uint32_t first,
			const struct link_journal_record *r, size_t n, size_t *put);

/* A name the watch takes (LINK_KEY_NAME): 1 to LINK_NAME_MAX bytes of printable
 * ASCII, no space at either end */
bool link_name_valid(const uint8_t *name, size_t len);

#endif /* CB91AI_LINK_PROTO_H */
