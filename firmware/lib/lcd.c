/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Casio F-91W segment LCD driven by the ROHM BU97930 over 3-wire SPI (write
 * only). The driver holds a 27 x 4 bit display RAM: one nibble per SEG output,
 * sent MSB first with the driver COM0 bit first: nibble bit n is driver
 * COM(3 - n), checked on the V1 with raw fills (0x88 drives COM0, 0x44 COM1,
 * 0x22 COM2, 0x11 COM3).
 *
 * Glass wiring on the V1 board (established on 2026-09-16 with raw RAM writes
 * and the Sensor Watch schematic): the F-91W
 * module has its three COM plots at positions 7, 8 and 9, the V1 schematic
 * put COM0..COM2 on plots 6, 7 and 8. So glass COM0 is on driver COM1, glass
 * COM1 on driver COM2, glass COM2 on driver SEG2 (unusable, kept at 0), and
 * the glass SEG18 plot (hour tens A/D, E, F) hangs on driver COM0, which shows
 * a permanent grey bracket there. Sensor Watch SEG s sits on driver output
 * 23 - s for s < 16 and s - 16 for s >= 16 (s = 18 excluded).
 *
 * The segment map, the character set and the indicator positions come from
 * the Sensor Watch project, which drives the same glass (datasheet of the
 * driver: the ROHM BU97930MUV datasheet):
 * https://github.com/joeycastillo/Sensor-Watch (watch_private_display.h and
 * .c), MIT License, Copyright (c) 2020 Joey Castillo. Positions: 0 and 1
 * weekday, 2 and 3 day of month, 4 and 5 hours, 6 and 7 minutes, 8 and 9
 * seconds.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>

#include "lcd.h"

LOG_MODULE_REGISTER(cb91ai_lcd, LOG_LEVEL_INF);

static const struct spi_dt_spec lcd_spi =
	SPI_DT_SPEC_GET(DT_NODELABEL(lcd), SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB);

/* BU97930 commands: bit 7 set on a command byte, clear on its parameter byte */
#define BU97930_MODESET 0x81
#define BU97930_DISCTL  0x82
#define BU97930_ADSET   0x83
#define BU97930_BLKSET  0x84
#define BU97930_APOFF   0x90
#define BU97930_APON    0x91
#define BU97930_SWRST   0x92
#define BU97930_NORON   0x93 /* leave the APON/APOFF state, show the RAM */
#define BU97930_RAMWR   0xA0

#define MODESET_DISPLAY_ON  0x08
#define MODESET_LINE_INV    0x04 /* P2: line inversion; 0 = frame inversion, reset state */
#define MODESET_POWER_SAVE1 0x00
#define MODESET_POWER_SAVE2 0x01
#define MODESET_NORMAL      0x02
#define DISCTL_DUTY_1_4     0x00 /* P3 P2 = 0 0, reset state, COM0 to COM3 driven */
#define DISCTL_DUTY_1_3     0x04 /* P3 P2 = 0 1, COM0 to COM2 driven, COM3 = COM1 */
#define DISCTL_FRAME_64HZ   0x02 /* P1 P0 = 1 0: 65 Hz in 1/3 duty, reset state */

#define DRIVER_SEGS 27 /* SEG0 to SEG26 outputs, one nibble each */
#define GLASS_SEGS  24
#define GLASS_COMS  3
#define POSITIONS   10

/*
 * Framebuffer. Edited from the main loop (clock) and, during an update, from
 * the SMP thread (the "UPd" paint in main.c's reset hook). There is no lock:
 * this is safe only because every edit here is pure CPU with no k_ yield and
 * lcd_flush() snapshots fb[] into a local buffer before its first (serialized)
 * SPI transfer, so no partial frame is ever transmitted, and the "UPd" paint
 * runs after clock_enabled is cleared. A future multi-writer use must add a
 * mutex or route all drawing through one owner thread.
 */
static uint8_t fb[DRIVER_SEGS]; /* bit n = COMn of that SEG output */

/* Sensor Watch tables: one byte per segment A..G plus one extra, low byte first;
 * byte = (COM << 6) | SEG, COM 3 meaning no such segment at that position.
 */
static const uint64_t segment_map[POSITIONS] = {
	0x4e4f0e8e8f8d4d0d, /* 0: weekday */
	0xc8c4c4c8b4b4b0b,  /* 1: weekday (B and C shared, E and F shared) */
	0xc049c00a49890949, /* 2: day of month (A, D, G shared, no F) */
	0xc048088886874707, /* 3: day of month */
	0xc053921252139352, /* 4: hours (A and D shared) */
	0xc054511415559594, /* 5: hours */
	0xc057965616179716, /* 6: minutes (A and D shared) */
	0xc041804000018a81, /* 7: minutes */
	0xc043420203048382, /* 8: seconds */
	0xc045440506468584, /* 9: seconds */
};

/* Segments A..G in bits 0..6, extra segment in bit 7, for ASCII 0x20 to 0x7e */
static const uint8_t character_set[] = {
	0x00, 0x60, 0x22, 0x63, 0x2d, 0x00, 0x44, 0x20, 0x39, 0x0f, 0xc0, 0x70, 0x04, 0x40, 0x40, 0x12,
	0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f, 0x00, 0x00, 0x58, 0x48, 0x4c, 0x53,
	0xff, 0x77, 0x7f, 0x39, 0x3f, 0x79, 0x71, 0x3d, 0x76, 0x89, 0x0e, 0x75, 0x38, 0xb7, 0x37, 0x3f,
	0x73, 0x67, 0xf7, 0x6d, 0x81, 0x3e, 0x3e, 0xbe, 0x7e, 0x6e, 0x1b, 0x39, 0x24, 0x0f, 0x23, 0x08,
	0x02, 0x5f, 0x7c, 0x58, 0x5e, 0x7b, 0x71, 0x6f, 0x74, 0x10, 0x42, 0x75, 0x30, 0xb7, 0x54, 0x5c,
	0x73, 0x67, 0x50, 0x6d, 0x78, 0x62, 0x1c, 0xbe, 0x7e, 0x6e, 0x1b, 0x16, 0x36, 0x34, 0x01,
};

static const uint8_t indicator_com[LCD_INDICATOR_COUNT] = { 0, 0, 2, 2, 1 };
static const uint8_t indicator_seg[LCD_INDICATOR_COUNT] = { 17, 16, 17, 16, 10 };

static int lcd_write(const uint8_t *data, size_t len)
{
	const struct spi_buf buf = { .buf = (void *)data, .len = len };
	const struct spi_buf_set tx = { .buffers = &buf, .count = 1 };

	return spi_write_dt(&lcd_spi, &tx);
}

static int lcd_command(uint8_t command, uint8_t parameter)
{
	const uint8_t msg[2] = { command, parameter };

	return lcd_write(msg, sizeof(msg));
}

static int lcd_command_only(uint8_t command)
{
	return lcd_write(&command, 1);
}

#define SEG_NONE 0xff

/* Sensor Watch SEG number to driver output, SEG_NONE when unreachable on V1 */
static inline uint8_t driver_seg(uint8_t seg)
{
	if (seg == 18) {
		return SEG_NONE; /* plot 6, wired to driver COM0 on V1 */
	}
	return seg < 16 ? 23 - seg : seg - 16;
}

/* Framebuffer nibble (bit n = Sensor Watch COMn) to driver RAM nibble (bit n
 * = driver COM(3 - n)): glass COM0 is driver COM1 (bit 2), glass COM1 is
 * driver COM2 (bit 1); glass COM2 has no COM output on V1, and driver COM0
 * (bit 3) and COM3 (bit 0) are never set.
 */
static bool glass_com2_on_com0 = IS_ENABLED(CONFIG_CB91AI_LCD_GLASS_COM2_ON_COM0);

static inline uint8_t wire_nibble(uint8_t coms)
{
	uint8_t out = ((coms & BIT(0)) << 2) | (coms & BIT(1));

	if (glass_com2_on_com0) {
		out |= (coms & BIT(2)) << 1; /* reworked V1: glass COM2 on driver COM0 */
	}
	return out;
}

int lcd_set_glass_com2(bool on_driver_com0)
{
	glass_com2_on_com0 = on_driver_com0;
	return lcd_flush();
}

bool lcd_glass_com2(void)
{
	return glass_com2_on_com0;
}

int lcd_flush(void)
{
	uint8_t msg[1 + (DRIVER_SEGS + 1) / 2];
	int err;

	msg[0] = BU97930_RAMWR;
	for (size_t i = 0; i < (DRIVER_SEGS + 1) / 2; i++) {
		uint8_t hi = wire_nibble(fb[2 * i]);
		uint8_t lo = (2 * i + 1 < DRIVER_SEGS) ? wire_nibble(fb[2 * i + 1]) : 0;

		msg[1 + i] = (hi << 4) | lo;
	}
	err = lcd_command(BU97930_ADSET, 0x00);
	err = err ? err : lcd_write(msg, sizeof(msg));
	if (err) {
		LOG_ERR("RAM write failed (%d)", err);
	}
	return err;
}

/* Diagnostic: ADSET 0 then RAMWR followed by the given bytes as they are,
 * either in two chip-select windows (as lcd_flush) or in a single one.
 */
int lcd_write_raw(const uint8_t *data, size_t len, bool single_cs)
{
	uint8_t msg[3 + LCD_RAW_MAX];
	size_t n = 0;
	int err;

	if (len > LCD_RAW_MAX) {
		return -EINVAL;
	}
	if (single_cs) {
		msg[n++] = BU97930_ADSET;
		msg[n++] = 0x00;
	} else {
		err = lcd_command(BU97930_ADSET, 0x00);
		if (err) {
			return err;
		}
	}
	msg[n++] = BU97930_RAMWR;
	memcpy(&msg[n], data, len);
	n += len;
	return lcd_write(msg, n);
}

/* MODESET state: display on or off (P3), line or frame inversion (P2) and bias
 * current mode (P1 P0); DISCTL state: duty and frame frequency (datasheet p.11
 * and p.12) */
static bool display_on;
static uint8_t drive_mode = MODESET_NORMAL;
static uint8_t inversion;
static uint8_t frame_code = DISCTL_FRAME_64HZ;
static uint8_t duty_coms = 3;

static int lcd_apply_mode(void)
{
	return lcd_command(BU97930_MODESET,
			   (display_on ? MODESET_DISPLAY_ON : 0) | inversion | drive_mode);
}

int lcd_set_line_inversion(bool line)
{
	inversion = line ? MODESET_LINE_INV : 0;
	return lcd_apply_mode();
}

int lcd_set_drive_mode(enum lcd_drive_mode mode)
{
	switch (mode) {
	case LCD_DRIVE_POWER_SAVE1:
		drive_mode = MODESET_POWER_SAVE1;
		break;
	case LCD_DRIVE_POWER_SAVE2:
		drive_mode = MODESET_POWER_SAVE2;
		break;
	case LCD_DRIVE_NORMAL:
		drive_mode = MODESET_NORMAL;
		break;
	default:
		return -EINVAL;
	}
	return lcd_apply_mode();
}

int lcd_set_low_power(bool enable)
{
	return lcd_set_drive_mode(enable ? LCD_DRIVE_POWER_SAVE1 : LCD_DRIVE_NORMAL);
}

int lcd_display_power(bool on)
{
	display_on = on;
	return lcd_apply_mode();
}

bool lcd_is_on(void)
{
	return display_on;
}

/* APON forces every SEG output on for all the driven COM lines, whatever the
 * RAM holds; NORON (not APOFF, which forces everything off) returns to the RAM.
 */
int lcd_all_pixels(bool on)
{
	return lcd_command_only(on ? BU97930_APON : BU97930_NORON);
}

int lcd_blank(void)
{
	return lcd_command_only(BU97930_APOFF);
}

/* 3 = 1/3 duty as the glass wants (COM0 to COM2), 4 = 1/4 duty so that COM3
 * gets its own data too: a diagnostic to find a COM plot wired elsewhere. The
 * datasheet sequence turns the display off around the drive mode change.
 */
int lcd_set_duty(uint8_t coms)
{
	uint8_t duty = coms == 4 ? DISCTL_DUTY_1_4 : DISCTL_DUTY_1_3;
	int err;

	if (coms != 3 && coms != 4) {
		return -EINVAL;
	}
	duty_coms = coms;
	err = lcd_command(BU97930_MODESET, 0x00);
	err = err ? err : lcd_command(BU97930_DISCTL, duty | frame_code);
	err = err ? err : lcd_flush();
	return err ? err : lcd_apply_mode();
}

/* DISCTL P1 P0 in 1/3 duty (datasheet p.12, internal oscillator at 20.48 kHz):
 * 0 0 = 130 Hz, 0 1 = 86 Hz, 1 0 = 65 Hz, 1 1 = 52 Hz */
int lcd_set_frame_rate(unsigned int hz)
{
	switch (hz) {
	case 130:
		frame_code = 0x00;
		break;
	case 86:
		frame_code = 0x01;
		break;
	case 65:
		frame_code = 0x02;
		break;
	case 52:
		frame_code = 0x03;
		break;
	default:
		return -EINVAL;
	}
	return lcd_set_duty(duty_coms);
}

/* Every SEG output on one driver COM line (0 to 3), on or off, in the RAM */
void lcd_fill_com(uint8_t com, bool on)
{
	if (com >= 4) {
		return;
	}
	for (size_t i = 0; i < DRIVER_SEGS; i++) {
		if (on) {
			fb[i] |= BIT(com);
		} else {
			fb[i] &= ~BIT(com);
		}
	}
}

void lcd_clear(void)
{
	memset(fb, 0, sizeof(fb));
}

void lcd_set_pixel(uint8_t com, uint8_t seg, bool on)
{
	uint8_t out;

	if (com >= GLASS_COMS || seg >= GLASS_SEGS) {
		return;
	}
	out = driver_seg(seg);
	if (out == SEG_NONE) {
		return;
	}
	if (on) {
		fb[out] |= BIT(com);
	} else {
		fb[out] &= ~BIT(com);
	}
}

void lcd_display_character(char c, uint8_t position)
{
	if (position >= POSITIONS) {
		return;
	}
	/* Glyph substitutions from Sensor Watch, for the segments shared or
	 * missing at some positions.
	 */
	if (position == 4 || position == 6) {
		if (c == '7') c = '&';
		else if (c == 'A') c = 'a';
		else if (c == 'o') c = 'O';
		else if (c == 'L') c = '!';
		else if (c == 'M' || c == 'm' || c == 'N') c = 'n';
		else if (c == 'c') c = 'C';
		else if (c == 'J') c = 'j';
		else if (c == 't' || c == 'T') c = '+';
		else if (c == 'y' || c == 'Y') c = '4';
		else if (c == 'v' || c == 'V' || c == 'U' || c == 'W' || c == 'w') c = 'u';
	} else {
		if (c == 'u') c = 'v';
		else if (c == 'j') c = 'J';
	}
	if (position > 1 && c == 'T') {
		c = 't';
	}
	/* The middle bar of an I is the extra segment of the weekday characters:
	 * elsewhere the I is the left bar alone, as an l (a bare top and bottom
	 * bar read "=") */
	if (position > 1 && c == 'I') {
		c = 'l';
	}
	if (position == 1) {
		if (c == 'a') c = 'A';
		else if (c == 'o') c = 'O';
		else if (c == 'i') c = 'l';
		else if (c == 'n') c = 'N';
		else if (c == 'r') c = 'R';
		else if (c == 'd') c = 'D';
		else if (c == 'v' || c == 'V' || c == 'u') c = 'U';
		else if (c == 'b') c = 'B';
		else if (c == 'c') c = 'C';
	} else if (c == 'R') {
		c = 'r';
	}
	if (c < 0x20 || c > 0x7e) {
		c = ' ';
	}

	uint64_t map = segment_map[position];
	uint8_t data = character_set[c - 0x20];

	for (int i = 0; i < 8; i++) {
		uint8_t com = (map & 0xff) >> 6;
		uint8_t seg = map & 0x3f;

		if (com < GLASS_COMS) {
			lcd_set_pixel(com, seg, data & 1);
		}
		map >>= 8;
		data >>= 1;
	}
}

void lcd_display_string(const char *s, uint8_t position)
{
	for (size_t i = 0; s[i] != 0 && position + i < POSITIONS; i++) {
		lcd_display_character(s[i], position + i);
	}
}

void lcd_set_colon(bool on)
{
	lcd_set_pixel(1, 16, on);
}

void lcd_set_indicator(enum lcd_indicator indicator, bool on)
{
	if (indicator < LCD_INDICATOR_COUNT) {
		lcd_set_pixel(indicator_com[indicator], indicator_seg[indicator], on);
	}
}

static void append(char *buf, size_t len, size_t *used, const char *text)
{
	if (*used < len) {
		int n = snprintf(buf + *used, len - *used, "%s%s", *used ? " " : "", text);

		*used += n > 0 ? n : 0;
	}
}

/* Names the glass element(s) at (com, F-91W seg) per the Sensor Watch tables:
 * "P5:B" (position 5, segment B), "P4:A P4:D" when shared, "bell", "colon"
 */
void lcd_describe_pixel(uint8_t com, uint8_t seg, char *buf, size_t len)
{
	static const char *const indicator_names[LCD_INDICATOR_COUNT] = {
		"alarm (waves)", "chime (bell)", "PM", "24H", "LAP"
	};
	size_t used = 0;
	char item[8];

	buf[0] = 0;
	for (int pos = 0; pos < POSITIONS; pos++) {
		uint64_t map = segment_map[pos];

		for (int i = 0; i < 8; i++, map >>= 8) {
			if ((map & 0xff) >> 6 == com && (map & 0x3f) == seg) {
				snprintf(item, sizeof(item), "P%d:%c", pos, i < 7 ? 'A' + i : 'X');
				append(buf, len, &used, item);
			}
		}
	}
	for (int i = 0; i < LCD_INDICATOR_COUNT; i++) {
		if (indicator_com[i] == com && indicator_seg[i] == seg) {
			append(buf, len, &used, indicator_names[i]);
		}
	}
	if (com == 1 && seg == 16) {
		append(buf, len, &used, "colon");
	}
	if (used == 0) {
		append(buf, len, &used, "nothing in the map");
	}
}

int lcd_init(void)
{
	int err;

	if (!spi_is_ready_dt(&lcd_spi)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}
	/* Datasheet initialise sequence: reset, display off, setup, RAM, display on */
	k_msleep(2);
	err = lcd_command_only(BU97930_SWRST);
	if (err) {
		LOG_ERR("SPI write failed (%d)", err);
		return err;
	}
	k_msleep(1);
	err = lcd_command(BU97930_MODESET, 0x00);
	duty_coms = 3;
	err = err ? err : lcd_command(BU97930_DISCTL, DISCTL_DUTY_1_3 | frame_code);
	err = err ? err : lcd_command(BU97930_BLKSET, 0x00);
	if (err) {
		return err;
	}
	lcd_clear();
	err = lcd_flush();
	if (err) {
		return err;
	}
	drive_mode = MODESET_NORMAL;
	return lcd_display_power(true);
}
