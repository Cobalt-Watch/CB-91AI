/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CB91AI_BLE_H
#define CB91AI_BLE_H

#include <stdbool.h>
#include <stdint.h>

/* Enable Bluetooth and start connectable advertising. The 3 s receiver check
 * (passive scan) is for the bench only: skip it on a coin cell.
 * Returns 0 on success.
 */
int ble_start(bool rx_check_enabled);

/* Update the Battery Service level, 0 to 100 percent. */
void ble_set_battery_level(uint8_t percent);

/* "advertising" or "connected", for the periodic log line. */
const char *ble_state(void);

/* Number of connections accepted since boot. */
unsigned int ble_connect_count(void);

/* HCI reason of the last disconnection since boot, 0 if none. */
uint8_t ble_last_disconnect_reason(void);

/* Update the debug/status characteristic value and notify any subscriber. */
void ble_dbg_set_status(const char *status);

/* Back to fast advertising for 30 s (button press): no-op while connected. In
 * trial mode, a burst instead, unless one runs already. */
void ble_adv_kick(void);

/* Keep fast advertising for `seconds` whenever no host is connected, for a
 * test session on a sealed watch (0: back to normal). An advertising event
 * every 100 to 150 ms instead of every 1 to 1.2 s: some ten times the radio
 * charge of slow advertising while it lasts.
 */
void ble_adv_hold_fast(uint32_t seconds);

/* RSSI of the current connection in dBm: the host as the watch hears it. */
int ble_conn_rssi(int8_t *rssi);

/* "fast", "slow" or "off", and the last advertising error, for the status line. */
const char *ble_adv_mode(void);
int ble_adv_last_error(void);

/* Host activity seen (SMP command, status read): pushes back the idle
 * disconnect of EF-33.
 */
void ble_activity(void);

/* Transmit power in dBm for advertising and the connection (range tests);
 * the controller picks the nearest level it supports.
 */
int ble_set_tx_power(int8_t dbm);
int8_t ble_tx_power(void);

/*
 * Advertising trials (lot N1a, risk R11): how long a phone that keeps a
 * pending connection takes to get in once the watch advertises after a
 * silence. In trial mode the radio is silent between trials, a press on a case
 * button (ble_adv_kick) starts a burst, and the watch times it from its first
 * packet to the connection. See ble.c for the two profiles.
 */
enum ble_trial_profile {
	BLE_TRIAL_APPLE, /* 20 ms for 30 s, then 152.5 ms, then 1022.5 ms */
	BLE_TRIAL_LINK,  /* 100 ms for 10 s, then 1 s (the Cobalt Link specification) */
};

#define BLE_TRIAL_NONE (-1) /* no connection in the window, or no read */
#define BLE_TRIAL_LOG  12   /* trials kept, the oldest dropped first */

struct ble_trial_entry {
	uint32_t number;    /* trials since boot, from 1 */
	uint32_t at_s;      /* uptime at the start of the burst */
	int32_t connect_ms; /* first packet to the connection, or BLE_TRIAL_NONE */
	int32_t read_ms;    /* connection to the first read of the trial value, or NONE */
	uint16_t interval;  /* of the connection, in 1.25 ms units */
	uint16_t latency;
	uint16_t timeout;   /* supervision timeout, in 10 ms units */
	uint8_t profile;    /* enum ble_trial_profile */
	uint8_t step;       /* step of the burst the phone answered, from 0 */
	uint8_t peer[3];    /* last three bytes of the peer address, most significant first */
	uint8_t peer_random;
};

/* What the glass shows: a burst running (its step and time so far), and the
 * last trial, filled even when trial mode is off. False when it is off. */
struct ble_trial_view {
	bool running;
	uint8_t step;
	uint32_t elapsed_ms;
	struct ble_trial_entry last; /* number 0 before the first trial */
};
bool ble_trial_view(struct ble_trial_view *view);

/* Trial mode on, with a profile; it lapses after `lifetime_s` without a trial
 * and normal advertising resumes. -EINVAL on an unknown profile. */
int ble_trial_start(enum ble_trial_profile profile, uint32_t lifetime_s);
void ble_trial_stop(void);
bool ble_trial_mode(void);
bool ble_trial_running(void);
/* A burst as soon as the connected host leaves (a trial run from the PC) */
int ble_trial_go_on_disconnect(void);
/* The trials since boot, oldest first, at most `max`; returns how many */
size_t ble_trial_log(struct ble_trial_entry *out, size_t max);
const char *ble_trial_profile_name(uint8_t profile);
/* Interval of a step of a profile in 0.625 ms units, 0 past the last step */
uint16_t ble_trial_step_interval(uint8_t profile, uint8_t step);

#endif /* CB91AI_BLE_H */
