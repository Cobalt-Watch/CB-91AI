/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests of a Cobalt Link session on the watch side
 * (firmware/watch/src/session.c, lot D6). The tests play the phone with the
 * messages of lib/link_proto.h, and a note store of made-up notes.
 */

#include <string.h>

#include "harness.h"
#include "link_proto.h"
#include "session.h"

#define MTU_ROOM 244 /* an MTU of 247, less the 3 bytes of a notification */

/* ---- A note store and settings ---------------------------------------------- */

struct store {
	struct session_note notes[4];
	size_t count;
	uint8_t format; /* the time format setting, 12 or 24 */
	unsigned int reads;
	bool broken;    /* reads fail */
};

static uint8_t note_byte(uint32_t id, uint32_t offset)
{
	return (uint8_t)(id * 31U + offset * 7U);
}

static bool store_next(void *ctx, uint32_t after, struct session_note *note)
{
	struct store *st = ctx;

	for (size_t i = 0; i < st->count; i++) {
		if (st->notes[i].id > after) {
			*note = st->notes[i];
			return true;
		}
	}
	return false;
}

static size_t store_read(void *ctx, uint32_t id, uint32_t offset, uint8_t *buf, size_t len)
{
	struct store *st = ctx;

	st->reads++;
	if (st->broken) {
		return 0;
	}
	for (size_t i = 0; i < len; i++) {
		buf[i] = note_byte(id, offset + (uint32_t)i);
	}
	return len;
}

static uint8_t store_set(void *ctx, uint8_t key, const uint8_t *value, uint8_t len)
{
	struct store *st = ctx;

	if (key != LINK_KEY_TIME_FORMAT) {
		return key == LINK_KEY_COUNTERS ? LINK_VALUE_READ_ONLY : LINK_VALUE_UNKNOWN;
	}
	if (len < 1 || (value[0] != 12 && value[0] != 24)) {
		return LINK_VALUE_REFUSED;
	}
	st->format = value[0];
	return LINK_VALUE_OK;
}

static size_t store_get(void *ctx, uint8_t key, uint8_t *out, size_t room, uint8_t *status)
{
	struct store *st = ctx;

	if (key == LINK_KEY_TIME_FORMAT) {
		out[0] = st->format;
		*status = LINK_VALUE_OK;
		return 1;
	}
	if (key == LINK_KEY_STATUS) {
		/* A status line of 200 characters, as long as the watch's get */
		const size_t n = room < 200 ? room : 200;

		for (size_t i = 0; i < n; i++) {
			out[i] = (uint8_t)('a' + i % 26U);
		}
		*status = LINK_VALUE_OK;
		return n;
	}
	*status = LINK_VALUE_UNKNOWN;
	return 0;
}

static struct session_io io_of(struct store *st)
{
	const struct session_io io = { st, store_next, store_read, store_set, store_get };

	return io;
}

/* ---- The phone -------------------------------------------------------------- */

struct phone {
	uint8_t last[MTU_ROOM];
	size_t last_len;
	unsigned int messages;
	uint32_t data_bytes;
	uint32_t data_start; /* offset of the first data byte seen, for the resume */
	bool data_ok;        /* every data byte was the one expected */
	uint32_t expect_id;
	uint32_t offset;
	uint8_t types[64];
};

/* Everything the watch has to send now, as the glue would send it */
static void drain(struct session *s, struct phone *p, uint32_t now)
{
	uint8_t buf[MTU_ROOM];
	size_t len;

	while ((len = session_next_tx(s, buf, sizeof(buf), now)) > 0) {
		if (p->messages < sizeof(p->types)) {
			p->types[p->messages] = buf[0];
		}
		p->messages++;
		memcpy(p->last, buf, len);
		p->last_len = len;
		if (buf[0] == LINK_NOTE_DATA) {
			for (size_t i = 1; i < len; i++) {
				if (buf[i] != note_byte(p->expect_id, p->offset)) {
					p->data_ok = false;
				}
				p->offset++;
			}
			p->data_bytes += (uint32_t)(len - 1);
		}
	}
}

static uint32_t say(struct session *s, const uint8_t *msg, size_t len, uint32_t now)
{
	return session_rx(s, msg, len, now);
}

static const struct link_hello hello = { .version = LINK_PROTO_VERSION, .reason = 1 };

static uint32_t count_type(const struct phone *p, uint8_t type)
{
	uint32_t n = 0;

	for (unsigned int i = 0; i < p->messages && i < sizeof(p->types); i++) {
		n += p->types[i] == type;
	}
	return n;
}

/* ---- Sessions ------------------------------------------------------------------ */

static void no_note_then_bye(void)
{
	struct store st = { .format = 12 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true };
	const uint8_t time[] = { LINK_TIME, 0xa0, 0x41, 0xe8, 0xcf, 0xa0, 0x01, 0, 0, 0x78, 0 };
	uint32_t fx, wait;

	fx = session_open(&s, &io, &hello, 1000);
	CHECK(fx & SESSION_FX_SEND);
	drain(&s, &p, 1000);
	CHECK_EQ(p.messages, 1);
	CHECK_EQ(p.types[0], LINK_HELLO);
	CHECK_EQ(p.last_len, LINK_SMALL_MAX);
	/* The phone gives the time: the clock is set, and the watch waits 2 s more */
	fx = say(&s, time, sizeof(time), 1100);
	CHECK(fx & SESSION_FX_TIME);
	CHECK_EQ(s.time_utc_ms, 1790194500000LL);
	CHECK_EQ(s.time_tz, 120);
	CHECK(session_next_ms(&s, 1100, &wait));
	CHECK_EQ(wait, SESSION_LINGER_MS);
	CHECK_EQ(session_tick(&s, 3099), 0);
	fx = session_tick(&s, 3100);
	CHECK_EQ(fx, SESSION_FX_SEND | SESSION_FX_CLOSE);
	drain(&s, &p, 3100);
	CHECK_EQ(p.types[1], LINK_BYE_WATCH);
	CHECK_EQ(p.last[1], LINK_BYE_DONE);
	CHECK(session_closing(&s));
}

static void one_note_whole(void)
{
	struct store st = { .notes = { { 7, 1000, 0x1234 } }, .count = 1 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true, .expect_id = 7 };
	const uint8_t accept[] = { LINK_NOTE_ACCEPT, 7, 0, 0, 0, 0, 0, 0, 0 };
	const uint8_t ack[] = { LINK_NOTE_ACK, 7, 0, 0, 0, LINK_ACK_RECEIVED, 20 };
	const uint8_t result[] = { LINK_RESULT, 7, 0, 0, 0, LINK_RESULT_SUCCESS };
	uint32_t fx;

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	/* HELLO, then the offer of note 7: 1000 bytes, CRC 0x1234 */
	CHECK_EQ(p.messages, 2);
	CHECK_EQ(p.types[1], LINK_NOTE_OFFER);
	CHECK_EQ(p.last[5] | (p.last[6] << 8), 1000);
	CHECK_EQ(s.state, SESSION_OFFERED);
	fx = say(&s, accept, sizeof(accept), 200);
	CHECK_EQ(fx, SESSION_FX_SEND);
	drain(&s, &p, 250);
	/* 243 bytes per notification: five of them, then NOTE_END */
	CHECK_EQ(p.data_bytes, 1000);
	CHECK(p.data_ok);
	CHECK_EQ(count_type(&p, LINK_NOTE_DATA), 5);
	CHECK_EQ(p.last[0], LINK_NOTE_END);
	CHECK_EQ(s.state, SESSION_ACK_WAIT);
	/* Received, a verdict within 20 s */
	fx = say(&s, ack, sizeof(ack), 900);
	CHECK(fx & SESSION_FX_DELIVERED);
	CHECK_EQ(s.delivered_id, 7);
	CHECK_EQ(s.state, SESSION_RESULT);
	fx = say(&s, result, sizeof(result), 4000);
	CHECK(fx & SESSION_FX_RESULT);
	CHECK_EQ(s.result_code, LINK_RESULT_SUCCESS);
	CHECK_EQ(s.state, SESSION_LINGER);
	CHECK_EQ(session_tick(&s, 6000), SESSION_FX_SEND | SESSION_FX_CLOSE);
	drain(&s, &p, 6000);
	CHECK_EQ(p.last[0], LINK_BYE_WATCH);
	CHECK_EQ(s.refused, 0);
}

static void resume_at_an_offset(void)
{
	struct store st = { .notes = { { 9, 5000, 1 } }, .count = 1 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true, .expect_id = 9, .offset = 4096 };
	/* The phone had 4096 bytes of it from an earlier session */
	const uint8_t accept[] = { LINK_NOTE_ACCEPT, 9, 0, 0, 0, 0x00, 0x10, 0, 0 };

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	say(&s, accept, sizeof(accept), 100);
	drain(&s, &p, 100);
	CHECK_EQ(p.data_bytes, 5000 - 4096);
	CHECK(p.data_ok);
	CHECK_EQ(p.last[0], LINK_NOTE_END);
}

static void phone_had_it_already(void)
{
	struct store st = { .notes = { { 3, 800, 1 }, { 4, 500, 2 } }, .count = 2 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true, .expect_id = 4 };
	/* Its acknowledgement had got lost: it acknowledges at once, no window */
	const uint8_t ack3[] = { LINK_NOTE_ACK, 3, 0, 0, 0, LINK_ACK_RECEIVED, 0 };
	uint32_t fx;

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	fx = say(&s, ack3, sizeof(ack3), 100);
	CHECK(fx & SESSION_FX_DELIVERED);
	CHECK_EQ(s.delivered_id, 3);
	/* The next note is offered, no data of note 3 was sent */
	drain(&s, &p, 100);
	CHECK_EQ(p.data_bytes, 0);
	CHECK_EQ(p.last[0], LINK_NOTE_OFFER);
	CHECK_EQ(p.last[1], 4);
}

static void damaged_note_waits_for_next_session(void)
{
	struct store st = { .notes = { { 5, 300, 1 } }, .count = 1 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true, .expect_id = 5 };
	const uint8_t accept[] = { LINK_NOTE_ACCEPT, 5, 0, 0, 0, 0, 0, 0, 0 };
	const uint8_t damaged[] = { LINK_NOTE_ACK, 5, 0, 0, 0, LINK_ACK_DAMAGED, 20 };
	uint32_t fx;

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	say(&s, accept, sizeof(accept), 100);
	drain(&s, &p, 100);
	fx = say(&s, damaged, sizeof(damaged), 200);
	/* Not delivered, not offered again in this session: linger, then BYE */
	CHECK_EQ(fx & SESSION_FX_DELIVERED, 0);
	CHECK_EQ(s.state, SESSION_LINGER);
	drain(&s, &p, 200);
	CHECK_EQ(count_type(&p, LINK_NOTE_OFFER), 1);
	CHECK_EQ(session_tick(&s, 2200), SESSION_FX_SEND | SESSION_FX_CLOSE);
}

static void silent_phone_is_left(void)
{
	struct store st = { .notes = { { 1, 100, 1 } }, .count = 1 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true };

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	/* No answer to the offer for 5 s */
	CHECK_EQ(session_tick(&s, 4999), 0);
	CHECK_EQ(session_tick(&s, 5000), SESSION_FX_SEND | SESSION_FX_CLOSE);
	drain(&s, &p, 5000);
	CHECK_EQ(p.last[0], LINK_BYE_WATCH);
	CHECK_EQ(p.last[1], LINK_BYE_IDLE);
}

static void verdict_after_its_window_is_ignored(void)
{
	struct store st = { .notes = { { 2, 10, 1 } }, .count = 1 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true, .expect_id = 2 };
	const uint8_t accept[] = { LINK_NOTE_ACCEPT, 2, 0, 0, 0, 0, 0, 0, 0 };
	const uint8_t ack[] = { LINK_NOTE_ACK, 2, 0, 0, 0, LINK_ACK_RECEIVED, 3 };
	const uint8_t result[] = { LINK_RESULT, 2, 0, 0, 0, LINK_RESULT_FAILURE };

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	say(&s, accept, sizeof(accept), 100);
	drain(&s, &p, 100);
	say(&s, ack, sizeof(ack), 200);
	/* The window closes at 3.2 s: linger, no LED */
	CHECK_EQ(session_tick(&s, 3200), 0);
	CHECK_EQ(s.state, SESSION_LINGER);
	CHECK_EQ(say(&s, result, sizeof(result), 3300) & SESSION_FX_RESULT, 0);
	CHECK_EQ(s.refused, 1);
}

/* E1: a note kept while the phone is there goes in the same session */
static void a_note_kept_during_the_session(void)
{
	struct store st = { 0 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true, .expect_id = 12 };
	const uint8_t accept[] = { LINK_NOTE_ACCEPT, 12, 0, 0, 0, 0, 0, 0, 0 };
	const uint8_t ack[] = { LINK_NOTE_ACK, 12, 0, 0, 0, LINK_ACK_RECEIVED, 0 };
	uint32_t fx;

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	CHECK_EQ(s.state, SESSION_LINGER);
	/* Kept at 1.5 s, before the BYE */
	st.notes[0] = (struct session_note){ 12, 300, 5 };
	st.count = 1;
	fx = session_note_added(&s, 1500);
	CHECK_EQ(fx, SESSION_FX_SEND);
	CHECK_EQ(s.state, SESSION_OFFERED);
	drain(&s, &p, 1500);
	CHECK_EQ(p.last[0], LINK_NOTE_OFFER);
	CHECK_EQ(p.last[1], 12);
	/* The offer waits 5 s for its answer, not what was left of the 2 s */
	CHECK_EQ(session_tick(&s, 3600), 0);
	say(&s, accept, sizeof(accept), 3600);
	drain(&s, &p, 3600);
	CHECK_EQ(p.data_bytes, 300);
	CHECK(p.data_ok);
	fx = say(&s, ack, sizeof(ack), 3700);
	CHECK(fx & SESSION_FX_DELIVERED);
	CHECK_EQ(s.delivered_id, 12);
	CHECK_EQ(s.state, SESSION_LINGER);
	CHECK_EQ(s.refused, 0);
}

static void a_note_kept_while_one_is_sent(void)
{
	struct store st = { .notes = { { 3, 800, 1 } }, .count = 1 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true, .expect_id = 3 };
	const uint8_t accept[] = { LINK_NOTE_ACCEPT, 3, 0, 0, 0, 0, 0, 0, 0 };
	const uint8_t ack[] = { LINK_NOTE_ACK, 3, 0, 0, 0, LINK_ACK_RECEIVED, 0 };

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	say(&s, accept, sizeof(accept), 100);
	/* Kept while note 3 goes: it waits its turn */
	st.notes[1] = (struct session_note){ 4, 500, 2 };
	st.count = 2;
	CHECK_EQ(session_note_added(&s, 150), 0);
	CHECK_EQ(s.state, SESSION_SENDING);
	drain(&s, &p, 200);
	CHECK_EQ(p.data_bytes, 800);
	say(&s, ack, sizeof(ack), 300);
	drain(&s, &p, 300);
	CHECK_EQ(p.last[0], LINK_NOTE_OFFER);
	CHECK_EQ(p.last[1], 4);
	CHECK_EQ(count_type(&p, LINK_NOTE_OFFER), 2);
}

static void a_note_kept_while_a_verdict_is_awaited(void)
{
	struct store st = { .notes = { { 2, 100, 1 } }, .count = 1 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true, .expect_id = 2 };
	const uint8_t accept2[] = { LINK_NOTE_ACCEPT, 2, 0, 0, 0, 0, 0, 0, 0 };
	const uint8_t ack2[] = { LINK_NOTE_ACK, 2, 0, 0, 0, LINK_ACK_RECEIVED, 20 };
	const uint8_t result2[] = { LINK_RESULT, 2, 0, 0, 0, LINK_RESULT_SUCCESS };
	const uint8_t accept3[] = { LINK_NOTE_ACCEPT, 3, 0, 0, 0, 0, 0, 0, 0 };
	const uint8_t ack3[] = { LINK_NOTE_ACK, 3, 0, 0, 0, LINK_ACK_RECEIVED, 0 };
	uint32_t fx;

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	say(&s, accept2, sizeof(accept2), 100);
	drain(&s, &p, 100);
	say(&s, ack2, sizeof(ack2), 200);
	CHECK_EQ(s.state, SESSION_RESULT);
	/* Kept while the verdict of note 2 is awaited: offered at once */
	st.notes[1] = (struct session_note){ 3, 50, 2 };
	st.count = 2;
	CHECK_EQ(session_note_added(&s, 1000), SESSION_FX_SEND);
	CHECK_EQ(s.state, SESSION_OFFERED);
	drain(&s, &p, 1000);
	CHECK_EQ(p.last[1], 3);
	/* The verdict of note 2 comes during the offer of note 3: still taken */
	fx = say(&s, result2, sizeof(result2), 1200);
	CHECK(fx & SESSION_FX_RESULT);
	CHECK_EQ(s.state, SESSION_OFFERED);
	p.expect_id = 3;
	p.offset = 0;
	say(&s, accept3, sizeof(accept3), 1300);
	drain(&s, &p, 1300);
	CHECK(p.data_ok);
	fx = say(&s, ack3, sizeof(ack3), 1400);
	CHECK(fx & SESSION_FX_DELIVERED);
	CHECK_EQ(s.state, SESSION_LINGER);
	CHECK_EQ(s.refused, 0);
}

static void a_note_kept_during_an_update(void)
{
	struct store st = { 0 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true, .expect_id = 4 };
	const uint8_t accept[] = { LINK_NOTE_ACCEPT, 4, 0, 0, 0, 0, 0, 0, 0 };

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	session_busy(&s, 500, SESSION_BUSY_MS); /* SMP in the link */
	st.notes[0] = (struct session_note){ 4, 300, 7 };
	st.count = 1;
	CHECK_EQ(session_note_added(&s, 1000), SESSION_FX_SEND);
	drain(&s, &p, 1000);
	CHECK_EQ(p.last[0], LINK_NOTE_OFFER);
	/* The phone, busy with the update, answers late: the watch waits for it */
	CHECK_EQ(session_tick(&s, 6000), 0);
	CHECK_EQ(s.state, SESSION_OFFERED);
	say(&s, accept, sizeof(accept), 20000);
	drain(&s, &p, 20000);
	CHECK_EQ(p.data_bytes, 300);
	CHECK(p.data_ok);
	CHECK_EQ(p.last[0], LINK_NOTE_END);
}

static void a_note_kept_in_other_states(void)
{
	struct store st = { .notes = { { 5, 300, 1 } }, .count = 1 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true, .expect_id = 5 };
	const uint8_t accept[] = { LINK_NOTE_ACCEPT, 5, 0, 0, 0, 0, 0, 0, 0 };
	const uint8_t damaged[] = { LINK_NOTE_ACK, 5, 0, 0, 0, LINK_ACK_DAMAGED, 0 };
	const uint8_t bye[] = { LINK_BYE_PHONE, LINK_BYE_DONE };

	/* No session: nothing */
	memset(&s, 0, sizeof(s));
	CHECK_EQ(session_note_added(&s, 0), 0);
	/* A damaged note is not offered again when another one comes */
	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	say(&s, accept, sizeof(accept), 100);
	drain(&s, &p, 100);
	say(&s, damaged, sizeof(damaged), 200);
	CHECK_EQ(s.state, SESSION_LINGER);
	st.notes[1] = (struct session_note){ 6, 100, 2 };
	st.count = 2;
	CHECK_EQ(session_note_added(&s, 300), SESSION_FX_SEND);
	drain(&s, &p, 300);
	CHECK_EQ(p.last[0], LINK_NOTE_OFFER);
	CHECK_EQ(p.last[1], 6);
	/* The phone said BYE: nothing more */
	say(&s, bye, sizeof(bye), 400);
	st.notes[2] = (struct session_note){ 7, 100, 3 };
	st.count = 3;
	CHECK_EQ(session_note_added(&s, 500), 0);
	CHECK(session_closing(&s));
}

static void settings_and_values(void)
{
	struct store st = { .format = 12 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true };
	const uint8_t set24[] = { LINK_SET, LINK_KEY_TIME_FORMAT, 24 };
	const uint8_t set13[] = { LINK_SET, LINK_KEY_TIME_FORMAT, 13 };
	const uint8_t get_unknown[] = { LINK_GET, 0x7e };
	const uint8_t set_counters[] = { LINK_SET, LINK_KEY_COUNTERS, 0 };

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	CHECK(say(&s, set24, sizeof(set24), 100) & SESSION_FX_SEND);
	drain(&s, &p, 100);
	/* VALUE: key, status, the value now in force */
	CHECK_EQ(p.last_len, 4);
	CHECK_EQ(p.last[0], LINK_VALUE);
	CHECK_EQ(p.last[2], LINK_VALUE_OK);
	CHECK_EQ(p.last[3], 24);
	CHECK_EQ(st.format, 24);
	say(&s, set13, sizeof(set13), 200);
	drain(&s, &p, 200);
	CHECK_EQ(p.last[2], LINK_VALUE_REFUSED);
	CHECK_EQ(p.last[3], 24); /* unchanged */
	say(&s, get_unknown, sizeof(get_unknown), 300);
	drain(&s, &p, 300);
	CHECK_EQ(p.last_len, 3);
	CHECK_EQ(p.last[2], LINK_VALUE_UNKNOWN);
	say(&s, set_counters, sizeof(set_counters), 400);
	drain(&s, &p, 400);
	CHECK_EQ(p.last[2], LINK_VALUE_READ_ONLY);
	/* Each word of the phone restarted the 2 s of the end */
	CHECK_EQ(session_tick(&s, 2399), 0);
	CHECK_EQ(session_tick(&s, 2400), SESSION_FX_SEND | SESSION_FX_CLOSE);
}

static void held_session_sends_gestures(void)
{
	struct store st = { .format = 12 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true };
	const uint8_t hold[] = { LINK_SET, LINK_KEY_HOLD, 1 };
	const uint8_t bye[] = { LINK_BYE_PHONE, LINK_BYE_DONE };

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	/* No EVENT before the phone asks for a held session */
	CHECK_EQ(session_event(&s, 1, 2), 0);
	say(&s, hold, sizeof(hold), 100);
	drain(&s, &p, 100);
	CHECK(s.held);
	CHECK_EQ(p.last[3], 1);
	/* No BYE of its own after 2 s */
	CHECK_EQ(session_tick(&s, 60000), 0);
	CHECK_EQ(session_event(&s, 2, 2), SESSION_FX_SEND);
	drain(&s, &p, 60000);
	CHECK_EQ(p.last[0], LINK_EVENT);
	CHECK_EQ(p.last[1], 2);
	/* The phone's BYE: cut at once */
	CHECK_EQ(say(&s, bye, sizeof(bye), 61000), SESSION_FX_CLOSE);
	CHECK(session_closing(&s));
}

static void held_session_ends_after_five_minutes(void)
{
	struct store st = { .format = 12 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true };
	const uint8_t hold[] = { LINK_SET, LINK_KEY_HOLD, 1 };

	session_open(&s, &io, &hello, 0);
	say(&s, hold, sizeof(hold), 0);
	drain(&s, &p, 0);
	CHECK_EQ(session_tick(&s, SESSION_HOLD_MS - 1), 0);
	CHECK_EQ(session_tick(&s, SESSION_HOLD_MS), SESSION_FX_SEND | SESSION_FX_CLOSE);
	drain(&s, &p, SESSION_HOLD_MS);
	CHECK_EQ(p.last[1], LINK_BYE_IDLE);
}

static void update_in_the_link_keeps_the_session(void)
{
	struct store st = { .format = 12 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true };
	const uint8_t get[] = { LINK_GET, LINK_KEY_TIME_FORMAT };
	uint32_t wait;

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	CHECK_EQ(s.state, SESSION_LINGER);
	/* An SMP command at 1 s: the 2 s of the end wait for the update */
	session_busy(&s, 1000, SESSION_BUSY_MS);
	CHECK_EQ(session_tick(&s, 2000), 0);
	CHECK(session_next_ms(&s, 2000, &wait));
	CHECK_EQ(wait, 1000 + SESSION_BUSY_MS - 2000);
	/* Another command pushes it back; a shorter wait never brings it forward */
	session_busy(&s, 20000, SESSION_BUSY_MS);
	session_busy(&s, 21000, 1000);
	CHECK_EQ(session_tick(&s, 1000 + SESSION_BUSY_MS), 0);
	/* A word of the phone meanwhile changes nothing to it */
	say(&s, get, sizeof(get), 35000);
	drain(&s, &p, 35000);
	CHECK_EQ(session_tick(&s, 37000), 0);
	CHECK_EQ(session_tick(&s, 20000 + SESSION_BUSY_MS - 1), 0);
	CHECK_EQ(session_tick(&s, 20000 + SESSION_BUSY_MS), SESSION_FX_SEND | SESSION_FX_CLOSE);
	drain(&s, &p, 20000 + SESSION_BUSY_MS);
	CHECK_EQ(p.last[0], LINK_BYE_WATCH);
	CHECK_EQ(p.last[1], LINK_BYE_DONE);
	CHECK(!s.busy);
}

static void update_in_the_link_holds_an_offer(void)
{
	struct store st = { .notes = { { 1, 100, 1 } }, .count = 1 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true };
	const uint8_t bye[] = { LINK_BYE_PHONE, LINK_BYE_DONE };

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	session_busy(&s, 100, SESSION_BUSY_MS);
	/* The 5 s of an unanswered offer wait too */
	CHECK_EQ(session_tick(&s, 5000), 0);
	CHECK_EQ(s.state, SESSION_OFFERED);
	CHECK_EQ(session_tick(&s, 100 + SESSION_BUSY_MS), SESSION_FX_SEND | SESSION_FX_CLOSE);
	drain(&s, &p, 100 + SESSION_BUSY_MS);
	CHECK_EQ(p.last[1], LINK_BYE_IDLE);

	/* The phone's BYE ends it at once, busy or not */
	session_closed(&s);
	session_open(&s, &io, &hello, 0);
	session_busy(&s, 0, SESSION_BUSY_MS);
	CHECK_EQ(say(&s, bye, sizeof(bye), 10), SESSION_FX_CLOSE);
	CHECK(session_closing(&s));
	/* Nothing to hold once the link is gone */
	session_closed(&s);
	session_busy(&s, 20, SESSION_BUSY_MS);
	CHECK(!s.busy);
	CHECK(!session_active(&s));
}

static void update_in_the_link_holds_the_acknowledgement(void)
{
	struct store st = { .notes = { { 4, 300, 1 } }, .count = 1 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true, .expect_id = 4 };
	const uint8_t accept[] = { LINK_NOTE_ACCEPT, 4, 0, 0, 0, 0, 0, 0, 0 };

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	say(&s, accept, sizeof(accept), 100);
	drain(&s, &p, 100);
	CHECK_EQ(s.state, SESSION_ACK_WAIT);
	/* The phone updates the watch before it acknowledges: the 5 s wait */
	session_busy(&s, 200, SESSION_BUSY_MS);
	CHECK_EQ(session_tick(&s, 5100), 0);
	CHECK_EQ(s.state, SESSION_ACK_WAIT);
	CHECK_EQ(session_tick(&s, 200 + SESSION_BUSY_MS), SESSION_FX_SEND | SESSION_FX_CLOSE);
	drain(&s, &p, 200 + SESSION_BUSY_MS);
	CHECK_EQ(p.last[1], LINK_BYE_IDLE);
}

static void update_in_the_link_after_the_verdict_window(void)
{
	struct store st = { .notes = { { 2, 10, 1 } }, .count = 1 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true, .expect_id = 2 };
	const uint8_t accept[] = { LINK_NOTE_ACCEPT, 2, 0, 0, 0, 0, 0, 0, 0 };
	const uint8_t ack[] = { LINK_NOTE_ACK, 2, 0, 0, 0, LINK_ACK_RECEIVED, 3 };

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	say(&s, accept, sizeof(accept), 100);
	drain(&s, &p, 100);
	say(&s, ack, sizeof(ack), 200);
	CHECK_EQ(s.state, SESSION_RESULT);
	session_busy(&s, 300, SESSION_BUSY_MS);
	/* The window of the verdict still closes at 3.2 s: no LED after it */
	CHECK_EQ(session_tick(&s, 3200), 0);
	CHECK_EQ(s.state, SESSION_LINGER);
	CHECK_EQ(s.result_id, 0);
	/* The end waits for the update */
	CHECK_EQ(session_tick(&s, 5200), 0);
	CHECK_EQ(session_tick(&s, 300 + SESSION_BUSY_MS), SESSION_FX_SEND | SESSION_FX_CLOSE);
}

static void update_in_a_held_session(void)
{
	struct store st = { .format = 12 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true };
	const uint8_t hold[] = { LINK_SET, LINK_KEY_HOLD, 1 };

	session_open(&s, &io, &hello, 0);
	say(&s, hold, sizeof(hold), 0);
	drain(&s, &p, 0);
	/* Busy for 30 s changes nothing to the five minutes of a held session */
	session_busy(&s, 1000, SESSION_BUSY_MS);
	CHECK_EQ(session_tick(&s, 1000 + SESSION_BUSY_MS), 0);
	CHECK_EQ(session_tick(&s, SESSION_HOLD_MS - 1), 0);
	CHECK_EQ(session_tick(&s, SESSION_HOLD_MS), SESSION_FX_SEND | SESSION_FX_CLOSE);
	/* ... nor does an update that ends after them: it holds the end */
	session_closed(&s);
	session_open(&s, &io, &hello, 0);
	say(&s, hold, sizeof(hold), 0);
	session_busy(&s, SESSION_HOLD_MS - 1000, SESSION_BUSY_MS);
	CHECK_EQ(session_tick(&s, SESSION_HOLD_MS), 0);
	CHECK_EQ(session_tick(&s, SESSION_HOLD_MS - 1000 + SESSION_BUSY_MS),
		 SESSION_FX_SEND | SESSION_FX_CLOSE);
}

static void update_begun_before_the_session(void)
{
	struct store st = { .format = 12 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true };

	/* The glue applies what is left of an SMP wait as the session opens */
	session_open(&s, &io, &hello, 10000);
	session_busy(&s, 10000, 12000);
	drain(&s, &p, 10000);
	CHECK_EQ(session_tick(&s, 12000), 0);
	CHECK_EQ(session_tick(&s, 21999), 0);
	CHECK_EQ(session_tick(&s, 22000), SESSION_FX_SEND | SESSION_FX_CLOSE);
}

static void a_long_value_goes_whole(void)
{
	struct store st = { .format = 12 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true };
	const uint8_t get[] = { LINK_GET, LINK_KEY_STATUS };
	uint8_t buf[MTU_ROOM];

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	/* The status line whole: key, status, 200 characters (cut at 64 before) */
	say(&s, get, sizeof(get), 100);
	drain(&s, &p, 100);
	CHECK_EQ(p.last_len, 203);
	CHECK_EQ(p.last[0], LINK_VALUE);
	CHECK_EQ(p.last[202], 'a' + 199 % 26);
	/* A small MTU cuts it to the room of a notification */
	say(&s, get, sizeof(get), 200);
	CHECK_EQ(session_next_tx(&s, buf, 20, 200), 20);
	CHECK_EQ(s.refused, 0);
}

static void unreadable_note_ends_the_session(void)
{
	struct store st = { .notes = { { 8, 600, 1 } }, .count = 1, .broken = true };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true, .expect_id = 8 };
	const uint8_t accept[] = { LINK_NOTE_ACCEPT, 8, 0, 0, 0, 0, 0, 0, 0 };

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	say(&s, accept, sizeof(accept), 100);
	drain(&s, &p, 100);
	CHECK_EQ(p.data_bytes, 0);
	CHECK_EQ(p.last[0], LINK_BYE_WATCH);
	CHECK_EQ(p.last[1], LINK_BYE_ERROR);
	CHECK(session_closing(&s));
}

static void out_of_turn_and_malformed(void)
{
	struct store st = { .notes = { { 6, 100, 1 } }, .count = 1 };
	const struct session_io io = io_of(&st);
	struct session s;
	struct phone p = { .data_ok = true };
	const uint8_t wrong_id[] = { LINK_NOTE_ACCEPT, 99, 0, 0, 0, 0, 0, 0, 0 };
	const uint8_t past_end[] = { LINK_NOTE_ACCEPT, 6, 0, 0, 0, 0xff, 0, 0, 0 };
	const uint8_t early_ack[] = { LINK_NOTE_ACK, 7, 0, 0, 0, 0, 0 };
	const uint8_t from_watch[] = { LINK_HELLO, 1 };
	const uint8_t short_ack[] = { LINK_NOTE_ACK, 6 };
	const uint8_t stray_result[] = { LINK_RESULT, 6, 0, 0, 0, 0 };

	session_open(&s, &io, &hello, 0);
	drain(&s, &p, 0);
	say(&s, wrong_id, sizeof(wrong_id), 10);
	say(&s, past_end, sizeof(past_end), 20);
	say(&s, early_ack, sizeof(early_ack), 30);
	say(&s, from_watch, sizeof(from_watch), 40);
	say(&s, short_ack, sizeof(short_ack), 50);
	say(&s, stray_result, sizeof(stray_result), 60);
	CHECK_EQ(s.refused, 6);
	/* Still waiting for a right answer to the offer */
	CHECK_EQ(s.state, SESSION_OFFERED);
}

static void nothing_after_the_link_is_gone(void)
{
	struct store st = { .format = 12 };
	const struct session_io io = io_of(&st);
	struct session s;
	const uint8_t get[] = { LINK_GET, LINK_KEY_TIME_FORMAT };
	uint8_t buf[MTU_ROOM];
	uint32_t wait;

	session_open(&s, &io, &hello, 0);
	session_closed(&s);
	CHECK(!session_active(&s));
	CHECK_EQ(say(&s, get, sizeof(get), 10), 0);
	CHECK_EQ(session_next_tx(&s, buf, sizeof(buf), 10), 0);
	CHECK(!session_next_ms(&s, 10, &wait));
}

int main(void)
{
	RUN(no_note_then_bye);
	RUN(one_note_whole);
	RUN(resume_at_an_offset);
	RUN(phone_had_it_already);
	RUN(damaged_note_waits_for_next_session);
	RUN(silent_phone_is_left);
	RUN(verdict_after_its_window_is_ignored);
	RUN(a_note_kept_during_the_session);
	RUN(a_note_kept_while_one_is_sent);
	RUN(a_note_kept_while_a_verdict_is_awaited);
	RUN(a_note_kept_during_an_update);
	RUN(a_note_kept_in_other_states);
	RUN(settings_and_values);
	RUN(held_session_sends_gestures);
	RUN(held_session_ends_after_five_minutes);
	RUN(update_in_the_link_keeps_the_session);
	RUN(update_in_the_link_holds_an_offer);
	RUN(update_in_the_link_holds_the_acknowledgement);
	RUN(update_in_the_link_after_the_verdict_window);
	RUN(update_in_a_held_session);
	RUN(update_begun_before_the_session);
	RUN(a_long_value_goes_whole);
	RUN(unreadable_note_ends_the_session);
	RUN(out_of_turn_and_malformed);
	RUN(nothing_after_the_link_is_gone);
	return harness_report("session");
}
