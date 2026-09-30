/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The spare image slot, erased ahead of the next update.
 *
 * MCUboot runs the image in place (direct execution with revert): an update is
 * uploaded to the slot this image does not run from, the spare one, which may be
 * slot 0 as well as slot 1. MCUmgr erases the pages the new image needs before
 * it answers the first chunk: 17 s of silence at the start of every update, over
 * a connection that slows the erase down by half. But what sits in the spare
 * slot after an update is the previous firmware, and once the running image is
 * confirmed nothing can bring it back: MCUboot starts the higher version, and
 * only gives up an image that was never confirmed. So it is erased here, at
 * rest, a few pages at a time from the main loop, and the next upload finds an
 * empty slot and starts at once (0.2 s instead of 16.7 s for the first chunk,
 * measured on 2026-09-20). Same erase, same energy, moved to a moment when
 * nobody waits.
 *
 * What is never erased:
 * - anything while the running image is not confirmed: the spare slot then
 *   holds the way back;
 * - an image newer than the running one: the way forward, marked for test or
 *   not yet, or an upload cut half way which the host will resume. An image
 *   that is not newer can go: MCUboot would never start it. (MCUmgr is no
 *   help here: in this mode it reports the old confirmed image as pending.)
 * - anything once a host has started an upload or marked an image during this
 *   boot (update_spare_cancel(), from the MCUmgr hooks): the slot is its
 *   business. Confirming the running image does not count: that is when the
 *   old one becomes useless.
 * And never at boot (specification 4.7) or during a connection. Not gated on
 * the cell voltage: the watch behaves the same over the whole range of the
 * CR2016 (2026-09-22).
 */

#include <zephyr/kernel.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <string.h>

#include "ble.h"
#include "update.h"

LOG_MODULE_REGISTER(cb91ai_update, LOG_LEVEL_INF);

#define SLOT0_AREA PARTITION_ID(slot0_partition)
#define SLOT1_AREA PARTITION_ID(slot1_partition)
#define PAGE_SIZE  4096
/* Not at boot, and not while the host that just updated the watch is still around */
#define START_AFTER_MS (60 * 1000)
/* Pages erased per pass of the main loop: about 0.2 s each */
#define PAGES_PER_PASS 6

enum spare_state { SPARE_CLEAN, SPARE_TO_ERASE, SPARE_BUSY, SPARE_KEPT };

static atomic_t state = ATOMIC_INIT(SPARE_CLEAN);
static uint8_t spare_area;
static int spare_slot;
static off_t next_offset;
static unsigned int erased_pages;
static int64_t started_at;

static int version_compare(const struct mcuboot_img_sem_ver *a, const struct mcuboot_img_sem_ver *b)
{
	if (a->major != b->major) {
		return a->major < b->major ? -1 : 1;
	}
	if (a->minor != b->minor) {
		return a->minor < b->minor ? -1 : 1;
	}
	if (a->revision != b->revision) {
		return a->revision < b->revision ? -1 : 1;
	}
	if (a->build_num != b->build_num) {
		return a->build_num < b->build_num ? -1 : 1;
	}
	return 0;
}

static bool page_is_blank(const struct flash_area *fa, off_t offset)
{
	uint32_t words[16];

	for (off_t at = offset; at < offset + PAGE_SIZE; at += sizeof(words)) {
		if (flash_area_read(fa, at, words, sizeof(words)) != 0) {
			return false;
		}
		for (size_t i = 0; i < ARRAY_SIZE(words); i++) {
			if (words[i] != 0xffffffffU) {
				return false;
			}
		}
	}
	return true;
}

int update_running_slot(void)
{
	return boot_fetch_active_slot() == SLOT1_AREA ? 1 : 0;
}

void update_spare_init(void)
{
	struct mcuboot_img_header running, spare;
	const uint8_t running_area = boot_fetch_active_slot();

	spare_slot = running_area == SLOT1_AREA ? 0 : 1;
	spare_area = spare_slot == 0 ? SLOT0_AREA : SLOT1_AREA;

	if (boot_read_bank_header(spare_area, &spare, sizeof(spare)) != 0) {
		/* No image header: empty already, or the leftovers of something. Blank
		 * pages cost a read each, the others are better gone.
		 */
		atomic_set(&state, SPARE_TO_ERASE);
		return;
	}
	if (boot_read_bank_header(running_area, &running, sizeof(running)) != 0 ||
	    version_compare(&spare.h.v1.sem_ver, &running.h.v1.sem_ver) > 0) {
		LOG_INF("spare slot %d: image %u.%u.%u+%u kept (newer than this one, or not comparable)",
			spare_slot, spare.h.v1.sem_ver.major, spare.h.v1.sem_ver.minor,
			spare.h.v1.sem_ver.revision, spare.h.v1.sem_ver.build_num);
		atomic_set(&state, SPARE_KEPT);
		return;
	}
	LOG_INF("spare slot %d: old image %u.%u.%u+%u, to be erased at rest once this one is confirmed",
		spare_slot, spare.h.v1.sem_ver.major, spare.h.v1.sem_ver.minor,
		spare.h.v1.sem_ver.revision, spare.h.v1.sem_ver.build_num);
	atomic_set(&state, SPARE_TO_ERASE);
}

void update_spare_cancel(void)
{
	if (atomic_get(&state) == SPARE_TO_ERASE || atomic_get(&state) == SPARE_BUSY) {
		LOG_INF("spare slot %d: left to the host (%u page(s) erased)", spare_slot,
			erased_pages);
	}
	atomic_set(&state, SPARE_KEPT);
}

char update_spare_state(void)
{
	static const char letters[] = { 'c', 'e', 'b', 'k' };

	return letters[atomic_get(&state)];
}

bool update_spare_process(void)
{
	const struct flash_area *fa;
	int current = atomic_get(&state);

	if (current != SPARE_TO_ERASE && current != SPARE_BUSY) {
		return false;
	}
	if (k_uptime_get() < START_AFTER_MS || !boot_is_img_confirmed() ||
	    strcmp(ble_state(), "connected") == 0) {
		return false; /* later: the state stays, the main loop keeps its slow pace */
	}
	if (flash_area_open(spare_area, &fa) != 0) {
		atomic_set(&state, SPARE_KEPT);
		return false;
	}
	if (current == SPARE_TO_ERASE) {
		started_at = k_uptime_get();
		atomic_cas(&state, SPARE_TO_ERASE, SPARE_BUSY);
	}

	for (int pages = 0; pages < PAGES_PER_PASS && next_offset < (off_t)fa->fa_size;) {
		/* A host may have arrived meanwhile: update_spare_cancel() */
		if (atomic_get(&state) != SPARE_BUSY) {
			break;
		}
		if (!page_is_blank(fa, next_offset)) {
			if (flash_area_erase(fa, next_offset, PAGE_SIZE) != 0) {
				LOG_ERR("spare slot %d: erase failed at 0x%lx", spare_slot,
					(long)next_offset);
				atomic_set(&state, SPARE_KEPT);
				break;
			}
			erased_pages++;
			pages++;
		}
		next_offset += PAGE_SIZE;
	}
	if (atomic_get(&state) == SPARE_BUSY && next_offset >= (off_t)fa->fa_size) {
		LOG_INF("spare slot %d: clean, %u page(s) erased in %lld ms", spare_slot, erased_pages,
			k_uptime_get() - started_at);
		atomic_set(&state, SPARE_CLEAN);
	}
	flash_area_close(fa);
	return atomic_get(&state) == SPARE_BUSY;
}
