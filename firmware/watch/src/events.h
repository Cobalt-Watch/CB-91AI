/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The event bus of the watch. One
 * queue, one consumer: the event loop of main.c, which is the only thread that
 * decides anything. Everything else - button and accelerometer interrupts,
 * Bluetooth callbacks, the audio thread, timers, MCUmgr hooks - posts an event
 * and returns.
 *
 * The loop blocks on this queue and nowhere else, so the core sleeps as soon as
 * there is nothing to do: anything periodic is a timer that posts an event.
 */

#ifndef CB91AI_WATCH_EVENTS_H
#define CB91AI_WATCH_EVENTS_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

enum evt_type {
	EVT_NONE = 0,
	/* Buttons: an edge, or the gesture machine wants to look again. The loop
	 * reads the levels and gets the gestures itself (buttons.c): clicks and
	 * long presses never wait in this queue. */
	EVT_BUTTON_EDGE,
	/* Accelerometer: one of its lines rose; the loop reads which (wrist.c) */
	EVT_MOTION,
	/* Wall clock, once a second, only while the display is lit */
	EVT_TICK,
	/* Bluetooth (radio.c): arg carries the reason of a disconnection */
	EVT_BLE_CONNECTED,
	EVT_BLE_DISCONNECTED,
	/* Cobalt Link (link.c): the phone listens on TX, wrote on RX, a
	 * notification left (room to send more), a time-out of the session */
	EVT_LINK_OPEN,
	EVT_LINK_RX,
	EVT_LINK_TX,
	EVT_LINK_TIMER,
	/* An SMP command came (update.c): the host is busy with the watch */
	EVT_SMP_ACTIVITY,
	/* Time to read the cell and refresh the status line */
	EVT_STATUS,
	/* Time for a record of the journal of the temperature and the cell (T1) */
	EVT_JOURNAL,
	/* The audio thread: a take is over, arg says how (recorder.h); the notes
	 * of the flash are read, at boot; all erased, for the reset */
	EVT_AUDIO_STOPPED,
	EVT_NOTES_READY,
	EVT_NOTES_WIPED,
	/* A phone pairs (pairing.h): arg says what, data the code to show */
	EVT_PAIRING,
	/* Battery crossed a threshold, val is the voltage in mV */
	EVT_BATTERY,
	/* An update is about to reset the board: paint the glass and answer */
	EVT_UPDATE_RESET,
	/* An image comes over SMP: arg is the share received, 0 to 100 (update.c) */
	EVT_UPDATE_PROGRESS,
	/* Settings to write to flash, from the loop and nowhere else */
	EVT_SETTINGS_DIRTY,
};

struct evt {
	uint8_t type;  /* enum evt_type */
	uint8_t arg;
	uint16_t val;
	uint32_t data;
};

/*
 * Post an event. Safe from an interrupt. Never blocks: a full queue is a
 * design fault, not a case to handle, so the event is dropped and counted
 * (EF-73) rather than delaying its producer - which could be the radio.
 * False when dropped: a producer that posts once until the loop has taken the
 * event (a pending flag) lets go of its flag then, or it would never post again.
 */
bool evt_post(uint8_t type, uint8_t arg, uint16_t val, uint32_t data);

/* Wait for the next event. Only the event loop calls this. */
void evt_get(struct evt *out);

/* The same, at most `timeout`: false when none came. The loop wakes this way
 * every few seconds to feed its watchdog channel, the one periodic wake-up of
 * the watch at rest (a few microseconds each). */
bool evt_wait(struct evt *out, k_timeout_t timeout);

/* Events dropped since boot, for the status line and the counters of EF-73 */
uint32_t evt_dropped(void);

#endif /* CB91AI_WATCH_EVENTS_H */
