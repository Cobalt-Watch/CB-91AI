/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The radio of the watch (lot D6): Bluetooth on, advertising for a reason and
 * then silent (the Cobalt Link specification), or always slow in the
 * development build, one connection at a time, dropped when idle (EF-33).
 * Connections and disconnections come to the event loop as events.
 */

#ifndef CB91AI_WATCH_RADIO_H
#define CB91AI_WATCH_RADIO_H

#include <stdbool.h>
#include <stdint.h>

struct bt_conn;

/* Why the watch advertises now: the reason HELLO gives (link_proto.h) */
enum radio_reason {
	RADIO_REASON_NONE = 0,  /* development: always there */
	RADIO_REASON_NOTE = 1,
	RADIO_REASON_SYNC = 2,
	RADIO_REASON_DAILY = 3,
	RADIO_REASON_UPDATED = 4,
};

/* Bluetooth on, and advertising if the build keeps it permanent */
int radio_init(void);

/* Advertise for a reason: 100 ms for 10 s, then 1 s for 2 min (section 2 of
 * the design), fast at once in the development build. While a link is up,
 * the window opens once it has ended. */
void radio_call(enum radio_reason reason);

/* A session ran to its BYE, from either side: the call is answered, and the
 * product falls silent at once rather than at the end of the window (the
 * phone, which keeps a connection pending, would only come back). From the
 * event loop, once the link is down. */
void radio_answered(void);

/* The name of the watch changed (bt_set_name(), from the loop): the scan
 * response carries the new one from now on */
void radio_renamed(void);

struct link_trial;

/* The trial of the calls (lot N1a, risk R11; Cobalt Link key 0x7A, development
 * builds with CONFIG_CB91AI_WATCH_DEBUG_TRIAL): for `minutes`, 0 to stop, the
 * radio behaves as the product's, silent between its calls and advertising
 * with `profile` (LINK_TRIAL_PRODUCT or LINK_TRIAL_APPLE) during each, and the
 * watch times each call from its first advertisement to the connection. It
 * shows from the end of the link. False on a value out of range, or without
 * the option. */
bool radio_trial_set(uint8_t minutes, uint8_t profile);

/* The trial and the last call answered during one (LINK_KEY_DEBUG_TRIAL); the
 * first read after that connection stamps its delay */
void radio_trial_get(struct link_trial *out);

/* The reason of the last call, for HELLO */
enum radio_reason radio_reason(void);

/* The uptime, in s, of the last call and of the end of the last link, 0 for
 * none since boot: whether the phone is due another call (lot E2, recall.h) */
uint32_t radio_last_call_s(void);
uint32_t radio_last_link_end_s(void);

/* The calls since boot, whatever their reason */
uint32_t radio_calls(void);

/* A reference to the connection, NULL when there is none: unref it */
struct bt_conn *radio_conn(void);

/* Host activity (SMP, a message): pushes back the idle disconnect (EF-33) */
void radio_activity(void);

/* The number of the current connection since boot, 0 when there is none */
unsigned int radio_link(void);

/* End connection `link` (radio_link()), from the event loop: the session said
 * BYE. Nothing if that link is over already. */
void radio_disconnect(unsigned int link);

/* Transmit power in dBm, 0 by default (EF-31); the controller picks the
 * nearest level */
int radio_set_tx_power(int8_t dbm);
int8_t radio_tx_power(void);

bool radio_connected(void);
bool radio_advertising(void);

/* For the status line: "off", "fast", "slow", and the counts */
const char *radio_adv_mode(void);
unsigned int radio_connections(void);
uint8_t radio_last_disconnect_reason(void);
uint32_t radio_connected_s(void); /* time spent connected since boot (EF-73) */

/* The parameters of the link in force (the central's answer to the watch's
 * request, prj.conf and MCUmgr's during an update): false without a link */
bool radio_link_params(uint32_t *interval_us, uint16_t *latency, uint16_t *timeout_10ms);

#endif /* CB91AI_WATCH_RADIO_H */
