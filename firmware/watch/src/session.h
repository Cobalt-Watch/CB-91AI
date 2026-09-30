/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * A Cobalt Link session, on the watch side (lot D6): what the watch says and
 * does, message after message, as the Cobalt Link specification sets
 * the course of a session. Pure logic over lib/link_proto.c: the Bluetooth glue
 * feeds it the phone's messages, the time and the room to send, and carries out
 * what it returns. The host tests play the phone (firmware/tests/host).
 *
 * The watch leads: HELLO as soon as the phone listens, then its notes one by
 * one (offer, data from the offset the phone asks, end, acknowledgement), the
 * verdict of the last one within the window the phone announced, then BYE and
 * it cuts the link. Each note is offered once per session: a damaged one waits
 * for the next session, rather than looping.
 */

#ifndef CB91AI_WATCH_SESSION_H
#define CB91AI_WATCH_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_proto.h"

#define SESSION_LINGER_MS 2000U   /* nothing left to do: this long for the phone's word */
#define SESSION_IDLE_MS   5000U   /* waiting for an answer that does not come */
#define SESSION_HOLD_MS   300000U /* held session without a message (EF-33) */
#define SESSION_BUSY_MS   30000U  /* after an SMP command: the first chunk of an update
                                   * alone waits 10 to 18 s for the erase of a slot */
#define SESSION_MSG_MAX   244U    /* the longest message queued: a VALUE as long as a
                                   * notification of an MTU of 247 carries; the status
                                   * line, longer once the watch has run for days, is
                                   * cut after its last whole field (watch_status()) */
#define SESSION_QUEUE     4U

struct session_note {
	uint32_t id;
	uint32_t size;
	uint32_t crc32;
};

/* What the session asks of the rest of the watch */
struct session_io {
	void *ctx;
	/* The oldest note waiting with an id above `after` (0: from the start):
	 * false when there is none */
	bool (*note_next)(void *ctx, uint32_t after, struct session_note *note);
	/* Up to `len` bytes of a note from `offset`: how many were read */
	size_t (*note_read)(void *ctx, uint32_t id, uint32_t offset, uint8_t *buf, size_t len);
	/* A setting from the phone: its LINK_VALUE_* status */
	uint8_t (*setting_set)(void *ctx, uint8_t key, const uint8_t *value, uint8_t len);
	/* A setting for the phone: the value's length, 0 with *status set if none */
	size_t (*setting_get)(void *ctx, uint8_t key, uint8_t *out, size_t room, uint8_t *status);
};

/* What the glue carries out, bits of the values the calls return */
#define SESSION_FX_SEND      (1U << 0) /* call session_next_tx() until it gives 0 */
#define SESSION_FX_TIME      (1U << 1) /* set the clock: time_utc_ms, time_tz */
#define SESSION_FX_DELIVERED (1U << 2) /* note delivered_id may be erased, at rest */
#define SESSION_FX_RESULT    (1U << 3) /* the LED: result_code (green or red) */
#define SESSION_FX_CLOSE     (1U << 4) /* send what is queued, then cut the link */

enum session_state {
	SESSION_OFF,
	SESSION_OFFERED,   /* a note offered, its answer awaited */
	SESSION_SENDING,   /* its bytes on their way */
	SESSION_ACK_WAIT,  /* NOTE_END sent, the acknowledgement awaited */
	SESSION_RESULT,    /* all offered; the verdict of the last note awaited */
	SESSION_LINGER,    /* nothing left: a last word from the phone, or BYE */
	SESSION_CLOSING,   /* BYE queued */
};

struct session {
	struct session_io io;
	uint8_t state;
	bool held;             /* LINK_KEY_HOLD: no BYE of its own, gestures as EVENT */
	uint32_t deadline;     /* ms, the time-out of the state; valid while `timed` */
	bool timed;
	struct session_note note; /* offered or being sent */
	uint32_t offset;
	uint32_t last_offered; /* the notes above it have not been offered this session */
	uint32_t busy_until;   /* no BYE of the watch before, while `busy` */
	bool busy;
	uint32_t result_id;    /* the note whose verdict may come, 0: none */
	uint32_t result_until;
	/* Carried out by the glue */
	int64_t time_utc_ms;
	int16_t time_tz;
	uint32_t delivered_id;
	uint8_t result_code;
	/* Counted for EF-73 and the tests */
	uint32_t refused;      /* messages dropped: short, unknown, wrong way, out of turn */
	/* Messages waiting to go, oldest first */
	uint8_t queue[SESSION_QUEUE][SESSION_MSG_MAX];
	uint8_t queue_len[SESSION_QUEUE];
	uint8_t queue_head;
	uint8_t queue_count;
};

/* The phone listens on TX: the session starts with HELLO */
uint32_t session_open(struct session *s, const struct session_io *io,
		      const struct link_hello *hello, uint32_t now_ms);

/* A write of the phone on RX */
uint32_t session_rx(struct session *s, const uint8_t *buf, size_t len, uint32_t now_ms);

/* Time goes by; also when the wait of session_next_ms() is over */
uint32_t session_tick(struct session *s, uint32_t now_ms);

/* The phone is busy with something else in this link, an update over SMP:
 * the watch ends the session no sooner than `ms` from now (the time-outs of
 * the states wait; a held session and the phone's BYE go on as before) */
void session_busy(struct session *s, uint32_t now_ms, uint32_t ms);

/* A gesture of the buttons: an EVENT, in a held session only */
uint32_t session_event(struct session *s, uint8_t gesture, uint8_t button);

/* A note was kept during the session: offered at once if the watch was only
 * waiting for the phone (its last word, a verdict, a held session), after the
 * note under way otherwise */
uint32_t session_note_added(struct session *s, uint32_t now_ms);

/* The next message to send, at most `room` bytes (the MTU less 3): its
 * length, 0 when nothing is to go now. Call again after each successful
 * send; a refused send keeps the message for the glue to try again. */
size_t session_next_tx(struct session *s, uint8_t *buf, size_t room, uint32_t now_ms);

/* BYE is queued or sent: once nothing is left to send, cut the link. Also
 * set when the watch ends the session in the middle of a send. */
bool session_closing(const struct session *s);

/* How long until session_tick() is due, false when nothing is timed */
bool session_next_ms(const struct session *s, uint32_t now_ms, uint32_t *wait_ms);

/* The link is gone: forget the session */
void session_closed(struct session *s);

bool session_active(const struct session *s);

#endif /* CB91AI_WATCH_SESSION_H */
