/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CB91AI_REMOTE_H
#define CB91AI_REMOTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Bench commands over Bluetooth LE, for a board sealed in its watch: the
 * `cb91ai` shell commands, and nothing else, run from a host with
 * tools/ble_shell.py. The host writes a command line to the command
 * characteristic; the main loop runs it (remote_process) and sends its output
 * back as notifications, ended by one that starts with 0x04.
 */

/* From the main loop: run the command a host wrote, if any. True if one ran. */
bool remote_process(void);

/* Run a command line (without the "cb91ai" prefix) on the dummy shell, from
 * the main loop, and return its output (valid until the next call). Used by
 * the armed commands of hwtest.c as well.
 */
const char *remote_run(const char *line, int *ret, size_t *len);

/*
 * From a command that runs on behalf of a host: stream bytes on the data
 * characteristic, each notification carrying its offset (4 bytes, little
 * endian) before the payload. Blocks while the link is busy. Returns the
 * number of bytes sent, or a negative error (no host, link lost).
 */
int remote_stream(const void *data, size_t len);

/* The same for a stream sent in parts, such as one read from the storage
 * flash a buffer at a time: `offset` is where `data` sits in the whole stream,
 * 0 for the first part. Returns the bytes of this part sent, or an error.
 */
int remote_stream_at(const void *data, size_t len, uint32_t offset);

#endif /* CB91AI_REMOTE_H */
