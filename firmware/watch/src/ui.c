/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * What the buttons do and what the glass shows, see ui.h. Pure C, no Zephyr:
 * the host tests build this file as it is.
 *
 * Glyphs are chosen for what the F-91W glass can draw:
 * the hour tens of the V1 draws "1" or nothing, the minute tens ties its top
 * and bottom bars, the second weekday character ties B with C and E with F,
 * and the tens of the day of month has no upper left bar: it draws 1, 2 and 3,
 * no letter. So a label takes the two weekday characters and the units of the
 * day, a blank before its third letter ("bL E"), and numbers go where the
 * digits of the time go.
 */

#include <string.h>

#include "ui.h"

#define BUTTON_BIT(b) ((uint8_t)(1U << (b)))
#define RESET_CHORD   (BUTTON_BIT(UI_MODE) | BUTTON_BIT(UI_ALARM))

static const char *const weekdays[7] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };

void ui_init(struct ui *ui)
{
	memset(ui, 0, sizeof(*ui));
}

/* ---- Gestures ------------------------------------------------------------ */

static uint32_t chord_start(struct ui *ui, uint8_t mask, uint32_t spread_ms)
{
	uint32_t fx = UI_FX_DISPLAY;

	/* A chord with ALARM ends what its presses began: a take that runs on by
	 * itself is kept, as by a press; one that has not shown "rEC" yet is
	 * dropped. Without ALARM, a take that runs on goes on. */
	if (mask & BUTTON_BIT(UI_ALARM)) {
		if (ui->recording) {
			fx |= UI_FX_TAKE_STOP;
		} else if (ui->taking) {
			fx |= UI_FX_TAKE_DROP;
		}
		ui->taking = false;
		ui->recording = false;
	} else if (ui->taking && !ui->recording) {
		/* A chord of the other buttons right after a tap of ALARM: the
		 * gesture machine drops that tap's click, which would have dropped
		 * the take; it is dropped here, or it would run unseen */
		fx |= UI_FX_TAKE_DROP;
		ui->taking = false;
	}
	ui->alarm_woke = false;
	ui->consumed = 0;
	/* The reset only for MODE and ALARM pressed at once, as one does on
	 * purpose: a MODE really stuck makes a chord with any press of ALARM, and
	 * a take dictated with ALARM held would reset the watch (26/09) */
	if (mask == RESET_CHORD && spread_ms <= UI_RESET_SPREAD_MS) {
		ui->screen = UI_SCREEN_TIME;
		ui->chord = true;
		ui->chord_ms = 0;
		ui->reset_sent = false;
	}
	return fx;
}

static uint32_t press(struct ui *ui, uint8_t button, const struct ui_context *ctx)
{
	if (button == UI_LIGHT) {
		/* The light at once, dark glass or not (PO-03) */
		return UI_FX_LIGHT_ON | UI_FX_DISPLAY;
	}
	if (button == UI_ALARM) {
		if (ui->recording) {
			/* A take that runs on ends at a press: the note is kept. The
			 * click or the long press that follows is this press's. */
			ui->recording = false;
			ui->taking = false;
			ui->consumed |= BUTTON_BIT(UI_ALARM);
			return UI_FX_TAKE_STOP | UI_FX_DISPLAY;
		}
		if (ui->ended_alone) {
			/* Meant to end a take that ended by itself: nothing */
			ui->ended_alone = false;
			ui->consumed |= BUTTON_BIT(UI_ALARM);
			return UI_FX_DISPLAY;
		}
		if (ui->taking) {
			/* A press of the same series (a double click): the take of its
			 * first press goes on, and the click or the long press decides */
			return UI_FX_DISPLAY;
		}
		/* Record then cancel: the take starts now, dark glass or not. A
		 * press spent just before (its click still to come, less than
		 * 300 ms ago) does not hold this one's click or long press. */
		ui->consumed &= (uint8_t)~BUTTON_BIT(UI_ALARM);
		ui->alarm_woke = !ctx->display_on;
		ui->taking = true;
		ui->take_at = ctx->uptime_ms;
		return UI_FX_TAKE_START | UI_FX_DISPLAY;
	}
	/* MODE: the next screen, once the glass is lit; "rEC" stays meanwhile */
	if (ctx->display_on && !ui->recording) {
		ui->screen = (uint8_t)((ui->screen + 1U) % UI_SCREENS);
	}
	return UI_FX_DISPLAY;
}

uint32_t ui_gesture(struct ui *ui, const struct gesture *g, const struct ui_context *ctx)
{
	const uint8_t bit = BUTTON_BIT(g->button);
	uint32_t fx = 0;

	if (ui->resetting) {
		/* Nothing more until the restart, but the loop must know when the
		 * buttons are let go */
		if (g->type == GESTURE_CHORD_END) {
			ui->chord = false;
		}
		return 0;
	}
	switch (g->type) {
	case GESTURE_CHORD:
		return chord_start(ui, g->button, g->ms);
	case GESTURE_CHORD_HOLD:
		if (!ui->chord) {
			return 0;
		}
		ui->chord_ms = g->ms;
		if (g->ms >= UI_RESET_HOLD_MS && !ui->reset_sent) {
			ui->reset_sent = true;
			fx |= UI_FX_RESET;
		}
		return fx | UI_FX_DISPLAY;
	case GESTURE_CHORD_END:
		if (!ui->chord) {
			return 0;
		}
		ui->chord = false;
		return UI_FX_DISPLAY;
	default:
		break;
	}
	if (g->button > UI_ALARM) {
		return 0;
	}
	if (g->type == GESTURE_PRESS) {
		return press(ui, g->button, ctx);
	}
	/* A click, a long press or its end, of a press already used: nothing */
	if (ui->consumed & bit) {
		if (g->type != GESTURE_LONG) {
			ui->consumed &= (uint8_t)~bit;
		}
		return 0;
	}
	switch (g->type) {
	case GESTURE_CLICK:
		if (g->button != UI_ALARM) {
			return 0; /* MODE and LIGHT acted on the press */
		}
		if (ui->taking) {
			fx |= UI_FX_TAKE_DROP;
			ui->taking = false;
		}
		/* EF-11: a single click syncs by hand, unless it only lit the glass */
		if (g->count == 1 && !ui->alarm_woke) {
			fx |= UI_FX_SYNC;
		}
		ui->alarm_woke = false;
		return fx;
	case GESTURE_LONG:
		if (g->button == UI_LIGHT) {
			return UI_FX_LIGHT_HOLD;
		}
		if (g->button == UI_ALARM) {
			ui->alarm_woke = false;
			if (ui->taking) {
				ui->recording = true; /* EF-10, latched: runs on after the release */
				return UI_FX_TAKE_KEEP | UI_FX_DISPLAY;
			}
		}
		return 0; /* MODE held: nothing, the settings are the phone's */
	case GESTURE_LONG_END:
		/* ALARM released: the take runs on, a press ends it */
		if (g->button == UI_LIGHT) {
			return UI_FX_LIGHT_OFF;
		}
		return g->button == UI_ALARM && ui->recording ? UI_FX_TAKE_LISTEN : 0U;
	default:
		return 0;
	}
}

void ui_display_off(struct ui *ui)
{
	ui->screen = UI_SCREEN_TIME;
	ui->alarm_woke = false;
	ui->ended_alone = false;
}

void ui_take_ended(struct ui *ui)
{
	/* Only a take that ran on by itself ("rEC") awaited a press to end it; one
	 * refused before (the flash full) leaves the next press free */
	ui->ended_alone = ui->recording;
	ui->taking = false;
	ui->recording = false;
}

void ui_pairing(struct ui *ui, bool on, uint32_t passkey)
{
	ui->pairing = on;
	ui->passkey = passkey % 100000U;
}

void ui_resetting(struct ui *ui)
{
	ui->resetting = true;
}

uint32_t ui_display_ms(const struct ui *ui, uint32_t normal_ms)
{
	return ui->recording || ui->chord || ui->pairing || ui->resetting ? 0U : normal_ms;
}

uint32_t ui_tick_ms(const struct ui *ui)
{
	return ui->resetting ? UI_BLINK_MS : 1000U;
}

int16_t ui_temp_follow(int16_t shown, bool known, int32_t cc)
{
	const int32_t whole = cc >= 0 ? (cc + 50) / 100 : -((-cc + 50) / 100);
	const int32_t gap = cc - (int32_t)shown * 100;

	if (!known || gap > UI_TEMP_MARGIN_CC || gap < -UI_TEMP_MARGIN_CC) {
		return (int16_t)whole;
	}
	return shown;
}

/* ---- The glass ------------------------------------------------------------- */

static void put(struct ui_frame *f, unsigned int pos, const char *s)
{
	for (unsigned int i = 0; s[i] != '\0' && pos + i < 10U; i++) {
		f->text[pos + i] = s[i];
	}
}

/* Two digits; the tens left blank when zero, unless `zero` */
static void two(struct ui_frame *f, unsigned int pos, unsigned int v, bool zero)
{
	v %= 100U;
	f->text[pos] = (v >= 10U || zero) ? (char)('0' + v / 10U) : ' ';
	f->text[pos + 1U] = (char)('0' + v % 10U);
}

/* A label of three letters on the top row: the tens of the day draws no
 * letter, so the third one goes to its units ("bL E") */
static void label(struct ui_frame *f, const char *first_two, char third)
{
	put(f, 0, first_two);
	f->text[3] = third;
}

static void top_row(struct ui_frame *f, const struct cal_time *t)
{
	put(f, 0, weekdays[t->weekday % 7U]);
	two(f, 2, t->day, false);
}

/* Hours without a leading zero (the hour tens of the V1 draws "1" alone), in
 * 12 h with PM, or in 24 h */
static void time_row(struct ui_frame *f, const struct cal_time *t, bool h24)
{
	unsigned int hour = t->hour;

	if (!h24) {
		f->pm = hour >= 12U;
		hour %= 12U;
		if (hour == 0U) {
			hour = 12U;
		}
	}
	f->h24 = h24;
	f->colon = true;
	two(f, 4, hour, false);
	two(f, 6, t->minute, true);
	two(f, 8, t->second, true);
}

void ui_render(const struct ui *ui, const struct ui_context *ctx, struct ui_frame *f)
{
	struct cal_time t;

	memset(f, 0, sizeof(*f));
	memset(f->text, ' ', 10);
	if (ui->pairing) {
		/* "PA" and the five digits of the code, zeros included */
		put(f, 0, "PA");
		f->text[5] = (char)('0' + ui->passkey / 10000U);
		two(f, 6, ui->passkey / 100U % 100U, true);
		two(f, 8, ui->passkey % 100U, true);
		return;
	}
	if (ui->resetting) {
		/* The reset runs: the word alone, blinking */
		if ((ctx->uptime_ms / UI_BLINK_MS) % 2U == 0U) {
			put(f, 5, "rESEt");
		}
		return;
	}
	if (ui->chord) {
		/* The reset: the seconds left, from 10 */
		const uint32_t left = ui->chord_ms >= UI_RESET_HOLD_MS
					      ? 0U
					      : (UI_RESET_HOLD_MS - ui->chord_ms + 999U) / 1000U;

		two(f, 2, left, false);
		put(f, 5, "rESEt");
		return;
	}
	if (ui->recording) {
		/* "rEC" and how long the take has run, m:ss (EF-24), 9:59 at most */
		const uint32_t run_s = (ctx->uptime_ms - ui->take_at) / 1000U;
		const uint32_t s = run_s > 599U ? 599U : run_s;

		label(f, "rE", 'C');
		f->text[5] = (char)('0' + s / 60U);
		two(f, 6, s % 60U, true);
		f->colon = true;
		return;
	}
	switch (ui->screen) {
	case UI_SCREEN_LINK:
		label(f, "bL", 'E');
		two(f, 4, ctx->notes > 19U ? 19U : ctx->notes, false);
		put(f, 7, ctx->link == UI_LINK_CONNECTED     ? "Con"
			  : ctx->link == UI_LINK_ADVERTISING ? "Adv"
							       : "OFF");
		return;
	case UI_SCREEN_TEMP:
		/* The case, whole degrees C: the thermometer of the nRF52840, +/-5
		 * degC, warmed by the wrist when worn. The unit on the seconds,
		 * degree sign first (2026-09-25): '#' draws it, the upper
		 * square (segments A, B, F and G in the character set of the glass) */
		label(f, "tE", 'P');
		put(f, 8, "#C");
		if (!ctx->temp_known) {
			put(f, 6, "--");
		} else {
			const int32_t whole = ctx->temp_c;
			const uint32_t a = (uint32_t)(whole < 0 ? -whole : whole);

			two(f, 6, a > 99U ? 99U : a, false);
			if (whole < 0) {
				f->text[a >= 10U ? 5 : 6] = '-';
			}
		}
		return;
	case UI_SCREEN_BATTERY:
		label(f, "bA", 't');
		if (ctx->battery_mv == 0U) {
			put(f, 6, "----");
		} else {
			two(f, 6, ctx->battery_mv / 100U, false);
			two(f, 8, ctx->battery_mv % 100U, true);
		}
		return;
	default:
		break;
	}
	if (!ctx->time_set) {
		put(f, 5, "-----");
		return;
	}
	cal_from_ms(ctx->local_ms, &t);
	top_row(f, &t);
	time_row(f, &t, ctx->h24);
}
