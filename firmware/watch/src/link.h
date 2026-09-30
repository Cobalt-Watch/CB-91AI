/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Cobalt Link service of the watch (lot D6): its two characteristics,
 * RX written by the phone and TX notified by the watch
 * (the Cobalt Link specification), and the session of session.c run
 * by the event loop. The Bluetooth callbacks only copy and post events: the
 * loop parses, decides and sends.
 */

#ifndef CB91AI_WATCH_LINK_H
#define CB91AI_WATCH_LINK_H

#include <stdbool.h>
#include <stdint.h>

/* What the loop has to carry out after a link event (bits) */
#define LINK_DO_TIME   (1U << 0) /* link_time() gives the phone's time */
#define LINK_DO_RESULT (1U << 1) /* link_result() says green or red */

/* From the event loop only */
uint32_t link_open(void);                   /* EVT_LINK_OPEN: the phone listens on TX */
uint32_t link_rx(void);                     /* EVT_LINK_RX: what the phone wrote */
uint32_t link_tx(void);                     /* EVT_LINK_TX: room to send again */
uint32_t link_tick(void);                   /* EVT_LINK_TIMER */
void link_closed(void);                     /* EVT_BLE_DISCONNECTED */
void link_smp(void);                        /* an SMP command in this link */
uint32_t link_gesture(uint8_t gesture, uint8_t button); /* EVENT, held session */
/* Note `id` was kept during the session: offered in it, or, should the session
 * end first, the phone is called again once the link is down */
uint32_t link_note_added(uint32_t id);
bool link_in_session(void);

void link_time(int64_t *utc_ms, int16_t *tz_minutes);
uint8_t link_result(void); /* LINK_RESULT_SUCCESS or LINK_RESULT_FAILURE */

/* Messages refused or lost since boot, both ways (short, unknown, out of turn,
 * ring full, notification refused) */
uint32_t link_refused(void);

#endif /* CB91AI_WATCH_LINK_H */
