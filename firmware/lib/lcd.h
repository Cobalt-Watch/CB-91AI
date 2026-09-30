/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CB91AI_LCD_H
#define CB91AI_LCD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Icons of the F-91W glass, named after what Casio shows with them (module 593
 * manual, confirmed on 2026-09-23). The Sensor Watch tables this
 * driver comes from call the sound-wave icon "signal" and the bell "bell", and
 * use them the other way round.
 */
enum lcd_indicator {
	LCD_INDICATOR_ALARM,	/* sound waves, top left: daily alarm on */
	LCD_INDICATOR_CHIME,	/* bell: hourly time signal on */
	LCD_INDICATOR_PM,
	LCD_INDICATOR_24H,
	LCD_INDICATOR_LAP,
	LCD_INDICATOR_COUNT,
};

/* Reset and configure the BU97930, clear the display, turn it on. */
int lcd_init(void);

/* All segments on through the driver test command (RAM untouched), or back
 * to the normal display of the RAM.
 */
int lcd_all_pixels(bool on);

/* All segments forced off (driver test command), until lcd_all_pixels(false). */
int lcd_blank(void);

/* Drive 3 COM lines (1/3 duty, the F-91W glass) or 4 (1/4 duty, diagnostic). */
int lcd_set_duty(uint8_t coms);

/* Normal drive or power save mode 1 (lower current, lower contrast). */
int lcd_set_low_power(bool low_power);

/* Bias current of the driver while the display is on (MODESET P1 P0). The
 * datasheet gives the relative consumption: x1.0, x1.7 and x2.7. High power
 * mode needs VLCD above 3 V and is not offered.
 */
enum lcd_drive_mode {
	LCD_DRIVE_POWER_SAVE1,
	LCD_DRIVE_POWER_SAVE2,
	LCD_DRIVE_NORMAL,
};
int lcd_set_drive_mode(enum lcd_drive_mode mode);

/* Drive waveform: line inversion (true) or frame inversion (false, the reset
 * state), MODESET P2. Both give the same RMS voltage on paper; the difference
 * shows on a glass driven below its voltage, VLCD being the rail on the V1.
 */
int lcd_set_line_inversion(bool line);

/* Frame frequency in 1/3 duty: 130, 86, 65 (the default) or 52 Hz. */
int lcd_set_frame_rate(unsigned int hz);

/*
 * Display on or off through MODESET. Off is the software standby of the
 * BU97930: oscillator and LCD bias supply stopped, every output at VSS, about
 * 3.5 uA instead of some tens (datasheet p.3 and p.11). The display RAM and the
 * settings are kept, and RAM writes are still accepted, so turning the display
 * back on shows the last lcd_flush(). INHb is tied to the 3V rail on the V1
 * board: this command is the only way to put the driver in standby.
 */
int lcd_display_power(bool on);
bool lcd_is_on(void);

/* Framebuffer edits; nothing reaches the glass before lcd_flush(). */
void lcd_clear(void);
void lcd_set_pixel(uint8_t com, uint8_t seg, bool on);	/* seg is the F-91W plot SEG0..SEG23 */
void lcd_fill_com(uint8_t com, bool on);		/* every SEG output of driver COM0..COM3 */
void lcd_display_character(char c, uint8_t position);	/* positions 0..9, see lcd.c */
void lcd_display_string(const char *s, uint8_t position);
void lcd_set_colon(bool on);
void lcd_describe_pixel(uint8_t com, uint8_t seg, char *buf, size_t len); /* "P5:B", "bell"... */
void lcd_set_indicator(enum lcd_indicator indicator, bool on);
int lcd_flush(void);

/* Reworked V1 (V2-31): glass COM2 plot bridged to the driver COM0 output */
int lcd_set_glass_com2(bool on_driver_com0);
bool lcd_glass_com2(void);

/* Diagnostic: raw display RAM bytes after ADSET 0 and RAMWR, at most LCD_RAW_MAX */
#define LCD_RAW_MAX 32
int lcd_write_raw(const uint8_t *data, size_t len, bool single_cs);

#endif /* CB91AI_LCD_H */
