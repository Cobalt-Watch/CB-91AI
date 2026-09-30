/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The pairing of the watch over Bluetooth, see pairing.h.
 *
 * The watch displays and has no keyboard: with LE Secure Connections only
 * (CONFIG_BT_SMP_SC_ONLY), that makes a passkey entry on the phone, which
 * authenticates the link. The passkey comes from here (CONFIG_BT_APP_PASSKEY):
 * under 100 000, from the hardware generator of the nRF52840, as Zephyr leaves
 * the randomness of an application passkey to the application. A pairing is
 * accepted only while pair_open() says so (CONFIG_BT_SMP_APP_PAIRING_ACCEPT).
 *
 * Only a pairing that failed after its code was shown counts: a request the
 * host refuses at once (Just Works, legacy pairing, a short key) would
 * otherwise let anyone in range close the pairing of a blank watch in three
 * tries. The count is kept by the Bluetooth callbacks themselves, in atomics,
 * so that a lost event loses no failure; the loop writes it to the settings.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>

#include "events.h"
#include "pair.h"
#include "pairing.h"

LOG_MODULE_REGISTER(watch_pairing, LOG_LEVEL_INF);

#define SETTING_FAILURES "pair/fails"
/* Out of the range of a passkey: the host gives up before any code shows */
#define NO_PASSKEY 1000000U

static atomic_t bonded;
static atomic_t failures;
static atomic_t shown;       /* a code is on the glass: its failure counts */
static uint8_t failures_kept; /* as the settings hold them (loop only) */

static int pair_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "fails", NULL) && len == 1) {
		uint8_t v;

		if (read_cb(cb_arg, &v, 1) == 1) {
			failures_kept = MIN(v, PAIR_FAILURES_MAX);
			atomic_set(&failures, failures_kept);
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(cb91ai_pair, "pair", NULL, pair_settings_set, NULL, NULL);

static struct pair now(void)
{
	struct pair p;

	pair_init(&p, atomic_get(&bonded) != 0, (uint8_t)atomic_get(&failures));
	return p;
}

/* ---- The Bluetooth host, in its own threads ---------------------------------- */

static enum bt_security_err pairing_accept(struct bt_conn *conn,
					   const struct bt_conn_pairing_feat *const feat)
{
	const struct pair p = now();

	ARG_UNUSED(conn);
	ARG_UNUSED(feat);
	if (!pair_open(&p)) {
		LOG_WRN("pairing refused: a phone holds the bond, or three failures (reset to reopen)");
		return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
	}
	return BT_SECURITY_ERR_SUCCESS;
}

static uint32_t app_passkey(struct bt_conn *conn)
{
	uint32_t r = 0;

	ARG_UNUSED(conn);
#if CONFIG_CB91AI_WATCH_DEBUG_PASSKEY > 0
	/* Development images: the code the bench knows (ble_pair.py), on the glass as always */
	return CONFIG_CB91AI_WATCH_DEBUG_PASSKEY;
#endif
	if (sys_csrand_get(&r, sizeof(r)) != 0) {
		LOG_ERR("no random passkey: pairing given up");
		return NO_PASSKEY;
	}
	return pair_passkey(r);
}

static void passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	ARG_UNUSED(conn);
	atomic_set(&shown, 1);
	(void)evt_post(EVT_PAIRING, PAIRING_SHOW, 0, passkey);
}

/* The host cancels the code shown, then reports the failure itself */
static void cancelled(struct bt_conn *conn)
{
	ARG_UNUSED(conn);
	(void)evt_post(EVT_PAIRING, PAIRING_HIDE, 0, 0);
}

static void complete(struct bt_conn *conn, bool is_bonded)
{
	ARG_UNUSED(conn);
	atomic_set(&shown, 0);
	if (is_bonded) {
		atomic_set(&bonded, 1);
	}
	(void)evt_post(EVT_PAIRING, is_bonded ? PAIRING_BONDED : PAIRING_HIDE, 0, 0);
}

static void failed(struct bt_conn *conn, enum bt_security_err reason)
{
	ARG_UNUSED(conn);
	if (atomic_cas(&shown, 1, 0)) {
		/* A code was shown and not typed right, or the phone gave up */
		if (atomic_get(&failures) < PAIR_FAILURES_MAX) {
			atomic_inc(&failures);
		}
		(void)evt_post(EVT_PAIRING, PAIRING_FAILED, (uint16_t)reason, 0);
	} else {
		(void)evt_post(EVT_PAIRING, PAIRING_HIDE, (uint16_t)reason, 0);
	}
}

static struct bt_conn_auth_cb auth_cb = {
	.pairing_accept = pairing_accept,
	.passkey_display = passkey_display,
	.cancel = cancelled,
	.app_passkey = app_passkey,
};

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = complete,
	.pairing_failed = failed,
};

/* ---- From the event loop ---------------------------------------------------- */

static void count_bond(const struct bt_bond_info *info, void *user_data)
{
	ARG_UNUSED(info);
	(*(unsigned int *)user_data)++;
}

int pairing_init(void)
{
	unsigned int bonds = 0;
	int err;

	bt_foreach_bond(BT_ID_DEFAULT, count_bond, &bonds);
	atomic_set(&bonded, bonds > 0 ? 1 : 0);
	err = bt_conn_auth_cb_register(&auth_cb);
	if (err == 0) {
		err = bt_conn_auth_info_cb_register(&auth_info_cb);
	}
	LOG_INF("%s, %u failure(s): pairing %s", bonds > 0 ? "bonded" : "no bond",
		(unsigned int)atomic_get(&failures), pairing_open() ? "open" : "closed");
	return err;
}

bool pairing_bonded(void)
{
	return atomic_get(&bonded) != 0;
}

bool pairing_open(void)
{
	const struct pair p = now();

	return pair_open(&p);
}

void pairing_keep(void)
{
	const uint8_t count = (uint8_t)atomic_get(&failures);

	/* Written from the loop, off the Bluetooth callbacks */
	if (count != failures_kept) {
		failures_kept = count;
		(void)settings_save_one(SETTING_FAILURES, &failures_kept, sizeof(failures_kept));
		LOG_INF("%u pairing failure(s): pairing %s", count, pairing_open() ? "open" : "closed");
	}
}

void pairing_reset(void)
{
	const int err = bt_unpair(BT_ID_DEFAULT, NULL);

	if (err) {
		LOG_ERR("bonds not forgotten (%d)", err);
	}
	atomic_set(&bonded, 0);
	atomic_set(&failures, 0);
	atomic_set(&shown, 0);
	failures_kept = 0;
	(void)settings_delete(SETTING_FAILURES);
}
