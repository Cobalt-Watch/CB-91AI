/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bench commands over Bluetooth LE (0.1.46). Sealed in its watch on a CR2016,
 * the board has neither the RTT of the debug probe nor its USB port: the
 * `cb91ai` shell commands reach it through this service instead, from
 * tools/ble_shell.py.
 *
 * Cobalt test service, next to the debug service of ble.c:
 * - command characteristic (write, notify): the host writes one command line,
 *   without the "cb91ai" prefix; the output comes back as notifications, the
 *   last one starting with 0x04 followed by the return code in ASCII;
 * - data characteristic (notify): binary streams asked for by a command, such
 *   as a voice capture (`cb91ai rec get`), each notification made of its offset
 *   (4 bytes, little endian) and the payload.
 *
 * Only `cb91ai` commands run: the prefix is added here, so the generic shells
 * of the bench (flash, gpio, i2c, pwm) stay out of reach of the radio. Like the
 * SMP service, this one is open until pairing lands (EF-37, phase S), and it
 * belongs to the self-test only, never to the watch.
 *
 * The command runs on the dummy shell backend, in the main loop: the write
 * callback only copies the line (BT RX thread, no blocking there), the main
 * thread and its 6 KB stack do the work, and the notifications may block
 * there until the link takes them, which is the flow control of a stream.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "ble.h"
#include "remote.h"

LOG_MODULE_REGISTER(cb91ai_remote, LOG_LEVEL_INF);

/* Defined in main.c */
void selftest_wake(void);
void selftest_wdt_feed(void);

#define LINE_MAX  160
#define PREFIX    "cb91ai "
#define END_MARK  0x04
#define ATT_HDR   3 /* opcode and handle of a notification */
#define OFFSET_SZ 4

#define BT_UUID_CB_TEST_SVC  BT_UUID_128_ENCODE(0xc0b91a10, 0x1db7, 0x4cd3, 0x868b, 0x8a527460db61)
#define BT_UUID_CB_TEST_CMD  BT_UUID_128_ENCODE(0xc0b91a11, 0x1db7, 0x4cd3, 0x868b, 0x8a527460db61)
#define BT_UUID_CB_TEST_DATA BT_UUID_128_ENCODE(0xc0b91a12, 0x1db7, 0x4cd3, 0x868b, 0x8a527460db61)
static const struct bt_uuid_128 test_svc_uuid = BT_UUID_INIT_128(BT_UUID_CB_TEST_SVC);
static const struct bt_uuid_128 test_cmd_uuid = BT_UUID_INIT_128(BT_UUID_CB_TEST_CMD);
static const struct bt_uuid_128 test_data_uuid = BT_UUID_INIT_128(BT_UUID_CB_TEST_DATA);

/* Command slot: IDLE, then FILLING while the BT thread copies a line, READY
 * for the main loop, RUNNING until its output is sent. A host that writes
 * while a command runs is told to wait (ATT error "procedure in progress").
 */
enum { SLOT_IDLE, SLOT_FILLING, SLOT_READY, SLOT_RUNNING };
static atomic_t slot = ATOMIC_INIT(SLOT_IDLE);
static char line[LINE_MAX];
static struct bt_conn *host; /* reference held from the write to the end of the output */

static ssize_t write_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset, uint8_t flags);

BT_GATT_SERVICE_DEFINE(cb_test_svc,
	BT_GATT_PRIMARY_SERVICE(&test_svc_uuid),
	BT_GATT_CHARACTERISTIC(&test_cmd_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_WRITE, NULL, write_cmd, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&test_data_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* attrs: 0 service, 1 and 2 command declaration and value, 3 its CCC,
 * 4 and 5 data declaration and value, 6 its CCC */
#define CMD_ATTR  (&cb_test_svc.attrs[2])
#define DATA_ATTR (&cb_test_svc.attrs[5])

static ssize_t write_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0 || len == 0 || len >= LINE_MAX) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	if (!atomic_cas(&slot, SLOT_IDLE, SLOT_FILLING)) {
		return BT_GATT_ERR(BT_ATT_ERR_PROCEDURE_IN_PROGRESS);
	}
	memcpy(line, buf, len);
	line[len] = '\0';
	/* One line, printable: a stray CR, LF or control byte becomes a space */
	for (uint16_t i = 0; i < len; i++) {
		if ((unsigned char)line[i] < 0x20 || (unsigned char)line[i] == 0x7f) {
			line[i] = ' ';
		}
	}
	host = bt_conn_ref(conn);
	atomic_set(&slot, SLOT_READY);
	ble_activity();
	selftest_wake();
	return len;
}

/* Payload room of one notification on this link, 0 if there is no link */
static size_t notify_room(struct bt_conn *conn)
{
	uint16_t mtu = conn ? bt_gatt_get_mtu(conn) : 0;

	return mtu > ATT_HDR + OFFSET_SZ ? mtu - ATT_HDR : 0;
}

static int notify_text(struct bt_conn *conn, const char *text, size_t len)
{
	size_t room = notify_room(conn);

	if (room == 0) {
		return -ENOTCONN;
	}
	while (len > 0) {
		size_t n = MIN(len, room);
		int err = bt_gatt_notify(conn, CMD_ATTR, text, n);

		if (err) {
			return err;
		}
		text += n;
		len -= n;
	}
	return 0;
}

const char *remote_run(const char *cmd, int *ret, size_t *len)
{
	static char full[sizeof(PREFIX) + LINE_MAX];
	static bool plain;
	const struct shell *sh = shell_backend_dummy_get_ptr();

	if (!plain) {
		/* Text for a script, not for a terminal */
		(void)shell_use_colors_set(sh, false);
		(void)shell_use_vt100_set(sh, false);
		plain = true;
	}
	snprintk(full, sizeof(full), PREFIX "%s", cmd);
	shell_backend_dummy_clear_output(sh);
	*ret = shell_execute_cmd(sh, full);
	return shell_backend_dummy_get_output(sh, len);
}

bool remote_process(void)
{
	char end[12];
	size_t len;
	int ret, err, n;
	const char *out;

	if (!atomic_cas(&slot, SLOT_READY, SLOT_RUNNING)) {
		return false;
	}
	LOG_INF("command from the host: %s", line);
	out = remote_run(line, &ret, &len);
	err = notify_text(host, out, len);
	n = snprintk(end, sizeof(end), "%c%d", END_MARK, ret);
	if (!err) {
		err = notify_text(host, end, n);
	}
	if (err) {
		LOG_WRN("command output not delivered (%d)", err);
	}
	ble_activity();
	bt_conn_unref(host);
	host = NULL;
	atomic_set(&slot, SLOT_IDLE);
	return true;
}

int remote_stream(const void *data, size_t len)
{
	return remote_stream_at(data, len, 0);
}

int remote_stream_at(const void *data, size_t len, uint32_t offset)
{
	static uint8_t chunk[OFFSET_SZ + 512];
	const uint8_t *bytes = data;
	size_t avail = notify_room(host);
	size_t room, sent = 0;

	if (avail == 0) {
		return -ENOTCONN;
	}
	/* Whole 16-bit samples in each notification */
	room = (MIN(avail, sizeof(chunk)) - OFFSET_SZ) & ~(size_t)1;
	if (offset == 0) {
		/* A short interval for the time of the stream, supervision timeout
		 * kept at 4 s (lot B5); the central may refuse, the stream only goes
		 * slower then */
		(void)bt_conn_le_param_update(host, BT_LE_CONN_PARAM(6, 12, 0, 400));
	}
	while (sent < len) {
		size_t n = MIN(len - sent, room);
		int err;

		sys_put_le32(offset + (uint32_t)sent, chunk);
		memcpy(&chunk[OFFSET_SZ], &bytes[sent], n);
		err = bt_gatt_notify(host, DATA_ATTR, chunk, OFFSET_SZ + n);
		if (err) {
			LOG_WRN("stream stopped at %u of %u bytes (%d)", offset + (unsigned int)sent,
				offset + (unsigned int)len, err);
			return err;
		}
		sent += n;
		selftest_wdt_feed();
	}
	ble_activity();
	return (int)sent;
}
