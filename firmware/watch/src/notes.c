/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The voice notes of the storage flash, see notes.h. The QSPI driver puts the
 * flash in deep power-down 10 ms after the last access
 * (zephyr,pm-device-runtime-auto, EF-52).
 */

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "notes.h"

LOG_MODULE_REGISTER(watch_notes, LOG_LEVEL_INF);

static const struct device *const qspi = DEVICE_DT_GET(DT_NODELABEL(zd25wq80c));

static K_MUTEX_DEFINE(lock);
static struct store store;
static struct tlog tlog;
static bool mounted;
static bool hidden; /* a reset erases them: none offered meanwhile */

static int io_read(void *ctx, uint32_t addr, void *buf, size_t len)
{
	ARG_UNUSED(ctx);
	return flash_read(qspi, (off_t)addr, buf, len);
}

static int io_write(void *ctx, uint32_t addr, const void *buf, size_t len)
{
	ARG_UNUSED(ctx);
	return flash_write(qspi, (off_t)addr, buf, len);
}

static int io_erase(void *ctx, uint32_t addr, size_t len)
{
	ARG_UNUSED(ctx);
	return flash_erase(qspi, (off_t)addr, len);
}

static const struct store_io io = {
	.read = io_read,
	.write = io_write,
	.erase = io_erase,
};

BUILD_ASSERT(DT_PROP(DT_NODELABEL(zd25wq80c), size) / 8 == STORE_SIZE,
	     "the notes fill the whole storage flash");

int notes_init(void)
{
	int err;

	if (!device_is_ready(qspi)) {
		LOG_ERR("storage flash not ready");
		return -ENODEV;
	}
	k_mutex_lock(&lock, K_FOREVER);
	/* The journal first: its headers in the tail tell the store where its
	 * ring ends. Not read, the notes are not either: a ring of the wrong
	 * length would misread a note round its end and give an id twice. */
	err = tlog_mount(&tlog, &io);
	if (err == 0) {
		err = store_mount(&store, &io, tlog_found(&tlog));
	}
	mounted = err == 0;
	k_mutex_unlock(&lock);
	if (err) {
		LOG_ERR("notes not read (%d)", err);
	} else {
		LOG_INF("%u note(s) waiting, %u bytes; last id %u, room %u sectors",
			store_count(&store), store_bytes(&store), store.last_id, store_room(&store));
		if (tlog_found(&tlog)) {
			LOG_INF("journal %08x, next record %u", tlog.id, tlog_next(&tlog));
		}
	}
	return err;
}

int notes_journal_append(const struct tlog_record *r, bool may_take_tail)
{
	int err = 0;

	k_mutex_lock(&lock, K_FOREVER);
	if (!mounted) {
		err = -ENODEV;
	} else if (!tlog_found(&tlog)) {
		/* The tail taken the first time the notes have left it: the
		 * journal's header first, then the ring ends before it */
		uint32_t id = 0;

		if (!may_take_tail || !store_tail_free(&store)) {
			err = -EAGAIN;
		} else if (sys_csrand_get(&id, sizeof(id)) != 0) {
			err = -EIO;
		} else {
			err = tlog_start(&tlog, id);
			if (err == 0) {
				store_give_tail(&store);
				LOG_INF("journal %08x started in the tail", id);
			} else {
				store_tail_dirty(&store); /* erased before a take goes there */
			}
		}
	}
	if (err == 0) {
		err = tlog_append(&tlog, r);
	}
	k_mutex_unlock(&lock);
	return err;
}

size_t notes_journal_read(uint32_t from, struct tlog_record *out, size_t max, uint32_t *first,
			  uint32_t *id)
{
	size_t n;

	k_mutex_lock(&lock, K_FOREVER);
	/* The records and the name together: a reset between them would give the
	 * old owner's records under the new name */
	n = mounted ? tlog_read(&tlog, from, out, max, first) : 0U;
	*id = tlog_found(&tlog) ? tlog.id : 0U;
	k_mutex_unlock(&lock);
	return n;
}

uint32_t notes_journal_next(void)
{
	uint32_t next;

	k_mutex_lock(&lock, K_FOREVER);
	next = tlog_next(&tlog);
	k_mutex_unlock(&lock);
	return next;
}


void notes_hide(bool hide)
{
	k_mutex_lock(&lock, K_FOREVER);
	hidden = hide;
	k_mutex_unlock(&lock);
}

bool notes_next(uint32_t after, struct store_note *out)
{
	bool found;

	k_mutex_lock(&lock, K_FOREVER);
	found = mounted && !hidden && store_next(&store, after, out);
	k_mutex_unlock(&lock);
	return found;
}

size_t notes_read(uint32_t id, uint32_t offset, uint8_t *buf, size_t len)
{
	size_t n;

	k_mutex_lock(&lock, K_FOREVER);
	n = mounted && !hidden ? store_read(&store, id, offset, buf, len) : 0U;
	k_mutex_unlock(&lock);
	return n;
}

int notes_delivered(uint32_t id)
{
	int err;

	k_mutex_lock(&lock, K_FOREVER);
	err = mounted ? store_delivered(&store, id) : -ENODEV;
	k_mutex_unlock(&lock);
	if (err) {
		LOG_WRN("note %u not marked delivered (%d)", id, err);
	}
	return err;
}

/* A mount cut short leaves half a list: nothing is announced from it */
uint16_t notes_count(void)
{
	uint16_t n;

	k_mutex_lock(&lock, K_FOREVER);
	n = mounted && !hidden ? store_count(&store) : 0U;
	k_mutex_unlock(&lock);
	return n;
}

uint32_t notes_bytes(void)
{
	uint32_t n;

	k_mutex_lock(&lock, K_FOREVER);
	n = mounted && !hidden ? store_bytes(&store) : 0U;
	k_mutex_unlock(&lock);
	return n;
}

uint32_t notes_last_id(void)
{
	uint32_t id;

	k_mutex_lock(&lock, K_FOREVER);
	id = mounted ? store.last_id : 0U;
	k_mutex_unlock(&lock);
	return id;
}

uint16_t notes_room(void)
{
	uint16_t n;

	k_mutex_lock(&lock, K_FOREVER);
	n = mounted ? store_room(&store) : 0U;
	k_mutex_unlock(&lock);
	return n;
}

int notes_prepare(uint16_t want)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	ret = mounted ? store_prepare(&store, want) : 0;
	k_mutex_unlock(&lock);
	return ret;
}

int notes_tidy(void)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	ret = mounted ? store_tidy(&store) : 0;
	k_mutex_unlock(&lock);
	return ret;
}

/* The reset (lot S3): the notes, then the journal under a new name, so that a
 * watch given away carries neither; a failure leaves the reset marked, done
 * again at the next boot */
int notes_wipe(void)
{
	int err;

	k_mutex_lock(&lock, K_FOREVER);
	err = mounted ? store_wipe(&store) : -ENODEV;
	if (err == 0 && tlog_found(&tlog)) {
		uint32_t id = 0;

		err = sys_csrand_get(&id, sizeof(id));
		if (err == 0) {
			err = tlog_wipe(&tlog, id);
		}
	}
	k_mutex_unlock(&lock);
	LOG_INF("notes and journal wiped (%d)", err);
	return err;
}

int notes_begin(void)
{
	int err;

	k_mutex_lock(&lock, K_FOREVER);
	err = mounted ? store_begin(&store) : -ENODEV;
	k_mutex_unlock(&lock);
	return err;
}

size_t notes_append(const uint8_t *data, size_t len)
{
	size_t n;

	k_mutex_lock(&lock, K_FOREVER);
	n = store_append(&store, data, len);
	k_mutex_unlock(&lock);
	return n;
}

int notes_finish(struct note_header *h, uint32_t keep)
{
	int err;

	k_mutex_lock(&lock, K_FOREVER);
	err = store_finish(&store, h, keep);
	k_mutex_unlock(&lock);
	return err;
}

void notes_abandon(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	store_abandon(&store);
	k_mutex_unlock(&lock);
}
