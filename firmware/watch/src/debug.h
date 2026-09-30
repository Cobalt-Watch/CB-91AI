/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CB91AI_WATCH_DEBUG_H
#define CB91AI_WATCH_DEBUG_H

/* The status line of the debug service (development build), from the loop */
#if defined(CONFIG_CB91AI_WATCH_DEBUG_SERVICE)
void debug_set_status(const char *line);
#else
static inline void debug_set_status(const char *line)
{
	(void)line;
}
#endif

#endif /* CB91AI_WATCH_DEBUG_H */
