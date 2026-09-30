/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * When the watch calls the phone again, see recall.h. Pure C: the host tests
 * build this file as it is.
 */

#include "recall.h"

uint32_t recall_silence(uint32_t again)
{
	/* 900 s shifted twice is 3600: no further, and no overflow */
	return again >= 2U ? RECALL_SILENCE_MAX_S : RECALL_SILENCE_S << again;
}

uint32_t recall_at(uint32_t call_s, uint32_t link_end_s, uint32_t again)
{
	/* The silence starts at the end of the window of the last call, or at
	 * the end of the last link if that came later. A link lost within the
	 * window leaves it to run; a session that ran to its BYE closes it early
	 * (radio.c): the silence is then longer, never shorter. */
	const uint32_t window_end = call_s + RECALL_WINDOW_S;
	const uint32_t silent_from = window_end > link_end_s ? window_end : link_end_s;

	return silent_from + recall_silence(again);
}

bool recall_due(uint32_t now_s, uint32_t call_s, uint32_t link_end_s, uint32_t again,
		bool linked, bool reachable)
{
	/* A watch whose notes no phone may take (not paired, new or reset)
	 * keeps quiet: a press of ALARM calls for a pairing */
	return !linked && reachable && now_s >= recall_at(call_s, link_end_s, again);
}
