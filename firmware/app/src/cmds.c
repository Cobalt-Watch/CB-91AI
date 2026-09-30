/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shell commands to exercise the peripherals on demand, from the RTT shell,
 * the USB console (usb_console.py --cmd "cb91ai mic") or, on a board sealed in
 * its watch, over Bluetooth (ble_shell.py "mic", see remote.c). The checks
 * written for the sealed watch are in hwtest.c.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/shell/shell.h>

#include "accel.h"
#include "ble.h"
#include "clock.h"
#include "codec_bench.h"
#include "hwtest.h"
#include "lcd.h"
#include "lcdwalk.h"
#include "mic.h"

void selftest_clock_enable(bool enable);
void selftest_heartbeat_enable(bool enable);
void selftest_display_hold(bool hold);
bool selftest_display_held(void);

/* RGB LED through PWM (common anode, inverted polarity in the devicetree) */
static const struct pwm_dt_spec pwm_leds[] = {
	PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led_red)),
	PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led_green)),
	PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led_blue)),
};

static int cmd_mic(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (argc >= 2 && strcmp(argv[1], "gain") == 0) {
		if (argc >= 3 && mic_set_gain_db((int)strtol(argv[2], NULL, 10))) {
			shell_print(sh, "gain refused: -20 to 20 dB");
			return -EINVAL;
		}
		shell_print(sh, "microphone gain %+d dB around the PDM default", mic_gain_db());
		return 0;
	}
	ret = mic_test();

	shell_print(sh, "mic test %s (%d), levels are in the log", ret ? "failed" : "done", ret);
	return ret;
}

static int cmd_accel(const struct shell *sh, size_t argc, char **argv)
{
	int32_t mg[3], temp_dc;
	int ret;

	if (argc >= 2 && strcmp(argv[1], "int") == 0) {
		return hwtest_accel_int(sh);
	}
	ret = accel_read(mg, &temp_dc);

	if (ret) {
		shell_print(sh, "accelerometer read failed (%d)", ret);
		return ret;
	}
	shell_print(sh, "x %d y %d z %d mg, temp %d.%d degC", mg[0], mg[1], mg[2], temp_dc / 10,
		    abs(temp_dc % 10));
	return 0;
}

/*
 * Drive settings to compare by eye on the coin cell, VLCD being the rail on the
 * V1 (V2-25, V2-42): all within the datasheet, the first one the default. The
 * high power mode is left out: the datasheet wants VLCD above 3 V for it.
 */
static const struct {
	enum lcd_drive_mode mode;
	bool line_inversion;
	uint16_t frame_hz;
	const char *what;
} lcd_tries[] = {
	{ LCD_DRIVE_NORMAL, false, 65, "normal mode, frame inversion, 65 Hz (the default)" },
	{ LCD_DRIVE_NORMAL, true, 65, "normal mode, line inversion, 65 Hz" },
	{ LCD_DRIVE_NORMAL, false, 52, "normal mode, frame inversion, 52 Hz" },
	{ LCD_DRIVE_NORMAL, false, 86, "normal mode, frame inversion, 86 Hz" },
	{ LCD_DRIVE_NORMAL, false, 130, "normal mode, frame inversion, 130 Hz" },
	{ LCD_DRIVE_POWER_SAVE2, false, 65, "power save mode 2, frame inversion, 65 Hz" },
	{ LCD_DRIVE_POWER_SAVE1, false, 65, "power save mode 1, frame inversion, 65 Hz" },
};
static int lcd_try_at = -1;

/*
 * Apply one setting and show it: "C" top left, its number on the units of the
 * day of month, then 12:34:56 with the chime and PM icons. The number sits on a
 * full digit: the second weekday character shares its verticals, B with C and
 * E with F, and turns a 2 into three bars or a 4 into an H (2026-09-23).
 */
static int lcd_try(const struct shell *sh, int index)
{
	char number[2] = { (char)('1' + index), '\0' };
	int ret = lcd_set_drive_mode(lcd_tries[index].mode);

	ret = ret ? ret : lcd_set_line_inversion(lcd_tries[index].line_inversion);
	ret = ret ? ret : lcd_set_frame_rate(lcd_tries[index].frame_hz);
	lcd_try_at = index;
	selftest_clock_enable(false);
	selftest_display_hold(true);
	lcd_all_pixels(false);
	lcd_clear();
	lcd_display_string("C", 0);
	lcd_display_string(number, 3);
	lcd_display_string("123456", 4);
	lcd_set_colon(true);
	lcd_set_indicator(LCD_INDICATOR_CHIME, true);
	lcd_set_indicator(LCD_INDICATOR_PM, true);
	ret = ret ? ret : lcd_flush();
	shell_print(sh, "C%d: %s (%d)", index + 1, lcd_tries[index].what, ret);
	return ret;
}

static int cmd_lcd(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "usage: cb91ai lcd <text up to 10 chars> | all on | all off | full | "
			    "blank | clock | hold on|off | mode save1|save2|normal | "
			    "frame 130|86|65|52 | inversion line|frame | try <1-7>|next|off | "
			    "duty <3|4> | com <0-3> | com2 on|off | raw [one] <hex>[*n]...");
		return -EINVAL;
	}
	if (strcmp(argv[1], "hold") == 0 && argc >= 3) {
		/* on: display always lit; off: on demand (30 s at boot, 10 s per press) */
		selftest_display_hold(strcmp(argv[2], "on") == 0);
		shell_print(sh, "display %s", selftest_display_held() ? "held on" : "on demand");
		return 0;
	}
	if (strcmp(argv[1], "mode") == 0 && argc >= 3) {
		/* Bias current of the driver, to judge the contrast by eye (EV-05) */
		enum lcd_drive_mode mode = strcmp(argv[2], "save1") == 0 ? LCD_DRIVE_POWER_SAVE1 :
					   strcmp(argv[2], "save2") == 0 ? LCD_DRIVE_POWER_SAVE2 :
									   LCD_DRIVE_NORMAL;
		int ret = lcd_set_drive_mode(mode);

		shell_print(sh, "lcd drive mode %s (%d): x1.0 save1, x1.7 save2, x2.7 normal",
			    argv[2], ret);
		return ret;
	}
	if (strcmp(argv[1], "frame") == 0 && argc >= 3) {
		int ret = lcd_set_frame_rate((unsigned int)atoi(argv[2]));

		shell_print(sh, "lcd frame frequency %s Hz (%d): 130, 86, 65 or 52", argv[2], ret);
		return ret;
	}
	if (strcmp(argv[1], "inversion") == 0 && argc >= 3) {
		int ret = lcd_set_line_inversion(strcmp(argv[2], "line") == 0);

		shell_print(sh, "lcd %s inversion (%d)", strcmp(argv[2], "line") == 0 ? "line" : "frame",
			    ret);
		return ret;
	}
	if (strcmp(argv[1], "try") == 0 && argc >= 3) {
		const int count = ARRAY_SIZE(lcd_tries);

		if (strcmp(argv[2], "off") == 0) {
			/* Back to the default drive and to the clock */
			int ret = lcd_set_drive_mode(LCD_DRIVE_NORMAL);

			ret = ret ? ret : lcd_set_line_inversion(false);
			ret = ret ? ret : lcd_set_frame_rate(65);
			lcd_try_at = -1;
			lcd_all_pixels(false);
			selftest_display_hold(false);
			selftest_clock_enable(true);
			shell_print(sh, "lcd back to C1 and to the clock (%d)", ret);
			return ret;
		}
		if (strcmp(argv[2], "next") == 0) {
			return lcd_try(sh, (lcd_try_at + 1) % count);
		}
		if (atoi(argv[2]) >= 1 && atoi(argv[2]) <= count) {
			return lcd_try(sh, atoi(argv[2]) - 1);
		}
		shell_print(sh, "usage: cb91ai lcd try <1-%d> | next | off", count);
		return -EINVAL;
	}
	if (strcmp(argv[1], "clock") == 0) {
		/* Back to the normal behaviour: uptime clock, display on demand */
		lcd_all_pixels(false);
		selftest_display_hold(false);
		selftest_clock_enable(true);
		return 0;
	}
	/* Every other lcd command is a visual diagnostic: keep the display lit
	 * until `cb91ai lcd clock` (or `lcd hold off`).
	 */
	selftest_display_hold(true);
	if (strcmp(argv[1], "all") == 0 && argc >= 3) {
		return lcd_all_pixels(strcmp(argv[2], "on") == 0);
	}
	if (strcmp(argv[1], "blank") == 0) {
		return lcd_blank();
	}
	if (strcmp(argv[1], "full") == 0) {
		/* Every element through the framebuffer and the glass map of lcd.c,
		 * unlike `all on` (driver test command, RAM untouched): an 8 at each
		 * of the ten positions, the colon and the five icons. The hour tens
		 * (position 4) only draws its "1" on the V1 (SEG18, V2-31). */
		selftest_clock_enable(false);
		lcd_all_pixels(false);
		lcd_clear();
		lcd_display_string("8888888888", 0);
		lcd_set_colon(true);
		for (int i = 0; i < LCD_INDICATOR_COUNT; i++) {
			lcd_set_indicator((enum lcd_indicator)i, true);
		}
		return lcd_flush();
	}
	if (strcmp(argv[1], "duty") == 0 && argc >= 3) {
		int ret = lcd_set_duty(atoi(argv[2]));

		shell_print(sh, "lcd duty 1/%s %s (%d)", argv[2], ret ? "failed" : "set", ret);
		return ret;
	}
	if (strcmp(argv[1], "com2") == 0 && argc >= 3) {
		int ret = lcd_set_glass_com2(strcmp(argv[2], "on") == 0);

		shell_print(sh, "glass COM2 on driver COM0: %s (%d)", lcd_glass_com2() ? "on" : "off", ret);
		return ret;
	}
	if (strcmp(argv[1], "raw") == 0 && argc >= 3) {
		/* raw [one] <hex byte>[*count] ...: RAM bytes as given, "one" = single CS window */
		uint8_t data[LCD_RAW_MAX];
		size_t n = 0;
		int i = 2;
		bool one = strcmp(argv[2], "one") == 0;
		int ret;

		selftest_clock_enable(false);
		lcd_all_pixels(false);
		if (one) {
			i = 3;
		}
		for (; i < argc; i++) {
			char *star = strchr(argv[i], '*');
			int count = star ? atoi(star + 1) : 1;
			uint8_t value = (uint8_t)strtoul(argv[i], NULL, 16);

			while (count-- > 0 && n < sizeof(data)) {
				data[n++] = value;
			}
		}
		ret = lcd_write_raw(data, n, one);
		shell_print(sh, "raw RAM write of %u byte(s) %s (%d)", (unsigned int)n,
			    ret ? "failed" : "done", ret);
		return ret;
	}
	if (strcmp(argv[1], "com") == 0 && argc >= 3) {
		/* Only the segments of that driver COM line, all of them */
		selftest_clock_enable(false);
		lcd_all_pixels(false);
		lcd_clear();
		lcd_fill_com(atoi(argv[2]), true);
		return lcd_flush();
	}
	selftest_clock_enable(false);
	lcd_all_pixels(false);
	lcd_clear();
	lcd_display_string(argv[1], 0);
	return lcd_flush();
}

static int cmd_pixel(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 3) {
		shell_print(sh, "usage: cb91ai pixel <com 0-2> <seg 0-23> [off]");
		return -EINVAL;
	}
	selftest_clock_enable(false);
	selftest_display_hold(true);
	lcd_set_pixel(atoi(argv[1]), atoi(argv[2]), argc < 4 || strcmp(argv[3], "off") != 0);
	return lcd_flush();
}

static int cmd_walk(const struct shell *sh, size_t argc, char **argv)
{
	int ret = 0;

	if (argc < 2) {
		shell_print(sh, "usage: cb91ai walk start [pixel|seg] [step] | next | prev | goto <step> | "
			    "stop | status");
		return -EINVAL;
	}
	if (strcmp(argv[1], "start") == 0) {
		enum lcdwalk_unit unit = (argc >= 3 && strcmp(argv[2], "seg") == 0) ? LCDWALK_SEG
									     : LCDWALK_PIXEL;

		if (ble_trial_mode()) {
			/* The walk polls the buttons with their interrupts off: in trial
			 * mode no press could start a burst, and the watch would stay silent */
			shell_print(sh, "refused: the advertising trials need the buttons (adv trial off)");
			return -EBUSY;
		}
		ret = lcdwalk_start(unit, argc >= 4 ? atoi(argv[3]) - 1 : 0);
		shell_print(sh, "walk %s (%d): MODE next, LIGHT previous, ALARM next COM line, "
			    "steps in the log", ret ? "failed" : "started", ret);
	} else if (strcmp(argv[1], "next") == 0) {
		ret = lcdwalk_move(1);
	} else if (strcmp(argv[1], "prev") == 0) {
		ret = lcdwalk_move(-1);
	} else if (strcmp(argv[1], "goto") == 0 && argc >= 3) {
		ret = lcdwalk_goto(atoi(argv[2]) - 1);
	} else if (strcmp(argv[1], "stop") == 0) {
		lcdwalk_stop();
	} else if (strcmp(argv[1], "status") != 0) {
		return -EINVAL;
	}
	shell_print(sh, "walk %s, step %d/%d", lcdwalk_active() ? "active" : "stopped",
		    lcdwalk_step() + 1, lcdwalk_count());
	return ret;
}

static int cmd_led(const struct shell *sh, size_t argc, char **argv)
{
	static const struct {
		const char *name;
		uint8_t mask; /* bit 0 red, bit 1 green, bit 2 blue */
	} colors[] = {
		{ "off", 0 }, { "red", 1 }, { "green", 2 }, { "yellow", 3 }, { "blue", 4 },
		{ "magenta", 5 }, { "cyan", 6 }, { "white", 7 },
	};
	long percent = argc >= 3 ? strtol(argv[2], NULL, 10) : 100;
	long ms = argc >= 4 ? strtol(argv[3], NULL, 10) : 0;
	struct hwtest_rail rail;
	int ret = 0;
	int found = -1;

	if (argc < 2) {
		shell_print(sh, "usage: cb91ai led <off|red|green|blue|yellow|magenta|cyan|white> "
			    "[percent] [ms] | led hue");
		return -EINVAL;
	}
	if (strcmp(argv[1], "hue") == 0) {
		/* The colour wheel of the boot, with the rail */
		selftest_heartbeat_enable(false);
		ret = hwtest_led_hue(&rail);
		shell_print(sh, "LED hue: one turn of the colour wheel in 1.5 s (%d): rail %d mV before, "
			    "lowest %d mV (%u readings), %d mV after", ret, rail.before_mv,
			    rail.lowest_mv, rail.readings, rail.after_mv);
		return ret;
	}
	/* On a coin cell the LED only lights in pulses (specification 4.7): at
	 * most 1 s, 300 ms when no duration is given. On USB it stays lit unless
	 * a duration is given. */
	if (!hwtest_on_usb()) {
		ms = ms <= 0 ? 300 : MIN(ms, 1000);
	}
	for (size_t i = 0; i < ARRAY_SIZE(colors); i++) {
		if (strcmp(argv[1], colors[i].name) == 0) {
			found = i;
		}
	}
	if (found < 0 || percent < 0 || percent > 100) {
		return -EINVAL;
	}
	selftest_heartbeat_enable(false);
	(void)hwtest_rail_mv(&rail.before_mv);
	for (size_t i = 0; i < ARRAY_SIZE(pwm_leds) && !ret; i++) {
		uint32_t pulse = (colors[found].mask & BIT(i)) ?
				 (uint32_t)((uint64_t)pwm_leds[i].period * percent / 100) : 0;

		ret = pwm_set_pulse_dt(&pwm_leds[i], pulse);
	}
	if (ms <= 0 || found == 0) {
		shell_print(sh, "LED %s at %ld %% (%d), heartbeat off", colors[found].name, percent,
			    ret);
		return ret;
	}
	/* A pulse: the rail is watched while it lasts, then everything is off */
	hwtest_rail_watch(&rail, (uint32_t)ms);
	for (size_t i = 0; i < ARRAY_SIZE(pwm_leds); i++) {
		(void)pwm_set_pulse_dt(&pwm_leds[i], 0);
	}
	k_msleep(3);
	(void)hwtest_rail_mv(&rail.after_mv);
	shell_print(sh, "LED %s at %ld %% for %ld ms (%d): rail %d mV before, lowest %d mV, "
		    "%d mV after", colors[found].name, percent, ms, ret, rail.before_mv,
		    rail.lowest_mv, rail.after_mv);
	return ret;
}

static int cmd_txpower(const struct shell *sh, size_t argc, char **argv)
{
	if (argc >= 2) {
		int ret = ble_set_tx_power((int8_t)strtol(argv[1], NULL, 10));

		if (ret) {
			shell_print(sh, "tx power not set (%d)", ret);
			return ret;
		}
	}
	shell_print(sh, "tx power %d dBm (0 by default, up to 8 for range tests)", ble_tx_power());
	return 0;
}

static int cmd_time(const struct shell *sh, size_t argc, char **argv)
{
	struct tm tm;

	if (argc >= 3 && strcmp(argv[1], "set") == 0) {
		/* time set <UTC seconds since 1970> [local offset in minutes] */
		clock_set(strtoll(argv[2], NULL, 10) * 1000,
			  argc >= 4 ? (int16_t)strtol(argv[3], NULL, 10) : clock_tz_minutes(), false);
	} else if (argc >= 3 && strcmp(argv[1], "ppb") == 0) {
		int ret = clock_set_ppb((int32_t)strtol(argv[2], NULL, 10));

		if (ret) {
			shell_print(sh, "correction refused (%d): +/-1000000 ppb at most", ret);
			return ret;
		}
	} else if (argc >= 3 && strcmp(argv[1], "format") == 0) {
		clock_set_24h(strcmp(argv[2], "24") == 0);
	} else if (argc >= 2) {
		shell_print(sh, "usage: cb91ai time | time set <utc seconds> [tz minutes] | "
			    "time ppb <ppb> | time format 12|24");
		return -EINVAL;
	}
	if (clock_local_tm(&tm)) {
		shell_print(sh, "%04d-%02d-%02d %02d:%02d:%02d local (UTC%+d min), utc ms %lld%s",
			    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
			    tm.tm_sec, clock_tz_minutes(), clock_now_ms(),
			    (clock_flags() & CLOCK_FLAG_APPROXIMATE) ? ", approximate (restored)" : "");
	} else {
		shell_print(sh, "time not set");
	}
	{
		char digits[7];
		bool pm;

		if (clock_display(digits, &pm, &tm)) {
			shell_print(sh, "glass: \"%c%c:%c%c %c%c\"%s, %s-hour format", digits[0],
				    digits[1], digits[2], digits[3], digits[4], digits[5],
				    pm ? " PM" : "", clock_24h() ? "24" : "12");
		}
	}
	shell_print(sh, "crystal correction %d ppb (%s), uptime %lld ms", clock_ppb(),
		    (clock_flags() & CLOCK_FLAG_CALIBRATED) ? "stored or calibrated" : "board default",
		    k_uptime_get());
	return 0;
}

static int cmd_heartbeat(const struct shell *sh, size_t argc, char **argv)
{
	bool on = argc >= 2 && strcmp(argv[1], "on") == 0;

	if (on) {
		for (size_t i = 0; i < ARRAY_SIZE(pwm_leds); i++) {
			pwm_set_pulse_dt(&pwm_leds[i], 0);
		}
	}
	selftest_heartbeat_enable(on);
	shell_print(sh, "green heartbeat %s", on ? "on" : "off");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cb91ai,
	SHELL_CMD(mic, NULL, "mic: capture and log the level | mic gain [dB]: PDM gain, -20 to 20", cmd_mic),
	SHELL_CMD(accel, NULL, "accel: read the accelerometer and its temperature | accel int: check "
		  "its two interrupt lines", cmd_accel),
	SHELL_CMD(lcd, NULL, "lcd <text> | all on|off | full | blank | clock | hold on|off | mode <m> | "
		  "frame <Hz> | inversion line|frame | try <n>|next|off | duty <3|4> | com <0-3>",
		  cmd_lcd),
	SHELL_CMD(time, NULL, "time | time set <utc s> [tz min] | time ppb <ppb> | time format 12|24", cmd_time),
	SHELL_CMD(txpower, NULL, "txpower [dBm]: radio transmit power, -40 to 8", cmd_txpower),
	SHELL_CMD(led, NULL, "led <off|red|green|blue|yellow|magenta|cyan|white> [percent] [ms]: "
		  "pulses only on the coin cell | led hue: the colour wheel of the boot", cmd_led),
	SHELL_CMD(heartbeat, NULL, "heartbeat on|off: green blink every second (off at boot)", cmd_heartbeat),
	SHELL_CMD(walk, NULL, "walk start [pixel|seg] [step] | next | prev | goto <n> | stop | status", cmd_walk),
	SHELL_CMD(pixel, NULL, "pixel <com> <seg> [off]: one LCD segment by COM and F-91W SEG", cmd_pixel),
	SHELL_CMD(keys, NULL, "keys | keys probe [s] | keys stop: case buttons read with both pulls | "
		  "keys gesture <s> | gesture | gesture stop: their gestures on the glass (lot D3)",
		  hwtest_cmd_keys),
	SHELL_CMD(beep, NULL, "beep <Hz> [ms] [duty percent] | beep sweep | beep ladder | beep alarm, "
		  "\"force\" last for full power on the coin cell: buzzer, with the rail", hwtest_cmd_beep),
	SHELL_CMD(rec, NULL, "rec [ms] | rec get | rec info: voice capture in RAM, sent over BLE",
		  hwtest_cmd_rec),
	SHELL_CMD(take, NULL, "take <1-5> [ms] | take get <1-5> | take info: voice take of up to 6 s "
		  "in the storage flash, sent over BLE", hwtest_cmd_take),
	SHELL_CMD(arm, NULL, "arm <n> <command...> | arm off | arm: a command at the next n presses "
		  "of ALARM", hwtest_cmd_arm),
	SHELL_CMD(rssi, NULL, "rssi: the host as the watch hears it", hwtest_cmd_rssi),
	SHELL_CMD(qspi, NULL, "qspi: erase, write and read back the last sector of the storage flash",
		  hwtest_cmd_qspi),
	SHELL_CMD(adv, NULL, "adv fast <min> | adv normal: fast advertising for a test session | "
		  "adv trial apple|link [min] | trial go | trial off | trial: bursts timed to a "
		  "phone's connection (lot N1a)", hwtest_cmd_adv),
	SHELL_COND_CMD(CONFIG_CB91AI_CODEC_BENCH, codec, NULL,
		       "codec signal | record [raw] | level | all | adpcm | lc3 <bit/s> [frame us] | "
		       "opus <bit/s> [complexity] [frame ms] [nb|mb|wb] | dump | "
		       "roundtrip adpcm | roundtrip lc3 <bit/s> [frame us]", codec_bench_cmd),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(cb91ai, &sub_cb91ai, "CB-91AI self-test commands", NULL);
