/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Messages of the Cobalt Link service, see link_proto.h. Pure C: the host
 * tests build this file as it is.
 */

#include <string.h>

#include "link_proto.h"

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
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static uint64_t get64(const uint8_t *p)
{
	return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

/* Bytes of each message from the phone, its type included; 0: unknown */
static size_t fixed_size(uint8_t type)
{
	switch (type) {
	case LINK_TIME:
		return 11;
	case LINK_NOTE_ACCEPT:
		return 9;
	case LINK_NOTE_ACK:
		return 7;
	case LINK_RESULT:
		return 6;
	case LINK_SET: /* key, then a value of any length */
	case LINK_GET:
	case LINK_BYE_PHONE:
		return 2;
	default:
		return 0;
	}
}

enum link_parse link_parse(const uint8_t *buf, size_t len, struct link_msg *out)
{
	size_t need;

	if (len == 0) {
		return LINK_EMPTY;
	}
	out->type = buf[0];
	if ((buf[0] & LINK_TO_WATCH) == 0) {
		return LINK_WRONG_WAY;
	}
	need = fixed_size(buf[0]);
	if (need == 0) {
		return LINK_UNKNOWN;
	}
	if (len < need) {
		return LINK_SHORT;
	}
	switch (buf[0]) {
	case LINK_TIME:
		out->u.time.utc_ms = (int64_t)get64(&buf[1]);
		out->u.time.tz_minutes = (int16_t)get16(&buf[9]);
		break;
	case LINK_NOTE_ACCEPT:
		out->u.accept.id = get32(&buf[1]);
		out->u.accept.offset = get32(&buf[5]);
		break;
	case LINK_NOTE_ACK:
		out->u.ack.id = get32(&buf[1]);
		out->u.ack.verdict = buf[5];
		out->u.ack.window_s = buf[6] > LINK_WINDOW_MAX_S ? LINK_WINDOW_MAX_S : buf[6];
		break;
	case LINK_RESULT:
		out->u.result.id = get32(&buf[1]);
		out->u.result.code = buf[5];
		break;
	case LINK_SET:
		/* A value longer than a small message is cut: no key needs more */
		out->u.set.key = buf[1];
		out->u.set.value = &buf[2];
		out->u.set.len = (uint8_t)(len - 2 > LINK_SMALL_MAX - 2 ? LINK_SMALL_MAX - 2 : len - 2);
		break;
	case LINK_GET:
		out->u.get.key = buf[1];
		break;
	default: /* LINK_BYE_PHONE */
		out->u.bye.reason = buf[1];
		break;
	}
	return LINK_PARSED;
}

size_t link_put_hello(uint8_t *buf, size_t room, const struct link_hello *m)
{
	if (room < 20) {
		return 0;
	}
	buf[0] = LINK_HELLO;
	buf[1] = m->version;
	buf[2] = m->reason;
	buf[3] = m->state;
	buf[4] = m->fw_major;
	buf[5] = m->fw_minor;
	put16(&buf[6], m->fw_revision);
	buf[8] = m->notes;
	put32(&buf[9], m->note_bytes);
	put16(&buf[13], m->battery_mv);
	buf[15] = m->codecs;
	put32(&buf[16], m->watch_id);
	return 20;
}

size_t link_put_note_offer(uint8_t *buf, size_t room, uint32_t id, uint32_t size, uint32_t crc32)
{
	if (room < 13) {
		return 0;
	}
	buf[0] = LINK_NOTE_OFFER;
	put32(&buf[1], id);
	put32(&buf[5], size);
	put32(&buf[9], crc32);
	return 13;
}

size_t link_put_note_data(uint8_t *buf, size_t room)
{
	if (room < 2) { /* a data message carries at least one byte */
		return 0;
	}
	buf[0] = LINK_NOTE_DATA;
	return 1;
}

size_t link_put_note_end(uint8_t *buf, size_t room, uint32_t id)
{
	if (room < 5) {
		return 0;
	}
	buf[0] = LINK_NOTE_END;
	put32(&buf[1], id);
	return 5;
}

size_t link_put_value(uint8_t *buf, size_t room, uint8_t key, uint8_t status, const void *value,
		      size_t len)
{
	if (room < 3 + len) {
		return 0;
	}
	buf[0] = LINK_VALUE;
	buf[1] = key;
	buf[2] = status;
	if (len > 0) {
		memcpy(&buf[3], value, len);
	}
	return 3 + len;
}

size_t link_put_event(uint8_t *buf, size_t room, uint8_t gesture, uint8_t button)
{
	if (room < 3) {
		return 0;
	}
	buf[0] = LINK_EVENT;
	buf[1] = gesture;
	buf[2] = button;
	return 3;
}

size_t link_put_bye(uint8_t *buf, size_t room, uint8_t reason)
{
	if (room < 2) {
		return 0;
	}
	buf[0] = LINK_BYE_WATCH;
	buf[1] = reason;
	return 2;
}

size_t link_put_counters(uint8_t *buf, size_t room, const struct link_counters *c)
{
	if (room < LINK_COUNTERS_SIZE) {
		return 0;
	}
	put32(&buf[0], c->notes);
	put32(&buf[4], c->recorded_s);
	put32(&buf[8], c->connected_s);
	put16(&buf[12], c->boots);
	put16(&buf[14], c->watchdog_resets);
	put16(&buf[16], c->events_lost);
	return LINK_COUNTERS_SIZE;
}

size_t link_put_trial(uint8_t *buf, size_t room, const struct link_trial *t)
{
	if (room < LINK_TRIAL_SIZE) {
		return 0;
	}
	buf[0] = 1; /* format */
	put16(&buf[1], t->minutes_left);
	buf[3] = t->profile;
	put32(&buf[4], t->number);
	buf[8] = t->reason;
	put32(&buf[9], t->connect_ms);
	put32(&buf[13], t->read_ms);
	put16(&buf[17], t->interval);
	put16(&buf[19], t->latency);
	put16(&buf[21], t->timeout);
	return LINK_TRIAL_SIZE;
}

size_t link_put_journal(uint8_t *buf, size_t room, uint32_t id, uint32_t first,
			const struct link_journal_record *r, size_t n, size_t *put)
{
	size_t len = LINK_JOURNAL_HEAD;
	size_t fit;

	*put = 0;
	if (room < LINK_JOURNAL_HEAD) {
		return 0;
	}
	fit = (room - LINK_JOURNAL_HEAD) / LINK_JOURNAL_RECORD;
	fit = fit < LINK_JOURNAL_MAX ? fit : LINK_JOURNAL_MAX;
	n = n < fit ? n : fit;
	put32(&buf[0], id);
	put32(&buf[4], first);
	buf[8] = (uint8_t)n;
	for (size_t i = 0; i < n; i++) {
		put32(&buf[len], r[i].time_s);
		put16(&buf[len + 4U], (uint16_t)r[i].temp_cc);
		put16(&buf[len + 6U], r[i].cell);
		len += LINK_JOURNAL_RECORD;
	}
	*put = n;
	return len;
}

bool link_name_valid(const uint8_t *name, size_t len)
{
	if (len == 0 || len > LINK_NAME_MAX || name[0] == ' ' || name[len - 1] == ' ') {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (name[i] < 0x20 || name[i] > 0x7e) {
			return false;
		}
	}
	return true;
}
