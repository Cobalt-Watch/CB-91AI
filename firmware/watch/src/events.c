/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "events.h"

LOG_MODULE_REGISTER(watch_events, LOG_LEVEL_INF);

/* Sized for the longest burst the watch can produce: a session being set up
 * (connected, secured, a few messages) while the buttons move; the buttons hold
 * one entry at most, however much their contacts bounce (buttons.c).
 */
#define EVT_QUEUE_DEPTH 16

K_MSGQ_DEFINE(evt_queue, sizeof(struct evt), EVT_QUEUE_DEPTH, 4);

static atomic_t dropped;

bool evt_post(uint8_t type, uint8_t arg, uint16_t val, uint32_t data)
{
	struct evt e = { .type = type, .arg = arg, .val = val, .data = data };

	if (k_msgq_put(&evt_queue, &e, K_NO_WAIT) != 0) {
		atomic_inc(&dropped);
		LOG_WRN("event queue full, event %u dropped", type);
		return false;
	}
	return true;
}

void evt_get(struct evt *out)
{
	(void)k_msgq_get(&evt_queue, out, K_FOREVER);
}

bool evt_wait(struct evt *out, k_timeout_t timeout)
{
	return k_msgq_get(&evt_queue, out, timeout) == 0;
}

uint32_t evt_dropped(void)
{
	return (uint32_t)atomic_get(&dropped);
}
