/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * A Cobalt Link session on the watch side, see session.h. Pure C: the host
 * tests build this file as it is.
 */

#include <string.h>

#include "session.h"

static void enqueue(struct session *s, const uint8_t *msg, size_t len)
{
	uint8_t slot;

	/* Sized for the longest burst (HELLO and an offer, or a reply while
	 * streaming): a full queue is a design fault, counted */
	if (len == 0 || len > SESSION_MSG_MAX || s->queue_count == SESSION_QUEUE) {
		s->refused++;
		return;
	}
	slot = (uint8_t)((s->queue_head + s->queue_count) % SESSION_QUEUE);
	memcpy(s->queue[slot], msg, len);
	s->queue_len[slot] = (uint8_t)len;
	s->queue_count++;
}

static void arm(struct session *s, uint32_t now_ms, uint32_t ms)
{
	s->timed = true;
	s->deadline = now_ms + ms;
}

static bool due(const struct session *s, uint32_t now_ms)
{
	return s->timed && (int32_t)(now_ms - s->deadline) >= 0;
}

/* Nothing left to do but hear the phone's last word */
static uint32_t linger(struct session *s, uint32_t now_ms)
{
	s->state = SESSION_LINGER;
	arm(s, now_ms, s->held ? SESSION_HOLD_MS : SESSION_LINGER_MS);
	return 0;
}

static uint32_t close_with(struct session *s, uint8_t reason)
{
	uint8_t msg[LINK_SMALL_MAX];

	enqueue(s, msg, link_put_bye(msg, sizeof(msg), reason));
	s->state = SESSION_CLOSING;
	s->timed = false;
	return SESSION_FX_SEND | SESSION_FX_CLOSE;
}

/* All notes offered: the verdict of the last one, if one may come, then linger */
static uint32_t notes_done(struct session *s, uint32_t now_ms)
{
	if (s->result_id != 0 && (int32_t)(s->result_until - now_ms) > 0) {
		s->state = SESSION_RESULT;
		s->timed = true;
		s->deadline = s->result_until;
		return 0;
	}
	s->result_id = 0;
	return linger(s, now_ms);
}

static uint32_t offer_next(struct session *s, uint32_t now_ms)
{
	uint8_t msg[LINK_SMALL_MAX];
	struct session_note note;

	if (s->io.note_next == NULL || !s->io.note_next(s->io.ctx, s->last_offered, &note)) {
		return notes_done(s, now_ms);
	}
	s->note = note;
	s->last_offered = note.id;
	s->offset = 0;
	enqueue(s, msg, link_put_note_offer(msg, sizeof(msg), note.id, note.size, note.crc32));
	s->state = SESSION_OFFERED;
	arm(s, now_ms, SESSION_IDLE_MS);
	return SESSION_FX_SEND;
}

/* VALUE for a key: `status` from a SET, or that of the reading for a GET */
static uint32_t reply_value(struct session *s, uint8_t key, bool after_set, uint8_t set_status)
{
	uint8_t value[SESSION_MSG_MAX - 3];
	uint8_t msg[SESSION_MSG_MAX];
	uint8_t status = LINK_VALUE_UNKNOWN;
	size_t n = 0;

	if (key == LINK_KEY_HOLD) {
		value[0] = s->held ? 1 : 0;
		n = 1;
		status = LINK_VALUE_OK;
	} else if (s->io.setting_get != NULL) {
		n = s->io.setting_get(s->io.ctx, key, value, sizeof(value), &status);
	}
	if (after_set) {
		status = set_status;
	}
	enqueue(s, msg, link_put_value(msg, sizeof(msg), key, status, value, n));
	return SESSION_FX_SEND;
}

static uint8_t set_hold(struct session *s, const uint8_t *value, uint8_t len)
{
	if (len < 1 || value[0] > 1) {
		return LINK_VALUE_REFUSED;
	}
	s->held = value[0] == 1;
	return LINK_VALUE_OK;
}

uint32_t session_open(struct session *s, const struct session_io *io,
		      const struct link_hello *hello, uint32_t now_ms)
{
	uint8_t msg[LINK_SMALL_MAX];

	memset(s, 0, sizeof(*s));
	s->io = *io;
	enqueue(s, msg, link_put_hello(msg, sizeof(msg), hello));
	return SESSION_FX_SEND | offer_next(s, now_ms);
}

uint32_t session_rx(struct session *s, const uint8_t *buf, size_t len, uint32_t now_ms)
{
	struct link_msg m;
	uint32_t fx = 0;
	uint8_t status;

	if (s->state == SESSION_OFF || s->state == SESSION_CLOSING) {
		return 0;
	}
	if (link_parse(buf, len, &m) != LINK_PARSED) {
		s->refused++;
		return 0;
	}
	switch (m.type) {
	case LINK_TIME:
		s->time_utc_ms = m.u.time.utc_ms;
		s->time_tz = m.u.time.tz_minutes;
		fx |= SESSION_FX_TIME;
		break;
	case LINK_SET:
		if (m.u.set.key == LINK_KEY_HOLD) {
			status = set_hold(s, m.u.set.value, m.u.set.len);
		} else if (s->io.setting_set != NULL) {
			status = s->io.setting_set(s->io.ctx, m.u.set.key, m.u.set.value, m.u.set.len);
		} else {
			status = LINK_VALUE_UNKNOWN;
		}
		fx |= reply_value(s, m.u.set.key, true, status);
		break;
	case LINK_GET:
		fx |= reply_value(s, m.u.get.key, false, 0);
		break;
	case LINK_NOTE_ACCEPT:
		if (s->state != SESSION_OFFERED || m.u.accept.id != s->note.id ||
		    m.u.accept.offset > s->note.size) {
			s->refused++;
			break;
		}
		s->offset = m.u.accept.offset;
		s->state = SESSION_SENDING;
		s->timed = false; /* paced by the link; a lost link ends the session */
		return fx | SESSION_FX_SEND;
	case LINK_NOTE_ACK:
		/* After the data, or at once when the phone had the note already */
		if ((s->state != SESSION_OFFERED && s->state != SESSION_ACK_WAIT) ||
		    m.u.ack.id != s->note.id) {
			s->refused++;
			break;
		}
		if (m.u.ack.verdict == LINK_ACK_RECEIVED) {
			s->delivered_id = m.u.ack.id;
			fx |= SESSION_FX_DELIVERED;
			s->result_id = m.u.ack.window_s > 0 ? m.u.ack.id : 0;
			s->result_until = now_ms + 1000U * m.u.ack.window_s;
		}
		/* A damaged note stays for the next session: it is not offered again */
		return fx | offer_next(s, now_ms);
	case LINK_RESULT:
		/* The verdict of the last note delivered, within its window only: a
		 * late LED says nothing any more */
		if (s->result_id == 0 || m.u.result.id != s->result_id ||
		    (int32_t)(s->result_until - now_ms) <= 0) {
			s->refused++;
			break;
		}
		s->result_code = m.u.result.code;
		s->result_id = 0;
		fx |= SESSION_FX_RESULT;
		if (s->state == SESSION_RESULT) {
			return fx | linger(s, now_ms);
		}
		break;
	default: /* LINK_BYE_PHONE */
		s->state = SESSION_CLOSING;
		s->timed = false;
		return fx | SESSION_FX_CLOSE;
	}
	/* Any word of the phone: it is there, the wait starts again */
	if (s->state == SESSION_OFFERED || s->state == SESSION_ACK_WAIT) {
		arm(s, now_ms, SESSION_IDLE_MS);
	} else if (s->state == SESSION_LINGER) {
		(void)linger(s, now_ms);
	}
	return fx;
}

void session_busy(struct session *s, uint32_t now_ms, uint32_t ms)
{
	if (s->state == SESSION_OFF || s->state == SESSION_CLOSING) {
		return;
	}
	/* Pushed back, never brought forward */
	if (!s->busy || (int32_t)(now_ms + ms - s->busy_until) > 0) {
		s->busy_until = now_ms + ms;
	}
	s->busy = true;
}

uint32_t session_tick(struct session *s, uint32_t now_ms)
{
	if (!due(s, now_ms)) {
		return 0;
	}
	/* An update in the link: no BYE of the watch before it is over */
	if (s->busy && (int32_t)(s->busy_until - now_ms) > 0) {
		if (s->state == SESSION_OFFERED || s->state == SESSION_ACK_WAIT ||
		    s->state == SESSION_LINGER) {
			s->deadline = s->busy_until;
			return 0;
		}
	} else {
		s->busy = false;
	}
	switch (s->state) {
	case SESSION_OFFERED:
	case SESSION_ACK_WAIT:
		return close_with(s, LINK_BYE_IDLE);
	case SESSION_RESULT:
		s->result_id = 0; /* the window is over: no LED */
		return linger(s, now_ms);
	case SESSION_LINGER:
		return close_with(s, s->held ? LINK_BYE_IDLE : LINK_BYE_DONE);
	default:
		s->timed = false;
		return 0;
	}
}

uint32_t session_event(struct session *s, uint8_t gesture, uint8_t button)
{
	uint8_t msg[LINK_SMALL_MAX];

	if (!s->held || s->state == SESSION_OFF || s->state == SESSION_CLOSING) {
		return 0;
	}
	enqueue(s, msg, link_put_event(msg, sizeof(msg), gesture, button));
	return SESSION_FX_SEND;
}

uint32_t session_note_added(struct session *s, uint32_t now_ms)
{
	/* In the middle of a note, the next offer finds it: its id is higher */
	if (s->state == SESSION_LINGER || s->state == SESSION_RESULT) {
		return offer_next(s, now_ms);
	}
	return 0;
}

size_t session_next_tx(struct session *s, uint8_t *buf, size_t room, uint32_t now_ms)
{
	if (s->queue_count > 0) {
		const uint8_t head = s->queue_head;
		/* A VALUE longer than the MTU allows goes cut: phones give 185 and more */
		const size_t len = s->queue_len[head] < room ? s->queue_len[head] : room;

		memcpy(buf, s->queue[head], len);
		s->queue_head = (uint8_t)((head + 1U) % SESSION_QUEUE);
		s->queue_count--;
		return len;
	}
	if (s->state != SESSION_SENDING) {
		return 0;
	}
	if (s->offset >= s->note.size) {
		s->state = SESSION_ACK_WAIT;
		arm(s, now_ms, SESSION_IDLE_MS);
		return link_put_note_end(buf, room, s->note.id);
	}
	{
		const size_t head_len = link_put_note_data(buf, room);
		size_t want, got;

		if (head_len == 0) {
			return 0;
		}
		want = room - head_len;
		if (want > s->note.size - s->offset) {
			want = s->note.size - s->offset;
		}
		got = s->io.note_read != NULL
			      ? s->io.note_read(s->io.ctx, s->note.id, s->offset, &buf[head_len], want)
			      : 0;
		if (got == 0) {
			/* The note cannot be read: end the session, it stays for later */
			(void)close_with(s, LINK_BYE_ERROR);
			return session_next_tx(s, buf, room, now_ms);
		}
		s->offset += (uint32_t)got;
		return head_len + got;
	}
}

bool session_next_ms(const struct session *s, uint32_t now_ms, uint32_t *wait_ms)
{
	int32_t left;

	if (!s->timed || s->state == SESSION_OFF || s->state == SESSION_CLOSING) {
		return false;
	}
	left = (int32_t)(s->deadline - now_ms);
	*wait_ms = left > 0 ? (uint32_t)left : 0U;
	return true;
}

void session_closed(struct session *s)
{
	memset(s, 0, sizeof(*s));
}

bool session_active(const struct session *s)
{
	return s->state != SESSION_OFF;
}

bool session_closing(const struct session *s)
{
	return s->state == SESSION_CLOSING;
}
