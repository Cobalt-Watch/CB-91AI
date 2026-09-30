/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * When the watch calls the phone once a day, see daily.h. Pure C: the host
 * tests build this file as it is.
 */

#include "daily.h"

uint32_t daily_at(uint32_t call_s, uint32_t window_s, uint32_t link_end_s, uint32_t period_s)
{
	/* The silence starts at the end of the window of the last call, or at
	 * the end of the last link if that came later: any contact with the
	 * phone puts the call off by a whole period */
	const uint32_t window_end = call_s + window_s;
	const uint32_t silent_from = window_end > link_end_s ? window_end : link_end_s;

	return silent_from + period_s;
}

bool daily_due(uint32_t now_s, uint32_t call_s, uint32_t window_s, uint32_t link_end_s,
	       uint32_t period_s, bool linked, bool reachable)
{
	/* A watch no phone may meet (not paired, new or reset) keeps quiet: a
	 * press of ALARM calls for a pairing */
	return !linked && reachable &&
	       now_s >= daily_at(call_s, window_s, link_end_s, period_s);
}
