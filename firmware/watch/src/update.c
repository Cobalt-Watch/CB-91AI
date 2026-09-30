/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Updates of the watch over SMP (lot D6), with what the self-test learnt on the
 * bench:
 * - an SMP command is host activity: it pushes back the idle disconnect, and
 *   a Cobalt Link session in the same link waits 30 s after each command, as
 *   it starts and as it ends, before its BYE: the first chunk of an update
 *   waits 10 to 18 s for the erase of the slot (link.c, session_busy());
 * - before the reset of an update: "UPd" on the glass, the time in retained
 *   RAM, and the watchdog fed last of all: it keeps counting across the reset
 *   and MCUboot checks the new image without feeding it (1.53 s for 408 KB);
 * - nothing blocking in the callbacks, which run on the 2 KB stack of SMP,
 *   but for the 50 ms the reset hook grants the event loop to paint "UPd"
 *   (the loop alone draws). Past that,
 *   the reset goes on anyway.
 *
 * Confirmation (EF-62): **the watch never confirms itself.** The host confirms
 * a new image over SMP once it finds it running (ble_update.py, the phone app),
 * which proves the way back works. An image still in test after
 * CONFIG_CB91AI_WATCH_CONFIRM_WAIT_S reboots, and MCUboot starts the previous
 * one again: a sealed watch is never left on an image that nobody can reach.
 * No pre-erase of the spare slot here, unlike the self-test: an upload then
 * starts with the erase of the slot, a few seconds longer, and nothing in this
 * application ever writes the slot that is the way back.
 *
 * **No upload while this image is in test**: the other slot holds the image to
 * go back to, and an upload cut half way there would leave nothing to boot at
 * the next reset. MCUmgr already refuses it in direct execution with revert
 * (img_mgmt_slot_in_use() keeps the revert slot, the upload fails with "no free
 * slot"); the upload hook below refuses it too, a second barrier that costs a
 * flash read. To go back instead, a reset does it (ble_update.py --reset).
 *
 * **A take goes first**: during one,
 * the chunks of an upload are refused (the internal flash would take the CPU
 * from the voice), and so is a reset, which would lose it; the reboot of an
 * image never confirmed waits for its end. The other way round, a take does
 * not begin while an upload runs (update_busy()): a click would otherwise
 * refuse the next chunk, and the upload would start over.
 */

#include <zephyr/kernel.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
/* After callbacks.h, which the callbacks of the image group need */
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_callbacks.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include "clock.h"
#include "events.h"
#include "radio.h"
#include "recorder.h"
#include "update.h"

LOG_MODULE_REGISTER(watch_update, LOG_LEVEL_INF);

#define SLOT1_AREA PARTITION_ID(slot1_partition)
/* An upload goes on this long after its last chunk, as the session waits
 * after SMP (session.h): the first chunk alone waits 10 to 18 s for an erase */
#define UPLOAD_IDLE_MS 30000U

static K_SEM_DEFINE(painted, 0, 1);
static void (*feed)(void);
static int64_t test_since_ms = -1; /* -1: confirmed */
static atomic_t smp_pending;
static atomic_t chunk_at;   /* uptime of the last chunk, ms; valid once `chunked` */
static atomic_t chunked;
static atomic_t shown_percent = ATOMIC_INIT(-1); /* the share last posted to the glass */

/* The share of the image received once this chunk is written, posted to the
 * event loop whenever it moves: "U P" and 0 to 100 on the glass during the
 * whole transfer (28/09). A dropped event is caught up by the next
 * one; nothing here blocks the SMP stack. */
static void progress(const void *data, size_t data_size)
{
	const struct img_mgmt_upload_check *chk = data;

	if (data == NULL || data_size < sizeof(*chk) || chk->action->size == 0) {
		return;
	}
	if (chk->req->off == 0) {
		atomic_set(&shown_percent, -1); /* a new upload: its 0 shown, even after a 0 */
	}
	const uint64_t done = (uint64_t)chk->req->off + (uint64_t)MAX(chk->action->write_bytes, 0);
	const atomic_val_t percent = (atomic_val_t)MIN(done * 100U / chk->action->size, 100U);

	if (atomic_set(&shown_percent, percent) != percent) {
		(void)evt_post(EVT_UPDATE_PROGRESS, (uint8_t)percent, 0, 0);
	}
}

static enum mgmt_cb_return hook(uint32_t event, enum mgmt_cb_return prev_status, int32_t *rc,
				uint16_t *group, bool *abort_more, void *data, size_t data_size)
{
	ARG_UNUSED(prev_status);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);

	if (event == MGMT_EVT_OP_CMD_RECV || event == MGMT_EVT_OP_CMD_DONE) {
		radio_activity();
		/* Dropped (queue full): let go, the next command posts again */
		if (atomic_cas(&smp_pending, 0, 1) && !evt_post(EVT_SMP_ACTIVITY, 0, 0, 0)) {
			atomic_set(&smp_pending, 0);
		}
	} else if (event == MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK) {
		/* A flash read of the trailer: nothing that blocks this stack */
		if (!boot_is_img_confirmed()) {
			LOG_WRN("upload refused: this image is in test, the other slot is the way back");
			*rc = MGMT_ERR_EBADSTATE;
			return MGMT_CB_ERROR_RC;
		}
		if (recorder_holds_updates()) {
			LOG_WRN("upload refused: a take runs");
			*rc = MGMT_ERR_EBUSY;
			return MGMT_CB_ERROR_RC;
		}
		atomic_set(&chunk_at, (atomic_val_t)k_uptime_get_32());
		atomic_set(&chunked, 1);
		progress(data, data_size);
	} else if (event == MGMT_EVT_OP_OS_MGMT_RESET && recorder_holds_updates()) {
		LOG_WRN("reset refused: a take runs");
		*rc = MGMT_ERR_EBUSY;
		return MGMT_CB_ERROR_RC;
	} else if (event == MGMT_EVT_OP_OS_MGMT_RESET) {
		clock_retain(); /* the freshest time across the reset */
		k_sem_reset(&painted);
		(void)evt_post(EVT_UPDATE_RESET, 0, 0, 0);
		(void)k_sem_take(&painted, K_MSEC(50));
		/* Last of all: the whole window of the watchdog is MCUboot's */
		feed();
	}
	return MGMT_CB_OK;
}

static struct mgmt_callback activity_cb = {
	.callback = hook,
	.event_id = MGMT_EVT_OP_CMD_RECV | MGMT_EVT_OP_CMD_DONE,
};

static struct mgmt_callback reset_cb = {
	.callback = hook,
	.event_id = MGMT_EVT_OP_OS_MGMT_RESET,
};

static struct mgmt_callback upload_cb = {
	.callback = hook,
	.event_id = MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK,
};

void update_init(void (*wdt_feed)(void))
{
	feed = wdt_feed;
	mgmt_callback_register(&activity_cb);
	mgmt_callback_register(&reset_cb);
	mgmt_callback_register(&upload_cb);
	if (!boot_is_img_confirmed()) {
		test_since_ms = k_uptime_get();
		LOG_WRN("image in test: confirm it over SMP within %d s, or the previous one comes "
			"back", CONFIG_CB91AI_WATCH_CONFIRM_WAIT_S);
	}
}

void update_process(void)
{
	atomic_set(&smp_pending, 0);
	if (test_since_ms < 0) {
		return;
	}
	if (boot_is_img_confirmed()) {
		LOG_INF("image confirmed by the host");
		test_since_ms = -1;
		return;
	}
	if (k_uptime_get() - test_since_ms >= CONFIG_CB91AI_WATCH_CONFIRM_WAIT_S * 1000LL &&
	    !recorder_holds_updates()) {
		LOG_ERR("image never confirmed: rebooting, MCUboot takes the previous one back");
		clock_retain();
		feed();
		sys_reboot(SYS_REBOOT_WARM);
	}
}

void update_reset_painted(void)
{
	k_sem_give(&painted);
}

bool update_confirmed(void)
{
	return test_since_ms < 0;
}

int update_slot(void)
{
	return boot_fetch_active_slot() == SLOT1_AREA ? 1 : 0;
}

bool update_busy(void)
{
	return atomic_get(&chunked) &&
	       k_uptime_get_32() - (uint32_t)atomic_get(&chunk_at) < UPLOAD_IDLE_MS;
}
