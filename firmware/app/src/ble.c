/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bluetooth LE part of the CB-91AI self-test: connectable advertising under
 * the device name, Device Information and Battery services, connection
 * logging. Exercises the 32 MHz crystal (X3), the antenna path (U106, L1, L2)
 * and the LFXO used as sleep clock by the controller (V2-23).
 *
 * Radio energy policy, so that the same image can live on a CR2016 in the
 * watch case and stay reachable for a Bluetooth update:
 * - 0 dBm by default (EF-31), `cb91ai txpower` changes it for range tests;
 * - fast advertising (100 to 150 ms) for 30 s after boot, after a disconnect
 *   and after any button press, then slow advertising (1 to 1.2 s, the GAP
 *   "slow" interval) for good. Never silent: a sealed watch has no other way
 *   in than the radio. At 2 to 2.5 s the scanner of the bench PC, which
 *   listens about one tenth of the time, heard one event in 40 s; at 1 s it
 *   still needs 10 to 70 s to connect, more than a whole update, so the
 *   advertiser stays fast while USB power is there: energy is free then;
 * - a connection left idle is dropped after 5 min (EF-33): the interval the
 *   central picked (30 to 50 ms) costs about 100 uA;
 * - trial mode (`cb91ai adv trial`, lot N1a): silent between trials, a burst
 *   of advertising at a button press, timed to the connection of a phone
 *   (section "Advertising trials" below). It lapses on its own.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#include <nrfx.h>

#include "ble.h"
#include "clock.h"

/* Defined in main.c: reading the debug status is the host's "I saw the new
 * firmware" signal, which confirms a pending image early (EF-62). */
void selftest_request_confirm(void);
/* Defined in main.c: wakes the main loop, which redraws the glass */
void selftest_wake(void);

LOG_MODULE_REGISTER(cb91ai_ble, LOG_LEVEL_INF);

#define ADV_FAST_WINDOW_S  30
#define IDLE_DISCONNECT_S  300

/* Slow advertising: 1 s to 1.2 s */
#define ADV_SLOW_INT_MIN BT_GAP_ADV_SLOW_INT_MIN
#define ADV_SLOW_INT_MAX BT_GAP_ADV_SLOW_INT_MAX

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_BAS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_DIS_VAL)),
	/*
	 * SMP service (MCUmgr over Bluetooth): advertised so nRF Connect Device
	 * Manager - which discovers by service UUID, not by name - lists the watch
	 * for a firmware update (EF-60). The Cobalt app instead filters its scan on
	 * the "Cobalt XXXX" name (EF-35); that dynamic name arrives with the product
	 * firmware (lot L2), so the in-app update path is not exercised by this
	 * self-test yet. UUID as encoded by the transport:
	 * 8D53DC1D-1DB7-4CD3-868B-8A527460AA84.
	 */
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		      BT_UUID_128_ENCODE(0x8d53dc1d, 0x1db7, 0x4cd3, 0x868b, 0x8a527460aa84)),
};

/*
 * Cobalt Link, the service of the product:
 * C0B91B00-256E-46F9-8D7B-D9C643908667. The self-test does not implement
 * it. It only puts its UUID in the advertisement of a trial (lot N1a), where
 * the product will put it: a phone in the background looks for an accessory
 * by the service UUID of its advertisements, so the trials find the watch the
 * way the product will be found.
 */
#define BT_UUID_CB_LINK_SVC BT_UUID_128_ENCODE(0xc0b91b00, 0x256e, 0x46f9, 0x8d7b, 0xd9c643908667)

static const struct bt_data ad_trial[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_CB_LINK_SVC),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};
/* Flags (3 bytes), the UUID (18) and the name: 31 bytes at most */
BUILD_ASSERT(3 + 18 + 2 + sizeof(CONFIG_BT_DEVICE_NAME) - 1 <= 31, "trial advertisement too long");

/*
 * State of the advertising trials, shared by the radio policy thread (the
 * burst), the Bluetooth callbacks (the connection, the read of the result), the
 * main loop (the glass) and the shell: every access under trial_lock, never
 * across an HCI command.
 */
#define TRIAL_IDLE_S 10 /* a trial connection with no host activity is dropped */
#define TRIAL_LOG    BLE_TRIAL_LOG

static struct k_spinlock trial_lock;
static struct {
	uint32_t until_s;   /* trial mode while the uptime is below; 0: off */
	uint32_t lifetime_s;
	uint8_t profile;
	uint8_t step;
	bool running;       /* a burst is on the air, or about to be */
	bool t0_valid;      /* t0_ms is the first packet, not the request */
	bool requested;     /* a button asked for a burst */
	bool go;            /* `adv trial go`: a burst when the host leaves */
	bool conn_open;     /* the connection of the last trial is up */
	int64_t t0_ms;
	int64_t conn_ms;
	uint32_t count;     /* trials since boot; log[(count - 1) % TRIAL_LOG] is the last */
	struct ble_trial_entry log[TRIAL_LOG];
} trial;

static struct ble_trial_entry *trial_last(void)
{
	return &trial.log[(trial.count - 1U) % TRIAL_LOG];
}

static void idle_push(uint32_t seconds);

/*
 * Trial characteristic (C0B91A03, read), for the phone apps of lot N1a: the
 * last trial as the watch timed it, 16 bytes little-endian: format (1), trial
 * number (low byte, 0 before the first trial), profile (0 apple, 1 link), step
 * of the burst the phone answered, first packet to connection in ms, connection
 * to the first read of this value in ms (0xffffffff: none), then the interval
 * (1.25 ms units) and the supervision timeout (10 ms units) of the link. The
 * first read after a trial connection is what an app does once the system has
 * woken it: the delay it took is the second figure. Reading keeps the link for
 * TRIAL_IDLE_S more; the status characteristic gives the usual five minutes.
 */
static ssize_t read_trial(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, uint16_t len, uint16_t offset)
{
	uint8_t value[16] = { 1 };
	k_spinlock_key_t key = k_spin_lock(&trial_lock);

	if (trial.count > 0) {
		struct ble_trial_entry *e = trial_last();

		if (trial.conn_open && e->read_ms == BLE_TRIAL_NONE) {
			e->read_ms = (int32_t)(k_uptime_get() - trial.conn_ms);
		}
		value[1] = (uint8_t)e->number;
		value[2] = e->profile;
		value[3] = e->step;
		sys_put_le32((uint32_t)e->connect_ms, &value[4]);
		sys_put_le32((uint32_t)e->read_ms, &value[8]);
		sys_put_le16(e->interval, &value[12]);
		sys_put_le16(e->timeout, &value[14]);
	} else {
		sys_put_le32(UINT32_MAX, &value[4]);
		sys_put_le32(UINT32_MAX, &value[8]);
	}
	k_spin_unlock(&trial_lock, key);
	idle_push(TRIAL_IDLE_S);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

/*
 * Cobalt debug service: one read + notify characteristic carrying a short text
 * status line (version, supplies, USB, uptime, image confirm state,
 * connections). It lets the bench read diagnostics over BLE when RTT and the
 * USB console are unavailable. Not advertised - the 31-byte scan response is
 * already full - so a client discovers it after connecting for SMP. Reading it
 * also confirms a pending image (EF-62 accelerator; tighten to an encrypted
 * link once EF-37 pairing lands at P2).
 */
#define BT_UUID_CB_DBG_SVC BT_UUID_128_ENCODE(0xc0b91a00, 0x1db7, 0x4cd3, 0x868b, 0x8a527460db61)
#define BT_UUID_CB_DBG_CHR BT_UUID_128_ENCODE(0xc0b91a01, 0x1db7, 0x4cd3, 0x868b, 0x8a527460db61)
static const struct bt_uuid_128 dbg_svc_uuid = BT_UUID_INIT_128(BT_UUID_CB_DBG_SVC);
static const struct bt_uuid_128 dbg_chr_uuid = BT_UUID_INIT_128(BT_UUID_CB_DBG_CHR);

/*
 * Written by the main loop (ble_dbg_set_status) and read by read_dbg() on the
 * BT thread without a lock: a concurrent GATT read can splice two status lines.
 * Benign - it is a diagnostic string, and image confirmation is driven by the
 * atomic confirm_requested flag in main.c, not by this buffer.
 */
static char dbg_status[224] = "boot";

static ssize_t read_dbg(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			void *buf, uint16_t len, uint16_t offset)
{
	selftest_request_confirm();
	ble_activity();
	return bt_gatt_attr_read(conn, attr, buf, len, offset, dbg_status, strlen(dbg_status));
}

/*
 * Time characteristic (EI-03, development form: it will move into the link
 * protocol of the product, behind an authenticated link). All little-endian.
 * Write, 8 or 10 bytes: UTC in ms since 1970 (int64), then the offset of the
 * local time in minutes (int16). Each write also lets the watch calibrate its
 * crystal (clock.c). Read, 23 bytes: corrected UTC in ms (int64, 0 when not
 * set), raw uptime in ms (int64), correction in ppb (int32), local offset in
 * minutes (int16), flags (CLOCK_FLAG_*).
 */
#define BT_UUID_CB_TIME_CHR BT_UUID_128_ENCODE(0xc0b91a02, 0x1db7, 0x4cd3, 0x868b, 0x8a527460db61)
static const struct bt_uuid_128 time_chr_uuid = BT_UUID_INIT_128(BT_UUID_CB_TIME_CHR);

static ssize_t read_time(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	uint8_t value[23];

	sys_put_le64((uint64_t)clock_now_ms(), &value[0]);
	sys_put_le64((uint64_t)k_uptime_get(), &value[8]);
	sys_put_le32((uint32_t)clock_ppb(), &value[16]);
	sys_put_le16((uint16_t)clock_tz_minutes(), &value[20]);
	value[22] = clock_flags();
	ble_activity();
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static ssize_t write_time(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *data = buf;

	ARG_UNUSED(flags);
	if (offset != 0 || (len != 8 && len != 10)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	clock_set((int64_t)sys_get_le64(data),
		  len == 10 ? (int16_t)sys_get_le16(&data[8]) : clock_tz_minutes(), true);
	ble_activity();
	return len;
}

#define BT_UUID_CB_TRIAL_CHR BT_UUID_128_ENCODE(0xc0b91a03, 0x1db7, 0x4cd3, 0x868b, 0x8a527460db61)
static const struct bt_uuid_128 trial_chr_uuid = BT_UUID_INIT_128(BT_UUID_CB_TRIAL_CHR);

BT_GATT_SERVICE_DEFINE(cb_dbg_svc,
	BT_GATT_PRIMARY_SERVICE(&dbg_svc_uuid),
	BT_GATT_CHARACTERISTIC(&dbg_chr_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_dbg, NULL, dbg_status),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&time_chr_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_time, write_time, NULL),
	BT_GATT_CHARACTERISTIC(&trial_chr_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_trial, NULL, NULL),
);

void ble_dbg_set_status(const char *status)
{
	strncpy(dbg_status, status, sizeof(dbg_status) - 1);
	dbg_status[sizeof(dbg_status) - 1] = '\0';
	/* attrs[2] is the characteristic value; a no-op if nobody has subscribed. */
	(void)bt_gatt_notify(NULL, &cb_dbg_svc.attrs[2], dbg_status, strlen(dbg_status));
}

static atomic_t connections;
static unsigned int connect_count;
/* HCI reason of the last disconnection, for the status line: on a sealed watch
 * it tells a supervision timeout (0x08) from a host that closed (0x13) */
static atomic_t last_disconnect_reason;
static struct bt_conn *current_conn;

/* ---- Transmit power ------------------------------------------------------ */

static int8_t tx_power_dbm = CONFIG_BT_CTLR_TX_PWR_DBM;

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

int ble_set_tx_power(int8_t dbm)
{
	int8_t selected = dbm;
	int err = write_tx_power(BT_HCI_VS_LL_HANDLE_TYPE_ADV, 0, dbm, &selected);

	if (err) {
		LOG_ERR("tx power %d dBm refused (%d)", dbm, err);
		return err;
	}
	tx_power_dbm = selected;
	if (current_conn) {
		apply_conn_tx_power(current_conn);
	}
	LOG_INF("tx power %d dBm (asked %d)", selected, dbm);
	return 0;
}

int8_t ble_tx_power(void)
{
	return tx_power_dbm;
}

/* ---- Receiver check ------------------------------------------------------ */

/* Passive scan used as a receiver check: advertisements heard and best RSSI */
static unsigned int rx_adverts;
static int8_t rx_best_rssi = -128;
static char rx_best_addr[BT_ADDR_LE_STR_LEN];

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type, struct net_buf_simple *buf)
{
	rx_adverts++;
	if (rssi > rx_best_rssi) {
		rx_best_rssi = rssi;
		bt_addr_le_to_str(addr, rx_best_addr, sizeof(rx_best_addr));
	}
}

/*
 * Listen for a few seconds before advertising. Hearing other devices proves
 * the receive path (antenna, matching, LNA, crystal): a board that hears
 * nothing while phones and PCs are around has a broken RF path or a crystal
 * far off frequency. Three seconds of receiver cost about 5 uAh and a long
 * draw on a coin cell: bench only.
 */
static int rx_check(int seconds)
{
	int err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, scan_cb);

	if (err) {
		LOG_ERR("scan failed to start (%d)", err);
		return err;
	}
	k_sleep(K_SECONDS(seconds));
	bt_le_scan_stop();
	if (rx_adverts == 0) {
		LOG_WRN("rx check: no advertisement heard in %d s", seconds);
	} else {
		LOG_INF("rx check: %u advertisements in %d s, best RSSI %d dBm from %s",
			rx_adverts, seconds, rx_best_rssi, rx_best_addr);
	}
	return 0;
}

/* ---- Radio policy thread: advertising mode and idle disconnect ------------ */

/*
 * Stopping and restarting the advertiser, and disconnecting, are synchronous
 * HCI commands. They run here, on a thread of their own, rather than on the
 * system work queue, where the host processes its own command queue.
 *
 * Trap: stopping a connectable advertiser releases the connection object it
 * had reserved, which fires the recycled() callback, exactly as the end of a
 * connection does. recycled() must therefore only bring fast advertising back
 * after a real disconnection (resume_after_disconnect), or every switch to
 * slow advertising undoes itself at once. The first 0.1.21 build restarted the
 * advertiser from recycled() inside its own stop, and reset 30 s after each
 * boot (2026-09-19).
 */
enum adv_mode { ADV_NONE, ADV_FAST, ADV_SLOW, ADV_TRIAL };

static atomic_t adv_current = ATOMIC_INIT(ADV_NONE);
static atomic_t adv_last_err;      /* last bt_le_adv_stop/start error, for the status line */
static atomic_t fast_requested;
static atomic_t fast_until_s;      /* uptime, s: end of the fast window */
static atomic_t fast_hold_until_s; /* uptime, s: fast held for a test session; 0 = none */
static atomic_t idle_deadline_s;   /* uptime, s: idle connection dropped; 0 = none */
static atomic_t ble_ready;
static atomic_t resume_after_disconnect;
static K_SEM_DEFINE(policy_sem, 0, 1);

static uint32_t uptime_s(void)
{
	return (uint32_t)(k_uptime_get() / 1000);
}

/* USB power: the bench, where the radio budget of a coin cell does not apply */
static bool usb_present(void)
{
	return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}

static void adv_apply(enum adv_mode mode)
{
	int err;

	err = bt_le_adv_stop();
	if (err) {
		LOG_WRN("advertising stop failed (%d)", err);
		atomic_set(&adv_last_err, err);
	}
	if (mode == ADV_FAST) {
		err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	} else {
		err = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, ADV_SLOW_INT_MIN,
						      ADV_SLOW_INT_MAX, NULL),
				      ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	}
	if (err) {
		/* Never stay silent: the loop below tries again in a few seconds */
		LOG_ERR("advertising failed to start (%d)", err);
		atomic_set(&adv_last_err, err);
		atomic_set(&adv_current, ADV_NONE);
		return;
	}
	atomic_set(&adv_current, mode);
	LOG_INF("advertising as \"%s\", %s", CONFIG_BT_DEVICE_NAME,
		mode == ADV_FAST ? "fast (100-150 ms)" : "slow (1-1.2 s)");
}

/* ---- Advertising trials (lot N1a, risk R11) -------------------------------- */

/*
 * The product keeps its radio off and advertises only when it has something to
 * say; the phone keeps a pending connection to it and gets in when it hears
 * it. How long that takes, per phone, screen
 * on or off, app in the background or not, decides the energy of every session
 * and whether the design holds: that is risk R11, measured in lot N1a.
 *
 * In trial mode the watch keeps silent between trials. A press on a case
 * button starts a burst of connectable advertising, with the Cobalt Link UUID
 * in the advertisement itself, stepping through the intervals of a profile:
 * - apple: what Apple recommends to accessories, 20 ms for 30 s, then
 *   152.5 ms, then 1022.5 ms, here to 5 min (Accessory Design Guidelines);
 * - link: what the Cobalt Link specification plans today, 100 ms for
 *   10 s, then 1 s for 2 min.
 * The controller advertises at the minimum interval it is given, plus the
 * random delay of up to 10 ms that the standard adds to every event.
 *
 * The watch times the burst from its first packet to the connection, and a
 * phone app can read the result (read_trial). As a product session would, the
 * watch drops a trial connection after TRIAL_IDLE_S without host activity; an
 * SMP or shell command gives it the usual five minutes. Trial mode lapses after
 * its lifetime without a trial, and normal advertising resumes, fast first: a
 * sealed watch is never left silent for good, and a press always starts a
 * burst meanwhile.
 */
struct trial_step {
	uint16_t interval; /* 0.625 ms units */
	uint32_t until_ms; /* end of the step, from the first packet */
};

static const struct trial_step trial_apple[] = {
	{ 32, 30000 },    /* 20 ms for 30 s */
	{ 244, 120000 },  /* 152.5 ms */
	{ 1636, 300000 }, /* 1022.5 ms, to 5 min */
};

static const struct trial_step trial_link[] = {
	{ 160, 10000 },   /* 100 ms for 10 s */
	{ 1600, 130000 }, /* 1 s for 2 min */
};

static const struct {
	const char *name;
	const struct trial_step *steps;
	uint8_t count;
} trial_profiles[] = {
	[BLE_TRIAL_APPLE] = { "apple", trial_apple, ARRAY_SIZE(trial_apple) },
	[BLE_TRIAL_LINK] = { "link", trial_link, ARRAY_SIZE(trial_link) },
};

static int adv_apply_trial(uint16_t interval)
{
	const struct bt_le_adv_param param =
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, interval, interval, NULL);
	int err = bt_le_adv_stop();

	if (err) {
		LOG_WRN("advertising stop failed (%d)", err);
		atomic_set(&adv_last_err, err);
	}
	err = bt_le_adv_start(&param, ad_trial, ARRAY_SIZE(ad_trial), sd, ARRAY_SIZE(sd));
	if (err) {
		/* The policy tries again within a second */
		LOG_ERR("trial advertising failed to start (%d)", err);
		atomic_set(&adv_last_err, err);
		atomic_set(&adv_current, ADV_NONE);
		return err;
	}
	atomic_set(&adv_current, ADV_TRIAL);
	LOG_INF("trial advertising, %u.%u ms", interval * 625U / 1000U, interval * 625U / 100U % 10U);
	return 0;
}

/* Between two trials. Stopping a connectable advertiser fires recycled(),
 * which does nothing then: see the note above policy_thread(). */
static void adv_silence(void)
{
	int err = bt_le_adv_stop();

	if (err) {
		LOG_WRN("advertising stop failed (%d)", err);
		atomic_set(&adv_last_err, err);
	}
	atomic_set(&adv_current, ADV_NONE);
}

/*
 * Trial mode, no connection: run the burst, or keep silent. Decides under the
 * lock, then acts without it (HCI commands block). Returns false when trial
 * mode is off or has just lapsed: the normal policy runs then.
 */
static bool trial_policy(k_timeout_t *wait)
{
	enum { KEEP, START, STEP, END, SILENCE, LAPSE } action = KEEP;
	const int64_t now_ms = k_uptime_get();
	const uint32_t now = (uint32_t)(now_ms / 1000);
	k_spinlock_key_t key = k_spin_lock(&trial_lock);
	const bool on_air = atomic_get(&adv_current) == ADV_TRIAL;
	const struct trial_step *steps = trial_profiles[trial.profile].steps;
	const uint8_t count = trial_profiles[trial.profile].count;
	uint32_t left_ms = 0, number;
	uint8_t step = 0;

	if (trial.until_s == 0) {
		k_spin_unlock(&trial_lock, key);
		return false;
	}
	if (trial.running) {
		const uint32_t elapsed = (uint32_t)(now_ms - trial.t0_ms);

		while (step < count && elapsed >= steps[step].until_ms) {
			step++;
		}
		if (step == count) {
			trial.running = false;
			/* The lifetime counts from the end of the last trial */
			trial.until_s = MAX(trial.until_s, now + trial.lifetime_s);
			action = END;
		} else {
			left_ms = steps[step].until_ms - elapsed;
			if (step != trial.step || !on_air) {
				trial.step = step;
				action = STEP;
			}
		}
	} else if (trial.requested || trial.go) {
		struct ble_trial_entry *e = &trial.log[trial.count % TRIAL_LOG];

		memset(e, 0, sizeof(*e));
		trial.count++;
		e->number = trial.count;
		e->at_s = now;
		e->connect_ms = BLE_TRIAL_NONE;
		e->read_ms = BLE_TRIAL_NONE;
		e->profile = trial.profile;
		trial.requested = false;
		trial.go = false;
		trial.running = true;
		trial.t0_valid = false;
		trial.t0_ms = now_ms; /* until the first packet, just below */
		trial.step = 0;
		trial.until_s = now + trial.lifetime_s;
		left_ms = steps[0].until_ms;
		action = START;
	} else if (now >= trial.until_s) {
		trial.until_s = 0;
		action = LAPSE;
	} else {
		left_ms = (trial.until_s - now) * 1000U;
		if (atomic_get(&adv_current) != ADV_NONE) {
			action = SILENCE;
		}
	}
	number = trial.count;
	k_spin_unlock(&trial_lock, key);

	switch (action) {
	case START:
		LOG_INF("trial %u: burst, profile %s", number, trial_profiles[trial.profile].name);
		__fallthrough;
	case STEP:
		if (adv_apply_trial(steps[step].interval) == 0) {
			key = k_spin_lock(&trial_lock);
			if (trial.running && !trial.t0_valid) {
				trial.t0_ms = k_uptime_get();
				trial.t0_valid = true;
			}
			k_spin_unlock(&trial_lock, key);
		} else {
			left_ms = MIN(left_ms, 1000U);
		}
		if (action == START) {
			selftest_wake();
		}
		break;
	case END:
		adv_silence();
		LOG_INF("trial %u: no connection in %u s", number, steps[count - 1].until_ms / 1000U);
		selftest_wake();
		key = k_spin_lock(&trial_lock);
		left_ms = trial.until_s > now ? (trial.until_s - now) * 1000U : 1000U;
		k_spin_unlock(&trial_lock, key);
		break;
	case SILENCE:
		adv_silence();
		break;
	case LAPSE:
		LOG_INF("trial mode over: normal advertising");
		atomic_set(&fast_requested, 1);
		selftest_wake();
		return false;
	default:
		break;
	}
	*wait = K_MSEC(MAX(left_ms, 1U));
	return true;
}

static void policy_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_timeout_t wait = K_FOREVER;
		uint32_t now = uptime_s();

		if (atomic_get(&connections) > 0) {
			uint32_t deadline = atomic_get(&idle_deadline_s);

			atomic_set(&adv_current, ADV_NONE); /* stopped by the stack */
			if (deadline != 0 && now >= deadline) {
				/* Own reference: disconnected() may drop current_conn meanwhile */
				unsigned int key = irq_lock();
				struct bt_conn *conn = current_conn ? bt_conn_ref(current_conn) : NULL;

				irq_unlock(key);
				atomic_set(&idle_deadline_s, 0);
				if (conn) {
					int err;

					LOG_INF("connection idle: disconnecting (EF-33)");
					err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
					bt_conn_unref(conn);
					if (err) {
						/* Refused (no command buffer, say): try again
						 * soon, or the link would stay for good */
						LOG_WRN("disconnection refused (%d), again in 5 s", err);
						atomic_set(&idle_deadline_s, now + 5U);
						wait = K_SECONDS(5);
					}
				}
			} else if (deadline != 0) {
				wait = K_SECONDS(deadline - now);
			}
		} else if (atomic_get(&ble_ready) && trial_policy(&wait)) {
			/* Trial mode: the burst or the silence, nothing else */
		} else if (atomic_get(&ble_ready)) {
			enum adv_mode mode = atomic_get(&adv_current);

			if (atomic_cas(&fast_requested, 1, 0)) {
				atomic_set(&fast_until_s, now + ADV_FAST_WINDOW_S);
				if (mode != ADV_FAST) {
					adv_apply(ADV_FAST);
				}
			} else if (mode == ADV_FAST && now >= (uint32_t)atomic_get(&fast_until_s)) {
				if (usb_present() || now < (uint32_t)atomic_get(&fast_hold_until_s)) {
					/* Stay fast, and look again later: the cable may go,
					 * the test session end (cb91ai adv fast) */
					atomic_set(&fast_until_s, now + ADV_FAST_WINDOW_S);
				} else {
					adv_apply(ADV_SLOW);
				}
			} else if (mode == ADV_NONE || mode == ADV_TRIAL) {
				adv_apply(ADV_SLOW);
			}
			mode = atomic_get(&adv_current);
			if (mode == ADV_FAST) {
				uint32_t until = atomic_get(&fast_until_s);

				wait = K_SECONDS(until > now ? until - now : 1);
			} else if (mode == ADV_NONE) {
				wait = K_SECONDS(5);
			}
		}
		k_sem_take(&policy_sem, wait);
	}
}

K_THREAD_DEFINE(ble_policy, 3072, policy_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

void ble_adv_kick(void)
{
	k_spinlock_key_t key = k_spin_lock(&trial_lock);
	const bool trial_on = trial.until_s != 0;

	/* In trial mode a press starts a burst, unless one runs or a host is in */
	if (trial_on && !trial.running && atomic_get(&connections) == 0) {
		trial.requested = true;
	}
	k_spin_unlock(&trial_lock, key);
	if (!trial_on) {
		atomic_set(&fast_requested, 1);
	}
	k_sem_give(&policy_sem);
}

void ble_adv_hold_fast(uint32_t seconds)
{
	atomic_set(&fast_hold_until_s, seconds ? uptime_s() + seconds : 0);
	if (seconds) {
		ble_adv_kick();
	} else {
		k_sem_give(&policy_sem);
	}
}

int ble_adv_last_error(void)
{
	return atomic_get(&adv_last_err);
}

const char *ble_adv_mode(void)
{
	switch (atomic_get(&adv_current)) {
	case ADV_FAST:
		return "fast";
	case ADV_SLOW:
		return "slow";
	case ADV_TRIAL:
		return "trial";
	default:
		/* Between two trials the silence is wanted */
		return ble_trial_mode() ? "quiet" : "off";
	}
}

/* ---- Connection ----------------------------------------------------------- */

/* The idle deadline moves later, never earlier: a trial read must not cut
 * short the five minutes an SMP or shell command gave the host. Pushed from
 * the Bluetooth, SMP and main threads at once: compare and swap. */
static void idle_push(uint32_t seconds)
{
	if (atomic_get(&connections) > 0) {
		const atomic_val_t want = (atomic_val_t)(uptime_s() + seconds);
		atomic_val_t old;

		do {
			old = atomic_get(&idle_deadline_s);
		} while (want > old && !atomic_cas(&idle_deadline_s, old, want));
		k_sem_give(&policy_sem);
	}
}

void ble_activity(void)
{
	idle_push(IDLE_DISCONNECT_S);
}

/* A connection came during a burst: the result of the trial */
static bool trial_connected(struct bt_conn *conn, const struct bt_conn_info *info, bool info_ok)
{
	const bt_addr_le_t *peer = bt_conn_get_dst(conn);
	k_spinlock_key_t key = k_spin_lock(&trial_lock);
	struct ble_trial_entry *e;
	struct ble_trial_entry copy;

	if (!trial.running) {
		k_spin_unlock(&trial_lock, key);
		return false;
	}
	e = trial_last();
	trial.conn_ms = k_uptime_get();
	e->connect_ms = (int32_t)(trial.conn_ms - trial.t0_ms);
	e->step = trial.step;
	e->peer[0] = peer->a.val[2];
	e->peer[1] = peer->a.val[1];
	e->peer[2] = peer->a.val[0];
	e->peer_random = peer->type != BT_ADDR_LE_PUBLIC;
	if (info_ok) {
		e->interval = (uint16_t)(info->le.interval_us / 1250U);
		e->latency = info->le.latency;
		e->timeout = info->le.timeout;
	}
	trial.running = false;
	trial.conn_open = true;
	copy = *e;
	k_spin_unlock(&trial_lock, key);
	LOG_INF("trial %u: connected after %d ms, step %u", copy.number, copy.connect_ms,
		copy.step + 1U);
	selftest_wake();
	return true;
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	struct bt_conn_info info;
	bool info_ok;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	if (err) {
		LOG_ERR("connection to %s failed (0x%02x)", addr, err);
		atomic_set(&resume_after_disconnect, 1); /* the advertiser is gone too */
		return;
	}
	atomic_inc(&connections);
	connect_count++;
	if (!current_conn) {
		current_conn = bt_conn_ref(conn);
	}
	apply_conn_tx_power(conn);
	LOG_INF("connected to %s", addr);
	info_ok = bt_conn_get_info(conn, &info) == 0;
	if (info_ok) {
		LOG_INF("link: interval %u us, latency %u, supervision timeout %u ms",
			info.le.interval_us, info.le.latency, info.le.timeout * 10U);
	}
	/* A trial connection ends soon, as a product session would. Set, not
	 * pushed: a push of the connection before may have landed after its end */
	atomic_set(&idle_deadline_s,
		   uptime_s() + (trial_connected(conn, &info, info_ok) || ble_trial_mode()
					 ? TRIAL_IDLE_S
					 : IDLE_DISCONNECT_S));
	k_sem_give(&policy_sem);
}

/* The supervision timeout in force decides whether the link outlives what takes
 * the radio away from it, a flash erase or a fade (lot B5, prj.conf), and the
 * central has the last word on it: log every change.
 */
static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	ARG_UNUSED(conn);
	LOG_INF("link: interval %u us, latency %u, supervision timeout %u ms",
		interval * 1250U, latency, timeout * 10U);
}

#if defined(CONFIG_BT_USER_PHY_UPDATE)
static void le_phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *param)
{
	ARG_UNUSED(conn);
	LOG_INF("link: PHY tx %s, rx %s", param->tx_phy == BT_GAP_LE_PHY_2M ? "2M" : "1M",
		param->rx_phy == BT_GAP_LE_PHY_2M ? "2M" : "1M");
}
#endif

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	atomic_dec(&connections);
	atomic_set(&last_disconnect_reason, reason);
	atomic_set(&idle_deadline_s, 0);
	atomic_set(&resume_after_disconnect, 1);
	k_spinlock_key_t tkey = k_spin_lock(&trial_lock);

	/* The lifetime of trial mode counts from the end of the last trial */
	if (trial.conn_open && trial.until_s != 0) {
		trial.until_s = MAX(trial.until_s, uptime_s() + trial.lifetime_s);
	}
	trial.conn_open = false;
	k_spin_unlock(&trial_lock, tkey);
	if (current_conn == conn) {
		unsigned int key = irq_lock();

		current_conn = NULL;
		irq_unlock(key);
		bt_conn_unref(conn);
	}
	LOG_INF("disconnected from %s (reason 0x%02x)", addr, reason);
}

static void recycled(void)
{
	/* A connection object is free again. After a disconnection: resume
	 * advertising, fast first, or, in trial mode, keep silent until the next
	 * press (or the burst of `adv trial go`). After our own bt_le_adv_stop():
	 * nothing to do, see the note above policy_thread().
	 */
	if (atomic_cas(&resume_after_disconnect, 1, 0)) {
		atomic_set(&fast_requested, 1); /* left pending in trial mode */
		k_sem_give(&policy_sem);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_updated = le_param_updated,
#if defined(CONFIG_BT_USER_PHY_UPDATE)
	.le_phy_updated = le_phy_updated,
#endif
	.recycled = recycled,
};

/* RSSI of the connection as the controller measures it (HCI Read RSSI): the
 * host as the watch hears it, for the range checks of V2-23 and R4 */
int ble_conn_rssi(int8_t *rssi)
{
	struct bt_hci_cp_read_rssi *cp;
	struct bt_hci_rp_read_rssi *rp;
	struct net_buf *buf, *rsp = NULL;
	struct bt_conn *conn;
	uint16_t handle;
	unsigned int key;
	int err;

	key = irq_lock();
	conn = current_conn ? bt_conn_ref(current_conn) : NULL;
	irq_unlock(key);
	if (!conn) {
		return -ENOTCONN;
	}
	err = bt_hci_get_conn_handle(conn, &handle);
	bt_conn_unref(conn);
	if (err) {
		return err;
	}
	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		return -ENOBUFS;
	}
	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = sys_cpu_to_le16(handle);
	err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_RSSI, buf, &rsp);
	if (err) {
		return err;
	}
	rp = (void *)rsp->data;
	*rssi = rp->rssi;
	net_buf_unref(rsp);
	return 0;
}

int ble_start(bool rx_check_enabled)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);
	char addr[BT_ADDR_LE_STR_LEN];
	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}
	bt_id_get(addrs, &count);
	if (count > 0) {
		bt_addr_le_to_str(&addrs[0], addr, sizeof(addr));
		LOG_INF("Bluetooth ready, address %s, tx power %d dBm", addr, tx_power_dbm);
	}
	if (rx_check_enabled) {
		rx_check(3);
	} else {
		LOG_INF("rx check skipped (battery-safe profile)");
	}
	atomic_set(&ble_ready, 1);
	ble_adv_kick();
	return 0;
}

void ble_set_battery_level(uint8_t percent)
{
	bt_bas_set_battery_level(percent);
}

const char *ble_state(void)
{
	return atomic_get(&connections) > 0 ? "connected" : "advertising";
}

unsigned int ble_connect_count(void)
{
	return connect_count;
}

uint8_t ble_last_disconnect_reason(void)
{
	return (uint8_t)atomic_get(&last_disconnect_reason);
}

/* ---- Advertising trials: control from the shell and the main loop ------------ */

int ble_trial_start(enum ble_trial_profile profile, uint32_t lifetime_s)
{
	k_spinlock_key_t key;

	if ((size_t)profile >= ARRAY_SIZE(trial_profiles) || lifetime_s == 0) {
		return -EINVAL;
	}
	key = k_spin_lock(&trial_lock);
	trial.profile = (uint8_t)profile;
	trial.lifetime_s = lifetime_s;
	trial.until_s = uptime_s() + lifetime_s;
	/* A burst of another profile is dropped: it stays in the log unanswered */
	trial.running = false;
	trial.requested = false;
	trial.go = false;
	k_spin_unlock(&trial_lock, key);
	LOG_INF("trial mode on, profile %s, lapses after %u s without a trial",
		trial_profiles[profile].name, lifetime_s);
	k_sem_give(&policy_sem);
	return 0;
}

void ble_trial_stop(void)
{
	k_spinlock_key_t key = k_spin_lock(&trial_lock);
	const bool was_on = trial.until_s != 0;

	trial.until_s = 0;
	trial.running = false;
	trial.requested = false;
	trial.go = false;
	k_spin_unlock(&trial_lock, key);
	if (was_on) {
		LOG_INF("trial mode off: normal advertising");
		atomic_set(&fast_requested, 1);
		k_sem_give(&policy_sem);
	}
}

bool ble_trial_mode(void)
{
	k_spinlock_key_t key = k_spin_lock(&trial_lock);
	const bool on = trial.until_s != 0;

	k_spin_unlock(&trial_lock, key);
	return on;
}

bool ble_trial_running(void)
{
	k_spinlock_key_t key = k_spin_lock(&trial_lock);
	const bool running = trial.until_s != 0 && trial.running;

	k_spin_unlock(&trial_lock, key);
	return running;
}

int ble_trial_go_on_disconnect(void)
{
	k_spinlock_key_t key = k_spin_lock(&trial_lock);
	const bool on = trial.until_s != 0;

	if (on) {
		trial.go = true;
	}
	k_spin_unlock(&trial_lock, key);
	k_sem_give(&policy_sem);
	return on ? 0 : -EPERM;
}

bool ble_trial_view(struct ble_trial_view *view)
{
	k_spinlock_key_t key = k_spin_lock(&trial_lock);
	const bool on = trial.until_s != 0;

	memset(view, 0, sizeof(*view));
	if (on) {
		view->running = trial.running;
		view->step = trial.step;
		view->elapsed_ms = trial.running ? (uint32_t)(k_uptime_get() - trial.t0_ms) : 0U;
	}
	/* The last trial even off trial mode: the main loop marks it seen */
	if (trial.count > 0) {
		view->last = *trial_last();
	}
	k_spin_unlock(&trial_lock, key);
	return on;
}

size_t ble_trial_log(struct ble_trial_entry *out, size_t max)
{
	k_spinlock_key_t key = k_spin_lock(&trial_lock);
	const uint32_t kept = MIN(trial.count, (uint32_t)TRIAL_LOG);
	size_t n = 0;

	for (uint32_t i = trial.count - kept; i < trial.count && n < max; i++) {
		out[n++] = trial.log[i % TRIAL_LOG];
	}
	k_spin_unlock(&trial_lock, key);
	return n;
}

const char *ble_trial_profile_name(uint8_t profile)
{
	return profile < ARRAY_SIZE(trial_profiles) ? trial_profiles[profile].name : "?";
}

uint16_t ble_trial_step_interval(uint8_t profile, uint8_t step)
{
	if (profile >= ARRAY_SIZE(trial_profiles) || step >= trial_profiles[profile].count) {
		return 0;
	}
	return trial_profiles[profile].steps[step].interval;
}
