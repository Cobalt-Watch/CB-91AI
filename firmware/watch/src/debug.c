/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The debug service of the development build (debug.conf), with the UUIDs and
 * the formats of the self-test's (app/src/ble.c), so that the bench tools work
 * on the watch as they are: the status line that ble_update.py prints
 * (C0B91A01, read and notify) and the time that cobalt_time.py sets and reads
 * (C0B91A02). Unlike the self-test, reading the status does not confirm an
 * image in test: only SMP does (update.c). The product has none of this: its
 * status goes to the phone through Cobalt Link (LINK_KEY_STATUS).
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>

#include "clock.h"
#include "debug.h"
#include "radio.h"

static const struct bt_uuid_128 dbg_svc_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xc0b91a00, 0x1db7, 0x4cd3, 0x868b, 0x8a527460db61));
static const struct bt_uuid_128 dbg_status_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xc0b91a01, 0x1db7, 0x4cd3, 0x868b, 0x8a527460db61));
static const struct bt_uuid_128 dbg_time_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xc0b91a02, 0x1db7, 0x4cd3, 0x868b, 0x8a527460db61));

/* Written by the event loop under the lock, notified from a copy; the read
 * of the Bluetooth thread takes no lock: it may splice two lines, which a
 * diagnostic string can afford */
static char status[256] = "boot";
static char sending[sizeof(status)];
static struct k_spinlock lock;

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	radio_activity();
	return bt_gatt_attr_read(conn, attr, buf, len, offset, status, strlen(status));
}

/* 8 or 10 bytes written: UTC in ms (int64), then the local offset in minutes
 * (int16); 23 bytes read: corrected UTC, raw uptime, correction in ppb, local
 * offset, flags (the self-test's format, EI-03) */
static ssize_t read_time(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	uint8_t value[23];

	sys_put_le64((uint64_t)clock_now_ms(), &value[0]);
	sys_put_le64((uint64_t)k_uptime_get(), &value[8]);
	sys_put_le32((uint32_t)clock_ppb(), &value[16]);
	sys_put_le16((uint16_t)clock_tz_minutes(), &value[20]);
	value[22] = clock_flags();
	radio_activity();
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static ssize_t write_time(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			  uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *data = buf;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	if (offset != 0 || (len != 8 && len != 10)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	/* Thread safe; the flash write of a new correction is the loop's */
	clock_set((int64_t)sys_get_le64(data),
		  len == 10 ? (int16_t)sys_get_le16(&data[8]) : clock_tz_minutes(), true);
	radio_activity();
	return len;
}

BT_GATT_SERVICE_DEFINE(dbg_svc,
	BT_GATT_PRIMARY_SERVICE(&dbg_svc_uuid),
	BT_GATT_CHARACTERISTIC(&dbg_status_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_status, NULL, status),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&dbg_time_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_time, write_time, NULL),
);

/* System work queue: ATT never waits for a buffer there (link.c) */
static void notify_status(struct k_work *work)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	ARG_UNUSED(work);
	memcpy(sending, status, sizeof(sending));
	k_spin_unlock(&lock, key);
	/* attrs[2] is the value; nothing happens if nobody subscribed */
	(void)bt_gatt_notify(NULL, &dbg_svc.attrs[2], sending, strlen(sending));
}

static K_WORK_DEFINE(status_work, notify_status);

void debug_set_status(const char *line)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	strncpy(status, line, sizeof(status) - 1);
	status[sizeof(status) - 1] = '\0';
	k_spin_unlock(&lock, key);
	(void)k_work_submit(&status_work);
}
