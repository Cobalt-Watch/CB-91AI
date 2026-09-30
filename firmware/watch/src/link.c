/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Cobalt Link service of the watch, see link.h.
 *
 * RX: the write callback (Bluetooth RX thread) copies the message into a small
 * ring and posts EVT_LINK_RX; the loop parses it (session.c).
 *
 * TX: the loop puts what the session gives into a second ring, and a work item
 * of the system work queue notifies it, two notifications in flight at most.
 * The notifications leave from there because ATT waits for a free buffer
 * forever on any other thread (att.c, bt_att_chan_create_pdu()): from the loop,
 * a link that stalls would stall the watch, until its watchdog bites. On the
 * work queue a buffer is taken or refused at once; refused, the work tries
 * again when a notification of ours has left, or a little later when SMP holds
 * the buffers. Each slot freed wakes the loop (EVT_LINK_TX) to fill it: the
 * loop never waits on the link, and the core sleeps while the radio works.
 *
 * Before the pairing of phase S the characteristics are open in the
 * development build (CONFIG_CB91AI_WATCH_LINK_OPEN), as SMP is; the product
 * asks for an encrypted, authenticated link.
 */

#include <string.h>
#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#endif
#include <nrfx.h>

#include "clock.h"
#include "counters.h"
#include "events.h"
#include "link.h"
#include "link_proto.h"
#include "notes.h"
#include "radio.h"
#include "recorder.h"
#include "session.h"
#include "watch.h"
#include "wrist.h"

LOG_MODULE_REGISTER(watch_link, LOG_LEVEL_INF);

#if defined(CONFIG_CB91AI_WATCH_LINK_OPEN)
#define LINK_PERM_READ  BT_GATT_PERM_READ
#define LINK_PERM_WRITE BT_GATT_PERM_WRITE
#else
#define LINK_PERM_READ  BT_GATT_PERM_READ_AUTHEN
#define LINK_PERM_WRITE BT_GATT_PERM_WRITE_AUTHEN
#endif

#define RX_SLOTS    8
#define TX_SLOTS    4    /* three messages queued at most: one slot stays empty */
#define IN_FLIGHT   2    /* of the 3 ATT buffers: one stays for SMP and the answers */
#define TX_ROOM     244  /* MTU of 247 at most here, less the 3 bytes of a notification */
#define TX_RETRY_MS 20   /* no ATT buffer free: try again this much later */

static const struct bt_uuid_128 link_svc_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xc0b91b00, 0x256e, 0x46f9, 0x8d7b, 0xd9c643908667));
static const struct bt_uuid_128 link_rx_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xc0b91b01, 0x256e, 0x46f9, 0x8d7b, 0xd9c643908667));
static const struct bt_uuid_128 link_tx_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xc0b91b02, 0x256e, 0x46f9, 0x8d7b, 0xd9c643908667));
static const struct bt_uuid_128 link_pair_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xc0b91b03, 0x256e, 0x46f9, 0x8d7b, 0xd9c643908667));

/* ---- RX: a ring of messages, filled by the Bluetooth thread ------------------ */

static struct {
	uint8_t data[LINK_SMALL_MAX];
	uint8_t len;
} rx_ring[RX_SLOTS];
static atomic_t rx_head; /* next slot the loop reads */
static atomic_t rx_tail; /* next slot the Bluetooth thread fills */
static atomic_t rx_lost;
static atomic_t rx_pending;

static ssize_t write_rx(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			uint16_t len, uint16_t offset, uint8_t flags)
{
	const atomic_val_t tail = atomic_get(&rx_tail);

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	/* Every message of the phone fits 20 bytes; a full ring is counted */
	if (len > LINK_SMALL_MAX || (tail + 1) % RX_SLOTS == atomic_get(&rx_head) % RX_SLOTS) {
		atomic_inc(&rx_lost);
		return len;
	}
	memcpy(rx_ring[tail % RX_SLOTS].data, buf, len);
	rx_ring[tail % RX_SLOTS].len = (uint8_t)len;
	atomic_set(&rx_tail, (tail + 1) % RX_SLOTS);
	radio_activity();
	/* Dropped (queue full): let go, the next message posts again */
	if (atomic_cas(&rx_pending, 0, 1) && !evt_post(EVT_LINK_RX, 0, 0, 0)) {
		atomic_set(&rx_pending, 0);
	}
	return len;
}

/* The phone listens on TX: the session starts, at each subscription the phone
 * writes. Not when a bond restores the subscription as the link is encrypted
 * (lot S1): the app may not listen yet, and its own subscription, the same
 * value, would change nothing to wake the watch. */
static ssize_t tx_ccc_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	if (value & BT_GATT_CCC_NOTIFY) {
		(void)evt_post(EVT_LINK_OPEN, 0, 0, 0);
	}
	return sizeof(value);
}

/* PAIR (lot S1): protected in every build, so that a phone that reads it pairs
 * (an iPhone pairs only on a read it is refused) */
static ssize_t read_pair(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	static const uint8_t paired = 1;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &paired, sizeof(paired));
}

BT_GATT_SERVICE_DEFINE(link_svc,
	BT_GATT_PRIMARY_SERVICE(&link_svc_uuid),
	BT_GATT_CHARACTERISTIC(&link_rx_uuid.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP, LINK_PERM_WRITE,
			       NULL, write_rx, NULL),
	BT_GATT_CHARACTERISTIC(&link_tx_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL,
			       NULL, NULL),
	BT_GATT_CCC_WITH_WRITE_CB(NULL, tx_ccc_write, LINK_PERM_READ | LINK_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&link_pair_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ_AUTHEN,
			       read_pair, NULL, NULL),
);

/* attrs: 0 service, 1 and 2 RX, 3 and 4 TX, 5 its CCC, 6 and 7 PAIR */
#define TX_ATTR (&link_svc.attrs[4])

/* ---- TX: a ring of messages, sent by the system work queue ------------------- */

static struct {
	uint8_t data[TX_ROOM];
	uint16_t len;
	uint32_t gen;
} tx_ring[TX_SLOTS];
static atomic_t tx_head;    /* next slot to notify: the work queue moves it */
static atomic_t tx_tail;    /* next slot to fill: the loop moves it */
static atomic_t tx_gen;     /* the session the loop fills for: older slots are dropped */
static atomic_t tx_pending;
static atomic_t tx_dropped;
/* Written by the work queue only; the loop reads in_flight */
static atomic_t in_flight;
static uint32_t flight_gen;

static void tx_send(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tx_work, tx_send);

static void wake_loop_tx(void)
{
	if (atomic_cas(&tx_pending, 0, 1) && !evt_post(EVT_LINK_TX, 0, 0, 0)) {
		atomic_set(&tx_pending, 0);
	}
}

/* A notification of ours left (system work queue): room for the next */
static void sent(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(conn);
	if ((uint32_t)(uintptr_t)user_data == flight_gen && atomic_get(&in_flight) > 0) {
		atomic_dec(&in_flight);
	}
	(void)k_work_reschedule(&tx_work, K_NO_WAIT);
	wake_loop_tx();
}

static void tx_send(struct k_work *work)
{
	const uint32_t gen = (uint32_t)atomic_get(&tx_gen);
	struct bt_conn *conn = radio_conn();
	const uint16_t mtu = conn != NULL ? bt_gatt_get_mtu(conn) : 0U;
	bool moved = false;

	ARG_UNUSED(work);
	if (gen != flight_gen) {
		/* A new session: what the last one left in flight is not counted */
		flight_gen = gen;
		atomic_set(&in_flight, 0);
	}
	while (atomic_get(&tx_head) != atomic_get(&tx_tail)) {
		const atomic_val_t head = atomic_get(&tx_head);

		/* A slot of an ended session, or no link any more: dropped. So is a
		 * message longer than the link takes: ATT refuses it with -ENOMEM
		 * too, and it would be tried again for ever (fill() cuts to the MTU,
		 * which only grows: this is a guard) */
		if (tx_ring[head].gen == gen && conn != NULL && mtu >= 23U &&
		    tx_ring[head].len > mtu - 3U) {
			LOG_WRN("message of %u bytes over the MTU of %u: dropped",
				tx_ring[head].len, mtu);
			atomic_inc(&tx_dropped);
		} else if (tx_ring[head].gen == gen && conn != NULL && mtu >= 23U) {
			struct bt_gatt_notify_params params = {
				.attr = TX_ATTR,
				.data = tx_ring[head].data,
				.len = tx_ring[head].len,
				.func = sent,
				.user_data = (void *)(uintptr_t)gen,
			};
			int err;

			if (atomic_get(&in_flight) >= IN_FLIGHT) {
				break; /* sent() comes back */
			}
			err = bt_gatt_notify_cb(conn, &params);
			if (err == -ENOMEM) {
				/* No ATT buffer now: ours are on their way, or SMP has them */
				(void)k_work_reschedule(&tx_work, K_MSEC(TX_RETRY_MS));
				break;
			}
			if (err == 0) {
				atomic_inc(&in_flight);
			} else {
				/* Not subscribed any more, or the link is going */
				LOG_WRN("notification dropped (%d)", err);
				atomic_inc(&tx_dropped);
			}
		}
		atomic_set(&tx_head, (head + 1) % TX_SLOTS);
		moved = true;
	}
	if (conn != NULL) {
		bt_conn_unref(conn);
	}
	if (moved) {
		wake_loop_tx();
	}
}

/* ---- The session (event loop) --------------------------------------------------- */

static struct session session;
static unsigned int session_link; /* radio_link() when it opened */
static uint32_t refused_total;
/* The newest note kept during the session: if the session ends before
 * offering it, the phone is called again (link_closed()) */
static uint32_t note_added;
/* The last SMP command, whether a session was open or not: an update may begin
 * before the phone listens on TX (link_open() applies what is left of it) */
static bool smp_seen;
static uint32_t smp_until;

static void timer_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	(void)evt_post(EVT_LINK_TIMER, 0, 0, 0);
}

/* Defined at build time: rearm() may stop it before any session opened */
static K_TIMER_DEFINE(session_timer, timer_expired, NULL);

static void rearm(void)
{
	uint32_t wait;

	if (session_next_ms(&session, k_uptime_get_32(), &wait)) {
		k_timer_start(&session_timer, K_MSEC(wait), K_NO_WAIT);
	} else {
		k_timer_stop(&session_timer);
	}
}

/* The session's messages into the ring, as long as there is room */
static void fill(void)
{
	struct bt_conn *conn = radio_conn();
	const uint32_t gen = (uint32_t)atomic_get(&tx_gen);
	uint16_t mtu;
	size_t room;

	if (conn == NULL) {
		return;
	}
	mtu = bt_gatt_get_mtu(conn);
	bt_conn_unref(conn);
	if (mtu < 23U) {
		return; /* the link is going: no MTU any more */
	}
	/* The MTU only grows: a message cut for this room fits any later one */
	room = MIN((size_t)mtu - 3U, sizeof(tx_ring[0].data));
	for (;;) {
		const atomic_val_t tail = atomic_get(&tx_tail);
		const atomic_val_t next = (tail + 1) % TX_SLOTS;
		size_t len;

		if (next == atomic_get(&tx_head)) {
			break; /* full: a slot freed brings EVT_LINK_TX */
		}
		len = session_next_tx(&session, tx_ring[tail].data, room, k_uptime_get_32());
		if (len == 0) {
			break;
		}
		tx_ring[tail].len = (uint16_t)len;
		tx_ring[tail].gen = gen;
		atomic_set(&tx_tail, next);
	}
	/* Already scheduled (a retry): it keeps its time */
	(void)k_work_schedule(&tx_work, K_NO_WAIT);
	/* The session said BYE: once it has left, cut */
	if (session_closing(&session) && atomic_get(&tx_head) == atomic_get(&tx_tail) &&
	    atomic_get(&in_flight) == 0) {
		radio_disconnect(session_link);
	}
}

/* The session is over, or starts again: what is left of it is not sent */
static void tx_forget(void)
{
	atomic_inc(&tx_gen);
	atomic_set(&tx_pending, 0);
	(void)k_work_schedule(&tx_work, K_NO_WAIT);
}

static uint32_t carry_out(uint32_t fx)
{
	uint32_t todo = 0;

	if (fx & SESSION_FX_TIME) {
		todo |= LINK_DO_TIME;
	}
	if (fx & SESSION_FX_RESULT) {
		todo |= LINK_DO_RESULT;
	}
	if (fx & SESSION_FX_DELIVERED) {
		/* Marked in its header at once (four bytes), erased at rest */
		LOG_INF("note %u delivered", session.delivered_id);
		if (notes_delivered(session.delivered_id) == 0) {
			recorder_prepare();
		}
	}
	if (fx & (SESSION_FX_SEND | SESSION_FX_CLOSE)) {
		fill();
	}
	rearm();
	return todo;
}

/* ---- What the session asks of the watch ---------------------------------------- */

static bool note_next(void *ctx, uint32_t after, struct session_note *note)
{
	struct store_note n;

	ARG_UNUSED(ctx);
	if (!notes_next(after, &n)) {
		return false;
	}
	note->id = n.id;
	note->size = n.size;
	note->crc32 = n.crc;
	return true;
}

/* From fill(), a notification's worth at a time: the flash sleeps between */
static size_t note_read(void *ctx, uint32_t id, uint32_t offset, uint8_t *buf, size_t len)
{
	ARG_UNUSED(ctx);
	return notes_read(id, offset, buf, len);
}

/* The session's cursor in the journal (lot T1): where its next page starts,
 * set by SET, past each page given; 0 at each session. A SET refused answers
 * an empty page, the cursor where it was. */
static uint32_t journal_cursor;
static bool journal_refused;

/* The room of a VALUE's value that a notification carries at the MTU of the
 * link: the type, key and status of the VALUE and the 3 bytes of the
 * notification taken off */
static size_t value_room(size_t room)
{
	struct bt_conn *conn = radio_conn();
	uint16_t mtu = 0;

	if (conn != NULL) {
		mtu = bt_gatt_get_mtu(conn);
		bt_conn_unref(conn);
	}
	return mtu >= 23U ? MIN(room, (size_t)mtu - 6U) : room;
}

/* A page of the journal from the cursor, which goes past it */
static size_t journal_page(uint8_t *out, size_t room)
{
	struct tlog_record r[LINK_JOURNAL_MAX];
	struct link_journal_record w[LINK_JOURNAL_MAX];
	const size_t fit = value_room(room);
	size_t max = fit > LINK_JOURNAL_HEAD ? (fit - LINK_JOURNAL_HEAD) / LINK_JOURNAL_RECORD : 0U;
	uint32_t first = journal_cursor;
	uint32_t id = 0;
	size_t n;
	size_t put;
	size_t len;

	max = journal_refused ? 0U : MIN(max, (size_t)LINK_JOURNAL_MAX);
	journal_refused = false;
	n = notes_journal_read(journal_cursor, r, max, &first, &id);
	for (size_t i = 0; i < n; i++) {
		w[i] = (struct link_journal_record){ r[i].time_s, r[i].temp_cc, r[i].cell };
	}
	len = link_put_journal(out, fit, id, n > 0 ? first : journal_cursor, w, n, &put);
	if (put > 0) {
		journal_cursor = first + (uint32_t)put;
	}
	return len;
}

static uint8_t setting_set(void *ctx, uint8_t key, const uint8_t *value, uint8_t len)
{
	ARG_UNUSED(ctx);
	if (len < 1) {
		return LINK_VALUE_REFUSED;
	}
	switch (key) {
	case LINK_KEY_JOURNAL:
		/* The index the phone wants from: its VALUE is the first page */
		if (len < 4) {
			journal_refused = true;
			return LINK_VALUE_REFUSED;
		}
		journal_cursor = sys_get_le32(value);
		return LINK_VALUE_OK;
#if defined(CONFIG_CB91AI_WATCH_DEBUG_JOURNAL)
	case LINK_KEY_DEBUG_JOURNAL_S:
		return len >= 2 && watch_set_journal_period(sys_get_le16(value)) ? LINK_VALUE_OK
										 : LINK_VALUE_REFUSED;
#endif
#if defined(CONFIG_CB91AI_WATCH_DEBUG_WRIST)
	case LINK_KEY_DEBUG_WRIST:
		/* The accelerometer configured again at once, from the loop */
		return wrist_tuning_set(value, len) ? LINK_VALUE_OK : LINK_VALUE_REFUSED;
	case LINK_KEY_DEBUG_ACCEL:
		return LINK_VALUE_READ_ONLY;
#endif
	case LINK_KEY_TIME_FORMAT:
		if (value[0] != 12 && value[0] != 24) {
			return LINK_VALUE_REFUSED;
		}
		clock_set_24h(value[0] == 24); /* written to flash by clock_process() */
		return LINK_VALUE_OK;
	case LINK_KEY_DISPLAY_S:
		return watch_set_display_s(value[0]) ? LINK_VALUE_OK : LINK_VALUE_REFUSED;
	case LINK_KEY_TX_POWER:
		/* 0 dBm by default; +4 and +8 for range tests (EF-31) */
		if ((int8_t)value[0] != 0 && (int8_t)value[0] != 4 && (int8_t)value[0] != 8) {
			return LINK_VALUE_REFUSED;
		}
		return radio_set_tx_power((int8_t)value[0]) == 0 ? LINK_VALUE_OK : LINK_VALUE_REFUSED;
	case LINK_KEY_LIGHT:
		/* The light of LIGHT: effect and colour, kept in the settings */
		return watch_set_light(value, len) ? LINK_VALUE_OK : LINK_VALUE_REFUSED;
	case LINK_KEY_NAME:
		/* The name of the watch, taken once the link is down */
		return watch_set_name(value, len) ? LINK_VALUE_OK : LINK_VALUE_REFUSED;
#if defined(CONFIG_CB91AI_WATCH_DEBUG_DAILY)
	case LINK_KEY_DEBUG_DAILY_S:
		/* The daily call in minutes: from the end of this link, or of the
		 * window of the last call if that comes later (daily.h) */
		return len >= 4 && watch_set_daily_period(sys_get_le32(value)) ? LINK_VALUE_OK
										: LINK_VALUE_REFUSED;
#endif
#if defined(CONFIG_CB91AI_WATCH_DEBUG_TRIAL)
	case LINK_KEY_DEBUG_TRIAL:
		/* The trial of the calls (lot N1a): minutes, then the profile;
		 * the radio behaves as the product's once this link is down */
		return len >= 2 && radio_trial_set(value[0], value[1]) ? LINK_VALUE_OK
								      : LINK_VALUE_REFUSED;
#endif
#if defined(CONFIG_CB91AI_WATCH_DEBUG_TAKE)
	case LINK_KEY_DEBUG_TAKE:
		/* The note comes in this session if it is held, in the next otherwise */
		return value[0] >= 1 && value[0] <= 60 && watch_debug_take(value[0])
			       ? LINK_VALUE_OK
			       : LINK_VALUE_REFUSED;
#endif
	case LINK_KEY_COUNTERS:
	case LINK_KEY_STATUS:
		return LINK_VALUE_READ_ONLY;
	default:
		return LINK_VALUE_UNKNOWN;
	}
}

static size_t setting_get(void *ctx, uint8_t key, uint8_t *out, size_t room, uint8_t *status)
{
	ARG_UNUSED(ctx);
	*status = LINK_VALUE_OK;
	switch (key) {
	case LINK_KEY_TIME_FORMAT:
		out[0] = clock_24h() ? 24 : 12;
		return 1;
	case LINK_KEY_DISPLAY_S:
		out[0] = watch_display_s();
		return 1;
	case LINK_KEY_TX_POWER:
		out[0] = (uint8_t)radio_tx_power();
		return 1;
	case LINK_KEY_LIGHT:
		return watch_light(out, room);
	case LINK_KEY_NAME:
		return watch_name(out, room);
	case LINK_KEY_COUNTERS: {
		const struct link_counters c = {
			.notes = notes_last_id(), /* ids count the notes, and never go back */
			.recorded_s = recorder_recorded_s(),
			.connected_s = radio_connected_s(),
			.boots = counters_boots(), /* EF-72, since the watch's first boot */
			.watchdog_resets = counters_bites(),
			.events_lost = evt_dropped(),
		};

		return link_put_counters(out, room, &c);
	}
	case LINK_KEY_STATUS:
		/* Cut at the MTU rather than dropped whole by the link */
		return watch_status((char *)out, value_room(room));
	case LINK_KEY_JOURNAL:
		return journal_page(out, room);
#if defined(CONFIG_CB91AI_WATCH_DEBUG_TRIAL)
	case LINK_KEY_DEBUG_TRIAL: {
		struct link_trial t;

		radio_trial_get(&t);
		return link_put_trial(out, room, &t);
	}
#endif
#if defined(CONFIG_CB91AI_WATCH_DEBUG_WRIST)
	case LINK_KEY_DEBUG_WRIST:
		return wrist_tuning_get(out, room);
	case LINK_KEY_DEBUG_ACCEL: {
		/* Read over I2C here, in the loop, as wrist_event() does */
		const size_t len = wrist_diag(out, value_room(room));

		if (len == 0) {
			*status = LINK_VALUE_REFUSED;
		}
		return len;
	}
#endif
	default:
		*status = LINK_VALUE_UNKNOWN;
		return 0;
	}
}

static const struct session_io io = {
	.note_next = note_next,
	.note_read = note_read,
	.setting_set = setting_set,
	.setting_get = setting_get,
};

/* ---- From the event loop ---------------------------------------------------- */

uint32_t link_open(void)
{
	struct link_hello hello = {
		.version = LINK_PROTO_VERSION,
		.reason = (uint8_t)radio_reason(),
		.fw_major = APP_VERSION_MAJOR,
		.fw_minor = APP_VERSION_MINOR,
		.fw_revision = APP_PATCHLEVEL,
		.notes = (uint8_t)MIN(notes_count(), UINT8_MAX),
		.note_bytes = notes_bytes(),
		.battery_mv = watch_battery_mv(),
		.codecs = LINK_CODEC_LC3,
		.watch_id = NRF_FICR->DEVICEID[0],
	};
	if (clock_is_set()) {
		hello.state |= LINK_STATE_TIME_SET;
	}
	if (clock_flags() & CLOCK_FLAG_APPROXIMATE) {
		hello.state |= LINK_STATE_TIME_APPROX;
	}
	if (recorder_flash_full()) {
		hello.state |= LINK_STATE_FLASH_FULL;
	}
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
	if (!boot_is_img_confirmed()) {
		hello.state |= LINK_STATE_IMAGE_TEST;
	}
#endif
	const uint32_t now = k_uptime_get_32();
	uint32_t fx;

	/* A second subscription in the same link starts a fresh session, which
	 * offers every note from the oldest */
	session_closed(&session);
	note_added = 0;
	journal_cursor = 0; /* the whole journal, unless the phone says where from */
	journal_refused = false;
	tx_forget();
	session_link = radio_link();
	LOG_INF("session opened, reason %u", hello.reason);
	fx = session_open(&session, &io, &hello, now);
	/* An update already under way in this link: no BYE before it is done */
	if (smp_seen && (int32_t)(smp_until - now) > 0) {
		session_busy(&session, now, smp_until - now);
	}
	return carry_out(fx);
}

uint32_t link_rx(void)
{
	uint32_t todo = 0;

	atomic_set(&rx_pending, 0);
	while (atomic_get(&rx_head) != atomic_get(&rx_tail)) {
		const atomic_val_t head = atomic_get(&rx_head);

		todo |= carry_out(session_rx(&session, rx_ring[head].data, rx_ring[head].len,
					     k_uptime_get_32()));
		atomic_set(&rx_head, (head + 1) % RX_SLOTS);
	}
	return todo;
}

uint32_t link_tx(void)
{
	atomic_set(&tx_pending, 0);
	fill();
	return 0;
}

uint32_t link_tick(void)
{
	return carry_out(session_tick(&session, k_uptime_get_32()));
}

void link_closed(void)
{
	if (session_active(&session)) {
		refused_total += session.refused;
		LOG_INF("session ended");
	}
	/* A note kept meanwhile and never offered, or the link lost in the middle
	 * of one (offered, sent or awaiting its acknowledgement): the product's
	 * radio is silent unless called, so call again. A note the phone put off
	 * is not, nor one the watch gave up on (its BYE then closed the session). */
	if (note_added > session.last_offered || session.state == SESSION_OFFERED ||
	    session.state == SESSION_SENDING || session.state == SESSION_ACK_WAIT) {
		LOG_INF("a note left undelivered by the session: calling again");
		radio_call(RADIO_REASON_NOTE);
	} else if (session.state == SESSION_CLOSING) {
		/* Run to its BYE: the call is answered, the notes put off wait
		 * for the next one (lot E2). A link lost on the way, or without a
		 * session, leaves the window to run. */
		radio_answered();
	}
	note_added = 0;
	session_closed(&session);
	k_timer_stop(&session_timer);
	tx_forget();
	atomic_set(&rx_head, atomic_get(&rx_tail));
	atomic_set(&rx_pending, 0);
	smp_seen = false; /* that link is over */
}

void link_smp(void)
{
	const uint32_t now = k_uptime_get_32();

	/* An update may go on in the link: no BYE before it is done */
	smp_seen = true;
	smp_until = now + SESSION_BUSY_MS;
	session_busy(&session, now, SESSION_BUSY_MS);
	rearm();
}

uint32_t link_gesture(uint8_t gesture, uint8_t button)
{
	return carry_out(session_event(&session, gesture, button));
}

uint32_t link_note_added(uint32_t id)
{
	note_added = MAX(note_added, id);
	return carry_out(session_note_added(&session, k_uptime_get_32()));
}

bool link_in_session(void)
{
	return session_active(&session);
}

void link_time(int64_t *utc_ms, int16_t *tz_minutes)
{
	*utc_ms = session.time_utc_ms;
	*tz_minutes = session.time_tz;
}

uint8_t link_result(void)
{
	return session.result_code;
}

uint32_t link_refused(void)
{
	return refused_total + session.refused + (uint32_t)atomic_get(&rx_lost) +
	       (uint32_t)atomic_get(&tx_dropped);
}
