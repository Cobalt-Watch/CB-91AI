/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The radio of the watch, see radio.h. The policy of the self-test (app/src/
 * ble.c), whose traps cost bench sessions, carried over as it is:
 * - starting and stopping the advertiser, and disconnecting, are synchronous
 *   HCI commands: they run on a thread of their own, never on the system work
 *   queue, where the host runs its own command queue;
 * - stopping a connectable advertiser releases its connection object and fires
 *   recycled(), exactly as the end of a connection does: recycled() only
 *   brings the advertiser back after a real disconnection (0.1.21 reset 30 s
 *   after every boot for want of this);
 * - 0 dBm (EF-31), the 250 ppm sleep clock of the V1 (EF-39, prj.conf), a
 *   supervision timeout of 4 s asked for (lot B5), 2M PHY.
 *
 * Two policies (the Cobalt Link specification). The product keeps the
 * radio silent and advertises for a reason only: 100 ms for 10 s, then 1 s up
 * to 2 min 10, then silent again, and silent at once after a session that ran to
 * its BYE; the event loop calls again after 15 min of silence while a note
 * waits, then more and more rarely (lot E2, recall.h). The development build
 * (CONFIG_CB91AI_WATCH_ADV_ALWAYS, debug.conf) never stops, as the self-test:
 * fast for 30 s after boot, a disconnection or a call, slow otherwise, so that
 * the PC always finds a sealed watch. Except during a trial of the calls
 * (Cobalt Link key 0x7A, lot N1a, risk R11): for the minutes it lasts, the
 * development build keeps the product's policy, with the product's rhythm or
 * Apple's, and times each call from its first advertisement to the
 * connection of the phone.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

#include "events.h"
#include "link_proto.h"
#include "radio.h"
#include "recall.h"

LOG_MODULE_REGISTER(watch_radio, LOG_LEVEL_INF);

#define FAST_INTERVAL      160U   /* 100 ms, 0.625 ms units */
#define SLOW_INTERVAL      1600U  /* 1 s */
#define CALL_FAST_MS       10000U /* product: 100 ms for 10 s ... */
#define CALL_WINDOW_MS     130000U /* ... then 1 s up to 2 min 10 */
#define DEV_FAST_MS        30000U /* development: fast for 30 s */
#define IDLE_DISCONNECT_S  300U   /* EF-33 */
/* What Apple recommends to accessories, within the same window (trial of the
 * calls, lot N1a): 20 ms for 30 s, then 152.5 ms */
#define APPLE_FAST_INTERVAL 32U
#define APPLE_SLOW_INTERVAL 244U
#define APPLE_FAST_MS       30000U
BUILD_ASSERT(CALL_WINDOW_MS == RECALL_WINDOW_S * 1000U, "recall.h counts another window");

/* Flags, the Cobalt Link UUID, and those of the Battery Service and Device
 * Information: 27 bytes of 31. A phone in the background finds an accessory by
 * the service UUID of its advertisements. */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		      BT_UUID_128_ENCODE(0xc0b91b00, 0x256e, 0x46f9, 0x8d7b, 0xd9c643908667)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_BAS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_DIS_VAL)),
};
BUILD_ASSERT(3 + 18 + 6 <= 31, "advertisement too long");

/* The scan response: SMP, by which nRF Connect Device Manager finds a watch to
 * update (EF-60), and the name of the watch (Cobalt Link key 0x06, "CB-91AI"
 * until the phone gives one): 31 bytes of 31 at most */
static const uint8_t smp_uuid[] = {
	BT_UUID_128_ENCODE(0x8d53dc1d, 0x1db7, 0x4cd3, 0x868b, 0x8a527460aa84)
};
BUILD_ASSERT(2 + sizeof(smp_uuid) + 2 + LINK_NAME_MAX <= 31, "scan response too long");
BUILD_ASSERT(CONFIG_BT_DEVICE_NAME_MAX == LINK_NAME_MAX, "the name the host keeps");

enum adv_mode { ADV_NONE, ADV_FAST, ADV_SLOW, ADV_APPLE_FAST, ADV_APPLE_SLOW };

static const uint16_t adv_interval[] = {
	[ADV_FAST] = FAST_INTERVAL,
	[ADV_SLOW] = SLOW_INTERVAL,
	[ADV_APPLE_FAST] = APPLE_FAST_INTERVAL,
	[ADV_APPLE_SLOW] = APPLE_SLOW_INTERVAL,
};

static atomic_t adv_current = ATOMIC_INIT(ADV_NONE);
static atomic_t called;             /* a reason to advertise came */
static atomic_t reason;
static atomic_t ready;
static atomic_t resume_after_disconnect;
static atomic_t connections;
static atomic_t idle_deadline_s;    /* 0: none */
static atomic_t cut_link;           /* the link the session ended, 0: none */
static atomic_t link_number;        /* connections since boot: the current one */
static atomic_t adv_stops;          /* connections that stopped the advertiser, even failed */
static atomic_t last_disconnect_reason;
/* When the radio last had something to say, in s of uptime (lot E2) */
static atomic_t call_s;             /* the last call, or the opening of its window */
static atomic_t link_end_s;         /* the end of the last link */
static atomic_t asked;              /* a call for a reason, its window to open */
static atomic_t calls;              /* calls since boot */
static atomic_t answered;           /* a session answered the call: window over */
static atomic_t renamed;            /* the name changed: advertise it */
static int64_t window_start_ms;     /* policy thread only */
static int64_t connected_at_ms;
static atomic_t connected_total_s;
static struct bt_conn *current_conn;
static int8_t tx_power_dbm = CONFIG_BT_CTLR_TX_PWR_DBM;
static K_SEM_DEFINE(policy_sem, 0, 1);

/* The trial of the calls (lot N1a, radio.h): on until this uptime in s, 0:
 * off; the first advertisement of the window, k_uptime_get_32() | 1, 0: none;
 * and the last call answered during a trial, under trial_lock */
static atomic_t trial_until_s;
static atomic_t trial_profile;
static atomic_t trial_changed;
static atomic_t window_adv_ms;
static struct k_spinlock trial_lock;
static struct link_trial trial_last;
static uint32_t trial_conn_ms;
static bool trial_conn_open;

static bool trial_on(void)
{
	return IS_ENABLED(CONFIG_CB91AI_WATCH_DEBUG_TRIAL) &&
	       (uint32_t)atomic_get(&trial_until_s) > (uint32_t)(k_uptime_get() / 1000);
}

static uint32_t uptime_s(void)
{
	return (uint32_t)(k_uptime_get() / 1000);
}

/* ---- Transmit power ------------------------------------------------------- */

static int write_tx_power(uint8_t handle_type, uint16_t handle, int8_t dbm, int8_t *selected)
{
	struct bt_hci_cp_vs_write_tx_power_level *cp;
	struct bt_hci_rp_vs_write_tx_power_level *rp;
	struct net_buf *buf, *rsp = NULL;
	int err;

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		return -ENOBUFS;
	}
	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = sys_cpu_to_le16(handle);
	cp->handle_type = handle_type;
	cp->tx_power_level = dbm;
	err = bt_hci_cmd_send_sync(BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL, buf, &rsp);
	if (err) {
		return err;
	}
	rp = (void *)rsp->data;
	if (selected) {
		*selected = rp->selected_tx_power;
	}
	net_buf_unref(rsp);
	return 0;
}

static void apply_conn_tx_power(struct bt_conn *conn)
{
	uint16_t handle;

	if (bt_hci_get_conn_handle(conn, &handle) == 0) {
		(void)write_tx_power(BT_HCI_VS_LL_HANDLE_TYPE_CONN, handle, tx_power_dbm, NULL);
	}
}

int radio_set_tx_power(int8_t dbm)
{
	int8_t selected = dbm;
	int err = write_tx_power(BT_HCI_VS_LL_HANDLE_TYPE_ADV, 0, dbm, &selected);
	struct bt_conn *conn;

	if (err) {
		return err;
	}
	tx_power_dbm = selected;
	conn = radio_conn();
	if (conn) {
		apply_conn_tx_power(conn);
		bt_conn_unref(conn);
	}
	LOG_INF("tx power %d dBm (asked %d)", selected, dbm);
	return 0;
}

int8_t radio_tx_power(void)
{
	return tx_power_dbm;
}

/* ---- Advertising policy ------------------------------------------------------ */

static void adv_apply(enum adv_mode mode)
{
	int err = bt_le_adv_stop();

	if (err) {
		LOG_WRN("advertising stop failed (%d)", err);
	}
	if (mode == ADV_NONE) {
		atomic_set(&adv_current, ADV_NONE);
		LOG_INF("advertising off");
		return;
	}
	const uint16_t interval = adv_interval[mode];
	const struct bt_le_adv_param param =
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, interval, interval, NULL);
	const atomic_val_t stops = atomic_get(&adv_stops);
	const char *name = bt_get_name();
	const struct bt_data sd[] = {
		BT_DATA(BT_DATA_UUID128_ALL, smp_uuid, sizeof(smp_uuid)),
		BT_DATA(BT_DATA_NAME_COMPLETE, name, MIN(strlen(name), (size_t)LINK_NAME_MAX)),
	};

	err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		/* Never silent by mistake: the thread tries again in a few seconds */
		LOG_ERR("advertising failed to start (%d)", err);
		atomic_set(&adv_current, ADV_NONE);
		return;
	}
	/* This thread has the lowest priority: a connection may have come and
	 * stopped the advertiser since it started (connected() sets ADV_NONE).
	 * Set, then check: whichever comes last, the mode is not left stale. */
	atomic_set(&adv_current, mode);
	if (atomic_get(&adv_stops) != stops) {
		atomic_set(&adv_current, ADV_NONE);
		return;
	}
	LOG_INF("advertising every %u.%03u ms", interval * 625U / 1000U, interval * 625U % 1000U);
}

/* The mode the policy wants now, and how long it holds */
static enum adv_mode wanted(int64_t now_ms, int64_t *until_ms)
{
	const int64_t elapsed = now_ms - window_start_ms;
	/* A trial of the calls makes the development build a product */
	const bool trial = trial_on();
	const bool apple = trial && atomic_get(&trial_profile) == LINK_TRIAL_APPLE;
	const int64_t fast_ms = apple ? APPLE_FAST_MS : CALL_FAST_MS;

	if (IS_ENABLED(CONFIG_CB91AI_WATCH_ADV_ALWAYS) && !trial) {
		if (elapsed < DEV_FAST_MS) {
			*until_ms = window_start_ms + DEV_FAST_MS;
			return ADV_FAST;
		}
		*until_ms = 0;
		return ADV_SLOW;
	}
	if (window_start_ms < 0 || elapsed >= CALL_WINDOW_MS) {
		*until_ms = 0;
		return ADV_NONE;
	}
	if (elapsed < fast_ms) {
		*until_ms = window_start_ms + fast_ms;
		return apple ? ADV_APPLE_FAST : ADV_FAST;
	}
	*until_ms = window_start_ms + CALL_WINDOW_MS;
	return apple ? ADV_APPLE_SLOW : ADV_SLOW;
}

static void policy_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	bool trial_was_on = false;
	bool window_timed = false;

	window_start_ms = IS_ENABLED(CONFIG_CB91AI_WATCH_ADV_ALWAYS) ? 0 : -1;
	for (;;) {
		k_timeout_t wait = K_FOREVER;
		const int64_t now_ms = k_uptime_get();
		const uint32_t now = (uint32_t)(now_ms / 1000);

		if (atomic_get(&connections) > 0) {
			const uint32_t deadline = (uint32_t)atomic_get(&idle_deadline_s);
			/* The session's BYE: that link only, however busy it is */
			const bool cut = atomic_get(&cut_link) != 0 &&
					 atomic_get(&cut_link) == atomic_get(&link_number);

			atomic_set(&adv_current, ADV_NONE); /* stopped by the controller */
			if (cut || (deadline != 0 && now >= deadline)) {
				/* Own reference: disconnected() may drop current_conn meanwhile */
				struct bt_conn *conn = radio_conn();

				atomic_set(&idle_deadline_s, 0);
				if (conn) {
					int err;

					LOG_INF("%s: disconnecting", cut ? "session over"
									 : "connection idle (EF-33)");
					err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
					bt_conn_unref(conn);
					if (err) {
						/* Refused: again soon, or the link would stay for good */
						atomic_set(&idle_deadline_s, now + 5U);
						wait = K_SECONDS(5);
					}
				}
			} else if (deadline != 0) {
				wait = K_SECONDS(deadline - now);
			}
		} else if (atomic_get(&ready)) {
			int64_t until_ms;
			enum adv_mode mode;
			const bool trial = trial_on();

			/* A trial of the calls starts silent; over or lapsed, it
			 * gives the development build its advertising back, fast */
			if (atomic_cas(&trial_changed, 1, 0) || trial != trial_was_on) {
				window_start_ms = trial ? -1 : now_ms;
				trial_was_on = trial;
			}
			/* A session ran to its BYE: the phone came, the rest of the
			 * window would only bring it back for nothing. A call made
			 * since opens a window of its own just after. */
			if (atomic_cas(&answered, 1, 0)) {
				window_start_ms = -1;
			}
			if (atomic_cas(&called, 1, 0)) {
				window_start_ms = now_ms;
				window_timed = false;
				if (atomic_cas(&asked, 1, 0)) {
					/* Made during a link, a call opens its window
					 * only now: the silence counts from here */
					atomic_set(&call_s, (atomic_val_t)now);
				}
			}
			mode = wanted(now_ms, &until_ms);
			/* A new name goes out at once, without waiting for the mode
			 * to change */
			const bool renew = atomic_cas(&renamed, 1, 0) && mode != ADV_NONE;

			if (mode != (enum adv_mode)atomic_get(&adv_current) || renew) {
				adv_apply(mode);
			}
			/* The first advertisement of a window, which the trial of the
			 * calls times the connection from */
			if (mode == ADV_NONE) {
				atomic_set(&window_adv_ms, 0);
			} else if (!window_timed && atomic_get(&adv_current) != ADV_NONE) {
				atomic_set(&window_adv_ms, (atomic_val_t)(k_uptime_get_32() | 1U));
				window_timed = true;
			}
			int64_t wake_ms = -1; /* none */

			if (mode != ADV_NONE && atomic_get(&adv_current) == ADV_NONE) {
				wake_ms = now_ms + 5000; /* failed to start: try again */
			} else if (until_ms > now_ms) {
				wake_ms = until_ms;
			}
			/* Awake when the trial lapses, whatever the window does */
			if (trial) {
				const int64_t end_ms = (int64_t)(uint32_t)atomic_get(&trial_until_s) * 1000;

				if (wake_ms < 0 || end_ms < wake_ms) {
					wake_ms = end_ms;
				}
			}
			if (wake_ms >= 0) {
				wait = K_MSEC(MAX(wake_ms - now_ms, 1));
			}
		}
		k_sem_take(&policy_sem, wait);
	}
}

/* 3 KB, as the self-test's, which runs the same HCI commands: a sealed watch
 * gives no way to measure the stack it needs */
K_THREAD_DEFINE(radio_policy, 3072, policy_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

void radio_call(enum radio_reason why)
{
	/* Stamped at once, so that the event loop never calls twice; again
	 * when the window opens, should a link delay it */
	atomic_set(&call_s, (atomic_val_t)uptime_s());
	atomic_inc(&calls);
	atomic_set(&reason, why);
	atomic_set(&asked, 1);
	atomic_set(&called, 1);
	k_sem_give(&policy_sem);
}

void radio_renamed(void)
{
	atomic_set(&renamed, 1);
	k_sem_give(&policy_sem);
}

void radio_answered(void)
{
	/* The development build advertises all the time, nothing to close, but
	 * in a trial of the calls */
	if (!IS_ENABLED(CONFIG_CB91AI_WATCH_ADV_ALWAYS) || trial_on()) {
		atomic_set(&answered, 1);
		k_sem_give(&policy_sem);
	}
}

enum radio_reason radio_reason(void)
{
	return (enum radio_reason)atomic_get(&reason);
}

uint32_t radio_calls(void)
{
	return (uint32_t)atomic_get(&calls);
}

uint32_t radio_last_call_s(void)
{
	return (uint32_t)atomic_get(&call_s);
}

uint32_t radio_last_link_end_s(void)
{
	return (uint32_t)atomic_get(&link_end_s);
}

const char *radio_adv_mode(void)
{
	switch (atomic_get(&adv_current)) {
	case ADV_FAST:
	case ADV_APPLE_FAST:
		return "fast";
	case ADV_SLOW:
	case ADV_APPLE_SLOW:
		return "slow";
	default:
		return "off";
	}
}

/* ---- Connections ------------------------------------------------------------ */

void radio_activity(void)
{
	if (atomic_get(&connections) > 0) {
		const atomic_val_t want = (atomic_val_t)(uptime_s() + IDLE_DISCONNECT_S);
		atomic_val_t old;

		do {
			old = atomic_get(&idle_deadline_s);
		} while (want > old && !atomic_cas(&idle_deadline_s, old, want));
		k_sem_give(&policy_sem);
	}
}

bool radio_link_params(uint32_t *interval_us, uint16_t *latency, uint16_t *timeout_10ms)
{
	struct bt_conn *conn = radio_conn();
	struct bt_conn_info info;
	bool ok = false;

	if (conn != NULL) {
		ok = bt_conn_get_info(conn, &info) == 0;
		if (ok) {
			*interval_us = info.le.interval_us;
			*latency = info.le.latency;
			*timeout_10ms = info.le.timeout;
		}
		bt_conn_unref(conn);
	}
	return ok;
}

struct bt_conn *radio_conn(void)
{
	unsigned int key = irq_lock();
	struct bt_conn *conn = current_conn ? bt_conn_ref(current_conn) : NULL;

	irq_unlock(key);
	return conn;
}

unsigned int radio_link(void)
{
	return atomic_get(&connections) > 0 ? (unsigned int)atomic_get(&link_number) : 0U;
}

void radio_disconnect(unsigned int link)
{
	/* An HCI command: done by the policy thread, as the idle disconnect is.
	 * A flag of its own, which host activity cannot push back, and tied to
	 * the link: a late call never cuts the next phone. */
	atomic_set(&cut_link, (atomic_val_t)link);
	k_sem_give(&policy_sem);
}

/* A call answered during a trial of the calls: its time from the first
 * advertisement of its window to this connection */
static void trial_timed(const struct bt_conn_info *info, uint32_t first_adv_ms)
{
	const uint32_t now = k_uptime_get_32();
	const uint32_t number = (uint32_t)atomic_get(&calls);
	k_spinlock_key_t key = k_spin_lock(&trial_lock);

	trial_last.number = number;
	trial_last.reason = (uint8_t)atomic_get(&reason);
	trial_last.connect_ms = now - first_adv_ms;
	trial_last.read_ms = LINK_TRIAL_NONE;
	trial_last.interval = (uint16_t)(info->le.interval_us / 1250U);
	trial_last.latency = info->le.latency;
	trial_last.timeout = info->le.timeout;
	trial_conn_ms = now;
	trial_conn_open = true;
	k_spin_unlock(&trial_lock, key);
	LOG_INF("trial: call %u answered %u ms after its first advertisement", number,
		now - first_adv_ms);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;
	/* The window this link answers, taken whatever happens: the next one
	 * counts from its own first advertisement */
	const uint32_t first_adv_ms = (uint32_t)atomic_set(&window_adv_ms, 0);

	/* Either way the controller has stopped advertising: the policy thread
	 * must not believe it still runs (0x3E, a link that failed to set up) */
	atomic_inc(&adv_stops);
	atomic_set(&adv_current, ADV_NONE);
	if (err) {
		LOG_ERR("connection failed (0x%02x)", err);
		atomic_set(&resume_after_disconnect, 1); /* recycled() brings it back */
		return;
	}
	atomic_inc(&connections);
	atomic_inc(&link_number);
	atomic_set(&cut_link, 0);
	connected_at_ms = k_uptime_get();
	if (!current_conn) {
		current_conn = bt_conn_ref(conn);
	}
	apply_conn_tx_power(conn);
	if (bt_conn_get_info(conn, &info) == 0) {
		LOG_INF("connected: interval %u us, latency %u, supervision %u ms",
			info.le.interval_us, info.le.latency, info.le.timeout * 10U);
		if (first_adv_ms != 0 && trial_on()) {
			trial_timed(&info, first_adv_ms);
		}
	}
	/* Set, not pushed: a push of the connection before may land after its end */
	atomic_set(&idle_deadline_s, uptime_s() + IDLE_DISCONNECT_S);
	k_sem_give(&policy_sem);
	(void)evt_post(EVT_BLE_CONNECTED, 0, 0, 0);
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	ARG_UNUSED(conn);
	LOG_INF("link: interval %u us, latency %u, supervision %u ms", interval * 1250U, latency,
		timeout * 10U);
}

static void disconnected(struct bt_conn *conn, uint8_t why)
{
	atomic_dec(&connections);
	atomic_set(&last_disconnect_reason, why);
	atomic_set(&link_end_s, (atomic_val_t)uptime_s());
	atomic_set(&idle_deadline_s, 0);
	atomic_set(&cut_link, 0);
	atomic_set(&resume_after_disconnect, 1);
	atomic_add(&connected_total_s, (atomic_val_t)((k_uptime_get() - connected_at_ms) / 1000));
	if (IS_ENABLED(CONFIG_CB91AI_WATCH_DEBUG_TRIAL)) {
		/* The first read of the trial times this link only */
		k_spinlock_key_t key = k_spin_lock(&trial_lock);

		trial_conn_open = false;
		k_spin_unlock(&trial_lock, key);
	}
	if (current_conn == conn) {
		unsigned int key = irq_lock();

		current_conn = NULL;
		irq_unlock(key);
		bt_conn_unref(conn);
	}
	LOG_INF("disconnected (reason 0x%02x)", why);
	(void)evt_post(EVT_BLE_DISCONNECTED, why, 0, 0);
}

static void recycled(void)
{
	/* A connection object is free again. After a disconnection, the
	 * development build advertises fast again; the product stays silent
	 * unless something calls it. After our own bt_le_adv_stop(): nothing,
	 * see the top of this file. */
	if (atomic_cas(&resume_after_disconnect, 1, 0)) {
		/* A trial of the calls keeps silent, as the product */
		if (IS_ENABLED(CONFIG_CB91AI_WATCH_ADV_ALWAYS) && !trial_on()) {
			atomic_set(&called, 1);
		}
		k_sem_give(&policy_sem);
	}
}

BT_CONN_CB_DEFINE(radio_conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_updated = le_param_updated,
	.recycled = recycled,
};

unsigned int radio_connections(void)
{
	return (unsigned int)atomic_get(&link_number);
}

bool radio_connected(void)
{
	return atomic_get(&connections) > 0;
}

bool radio_advertising(void)
{
	return atomic_get(&adv_current) != ADV_NONE;
}

uint8_t radio_last_disconnect_reason(void)
{
	return (uint8_t)atomic_get(&last_disconnect_reason);
}

uint32_t radio_connected_s(void)
{
	uint32_t total = (uint32_t)atomic_get(&connected_total_s);

	if (atomic_get(&connections) > 0) {
		total += (uint32_t)((k_uptime_get() - connected_at_ms) / 1000);
	}
	return total;
}

bool radio_trial_set(uint8_t minutes, uint8_t profile)
{
	if (!IS_ENABLED(CONFIG_CB91AI_WATCH_DEBUG_TRIAL) || minutes > LINK_TRIAL_MAX_MIN ||
	    (profile != LINK_TRIAL_PRODUCT && profile != LINK_TRIAL_APPLE)) {
		return false;
	}
	atomic_set(&trial_profile, profile);
	atomic_set(&trial_until_s, minutes ? (atomic_val_t)(uptime_s() + minutes * 60U) : 0);
	atomic_set(&trial_changed, 1);
	k_sem_give(&policy_sem);
	LOG_INF("trial of the calls: %u min, %s profile", minutes,
		profile == LINK_TRIAL_APPLE ? "apple" : "product");
	return true;
}

void radio_trial_get(struct link_trial *out)
{
	const uint32_t until = (uint32_t)atomic_get(&trial_until_s);
	const uint32_t now = uptime_s();
	k_spinlock_key_t key = k_spin_lock(&trial_lock);

	if (trial_conn_open && trial_last.number != 0 && trial_last.read_ms == LINK_TRIAL_NONE) {
		trial_last.read_ms = k_uptime_get_32() - trial_conn_ms;
	}
	*out = trial_last;
	k_spin_unlock(&trial_lock, key);
	out->minutes_left = (uint16_t)(until > now ? (until - now + 59U) / 60U : 0U);
	out->profile = (uint8_t)atomic_get(&trial_profile);
}

int radio_init(void)
{
	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}
	/* The identity and the bond (lot S1): the host finishes its start-up
	 * once they are read, advertising included. Unfinished, the watch would
	 * stay silent for good: the reset of main() instead. */
	err = settings_load_subtree("bt");
	if (err || !bt_is_ready()) {
		LOG_ERR("Bluetooth settings not read (%d)", err);
		return err ? err : -EAGAIN;
	}
	atomic_set(&ready, 1);
	k_sem_give(&policy_sem);
	return 0;
}
