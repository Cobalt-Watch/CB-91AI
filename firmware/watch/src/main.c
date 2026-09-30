/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * The watch: event loop and state machine.
 *
 * The loop, the states and the display on demand come from lot D1; the buttons
 * and the screens from D3 (buttons.c, lib/gesture.c, ui.c); the radio, the
 * Cobalt Link session and the updates over SMP from D6 (radio.c, link.c,
 * session.c, update.c), and the LED (light.c, led.c); the voice notes from E1
 * (recorder.c, notes.c, store.c), and the phone called again while they wait
 * from E2 (recall.c). Nothing is set on the watch itself: the
 * phone gives the time at every session, and the settings (2026-09-24).
 * The accelerometer (D4) and the energy policy (D5) plug into it,
 * one lot per case of the design: the effects of
 * ui.c that wait for them are only logged. Anything that wants something done
 * posts an event; this thread is the only one that decides, and the only
 * writer of the display.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel_version.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>
#include <zephyr/retention/retention.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <nrfx.h>

#include "accel.h"
#include "buttons.h"
#include "clock.h"
#include "debug.h"
#include "events.h"
#include "lcd.h"
#include "cell.h"
#include "counters.h"
#include "daily.h"
#include "led.h"
#include "light.h"
#include "link.h"
#include "link_proto.h"
#include "notes.h"
#include "pairing.h"
#include "power.h"
#include "radio.h"
#include "recall.h"
#include "recorder.h"
#include "ui.h"
#include "update.h"
#include "wrist.h"
#include "watch.h"

LOG_MODULE_REGISTER(watch, LOG_LEVEL_INF);

/* The software channel: a hung loop is caught well before the hardware one */
#define WDT_SOFT_TIMEOUT_MS 10000
/* The loop wakes at least this often, to feed that channel */
#define LOOP_WAKE_MS        5000
/* The cell read, the Battery Service and the status line refreshed */
#define STATUS_PERIOD_S     60
/* A pulse of the LED for the verdict of the app (EF-45), and for the end of a
 * take: short, on the cell */
#define LED_PULSE_MS        30
/* Bluetooth failed to start: a reset this much later */
#define RADIO_RETRY_MS      30000
/* The longest take: it then closes by itself, as it does 3 s after the last
 * word */
#define NOTE_MAX_MS         (CONFIG_CB91AI_WATCH_NOTE_MAX_S * 1000U)

static const struct device *const hw_wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;

enum state {
	STATE_SLEEP,
	STATE_DISPLAY,
	STATE_RECORD,
	STATE_RADIO,
	STATE_SESSION,
	STATE_PAIRING,
	STATE_UPDATE,
	STATE_BATTERY_EMPTY,
};

static const char *const state_names[] = {
	"sleep", "display", "record", "radio", "session", "pairing", "update", "battery-empty",
};

static enum state state = STATE_SLEEP;
static struct ui ui;
/* The glass goes dark at this uptime; INT64_MAX while a take or the reset
 * chord holds it */
static int64_t display_until;
static uint32_t tick_period_ms;
static uint16_t battery_mv;
/* The last reading at rest, kept across a reset: after an update the boot
 * read the cell still sagging from the upload (2.53 to 2.56 V for 2.85 at
 * rest, 26/09), and the phone, called at once, would have shown it spent */
static const struct device *const cell_kept = DEVICE_DT_GET(DT_NODELABEL(cell_retention));
/* A take ended (its flash writes too): the cell takes minutes to recover */
static uint32_t take_end_s;
#define CELL_SETTLE_S 120U
/* The display's time, key 0x02: 15 s at most (2026-09-29) */
#define DISPLAY_S_MAX 15U
static uint8_t display_s = CONFIG_CB91AI_WATCH_DISPLAY_SECONDS;
static uint32_t boot_reset_cause;
static int64_t radio_retry_at; /* 0: Bluetooth runs */
static struct light_style light_style = LIGHT_STYLE_DEFAULT;
/* Settings of the phone still to write: once the link is down (the flash is
 * written from the loop, off the session) */
static bool save_display;
static bool save_light;
/* The name the phone gave, taken once the link is down: bt_set_name() writes
 * it to the settings of the host ("bt/name") */
static char name_to_be[LINK_NAME_MAX + 1];
static bool save_name;
/* The take the glass and the LED follow, 0: none; still to stop or drop while
 * `take_live` */
static uint32_t take_seq;
static bool take_live;
/* The last take whose end was handled, and the last note announced to the
 * phone (a note is announced even when the report of its take was missed) */
static uint32_t handled_seq;
static uint32_t announced_id;
/* The reset of lot S3 (reset_begin()): under way while `resetting`, marked in
 * the settings until the notes are erased and the bond forgotten, so that a
 * cut finishes it at the next boot; asked on the buttons, it reboots the watch
 * once done, or at `reset_reboot_at` should the word of the erase not come */
#define SETTING_RESETTING "watch/resetting"
static bool resetting;
static bool reset_marked;   /* the mark read from the settings at boot */
static bool reset_unpaired; /* the bond forgotten in this reset */
static bool reset_erased;   /* the audio thread said the erase is over */
static int64_t reset_reboot_at;
/* Asked on the buttons, the reset restarts the watch once they are let go,
 * lest the restarted watch take them for a new reset; a minute at most */
#define RESET_LET_GO_MS 60000
/* The code of a pairing leaves the glass after this long at the latest
 * (the host gives a pairing 30 s) */
#define PAIR_SHOWN_MS 60000
static int64_t pair_shown_at;
/* The edges of the buttons already told to the recorder, or let go outside
 * a take; and when the take under way started (lot E1) */
static uint32_t heard_at[BUTTON_COUNT];
static uint32_t take_started_at;
/* The calls of the phone again of lot E2 since a call for another reason,
 * which radio_calls() tells: the silence before the next one grows with them */
static uint32_t recall_again;
static uint32_t recall_calls;
/* The daily call after a day without any contact with the phone (daily.h); the
 * development build shortens the day (key 0x7E) */
static uint32_t daily_period_s = DAILY_PERIOD_S;

static void tick_expired(struct k_timer *timer);
static K_TIMER_DEFINE(tick_timer, tick_expired, NULL);
/* The temperature of the case (a voice note of 2026-09-25), from the
 * thermometer of the nRF52840 since 0.2.1+12, chosen after a test side by side:
 * the BMA400's jumped by a degree from one reading to the next, the die's by
 * 0.09 degC. Read on the "tE P" screen every 5 s, and at
 * each status line; the degree shown goes through ui_temp_follow() */
#define TEMP_EVERY_MS 5000
#define TEMP_FRESH_MS 90000
static int32_t temp_cc;    /* the last reading, in hundredths of a degree */
static int16_t temp_shown; /* the degree on the glass */
static int64_t temp_at;
static bool temp_known;
static void status_expired(struct k_timer *timer);
static K_TIMER_DEFINE(status_timer, status_expired, NULL);

static void wdt_kick(void)
{
	if (wdt_channel >= 0) {
		task_wdt_feed(wdt_channel);
	}
}

/* The loop fed its channel no more (EF-72): without a callback, the task
 * watchdog would reboot by software, which the nRF reports as a software
 * reset, and the bite would never be counted. Held here with the interrupts
 * locked, nobody feeds the hardware watchdog any more: it bites within its
 * window (4 s), and the next boot counts it (counters.c). From the timer
 * interrupt: nothing else is safe to do here. */
static void loop_stalled(int channel, void *user_data)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(user_data);
	(void)irq_lock();
	for (;;) {
		arch_nop();
	}
}

static void tick_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	(void)evt_post(EVT_TICK, 0, 0, 0);
}

static void status_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	(void)evt_post(EVT_STATUS, 0, 0, 0);
}

/* The thermometer of the nRF52840 (TEMP: 0.25 degC steps, +/-0.25 degC from one
 * sample to the next, +/-5 degC), through Zephyr's driver */
static const struct device *const die_thermo = DEVICE_DT_GET(DT_NODELABEL(temp));

/* The die's temperature now, in hundredths of a degree: about a millisecond,
 * the HFXO started for the 36 us of the measurement, then released */
static int die_read(int32_t *centi)
{
	struct sensor_value v;
	int err;

	if (!device_is_ready(die_thermo)) {
		return -ENODEV;
	}
	err = sensor_sample_fetch(die_thermo);
	if (err == 0) {
		err = sensor_channel_get(die_thermo, SENSOR_CHAN_DIE_TEMP, &v);
	}
	if (err == 0) {
		*centi = v.val1 * 100 + v.val2 / 10000;
	}
	return err;
}

/* A reading, now: the degree shown follows it past the margin of
 * ui_temp_follow(), or at once when the last reading is old */
static void temp_read(void)
{
	const int64_t now = k_uptime_get();
	int32_t cc;

	if (die_read(&cc) != 0) {
		return;
	}
	temp_shown = ui_temp_follow(temp_shown, temp_known && now - temp_at < TEMP_FRESH_MS, cc);
	temp_cc = cc;
	temp_at = now;
	temp_known = true;
}

/* A temperature for the status line, in hundredths ("21.25"), "-" when
 * unknown */
static void temp_text(char *buf, size_t room, bool known, int32_t cc)
{
	const int32_t a = cc < 0 ? -cc : cc;

	if (!known) {
		(void)snprintf(buf, room, "-");
	} else {
		(void)snprintf(buf, room, "%s%d.%02d", cc < 0 ? "-" : "", (int)(a / 100),
			       (int)(a % 100));
	}
}

/* ---- The journal of the temperature and the cell (lot T1) ------------------- */

/* A voice note of 2026-09-25 and the choices: a record every 10 minutes
 * of UTC, the temperature and the cell, kept in the tail of the storage flash
 * (notes.h), read by the phone. None
 * written while a take runs, nor in the first minute after boot: they wait in
 * RAM for the next minute of the status line. */
#define JOURNAL_PERIOD_S 600U
#define JOURNAL_QUIET_MS 60000
#define JOURNAL_WAITING  8U
static uint32_t journal_period_s = JOURNAL_PERIOD_S;
static int64_t journal_mark_ms; /* the mark of UTC the timer aims at (tlog.h) */
static struct tlog_record journal_waiting[JOURNAL_WAITING];
static uint8_t journal_waiting_n;
static void journal_expired(struct k_timer *timer);
static K_TIMER_DEFINE(journal_timer, journal_expired, NULL);

static void journal_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	(void)evt_post(EVT_JOURNAL, 0, 0, 0);
}

/* The next record just past the next multiple of the period in UTC. None
 * while the watch has no time: the time given (link_do()), or found set at a
 * minute of the status line, arms it. */
static void journal_arm(void)
{
	int64_t now_ms;

	if (!clock_is_set()) {
		k_timer_stop(&journal_timer);
		return;
	}
	now_ms = clock_now_ms();
	journal_mark_ms = tlog_next_mark(now_ms, journal_period_s);
	k_timer_start(&journal_timer,
		      K_MSEC((uint32_t)(journal_mark_ms - now_ms) + TLOG_MARK_PAST_MS), K_NO_WAIT);
}

/* The records waiting, to the flash once allowed; one refused is dropped */
static void journal_flush(void)
{
	uint8_t done = 0;

	if (take_live || k_uptime_get() < JOURNAL_QUIET_MS) {
		return;
	}
	for (; done < journal_waiting_n; done++) {
		const int err = notes_journal_append(&journal_waiting[done], update_confirmed());

		if (err) {
			LOG_WRN("journal record dropped (%d)", err);
		}
	}
	journal_waiting_n = 0;
}

static void journal_tick(void)
{
	struct tlog_record r;
	int32_t mv = battery_mv;
	/* Early, as a fast crystal's timer is: the same mark again (tlog.h) */
	const bool early = clock_is_set() && tlog_early(clock_now_ms(), journal_mark_ms);

	journal_arm();
	if (!clock_is_set() || early) {
		return; /* a record carries its time */
	}
	temp_read();
	if (!temp_known) {
		return;
	}
	(void)power_vdd_mv(&mv);
	r.time_s = (uint32_t)(clock_now_ms() / 1000);
	r.temp_cc = (int16_t)CLAMP(temp_cc, INT16_MIN, INT16_MAX);
	r.cell = (uint16_t)((uint32_t)CLAMP(mv, 0, (int32_t)TLOG_CELL_MV) |
			    (take_live ? TLOG_CELL_TAKE : 0U) |
			    (radio_connected() ? TLOG_CELL_LINK : 0U));
	if (journal_waiting_n == JOURNAL_WAITING) {
		/* A take of more than an hour and a quarter: the oldest goes */
		memmove(&journal_waiting[0], &journal_waiting[1],
			sizeof(journal_waiting[0]) * (JOURNAL_WAITING - 1U));
		journal_waiting_n--;
	}
	journal_waiting[journal_waiting_n++] = r;
	journal_flush();
}

#if defined(CONFIG_CB91AI_WATCH_DEBUG_JOURNAL)
bool watch_set_journal_period(uint16_t seconds)
{
	if (seconds < 1U || seconds > JOURNAL_PERIOD_S) {
		return false;
	}
	journal_period_s = seconds;
	journal_arm();
	return true;
}
#endif

/* ---- The settings the phone gives, kept across a reboot -------------------- */

static int watch_settings_set(const char *name, size_t len, settings_read_cb read_cb,
			      void *cb_arg)
{
	uint8_t v[4];

	if (settings_name_steq(name, "resetting", NULL)) {
		reset_marked = true; /* a reset cut short: finished at this boot */
		return 0;
	}
	if (settings_name_steq(name, "display", NULL) && len == 1) {
		if (read_cb(cb_arg, v, 1) == 1 && v[0] >= 1) {
			/* 15 s at most since 0.2.1+31: a longer one kept from before is cut */
			display_s = MIN(v[0], DISPLAY_S_MAX);
		}
		return 0;
	}
	if (settings_name_steq(name, "light", NULL) && len == 4) {
		struct light_style s;

		if (read_cb(cb_arg, v, 4) == 4 && light_style_parse(v, 4, &s)) {
			light_style = s;
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(cb91ai_watch, "watch", NULL, watch_settings_set, NULL, NULL);

/* ---- What the loop lends the rest of the watch (watch.h) --------------------- */

uint16_t watch_battery_mv(void)
{
	return battery_mv;
}

uint8_t watch_display_s(void)
{
	return display_s;
}

/* From the loop (a SET of the session): the flash write is allowed here */
bool watch_set_display_s(uint8_t seconds)
{
	if (seconds < 1 || seconds > DISPLAY_S_MAX) {
		return false;
	}
	if (seconds != display_s) {
		display_s = seconds;
		save_display = true;
	}
	return true;
}

bool watch_set_light(const uint8_t *value, size_t len)
{
	struct light_style s;

	if (!light_style_parse(value, len, &s)) {
		return false;
	}
	if (memcmp(&s, &light_style, sizeof(s)) != 0) {
		light_style = s;
		led_set_style(&s);
		save_light = true;
	}
	/* Shown at once, as a press of LIGHT shows it: the app's list lights
	 * the watch at each choice (2026-09-29) */
	led_light_press();
	return true;
}

bool watch_set_name(const uint8_t *value, size_t len)
{
	if (!link_name_valid(value, len)) {
		return false;
	}
	memcpy(name_to_be, value, len);
	name_to_be[len] = '\0';
	save_name = true;
	return true;
}

size_t watch_name(uint8_t *out, size_t room)
{
	const char *name = save_name ? name_to_be : bt_get_name();
	const size_t len = MIN(strlen(name), room);

	memcpy(out, name, len);
	return len;
}

/* The settings of the phone, written once no link is up */
static void settings_flush(void)
{
	if (radio_connected()) {
		return;
	}
	if (save_name) {
		save_name = false;
		if (bt_set_name(name_to_be) == 0) {
			LOG_INF("named \"%s\"", name_to_be);
			radio_renamed();
		}
	}
	if (save_display) {
		save_display = false;
		(void)settings_save_one("watch/display", &display_s, sizeof(display_s));
	}
	if (save_light) {
		uint8_t raw[4];

		save_light = false;
		(void)light_style_put(&light_style, raw, sizeof(raw));
		(void)settings_save_one("watch/light", raw, sizeof(raw));
	}
}

size_t watch_light(uint8_t *out, size_t room)
{
	return light_style_put(&light_style, out, room);
}

/* The window of the hardware watchdog in force, in ms: it only changes at a
 * hardware reset, and after an update it is still that of the image that
 * started it */
static uint32_t wdt_window_ms(void)
{
	if ((NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk) == 0) {
		return 0;
	}
	return (uint32_t)(((uint64_t)NRF_WDT->CRV * 1000U) / 32768U);
}

size_t watch_status(char *buf, size_t room)
{
	char temp[16]; /* room for any int and its decimals, as the compiler counts */
	char params[32]; /* interval in ms / latency / supervision in ms, or "-" */
	uint32_t interval_us;
	uint16_t latency;
	uint16_t timeout_10ms;
	uint32_t changes;
	uint32_t raises;
	uint32_t taps;
	int32_t last_z;
	int n;

	if (room == 0) {
		return 0;
	}
	temp_read(); /* the line and the glass share the reading */
	if (radio_link_params(&interval_us, &latency, &timeout_10ms)) {
		(void)snprintf(params, sizeof(params), "%u.%02u/%u/%u", (unsigned int)(interval_us / 1000U),
			       (unsigned int)(interval_us % 1000U / 10U), (unsigned int)latency,
			       (unsigned int)timeout_10ms * 10U);
	} else {
		(void)strcpy(params, "-");
	}
	temp_text(temp, sizeof(temp), temp_known, temp_cc);
	wrist_counts(&changes, &raises, &taps, &last_z);
	n = snprintf(buf, room,
			       /* The version with its build (0.2.1+31): the app compares it with the updates of the website */
			       "%s watch vdd=%u up=%us img=%s rst=%x conn=%u adv=%s call=%us end=%us "
			       "again=%u day=%us t=%c ppb=%d wdt=%u slot=%d lost=%u refused=%u dis=%02x "
			       "temp=%s "
			       "log=%u wrist=%u/%u/%u wz=%d stk=%x boot=%u/%u cp=%s",
			       APP_VERSION_EXTENDED_STRING, battery_mv, (unsigned int)(k_uptime_get() / 1000),
			       update_confirmed() ? "confirmed" : "test",
			       (unsigned int)boot_reset_cause, radio_connections(), radio_adv_mode(),
			       (unsigned int)radio_last_call_s(), (unsigned int)radio_last_link_end_s(),
			       (unsigned int)recall_again,
			       (unsigned int)daily_at(radio_last_call_s(), RECALL_WINDOW_S,
						      radio_last_link_end_s(), daily_period_s),
			       !clock_is_set()                           ? '-'
			       : (clock_flags() & CLOCK_FLAG_APPROXIMATE) ? '~'
									   : 'y',
			       clock_ppb(), wdt_window_ms(), update_slot(),
			       (unsigned int)evt_dropped(), (unsigned int)link_refused(),
			       radio_last_disconnect_reason(), temp, (unsigned int)notes_journal_next(),
			       (unsigned int)changes, (unsigned int)raises, (unsigned int)taps,
			       (int)last_z, (unsigned int)buttons_stuck(),
			       (unsigned int)counters_boots(), (unsigned int)counters_bites(), params);

	if (n < 0) {
		return 0;
	}
	if ((size_t)n >= room) {
		/* Too long for the room (a VALUE of Cobalt Link, the MTU of a
		 * phone): cut after the last whole field, never inside one, so that
		 * a reader finds a field whole or not at all */
		size_t len = room - 1;

		while (len > 0 && buf[len] != ' ') {
			len--;
		}
		buf[len] = '\0';
		return len;
	}
	return (size_t)n;
}

/* The Battery Service notifies its subscribers: from the system work queue,
 * where ATT never waits for a buffer (link.c) */
static atomic_t bas_percent = ATOMIC_INIT(100);

static void bas_update(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)bt_bas_set_battery_level((uint8_t)atomic_get(&bas_percent));
}

static K_WORK_DEFINE(bas_work, bas_update);

/* The cell is read when nothing draws on it: a link, a take or the LED make
 * it sag (on 2026-09-26, 2.55 V read as a link came up for 2.85 V at rest),
 * and the phone, the Battery Service and "bA t" would take it for spent
 * (lot D5, cell.h). Nor right after: a CR2016 takes minutes to recover (2.70 V
 * after takes of 20 s, 2.85 a few minutes later), from the boot, a link or a
 * take with its flash writes. Otherwise the last reading at rest stands. */
static bool cell_quiet(void)
{
	const uint32_t now = (uint32_t)(k_uptime_get() / 1000);
	const uint32_t busy_until = MAX(radio_last_link_end_s(), take_end_s);

	return !radio_connected() && !take_live && !recorder_busy() && !led_lit() &&
	       now >= busy_until + CELL_SETTLE_S;
}

static void cell_set(uint16_t mv)
{
	battery_mv = mv;
	atomic_set(&bas_percent, cell_percent_at(mv, temp_cc, temp_known));
	(void)k_work_submit(&bas_work);
}

/* The cell (when quiet), the Battery Service and the status line */
static void refresh_status(void)
{
	char line[256];
	int32_t mv;

	if (battery_mv == 0U && device_is_ready(cell_kept) && retention_is_valid(cell_kept) == 1) {
		/* After a reset: the last reading at rest, until a new one */
		uint16_t kept = 0;

		if (retention_read(cell_kept, 0, (uint8_t *)&kept, sizeof(kept)) == 0 && kept != 0U) {
			cell_set(kept);
		}
	}
	/* Nothing kept (a new cell, whose boot draws nothing on it yet): read at
	 * once; then only at rest */
	if ((battery_mv == 0U || cell_quiet()) && power_vdd_mv(&mv) == 0 && mv > 0) {
		const uint16_t kept = (uint16_t)mv;

		cell_set(kept);
		if (device_is_ready(cell_kept)) {
			(void)retention_write(cell_kept, 0, (const uint8_t *)&kept, sizeof(kept));
		}
	}
	(void)watch_status(line, sizeof(line));
	debug_set_status(line);
}

/* ---- The glass ---------------------------------------------------------------- */

/* What the watch knows, for ui.c */
static void context(struct ui_context *c)
{
	c->uptime_ms = k_uptime_get_32();
	c->time_set = clock_is_set();
	c->local_ms = clock_now_ms() + (int64_t)clock_tz_minutes() * 60000;
	c->h24 = clock_24h();
	c->notes = (uint8_t)MIN(notes_count(), UINT8_MAX);
	c->link = radio_connected()     ? UI_LINK_CONNECTED
		  : radio_advertising() ? UI_LINK_ADVERTISING
					: UI_LINK_OFF;
	c->battery_mv = battery_mv;
	c->temp_c = temp_shown;
	c->temp_known = temp_known && k_uptime_get() - temp_at < TEMP_FRESH_MS;
	c->display_on = state == STATE_DISPLAY;
}

/* The "tE P" screen lit: a reading unless a recent one is there, then every
 * 5 s while it stays; true when one was taken, for the glass to show it */
static bool temp_follow(void)
{
	if (state == STATE_DISPLAY && ui.screen == UI_SCREEN_TEMP &&
	    (!temp_known || k_uptime_get() - temp_at >= TEMP_EVERY_MS)) {
		temp_read();
		return true;
	}
	return false;
}

/* The only writer of the framebuffer, by design */
static void draw(void)
{
	struct ui_context c;
	struct ui_frame frame;

	context(&c);
	ui_render(&ui, &c, &frame);
	lcd_clear();
	for (uint8_t pos = 0; pos < 10; pos++) {
		if (frame.text[pos] != ' ') {
			lcd_display_character(frame.text[pos], pos);
		}
	}
	lcd_set_colon(frame.colon);
	lcd_set_indicator(LCD_INDICATOR_PM, frame.pm);
	lcd_set_indicator(LCD_INDICATOR_24H, frame.h24);
	(void)lcd_flush();
}

/* "UPd" before the reset of an update, on the three last digits, where U, P and
 * d all draw whole; the driver keeps it on its own while
 * MCUboot checks the new image */
static void paint_update(void)
{
	lcd_clear();
	lcd_display_string("UPd", 7);
	(void)lcd_flush();
	(void)lcd_display_power(true);
}

/* During the whole transfer of an image (28/09): "U P" on top, U on
 * the weekday's second letter and P on the day units, the only top cells that
 * draw them whole (the day tens has no F), and the share received below, 0 to
 * 100, on the hour units and the minutes, colon off; the minute tens draws its
 * 7 as Sensor Watch does (lib/lcd.c) */
static bool transfer_shown;

static void paint_transfer(uint8_t percent)
{
	char share[4];

	lcd_clear();
	lcd_display_character('U', 1);
	lcd_display_character('P', 3);
	(void)snprintf(share, sizeof(share), "%3u", MIN(percent, 100U));
	lcd_display_string(share, 5);
	(void)lcd_flush();
	(void)lcd_display_power(true);
}

/* One wake-up per tick, and only while the glass is lit (EF-05): once a
 * second */
static void tick_follow(void)
{
	const uint32_t period = ui_tick_ms(&ui);

	if (state == STATE_DISPLAY && period != tick_period_ms) {
		tick_period_ms = period;
		k_timer_start(&tick_timer, K_MSEC(period), K_MSEC(period));
	}
}

static void enter(enum state next)
{
	if (next == state) {
		return;
	}
	LOG_INF("%s -> %s", state_names[state], state_names[next]);
	state = next;

	switch (state) {
	case STATE_DISPLAY:
		(void)lcd_display_power(true);
		tick_period_ms = 0;
		tick_follow();
		break;
	case STATE_SLEEP:
		k_timer_stop(&tick_timer);
		tick_period_ms = 0;
		(void)lcd_display_power(false);
		break;
	case STATE_UPDATE:
		/* "UPd" until the reset: no tick redraws the time or darkens the
		 * glass. A gesture still lights the time, should the reset not come. */
		k_timer_stop(&tick_timer);
		tick_period_ms = 0;
		break;
	default:
		break;
	}
}

/* The reset of lot S3, asked on the buttons or found marked at boot: the mark
 * first, then no note offered any more, the take under way dropped, the bond
 * and the settings of the phone forgotten, and the notes and the journal
 * erased by the audio thread, reset_end() when it says so. The calibration of
 * the crystal stays: it belongs to the watch. */
static void reset_begin(void)
{
	const uint8_t one = 1;

	LOG_WRN("reset: bond, notes and settings");
	resetting = true;
	(void)settings_save_one(SETTING_RESETTING, &one, sizeof(one));
	notes_hide(true);
	if (take_live) {
		recorder_drop(take_seq);
		take_live = false;
	}
	led_recording(false);
	if (radio_retry_at == 0) {
		pairing_reset();
		reset_unpaired = true;
	}
	/* A setting of the phone still to write must not come back after. The
	 * name of the watch stays, as the host keeps it (2026-09-25: a
	 * watch given away keeps its name); only one in flight is dropped. */
	save_display = false;
	save_light = false;
	save_name = false;
	clock_process();
	(void)settings_delete("watch/display");
	(void)settings_delete("watch/light");
	(void)settings_delete("clock/24h");
	journal_waiting_n = 0; /* records of the old owner, not written yet */
	recorder_wipe();
}

static void reset_reboot(void);

/* The erase of the notes is over, done or refused by the flash */
static void reset_end(bool wiped)
{
	if (!resetting) {
		return;
	}
	if (wiped && reset_unpaired) {
		(void)settings_delete(SETTING_RESETTING);
		notes_hide(false);
		resetting = false;
		LOG_WRN("reset done");
	} else {
		/* Marked still: the next boot starts it again, the notes hidden */
		LOG_ERR("reset unfinished (notes %s, bond %s)", wiped ? "erased" : "not erased",
			reset_unpaired ? "forgotten" : "kept");
	}
	/* Asked on the buttons: a clean start, once they are let go (the loop) */
	reset_erased = true;
}

/* A take begins on the press of ALARM: record then cancel (EF-10), straight to
 * the storage flash (recorder.c) */
static void take_start(void)
{
	struct recorder_take t = {
		.tz_minutes = clock_tz_minutes(),
		.time_approx = (clock_flags() & CLOCK_FLAG_APPROXIMATE) != 0,
		.max_ms = NOTE_MAX_MS,
		.until_silence = true,
	};

	if (update_busy()) {
		/* An image is on its way: a take would stop it (update.c) */
		LOG_WRN("take refused: an update is under way");
		ui_take_ended(&ui);
		led_pulse(false, LED_PULSE_MS);
		return;
	}
	if (take_live) {
		/* One of the harness: the wearer's press wins */
		recorder_drop(take_seq);
		led_recording(false);
	}
	if (clock_is_set()) {
		t.utc_ms = clock_now_ms();
	}
	take_started_at = k_uptime_get_32();
	take_seq = recorder_start(&t);
	take_live = true;
	LOG_INF("take %u: start", take_seq);
}

/* The press that stops a take: of the buttons down, the first pressed since
 * the take started, ALARM alone or the chord it makes (not the press of
 * ALARM that started the take, held since); ALARM's last edge otherwise,
 * which take.c cannot place and trims as before */
static uint32_t stop_press_at(void)
{
	uint32_t at = buttons_changed_at(BUTTON_ALARM);
	bool found = false;

	for (size_t b = 0; b < BUTTON_COUNT; b++) {
		const uint32_t moved = buttons_changed_at((enum button)b);

		if (buttons_down((enum button)b) && (int32_t)(moved - take_started_at) > 0 &&
		    (!found || (int32_t)(moved - at) < 0)) {
			at = moved;
			found = true;
		}
	}
	return at;
}

/* The effects that ui.c asks for (UI_FX_*) */
static void apply(uint32_t fx)
{
	if (fx & UI_FX_TAKE_START) {
		take_start();
	}
	if (fx & UI_FX_TAKE_KEEP) {
		/* "rEC" and the red blink until a press of ALARM, or the end; its
		 * frames go to the flash from now on */
		recorder_keep(take_seq);
		led_recording(true);
		LOG_INF("take %u: kept, runs on", take_seq);
	}
	if (fx & UI_FX_TAKE_LISTEN) {
		/* ALARM released: once its click is over, the voice counts */
		recorder_listen(take_seq, buttons_changed_at(BUTTON_ALARM));
	}
	if (fx & UI_FX_TAKE_STOP) {
		/* Up to where the finger was first heard, before its contact */
		recorder_stop(take_seq, stop_press_at());
		take_live = false;
		led_recording(false);
		LOG_INF("take %u: stop, the note is kept", take_seq);
	}
	if (fx & UI_FX_TAKE_DROP) {
		recorder_drop(take_seq);
		take_live = false;
		led_recording(false);
		LOG_INF("take %u: drop", take_seq);
	}
	if (fx & UI_FX_SYNC) {
		/* EF-11: the watch calls the phone */
		radio_call(RADIO_REASON_SYNC);
	}
	/* EF-15, PO-03: the light of LIGHT, as the app set it */
	if (fx & UI_FX_LIGHT_ON) {
		led_light_press();
	}
	if (fx & UI_FX_LIGHT_HOLD) {
		led_light_hold();
	}
	if (fx & UI_FX_LIGHT_OFF) {
		led_light_release();
	}
	if (fx & UI_FX_RESET && !resetting) {
		reset_begin();
		/* "rESEt" blinks until the restart (2026-09-25) */
		ui_resetting(&ui);
		reset_reboot_at = k_uptime_get() + 5000;
	}
	if (fx & UI_FX_DISPLAY) {
		const uint32_t lit_ms = ui_display_ms(&ui, display_s * 1000U);

		display_until = lit_ms ? k_uptime_get() + lit_ms : INT64_MAX;
		enter(STATE_DISPLAY);
		tick_follow();
		draw();
	}
}

/* A take is over (EVT_AUDIO_STOPPED, or found over at a wake-up should that
 * event have been lost). Each is handled once, even when a new press has taken
 * the glass while it was being finished. The one on the glass frees it when it
 * ended by itself; a note kept is a short green pulse, a take that left nothing
 * on its own a red one; and every note kept since the last one announced is
 * offered in the session under way, or the phone is called. */
static void take_over(void)
{
	uint8_t end;
	uint32_t id;
	const uint32_t seq = recorder_result(&end, &id);
	uint32_t last;

	if (seq == 0 || seq == handled_seq) {
		return;
	}
	handled_seq = seq;
	take_end_s = (uint32_t)(k_uptime_get() / 1000);
	if (seq == take_seq) {
		take_seq = 0;
		take_live = false;
		led_recording(false);
		if (ui.taking || ui.recording) {
			ui_take_ended(&ui);
			apply(UI_FX_DISPLAY);
		}
	}
	LOG_INF("take %u over: end %u, note %u", seq, end, id);
	if (id != 0) {
		led_pulse(true, LED_PULSE_MS);
	} else if (end != RECORDER_DROPPED) {
		led_pulse(false, LED_PULSE_MS);
	}
	last = notes_last_id();
	if (last > announced_id) {
		announced_id = last;
		if (link_in_session()) {
			(void)link_note_added(last);
		} else {
			radio_call(RADIO_REASON_NOTE);
		}
	}
}

/* Lot E2: while a note waits, the phone is called again once the radio has
 * been silent long enough, past the window of the last call and the end of
 * the last link: 15 min after a call for another reason (a note kept, a press,
 * the boot), then 30 min, then every hour (2026-09-25); if a phone
 * may take the notes: the bonded one, or any in the development build, whose
 * link is open (recall.c). Not during a take, whose end calls anyway when it
 * keeps a note; the notes counted only once a call is due by the time. */
static void recall_check(void)
{
	const bool reachable = pairing_bonded() || IS_ENABLED(CONFIG_CB91AI_WATCH_LINK_OPEN);
	uint16_t waiting;

	if (radio_calls() != recall_calls) {
		/* Called since for another reason: the rhythm starts again */
		recall_again = 0;
		recall_calls = radio_calls();
	}
	if (!recall_due((uint32_t)(k_uptime_get() / 1000), radio_last_call_s(),
			radio_last_link_end_s(), recall_again, radio_connected(), reachable)) {
		return;
	}
	waiting = notes_count();
	if (waiting > 0) {
		LOG_INF("%u note(s) waiting: calling the phone again (%u)", waiting,
			recall_again + 1U);
		radio_call(RADIO_REASON_NOTE);
		recall_again++;
		recall_calls = radio_calls();
	}
}

/* The daily call (daily.h): a day without any contact with the phone, a phone
 * to come, no link up, and no note waiting, which the recall calls for. Asked
 * at every wake-up, as recall_check(); the notes counted only once the day is
 * over. */
static void daily_check(void)
{
	const bool reachable = pairing_bonded() || IS_ENABLED(CONFIG_CB91AI_WATCH_LINK_OPEN);

	if (!daily_due((uint32_t)(k_uptime_get() / 1000), radio_last_call_s(), RECALL_WINDOW_S,
		       radio_last_link_end_s(), daily_period_s, radio_connected(), reachable)) {
		return;
	}
	if (notes_count() > 0) {
		return;
	}
	LOG_INF("a day without the phone: the daily call");
	radio_call(RADIO_REASON_DAILY);
}

#if defined(CONFIG_CB91AI_WATCH_DEBUG_DAILY)
bool watch_set_daily_period(uint32_t seconds)
{
	if (seconds < DAILY_PERIOD_MIN_S || seconds > DAILY_PERIOD_S) {
		return false;
	}
	daily_period_s = seconds;
	return true;
}
#endif

#if defined(CONFIG_CB91AI_WATCH_DEBUG_TAKE)
/* The whole chain of a take on a sealed watch, from the harness: the glass is
 * left alone, the LED blinks as for any take */
bool watch_debug_take(uint8_t seconds)
{
	struct recorder_take t = {
		.tz_minutes = clock_tz_minutes(),
		.time_approx = (clock_flags() & CLOCK_FLAG_APPROXIMATE) != 0,
		.max_ms = seconds * 1000U,
		.latched = true,
	};

	if (take_seq != 0 || recorder_busy() || update_busy()) {
		return false;
	}
	if (clock_is_set()) {
		t.utc_ms = clock_now_ms();
	}
	take_seq = recorder_start(&t);
	take_live = true;
	led_recording(true);
	LOG_INF("take %u: %u s for the harness", take_seq, seconds);
	return true;
}
#endif

/* A phone pairs (lot S1): its code on the glass until the end, then a pulse,
 * green bonded or red failed; the count of failures kept (pairing.c) */
static void pairing_event(uint8_t what, uint32_t passkey)
{
	if (what == PAIRING_SHOW) {
		ui_pairing(&ui, true, passkey);
		pair_shown_at = k_uptime_get();
		apply(UI_FX_DISPLAY);
		return;
	}
	if (what != PAIRING_HIDE) {
		led_pulse(what == PAIRING_BONDED, LED_PULSE_MS);
	}
	if (ui.pairing) {
		ui_pairing(&ui, false, 0);
		apply(UI_FX_DISPLAY);
	}
}

/* The code leaves the glass: the link gone, or too long shown (the event of
 * the end lost) */
static void pairing_forget(void)
{
	if (ui.pairing) {
		ui_pairing(&ui, false, 0);
		apply(UI_FX_DISPLAY);
	}
}

/* The reset is done, or overdue: a clean start, blank */
static void reset_reboot(void)
{
	LOG_WRN("reset: rebooting");
	clock_retain();
	wdt_kick(); /* last of all: MCUboot checks the image unfed */
	sys_reboot(SYS_REBOOT_WARM);
}

/* What the session asks of the watch (LINK_DO_*) */
static void link_do(uint32_t todo)
{
	if (todo & LINK_DO_TIME) {
		int64_t utc_ms;
		int16_t tz;

		/* From the phone at every session: it also calibrates the crystal */
		link_time(&utc_ms, &tz);
		clock_set(utc_ms, tz, true);
		journal_arm(); /* on the marks of the time given */
		if (state == STATE_DISPLAY) {
			draw();
		}
	}
	if (todo & LINK_DO_RESULT) {
		/* The verdict of the app: green success, red failure (EF-45) */
		led_pulse(link_result() == LINK_RESULT_SUCCESS, LED_PULSE_MS);
	}
}

static void on_gestures(void)
{
	struct gesture gestures[GESTURE_OUT_MAX(BUTTON_COUNT)];
	const size_t n = buttons_process(gestures);

	/* A take under way must not take the click of a button for a word: the
	 * audio thread learns of each edge as it comes (lot E1). The press that
	 * starts a take comes before it, and is let go. */
	for (size_t b = 0; b < BUTTON_COUNT; b++) {
		const uint32_t at = buttons_changed_at((enum button)b);

		if (at != heard_at[b]) {
			heard_at[b] = at;
			if (take_live) {
				recorder_heard(at, buttons_down((enum button)b));
			}
		}
	}
	for (size_t i = 0; i < n; i++) {
		struct ui_context c;

		context(&c);
		LOG_DBG("gesture %u, button %u, count %u, %u ms", gestures[i].type,
			gestures[i].button, gestures[i].count, gestures[i].ms);
		/* EF-11: clicks of ALARM go to the app in a held session */
		if (gestures[i].type == GESTURE_CLICK && gestures[i].button == BUTTON_ALARM) {
			(void)link_gesture(gestures[i].count, gestures[i].button);
		}
		apply(ui_gesture(&ui, &gestures[i], &c));
	}
	if (temp_follow()) {
		draw(); /* "tE P" just lit: its reading at once */
	}
}

static void handle(const struct evt *e)
{
	switch (e->type) {
	case EVT_TICK:
		if (state != STATE_DISPLAY) {
			break;
		}
		if (k_uptime_get() >= display_until) {
			ui_display_off(&ui);
			enter(STATE_SLEEP);
		} else {
			(void)temp_follow();
			draw();
		}
		break;
	case EVT_BUTTON_EDGE:
		on_gestures();
		break;
	case EVT_MOTION:
		/* EF-44: the display comes on demand, and goes away alone (lot D4):
		 * a raise of the wrist, or a double tap; never a take (EF-19). The
		 * status is read all the same during a transfer, which keeps the
		 * glass (the latched lines released) */
		if (wrist_event() != WRIST_NONE && !resetting && !transfer_shown) {
			apply(UI_FX_DISPLAY);
		}
		break;
	case EVT_BLE_CONNECTED:
		refresh_status();
		break;
	case EVT_BLE_DISCONNECTED:
		link_closed();
		pairing_forget();
		break;
	case EVT_LINK_OPEN:
		link_do(link_open());
		break;
	case EVT_LINK_RX:
		link_do(link_rx());
		break;
	case EVT_LINK_TX:
		link_do(link_tx());
		break;
	case EVT_LINK_TIMER:
		link_do(link_tick());
		break;
	case EVT_SMP_ACTIVITY:
		link_smp();
		break;
	case EVT_STATUS:
		refresh_status();
		counters_keep(); /* into the settings, past the first minute */
		if (wrist_line_up()) {
			/* Its event lost: a latched line stays up until read, or until
			 * the sensor goes back to low power (p. 36 of its datasheet) */
			(void)evt_post(EVT_MOTION, 0, 0, 0);
		}
		journal_flush(); /* records kept waiting by a take or the boot */
		if (clock_is_set() && k_timer_remaining_get(&journal_timer) == 0U) {
			journal_arm(); /* the time came by another way than a session */
		}
		break;
	case EVT_JOURNAL:
		journal_tick();
		break;
	case EVT_AUDIO_STOPPED:
		take_over();
		journal_flush();
		break;
	case EVT_NOTES_READY:
		/* The notes of the flash, read at boot: no news, but if some wait
		 * and no call was made for another reason, the phone is called */
		announced_id = MAX(announced_id, notes_last_id());
		if (notes_count() > 0 && radio_reason() == RADIO_REASON_NONE && !radio_connected()) {
			radio_call(RADIO_REASON_NOTE);
		}
		break;
	case EVT_NOTES_WIPED:
		reset_end(e->arg != 0);
		break;
	case EVT_PAIRING:
		pairing_event(e->arg, e->data);
		break;
	case EVT_UPDATE_RESET:
		transfer_shown = false;
		enter(STATE_UPDATE);
		paint_update();
		update_reset_painted();
		break;
	case EVT_UPDATE_PROGRESS:
		if (resetting || (state == STATE_UPDATE && !transfer_shown)) {
			break; /* "UPd" already: the reset is coming */
		}
		transfer_shown = true;
		enter(STATE_UPDATE);
		paint_transfer(e->arg);
		break;
	default:
		LOG_DBG("event %u ignored in state %s", e->type, state_names[state]);
		break;
	}
}

int main(void)
{
	uint32_t cause = 0;

	/* First of all, before anything that could hang or take time: the
	 * watchdog of the previous image is still running after an update, and
	 * nobody fed it while MCUboot checked this one.
	 */
	if (device_is_ready(hw_wdt) && task_wdt_init(hw_wdt) == 0) {
		wdt_channel = task_wdt_add(WDT_SOFT_TIMEOUT_MS, loop_stalled, NULL);
	}
	wdt_kick();

	LOG_INF("COBALT CB-91AI watch %s, Zephyr %u.%u.%u", APP_VERSION_EXTENDED_STRING,
		SYS_KERNEL_VER_MAJOR(sys_kernel_version_get()),
		SYS_KERNEL_VER_MINOR(sys_kernel_version_get()),
		SYS_KERNEL_VER_PATCHLEVEL(sys_kernel_version_get()));
	if (wdt_channel < 0) {
		LOG_WRN("watchdog not started (%d)", wdt_channel);
	}
	if (hwinfo_get_reset_cause(&cause) == 0) {
		boot_reset_cause = cause;
	}

	(void)clock_init(); /* also starts the settings subsystem */
	(void)settings_load_subtree("watch");
	(void)settings_load_subtree("pair");
	counters_boot(boot_reset_cause); /* EF-72: in the retained RAM at once */
	/* Cleared once counted only: a boot that dies before this line leaves
	 * the cause, a bite included, for the next one to count */
	(void)hwinfo_clear_reset_cause();
	(void)lcd_init();
	ui_init(&ui);
	if (buttons_init() != 0) {
		LOG_ERR("buttons not ready: the display only lights at boot");
	}
	if (led_init(&light_style) != 0) {
		LOG_WRN("LED not ready");
	}
	if (power_init() != 0) {
		LOG_WRN("cell reading not ready");
	}
	/* The wrist (lot D4): the accelerometer reset and started in low power,
	 * awake on motion; asleep (0.16 uA) should it not start, the buttons
	 * then the only way to the display */
	if (wrist_init() != 0) {
		(void)accel_sleep();
	}
	wdt_kick();
	/* The way back first: SMP and the countdown of an image in test */
	update_init(wdt_kick);
	if (radio_init() != 0) {
		/* A sealed watch without Bluetooth is out of reach for good, and
		 * bt_enable() cannot be called twice: a reset in a while, which also
		 * brings the previous image back while this one is in test */
		LOG_ERR("no Bluetooth: reset in %d s", RADIO_RETRY_MS / 1000);
		radio_retry_at = k_uptime_get() + RADIO_RETRY_MS;
	} else {
		/* A reset cut short finishes first, before the bond is counted or
		 * any note offered (lot S3); then the bond read with the settings of
		 * Bluetooth (lot S1) */
		if (reset_marked) {
			reset_begin();
		}
		if (pairing_init() != 0) {
			LOG_ERR("pairing callbacks not registered");
		}
		if (!update_confirmed()) {
			/* A new image in test: only a host can confirm it (update.c),
			 * and the product keeps silent unless called: call it now, or
			 * it goes back to the previous image after its wait */
			radio_call(RADIO_REASON_UPDATED);
		} else if (!clock_is_set()) {
			/* A new cell: only the phone sets the time now, call it */
			radio_call(RADIO_REASON_SYNC);
		} else if (!pairing_bonded()) {
			/* A blank watch, new or reset: a phone may pair, be found */
			radio_call(RADIO_REASON_SYNC);
		}
	}
	wdt_kick();
	refresh_status();
	k_timer_start(&status_timer, K_SECONDS(STATUS_PERIOD_S), K_SECONDS(STATUS_PERIOD_S));
	journal_arm();

	/* A fresh start shows the time for a while, as a sign of life */
	apply(UI_FX_DISPLAY);

	for (;;) {
		struct evt e;
		/* The only place this thread waits. The core sleeps here. */
		const bool got = evt_wait(&e, K_MSEC(LOOP_WAKE_MS));

		wdt_kick();
		update_process();
		if (radio_retry_at != 0 && k_uptime_get() >= radio_retry_at) {
			clock_retain();
			wdt_kick(); /* last of all: MCUboot checks the image unfed */
			sys_reboot(SYS_REBOOT_WARM);
		}
		if (got) {
			handle(&e);
		}
		/* A transfer given up (no chunk for 30 s, update.c): the glass goes
		 * dark, and the watch back to its day */
		if (transfer_shown && !update_busy()) {
			transfer_shown = false;
			ui_display_off(&ui);
			enter(STATE_SLEEP);
		}
		/* At every wake-up, 5 s apart at most: a take whose end was not
		 * heard, the call of the phone for the notes waiting, the retained
		 * time, and the flash writes of the loop */
		if (!recorder_busy()) {
			take_over();
			recall_check();
			daily_check();
		}
		/* The reset asked on the buttons: a clean start once the notes are
		 * erased (or the word of it overdue) and the buttons let go */
		if (reset_reboot_at != 0) {
			const int64_t now = k_uptime_get();

			if ((reset_erased || now >= reset_reboot_at) &&
			    (!ui.chord || now >= reset_reboot_at + RESET_LET_GO_MS)) {
				reset_reboot();
			}
		}
		if (ui.pairing && k_uptime_get() - pair_shown_at >= PAIR_SHOWN_MS) {
			pairing_forget();
		}
		clock_process();
		if (!resetting) {
			settings_flush();
			pairing_keep();
		}
	}
	return 0;
}
