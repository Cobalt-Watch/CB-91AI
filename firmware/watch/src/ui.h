/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * What the buttons do and what the glass shows (lots D3 and D6): screens
 * (EF-13), the light (EF-15), the voice (E1) and the reset (phase S) that the
 * gestures serve. Pure logic, like the gesture machine under it: it takes the
 * gestures and what the watch knows, and gives back effects for the event loop
 * and a frame for the glass. The host tests drive it
 * (firmware/tests/host/test_ui.c).
 *
 * The choices of 2026-09-24: nothing is
 * set on the watch any more, the phone sets the time at every session and the
 * format with the other settings; MODE walks the screens; LIGHT lights the LED,
 * in the colour or the animation chosen in the app (PO-03); a take held past
 * half a second shows "rEC" and runs on by itself until a press of ALARM, or
 * its longest length.
 *
 * A press that lights a dark glass only lights it: the next press acts. LIGHT
 * and ALARM act at once all the same, and long presses act whatever the glass.
 */

#ifndef CB91AI_WATCH_UI_H
#define CB91AI_WATCH_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "calendar.h"
#include "gesture.h"

/* Buttons, as numbered in the gestures (buttons.h) */
#define UI_LIGHT 0
#define UI_MODE  1
#define UI_ALARM 2

/* The screens MODE walks; the date is no longer one of them
 * (2026-09-25): the date shows on the time screen, day of the week and day */
enum ui_screen {
	UI_SCREEN_TIME,
	UI_SCREEN_LINK,    /* notes waiting, and the radio */
	UI_SCREEN_BATTERY, /* the cell, in mV */
	UI_SCREEN_TEMP,    /* the case, in whole degrees C (2026-09-25) */
	UI_SCREENS,
};

enum ui_link {
	UI_LINK_OFF,
	UI_LINK_ADVERTISING,
	UI_LINK_CONNECTED,
};

/* What the watch knows, handed to every call */
struct ui_context {
	uint32_t uptime_ms;
	bool time_set;
	int64_t local_ms;   /* local time, ms since 1970, when time_set */
	bool h24;
	uint8_t notes;      /* waiting to be sent */
	uint8_t link;       /* enum ui_link */
	uint16_t battery_mv; /* 0: not known */
	int16_t temp_c;      /* the case, the whole degree C to show (ui_temp_follow) ... */
	bool temp_known;     /* ... read not long ago */
	bool display_on;    /* the glass is lit */
};

/* The whole degree the "tE P" screen shows after a reading in hundredths of a
 * degree: it moves only once the reading is more than UI_TEMP_MARGIN_CC away
 * from it (a whole degree, with the 0.25 degC steps of the nRF52840), so that
 * a case sitting on a half degree does not flip between two values, the
 * sensor a quarter either way (2026-09-25); `known` false (no earlier
 * reading, or an old one): the reading rounded, half away from zero. */
#define UI_TEMP_MARGIN_CC 75
int16_t ui_temp_follow(int16_t shown, bool known, int32_t cc);

/* Effects, bits of the value that ui_gesture() returns */
#define UI_FX_DISPLAY    (1U << 0) /* light the glass, or keep it lit, and redraw */
#define UI_FX_SYNC       (1U << 1) /* advertise now: a sync by hand (EF-11) */
#define UI_FX_TAKE_START (1U << 2) /* start a take on the press (record then cancel, EF-10) */
#define UI_FX_TAKE_KEEP  (1U << 3) /* the press is long: "rEC", keep the take, it runs on */
#define UI_FX_TAKE_STOP  (1U << 4) /* a press ended the take: close it, the note is kept */
#define UI_FX_TAKE_DROP  (1U << 5) /* a click or a chord after all: drop the take */
#define UI_FX_RESET      (1U << 6) /* the two bottom buttons held 10 s (phase S) */
#define UI_FX_LIGHT_ON   (1U << 7) /* LIGHT pressed: the light, 1.5 s at least (EF-15) */
#define UI_FX_LIGHT_HOLD (1U << 8) /* LIGHT held: the light stays until it is released */
#define UI_FX_LIGHT_OFF  (1U << 9) /* LIGHT released after a long press */
#define UI_FX_TAKE_LISTEN (1U << 10) /* ALARM released after "rEC": the click of the
                                      * release is behind, the voice now counts */

#define UI_RESET_HOLD_MS 10000U
/* ... MODE and ALARM pressed within this of each other (the gesture's spread) */
#define UI_RESET_SPREAD_MS 2000U
/* "rESEt" blinks at this pace once the reset runs */
#define UI_BLINK_MS      500U

struct ui {
	uint8_t screen;
	uint8_t consumed;  /* buttons whose press was used: their click or long press is not */
	bool alarm_woke;   /* the press of ALARM lit the glass: its click only wakes */
	bool taking;       /* a take began on a press of ALARM */
	bool recording;    /* ... the press went long: "rEC" until a press or the end */
	bool ended_alone;  /* the take ended by itself: the next press of ALARM is spent */
	uint32_t take_at;  /* uptime of that press, for the counter */
	bool chord;        /* the reset chord is held */
	uint32_t chord_ms;
	bool reset_sent;
	bool resetting;     /* the reset runs: "rESEt" blinks until the restart */
	bool pairing;      /* a phone pairs: its code on the glass (lot S1) */
	uint32_t passkey;
};

void ui_init(struct ui *ui);

/* A gesture of the buttons: returns UI_FX_* */
uint32_t ui_gesture(struct ui *ui, const struct gesture *g, const struct ui_context *ctx);

/* The glass went dark after its time: back to the time screen next time */
void ui_display_off(struct ui *ui);

/* The take ended without the button: longest length, flash full, silence (E1),
 * or it could not begin. If it ran on ("rEC"), the press of ALARM that comes
 * next, while the glass is lit, meant to end it: it does nothing, rather than
 * start a take and sync on its click. */
void ui_take_ended(struct ui *ui);

/* A phone pairs (lot S1): "PA" and the code to type on it, five digits, the
 * hour tens of the V1 drawing no 0 (the phone takes a 0 before them); until
 * the pairing ends. Chosen on 2026-09-24. */
void ui_pairing(struct ui *ui, bool on, uint32_t passkey);

/* The reset runs (lot S3): "rESEt" blinks, no counter, until the watch
 * restarts, and the gestures do nothing meanwhile; the chord still ends when
 * the buttons are let go (2026-09-25: the counter no longer starts
 * again). */
void ui_resetting(struct ui *ui);

/* How long the glass stays lit after the last gesture (0: until further
 * notice, while a take, the reset chord or a pairing runs), and how often to
 * redraw it */
uint32_t ui_display_ms(const struct ui *ui, uint32_t normal_ms);
uint32_t ui_tick_ms(const struct ui *ui);

/* What the glass shows: ten characters for its positions (0 and 1 weekday, 2
 * and 3 day of month, then hours, minutes and seconds), ' ' for blank */
struct ui_frame {
	char text[11];
	bool colon;
	bool pm;
	bool h24;
};

void ui_render(const struct ui *ui, const struct ui_context *ctx, struct ui_frame *frame);

#endif /* CB91AI_WATCH_UI_H */
