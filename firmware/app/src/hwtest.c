/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware checks for a board sealed in its watch, on its CR2016 (0.1.46 on).
 * They run from the PC over Bluetooth (remote.c, tools/ble_shell.py), and what
 * needs the wearer's attention is triggered by the ALARM button:
 * - keys: the three case buttons read with each pull in turn for minutes, so
 *   that a contact to the battery + (high against the pull-down), a contact to
 *   ground (low against the pull-up) and no contact at all are told apart (in
 *   the module on 2026-09-22 only ALARM reacted, V2-22; all three since the
 *   rework of 2026-09-23); keys gesture: the gestures of lib/gesture.c on the
 *   real buttons, on the glass (lot D3);
 * - beep: the buzzer at a given frequency, with the lowest voltage of the rail
 *   during the beep (R3, V2-35: what a CR2016 makes of the buzzer), a sweep to
 *   find the loudest frequency in the closed case (V2-24), a ladder of lengths
 *   and an alarm pattern, at full power with "force" (a test, see below);
 * - led hue: one turn of the colour wheel on the RGB LED, as at boot, with the
 *   lowest voltage of the rail;
 * - qspi: erase, write and read back one sector of the storage flash;
 * - rec: a voice capture held in RAM, its level, and its samples streamed to
 *   the PC as a WAV file (V2-36, EV-07: the voice through the closed case).
 *   While it runs the glass shows "rEC" and the red LED blinks: no beep, which
 *   the wearer can hardly hear through the case (2026-09-23);
 * - take: the same, up to 6 s, straight to the storage flash, five slots
 *   fetched afterwards: sentences read at the wearer's pace (lots K1, K3);
 * - accel int: the two interrupt lines of the BMA400, raised and released by
 *   the sensor, each seen alone on its GPIO (lot D4 needs them);
 * - arm: `cb91ai` commands run at the next presses of ALARM, one step per
 *   press, output kept;
 * - rssi, adv: the link as the watch hears it, and fast advertising held for
 *   a test session (the PC finds a watch in slow advertising in 10 to 70 s);
 * - adv trial: silent radio, a burst of advertising at each press, timed to
 *   the connection of a phone on the glass (lot N1a, risk R11, see ble.c).
 *
 * Without USB every load stays within specification 4.7, amended on
 * 2026-09-23 for the buzzer: a beep lasts 30 ms at most, at 50 % (4.7 asked
 * 25 %), longer only with "force"; the LED is lit in pulses (cmds.c), but for
 * the colour wheel of 1.5 s at boot, his choice of the same day.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <nrfx.h>

#include "accel.h"
#include "ble.h"
#include "clock.h"
#include "gesture.h"
#include "hwtest.h"
#include "lcd.h"
#include "lcdwalk.h"
#include "mic.h"
#include "remote.h"

/* Defined in main.c */
void selftest_buttons_irq(bool enable);
void selftest_wdt_feed(void);
void selftest_clock_enable(bool enable);
void selftest_display_hold(bool hold);

static const struct adc_dt_spec vdd_adc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 1);
static const struct pwm_dt_spec buzzer = PWM_DT_SPEC_GET(DT_NODELABEL(buzzer));
static const struct gpio_dt_spec keys[] = {
	GPIO_DT_SPEC_GET(DT_NODELABEL(button_light), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(button_mode), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(button_alarm), gpios),
};
static const char *const key_names[] = { "LIGHT", "MODE", "ALARM" };
static const struct device *const qspi_flash = DEVICE_DT_GET(DT_NODELABEL(zd25wq80c));

bool hwtest_on_usb(void)
{
	return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}

/* ---- The rail under a load ------------------------------------------------ */

/* The VDD channel is set up by the boot self-test (main.c, test_battery) */
int hwtest_rail_mv(int32_t *mv)
{
	int16_t sample;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};
	int32_t value;
	int ret = adc_sequence_init_dt(&vdd_adc, &seq);

	if (ret == 0) {
		ret = adc_read_dt(&vdd_adc, &seq);
	}
	if (ret == 0) {
		value = sample;
		ret = adc_raw_to_millivolts_dt(&vdd_adc, &value);
	}
	if (ret == 0) {
		*mv = value;
	}
	return ret;
}

void hwtest_rail_watch(struct hwtest_rail *rail, uint32_t ms)
{
	const int64_t end = k_uptime_get() + ms;
	int32_t mv;

	rail->lowest_mv = INT32_MAX;
	rail->readings = 0;
	do {
		if (hwtest_rail_mv(&mv) == 0) {
			rail->lowest_mv = MIN(rail->lowest_mv, mv);
			rail->readings++;
		}
	} while (k_uptime_get() < end);
	if (rail->readings == 0) {
		rail->lowest_mv = -1;
	}
}

/* ---- Buzzer ------------------------------------------------------------- */

/* On the coin cell: 30 ms, and a duty of 50 % since the decision of
 * 2026-09-23, where specification 4.7 asked 25 % */
#define BEEP_MS_CELL   30
#define BEEP_DUTY_CELL 50
#define BEEP_MS_USB    200
#define BEEP_DUTY_USB  50

/*
 * The frequency the PWM peripheral really produces, read back from its
 * registers while it runs (nRF52840 MDK: MODE, COUNTERTOP, PRESCALER; the PWM
 * clock is 16 MHz divided by 2 to the prescaler): the proof, asked for on
 * 2026-09-23, that a beep sounds at the frequency the command says.
 */
static NRF_PWM_Type *const buzzer_pwm =
	(NRF_PWM_Type *)DT_REG_ADDR(DT_PWMS_CTLR(DT_NODELABEL(buzzer)));
static uint32_t buzzer_actual_hz;

static uint32_t pwm_actual_hz(void)
{
	const uint32_t top = buzzer_pwm->COUNTERTOP & PWM_COUNTERTOP_COUNTERTOP_Msk;
	const uint32_t prescaler = buzzer_pwm->PRESCALER & PWM_PRESCALER_PRESCALER_Msk;
	const uint32_t up_down = (buzzer_pwm->MODE & PWM_MODE_UPDOWN_Msk) ? 2U : 1U;

	return top ? 16000000U / (1U << prescaler) / (top * up_down) : 0U;
}

static int beep_once(uint32_t hz, uint32_t ms, uint32_t duty, struct hwtest_rail *rail)
{
	const uint32_t period = NSEC_PER_SEC / hz;
	const uint32_t pulse = (uint32_t)((uint64_t)period * duty / 100U);
	int ret;

	rail->before_mv = rail->lowest_mv = rail->after_mv = -1;
	rail->readings = 0;
	if (!pwm_is_ready_dt(&buzzer)) {
		return -ENODEV;
	}
	(void)hwtest_rail_mv(&rail->before_mv);
	ret = pwm_set_dt(&buzzer, period, pulse);
	if (ret) {
		return ret;
	}
	buzzer_actual_hz = pwm_actual_hz();
	hwtest_rail_watch(rail, ms);
	/* Output back low: P0.19 is never left high */
	ret = pwm_set_dt(&buzzer, period, 0);
	if (ret) {
		ret = pwm_set_dt(&buzzer, buzzer.period, 0);
	}
	k_msleep(3);
	(void)hwtest_rail_mv(&rail->after_mv);
	return ret;
}

/* Around the 4 kHz of the transducer, loudest out of the case (V2-24) */
static const uint16_t sweep_hz[] = { 2700, 3200, 3600, 4000, 4400, 4800, 5400 };

static int beep_sweep(const struct shell *sh)
{
	struct hwtest_rail first, second;
	int ret = 0;

	shell_print(sh, "sweep: %u steps of two beeps (%d ms, duty %d percent), 1 s apart",
		    (unsigned int)ARRAY_SIZE(sweep_hz), BEEP_MS_CELL, BEEP_DUTY_CELL);
	for (size_t i = 0; i < ARRAY_SIZE(sweep_hz) && ret == 0; i++) {
		ret = beep_once(sweep_hz[i], BEEP_MS_CELL, BEEP_DUTY_CELL, &first);
		k_msleep(120);
		if (ret == 0) {
			ret = beep_once(sweep_hz[i], BEEP_MS_CELL, BEEP_DUTY_CELL, &second);
		}
		selftest_wdt_feed();
		shell_print(sh, "  %u. %u Hz, PWM measured at %u Hz: rail %d mV before, lowest %d mV",
			    (unsigned int)(i + 1), sweep_hz[i], buzzer_actual_hz, first.before_mv,
			    MIN(first.lowest_mv, second.lowest_mv));
		k_msleep(900);
	}
	return ret;
}

/*
 * Longer beeps on the coin cell, for the tests asked for on 2026-09-23
 * (does the CR2016 hold an alarm at full power? it does): only with the word
 * "force". Full power is a duty of 50 %, the square wave with the strongest
 * fundamental, and the default since that day; 100 % would hold P0.19 high,
 * the buzzer silent and its coil drawing current for good.
 */
#define BEEP_MS_FORCE   500
#define BEEP_DUTY_FORCE 50

static bool forced(size_t argc, char **argv)
{
	return argc >= 2 && strcmp(argv[argc - 1], "force") == 0;
}

/* 4 kHz, longer and longer: the rail at each length, until it sags too far for
 * the next, longer step to be worth the risk of a brown-out mid-test */
static const uint16_t ladder_ms[] = { 30, 60, 100, 200, 500 };
#define LADDER_STOP_MV 2000

static int beep_ladder(const struct shell *sh, uint32_t duty, uint32_t max_ms)
{
	struct hwtest_rail rail;
	int ret = 0;

	shell_print(sh, "ladder: 4000 Hz, duty %u percent, lengths up to %u ms, 2 s apart", duty,
		    max_ms);
	for (size_t i = 0; i < ARRAY_SIZE(ladder_ms) && ladder_ms[i] <= max_ms && ret == 0; i++) {
		ret = beep_once(4000, ladder_ms[i], duty, &rail);
		selftest_wdt_feed();
		shell_print(sh, "  %u ms, PWM measured at %u Hz: rail %d mV before, lowest %d mV, %d mV "
			    "after", ladder_ms[i], buzzer_actual_hz, rail.before_mv, rail.lowest_mv,
			    rail.after_mv);
		if (rail.lowest_mv >= 0 && rail.lowest_mv < LADDER_STOP_MV) {
			shell_print(sh, "  stopped: the rail went under %d mV", LADDER_STOP_MV);
			break;
		}
		k_msleep(2000);
	}
	return ret;
}

/* An alarm in the manner of the F-91W: bursts of four short beeps, five times,
 * about 4 s in all; the lowest rail over the whole of it */
static int beep_alarm(const struct shell *sh, uint32_t duty)
{
	struct hwtest_rail rail;
	int32_t lowest = INT32_MAX, before = -1;
	int ret = 0;

	for (int burst = 0; burst < 5 && ret == 0; burst++) {
		for (int beep = 0; beep < 4 && ret == 0; beep++) {
			ret = beep_once(4000, BEEP_MS_CELL, duty, &rail);
			if (before < 0) {
				before = rail.before_mv;
			}
			if (rail.lowest_mv >= 0) {
				lowest = MIN(lowest, rail.lowest_mv);
			}
			k_msleep(60);
		}
		selftest_wdt_feed();
		k_msleep(500);
	}
	shell_print(sh, "alarm: 5 bursts of 4 beeps, 4000 Hz (PWM measured at %u Hz), %d ms, duty %u "
		    "percent (%d): rail %d mV before, lowest %d mV", buzzer_actual_hz, BEEP_MS_CELL,
		    duty, ret, before, lowest == INT32_MAX ? -1 : lowest);
	return ret;
}

int hwtest_cmd_beep(const struct shell *sh, size_t argc, char **argv)
{
	const bool usb = hwtest_on_usb();
	const bool force = forced(argc, argv);
	const uint32_t max_ms = usb ? BEEP_MS_USB : force ? BEEP_MS_FORCE : BEEP_MS_CELL;
	const uint32_t max_duty = usb ? BEEP_DUTY_USB : force ? BEEP_DUTY_FORCE : BEEP_DUTY_CELL;
	struct hwtest_rail rail;
	uint32_t hz, ms, duty;
	int ret;

	if (force) {
		argc--; /* the word is not an argument */
	}
	if (argc >= 2 && strcmp(argv[1], "sweep") == 0) {
		return beep_sweep(sh);
	}
	if (argc >= 2 && strcmp(argv[1], "ladder") == 0) {
		return beep_ladder(sh, max_duty, max_ms);
	}
	if (argc >= 2 && strcmp(argv[1], "alarm") == 0) {
		return beep_alarm(sh, max_duty);
	}
	if (argc < 2) {
		shell_print(sh, "usage: cb91ai beep <Hz> [ms] [duty percent] | beep sweep | beep ladder | "
			    "beep alarm, and \"force\" last for full power on the coin cell");
		return -EINVAL;
	}
	hz = strtoul(argv[1], NULL, 10);
	ms = argc >= 3 ? strtoul(argv[2], NULL, 10) : BEEP_MS_CELL;
	duty = argc >= 4 ? strtoul(argv[3], NULL, 10) : BEEP_DUTY_CELL;
	if (hz < 500 || hz > 8000 || ms == 0 || ms > max_ms || duty == 0 || duty > max_duty) {
		shell_print(sh, "refused: 500 to 8000 Hz, at most %u ms and %u percent %s", max_ms,
			    max_duty, usb ? "on USB" : force ? "with force" :
			    "on the coin cell (specification 4.7; \"force\" for a test)");
		return -EINVAL;
	}
	ret = beep_once(hz, ms, duty, &rail);
	shell_print(sh, "beep %u Hz (PWM measured at %u Hz), %u ms, duty %u percent (%d): rail %d mV "
		    "before, lowest %d mV during (%u readings), %d mV after", hz, buzzer_actual_hz, ms,
		    duty, ret, rail.before_mv, rail.lowest_mv, rail.readings, rail.after_mv);
	return ret;
}

/* ---- LEDs ------------------------------------------------------------------ */

static const struct pwm_dt_spec pwm_leds[] = {
	PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led_red)),
	PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led_green)),
	PWM_DT_SPEC_GET(DT_NODELABEL(pwm_led_blue)),
};

/*
 * One turn of the colour wheel, red to yellow, green, cyan, blue, magenta and
 * back to red in 1.5 s: the LEDs at boot since they were asked for on
 * 2026-09-23 (before, red, green and blue one after the other). The three
 * channels run at 1 kHz meanwhile, since the 50 Hz of the devicetree flickers
 * at low duty, and each ramp is squared so that the fades look even to the
 * eye. All off at the end, which stops the PWM and leaves the pins to the GPIO.
 */
#define HUE_SWEEP_MS  1500
#define HUE_STEP_MS   20
#define HUE_PERIOD_NS PWM_USEC(1000)
#define HUE_SECTOR    256 /* a turn is six sectors: 1536 steps of hue */

static void hue_to_rgb(uint32_t hue, uint8_t rgb[3])
{
	const uint8_t up = hue % HUE_SECTOR;
	const uint8_t down = 255 - up;

	switch (hue / HUE_SECTOR) {
	case 0: /* red, green rising: yellow */
		rgb[0] = 255;
		rgb[1] = up;
		rgb[2] = 0;
		break;
	case 1: /* red falling: green */
		rgb[0] = down;
		rgb[1] = 255;
		rgb[2] = 0;
		break;
	case 2: /* blue rising: cyan */
		rgb[0] = 0;
		rgb[1] = 255;
		rgb[2] = up;
		break;
	case 3: /* green falling: blue */
		rgb[0] = 0;
		rgb[1] = down;
		rgb[2] = 255;
		break;
	case 4: /* red rising: magenta */
		rgb[0] = up;
		rgb[1] = 0;
		rgb[2] = 255;
		break;
	default: /* blue falling: red again */
		rgb[0] = 255;
		rgb[1] = 0;
		rgb[2] = down;
		break;
	}
}

static void leds_off(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(pwm_leds); i++) {
		(void)pwm_set_pulse_dt(&pwm_leds[i], 0);
	}
}

int hwtest_led_hue(struct hwtest_rail *rail)
{
	const uint32_t steps = HUE_SWEEP_MS / HUE_STEP_MS;
	int32_t lowest = INT32_MAX;
	uint32_t readings = 0;
	int ret = 0;

	for (size_t i = 0; i < ARRAY_SIZE(pwm_leds); i++) {
		if (!pwm_is_ready_dt(&pwm_leds[i])) {
			return -ENODEV;
		}
	}
	/* Nothing dimmed at 50 Hz may hold the period of the PWM */
	leds_off();
	if (rail) {
		rail->before_mv = rail->after_mv = -1;
		(void)hwtest_rail_mv(&rail->before_mv);
	}
	for (uint32_t step = 0; step < steps && ret == 0; step++) {
		uint8_t rgb[3];

		hue_to_rgb(step * 6U * HUE_SECTOR / steps, rgb);
		for (size_t i = 0; i < ARRAY_SIZE(pwm_leds) && ret == 0; i++) {
			const uint32_t pulse =
				(uint32_t)((uint64_t)HUE_PERIOD_NS * rgb[i] * rgb[i] / (255U * 255U));

			ret = pwm_set_dt(&pwm_leds[i], HUE_PERIOD_NS, pulse);
		}
		if (rail) {
			struct hwtest_rail during;

			hwtest_rail_watch(&during, HUE_STEP_MS);
			if (during.readings > 0) {
				lowest = MIN(lowest, during.lowest_mv);
				readings += during.readings;
			}
		} else {
			k_msleep(HUE_STEP_MS);
		}
	}
	leds_off();
	selftest_wdt_feed();
	if (rail) {
		rail->lowest_mv = readings > 0 ? lowest : -1;
		rail->readings = readings;
		k_msleep(3);
		(void)hwtest_rail_mv(&rail->after_mv);
	}
	return ret;
}

/* ---- Case buttons ---------------------------------------------------------- */

#define PROBE_PERIOD_MS 20
#define PROBE_SETTLE_US 20
#define PROBE_DEFAULT_S 300
#define PROBE_MAX_S     900

/* One way a button can answer: its readings against one pull. Lengths are in
 * readings, 20 ms each; a press or a release shorter than two readings is
 * taken as a bounce and merged into what surrounds it. */
struct key_side {
	uint32_t active;         /* readings against the pull */
	uint32_t presses;        /* runs of two readings or more */
	uint32_t edges;          /* raw changes of the reading, bounces included */
	uint32_t shortest_press; /* UINT32_MAX until a press has ended */
	uint32_t longest_press;
	uint32_t shortest_gap;   /* release between two presses; UINT32_MAX if none */
	uint32_t short_gaps;     /* releases under SHORT_GAP readings: a press cut in two? */
	uint32_t length;         /* readings in the current state */
	uint8_t run;
	bool on;
	bool raw;
};
#define SHORT_GAP 5 /* 100 ms */

static struct {
	bool used;
	bool running;
	int64_t end_ms;
	uint32_t readings; /* per button and per pull */
	struct key_side plus[ARRAY_SIZE(keys)]; /* high against the pull-down: to a supply */
	struct key_side gnd[ARRAY_SIZE(keys)];  /* low against the pull-up: to ground */
	int64_t last_ms[ARRAY_SIZE(keys)];
} probe;

static struct k_work_delayable probe_work;

/* Two readings in a row change the state: 40 ms, one reading every 20 ms */
static void side_update(struct key_side *side, bool active)
{
	if (active) {
		side->active++;
	}
	if (active != side->raw) {
		side->edges++;
		side->raw = active;
	}
	side->length++;
	if (active == side->on) {
		side->run = 0;
		return;
	}
	if (++side->run >= 2) {
		/* The state that ends lasted until the first of these two readings */
		const uint32_t ended = side->length - side->run;

		if (side->on) {
			side->shortest_press = MIN(side->shortest_press, ended);
			side->longest_press = MAX(side->longest_press, ended);
		} else if (side->presses > 0) {
			side->shortest_gap = MIN(side->shortest_gap, ended);
			if (ended < SHORT_GAP) {
				side->short_gaps++;
			}
		}
		side->on = active;
		side->length = side->run;
		side->run = 0;
		if (active) {
			side->presses++;
		}
	}
}

/* Pull-up then pull-down: the pin is left in the rest state of the devicetree */
static void key_read(size_t i, int *up, int *down)
{
	(void)gpio_pin_configure(keys[i].port, keys[i].pin, GPIO_INPUT | GPIO_PULL_UP);
	k_busy_wait(PROBE_SETTLE_US);
	*up = gpio_pin_get_raw(keys[i].port, keys[i].pin);
	(void)gpio_pin_configure(keys[i].port, keys[i].pin, GPIO_INPUT | GPIO_PULL_DOWN);
	k_busy_wait(PROBE_SETTLE_US);
	*down = gpio_pin_get_raw(keys[i].port, keys[i].pin);
}

static void keys_restore(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		(void)gpio_pin_configure_dt(&keys[i], GPIO_INPUT);
	}
	selftest_buttons_irq(true);
}

static void probe_poll(struct k_work *work)
{
	const int64_t now = k_uptime_get();

	ARG_UNUSED(work);
	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		int up, down;

		key_read(i, &up, &down);
		side_update(&probe.gnd[i], up == 0);
		side_update(&probe.plus[i], down == 1);
		if (up == 0 || down == 1) {
			probe.last_ms[i] = now;
		}
	}
	probe.readings++;
	if (now >= probe.end_ms) {
		probe.running = false;
		keys_restore();
		return;
	}
	k_work_reschedule(&probe_work, K_MSEC(PROBE_PERIOD_MS));
}

static void probe_stop(void)
{
	struct k_work_sync sync;

	if (probe.running) {
		(void)k_work_cancel_delayable_sync(&probe_work, &sync);
		probe.running = false;
		probe.end_ms = k_uptime_get();
		keys_restore();
	}
}

static void probe_start(uint32_t seconds)
{
	static bool ready;

	if (!ready) {
		k_work_init_delayable(&probe_work, probe_poll);
		ready = true;
	}
	probe_stop();
	memset(&probe, 0, sizeof(probe));
	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		probe.last_ms[i] = -1;
		probe.plus[i].shortest_press = probe.gnd[i].shortest_press = UINT32_MAX;
		probe.plus[i].shortest_gap = probe.gnd[i].shortest_gap = UINT32_MAX;
	}
	probe.used = true;
	probe.running = true;
	probe.end_ms = k_uptime_get() + (int64_t)seconds * 1000;
	/* The pulls change every reading: no button interrupt meanwhile */
	selftest_buttons_irq(false);
	k_work_reschedule(&probe_work, K_NO_WAIT);
}

static const char *contact(int up, int down)
{
	return up == 0 ? "to ground" : down == 1 ? "to battery +" : "open";
}

static void probe_print(const struct shell *sh)
{
	const int64_t now = k_uptime_get();

	if (!probe.used) {
		shell_print(sh, "no probe since boot: cb91ai keys probe [seconds]");
		return;
	}
	if (probe.running) {
		shell_print(sh, "probe running, %d s left: %u readings per pull, one every %d ms",
			    (int)((probe.end_ms - now) / 1000), probe.readings, PROBE_PERIOD_MS);
	} else {
		shell_print(sh, "probe over %d s ago: %u readings per pull",
			    (int)((now - probe.end_ms) / 1000), probe.readings);
	}
	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		const struct key_side *p = &probe.plus[i];
		char last[20] = "never", lengths[64] = "";

		if (probe.last_ms[i] >= 0) {
			snprintk(last, sizeof(last), "%d s ago", (int)((now - probe.last_ms[i]) / 1000));
		}
		/* Lengths of the presses to the battery +, the way the buttons close */
		if (p->shortest_press != UINT32_MAX) {
			snprintk(lengths, sizeof(lengths), ", held %u to %u ms",
				 p->shortest_press * PROBE_PERIOD_MS, p->longest_press * PROBE_PERIOD_MS);
		}
		if (p->shortest_gap != UINT32_MAX) {
			size_t used = strlen(lengths);

			snprintk(&lengths[used], sizeof(lengths) - used, ", shortest release %u ms",
				 p->shortest_gap * PROBE_PERIOD_MS);
		}
		shell_print(sh, "%-5s to battery +: %u presses%s, %u releases under %d ms, %u edges | to ground: "
			    "%u readings | last contact %s", key_names[i], p->presses, lengths,
			    p->short_gaps, SHORT_GAP * PROBE_PERIOD_MS, p->edges, probe.gnd[i].active,
			    last);
	}
}

/*
 * Gestures (lot D3): the machine of lib/gesture.c on the real buttons, before
 * the watch can run it (it needs the link of D6 to be installed). The buttons
 * are read every 10 ms, with the pulls of the devicetree, and what the machine
 * makes of them is shown on the glass and kept: click and its count, long
 * press, its end and how long it was held, chords of two buttons and how long
 * they are held. The button interrupts are off meanwhile, so that a press does
 * nothing else (no armed step, no advertising burst).
 */
#define GESTURE_PERIOD_MS 10
#define GESTURE_KEPT      24

static struct {
	bool used;
	bool running;
	int64_t start_ms;
	int64_t end_ms;
	uint32_t count; /* gestures since the start; kept[(count - 1) % GESTURE_KEPT] is the last */
	struct {
		uint32_t at_ms;
		struct gesture g;
	} kept[GESTURE_KEPT];
	struct gesture_state state;
} gest;

static struct k_work_delayable gesture_work;

static const char *const key_codes[] = { "LI", "MO", "AL" };

static void trial_stopwatch(uint32_t ms, bool hundredths); /* the glass as a stopwatch */

static void gesture_show(const struct gesture *g)
{
	char digit[2] = { 0 };

	lcd_clear();
	switch (g->type) {
	case GESTURE_CLICK:
		lcd_display_string(key_codes[g->button], 0);
		digit[0] = (char)('0' + g->count);
		lcd_display_string(digit, 3);
		lcd_display_string("CLIC", 5);
		break;
	case GESTURE_LONG:
		lcd_display_string(key_codes[g->button], 0);
		lcd_display_string("LOnG", 5);
		break;
	case GESTURE_LONG_END:
		lcd_display_string(key_codes[g->button], 0);
		trial_stopwatch(g->ms, true);
		break;
	case GESTURE_CHORD:
	case GESTURE_CHORD_HOLD:
	case GESTURE_CHORD_END:
		lcd_display_string("CH", 0);
		digit[0] = (char)('0' + (g->button & 0x7));
		lcd_display_string(digit, 3);
		trial_stopwatch(g->ms, g->type == GESTURE_CHORD_END);
		break;
	default:
		return; /* a press: the glass waits for what it becomes */
	}
	(void)lcd_flush();
}

static void gesture_poll(struct k_work *work)
{
	const int64_t now = k_uptime_get();
	struct gesture out[GESTURE_OUT_MAX(ARRAY_SIZE(keys))];
	uint32_t levels = 0;
	size_t n;

	ARG_UNUSED(work);
	for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
		if (gpio_pin_get_dt(&keys[i]) > 0) {
			levels |= BIT(i);
		}
	}
	n = gesture_update(&gest.state, levels, (uint32_t)now, out);
	for (size_t i = 0; i < n; i++) {
		const uint32_t slot = gest.count % GESTURE_KEPT;

		gest.kept[slot].at_ms = (uint32_t)(now - gest.start_ms);
		gest.kept[slot].g = out[i];
		gest.count++;
		gesture_show(&out[i]);
	}
	if (now >= gest.end_ms) {
		gest.running = false;
		selftest_buttons_irq(true);
		lcd_all_pixels(false);
		selftest_display_hold(false);
		selftest_clock_enable(true);
		return;
	}
	k_work_reschedule(&gesture_work, K_MSEC(GESTURE_PERIOD_MS));
}

static void gesture_stop(void)
{
	struct k_work_sync sync;

	if (gest.running) {
		(void)k_work_cancel_delayable_sync(&gesture_work, &sync);
		gest.running = false;
		gest.end_ms = k_uptime_get();
		selftest_buttons_irq(true);
		lcd_all_pixels(false);
		selftest_display_hold(false);
		selftest_clock_enable(true);
	}
}

static void gesture_start(uint32_t seconds)
{
	static const struct gesture_cfg cfg = GESTURE_CFG_V1;
	static bool ready;

	if (!ready) {
		k_work_init_delayable(&gesture_work, gesture_poll);
		ready = true;
	}
	gesture_stop();
	memset(&gest, 0, sizeof(gest));
	gesture_init(&gest.state, &cfg, ARRAY_SIZE(keys));
	gest.used = true;
	gest.running = true;
	gest.start_ms = k_uptime_get();
	gest.end_ms = gest.start_ms + (int64_t)seconds * 1000;
	selftest_buttons_irq(false);
	selftest_clock_enable(false);
	selftest_display_hold(true);
	lcd_all_pixels(false);
	lcd_clear();
	lcd_display_string("PrESS", 5);
	(void)lcd_flush();
	k_work_reschedule(&gesture_work, K_NO_WAIT);
}

static void gesture_print(const struct shell *sh)
{
	const char *const *names = key_names;
	const uint32_t kept = MIN(gest.count, (uint32_t)GESTURE_KEPT);

	if (!gest.used) {
		shell_print(sh, "no gesture session since boot: cb91ai keys gesture [seconds]");
		return;
	}
	shell_print(sh, "gestures %s, %u seen, the last %u:", gest.running ? "running" : "over",
		    gest.count, kept);
	for (uint32_t k = gest.count - kept; k < gest.count; k++) {
		const struct gesture *g = &gest.kept[k % GESTURE_KEPT].g;
		const uint32_t at = gest.kept[k % GESTURE_KEPT].at_ms;
		char what[40];

		switch (g->type) {
		case GESTURE_PRESS:
			snprintk(what, sizeof(what), "%s press (%u before)", names[g->button], g->count);
			break;
		case GESTURE_CLICK:
			snprintk(what, sizeof(what), "%s click x%u", names[g->button], g->count);
			break;
		case GESTURE_LONG:
			snprintk(what, sizeof(what), "%s long", names[g->button]);
			break;
		case GESTURE_LONG_END:
			snprintk(what, sizeof(what), "%s long end, %u ms", names[g->button], g->ms);
			break;
		case GESTURE_CHORD:
			snprintk(what, sizeof(what), "chord %s%s%s", g->button & 1 ? "L" : "",
				 g->button & 2 ? "M" : "", g->button & 4 ? "A" : "");
			break;
		case GESTURE_CHORD_HOLD:
			snprintk(what, sizeof(what), "chord held %u s", g->ms / 1000U);
			break;
		default:
			snprintk(what, sizeof(what), "chord end, %u ms", g->ms);
			break;
		}
		shell_print(sh, "+%u.%03u s %s", at / 1000U, at % 1000U, what);
	}
}

int hwtest_cmd_keys(const struct shell *sh, size_t argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "gesture") == 0) {
		uint32_t seconds;

		if (argc < 3) {
			gesture_print(sh);
			return 0;
		}
		if (strcmp(argv[2], "stop") == 0) {
			gesture_stop();
			gesture_print(sh);
			return 0;
		}
		seconds = strtoul(argv[2], NULL, 10);
		if (seconds == 0 || seconds > PROBE_MAX_S) {
			shell_print(sh, "refused: 1 to %d s", PROBE_MAX_S);
			return -EINVAL;
		}
		if (probe.running || lcdwalk_active() || ble_trial_mode()) {
			shell_print(sh, "refused: the probe, the segment walk or the advertising trials "
				    "use the buttons (keys stop, walk stop, adv trial off)");
			return -EBUSY;
		}
		gesture_start(seconds);
		shell_print(sh, "gestures for %u s: the glass shows each one (click and count, long "
			    "press, how long it was held, chords); `keys gesture` lists them, "
			    "`keys gesture stop` ends early", seconds);
		return 0;
	}
	if (gest.running) {
		shell_print(sh, "refused: the gestures read the buttons (keys gesture stop)");
		return -EBUSY;
	}
	if (argc >= 2 && strcmp(argv[1], "probe") == 0) {
		uint32_t seconds = argc >= 3 ? strtoul(argv[2], NULL, 10) : PROBE_DEFAULT_S;

		if (seconds == 0 || seconds > PROBE_MAX_S) {
			shell_print(sh, "refused: 1 to %d s", PROBE_MAX_S);
			return -EINVAL;
		}
		if (lcdwalk_active()) {
			shell_print(sh, "refused: the segment walk reads the buttons (cb91ai walk stop)");
			return -EBUSY;
		}
		if (ble_trial_mode()) {
			/* Its presses start the bursts: with no button interrupt, the
			 * watch would stay silent */
			shell_print(sh, "refused: the advertising trials need the buttons (adv trial off)");
			return -EBUSY;
		}
		probe_start(seconds);
		shell_print(sh, "probe started for %u s: each button read with a pull-up, then a "
			    "pull-down, every %d ms; the buttons wake nothing meanwhile", seconds,
			    PROBE_PERIOD_MS);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
		probe_stop();
	} else if (argc >= 2) {
		shell_print(sh, "usage: cb91ai keys | keys probe [seconds] | keys stop");
		return -EINVAL;
	}
	if (!probe.running && !lcdwalk_active()) {
		int up[ARRAY_SIZE(keys)], down[ARRAY_SIZE(keys)];

		selftest_buttons_irq(false);
		for (size_t i = 0; i < ARRAY_SIZE(keys); i++) {
			key_read(i, &up[i], &down[i]);
		}
		keys_restore();
		shell_print(sh, "now: LIGHT %s, MODE %s, ALARM %s", contact(up[0], down[0]),
			    contact(up[1], down[1]), contact(up[2], down[2]));
	}
	probe_print(sh);
	return 0;
}

/* ---- Voice capture ---------------------------------------------------------- */

#if CONFIG_CB91AI_REC_MS > 0

#define REC_SAMPLES (CONFIG_CB91AI_REC_MS * (MIC_SAMPLE_RATE_HZ / 1000))
#define REC_WINDOW  (MIC_SAMPLE_RATE_HZ * 4 / 10) /* level every 0.4 s */

static int16_t rec_buf[REC_SAMPLES];
static size_t rec_count;

/* Tenths of a dB below full scale, "-12.3", down to "-99.9" */
static void db_text(char *buf, size_t size, double amplitude)
{
	double db10 = amplitude > 0.5 ? 200.0 * log10(amplitude / 32768.0) : -999.0;
	unsigned int below = (unsigned int)CLAMP(-db10 + 0.5, 0.0, 999.0);

	snprintk(buf, size, "%s%u.%u", below ? "-" : "", (below / 10U) % 100U, below % 10U);
}

static void rec_level(const struct shell *sh, const int16_t *s, size_t n, const char *what)
{
	int64_t sum = 0;
	uint64_t sum_sq = 0;
	uint32_t peak = 0, clipped = 0;
	char rms_txt[8], peak_txt[8];

	if (n == 0) {
		return;
	}
	for (size_t i = 0; i < n; i++) {
		int32_t v = s[i];
		uint32_t a = (uint32_t)abs(v);

		sum += v;
		sum_sq += (uint64_t)((int64_t)v * v);
		peak = MAX(peak, a);
		if (a >= 32767) {
			clipped++;
		}
	}
	db_text(rms_txt, sizeof(rms_txt), sqrt((double)sum_sq / (double)n));
	db_text(peak_txt, sizeof(peak_txt), (double)peak);
	shell_print(sh, "%s: rms %s dBFS, peak %s dBFS, %u clipped, mean %d", what, rms_txt,
		    peak_txt, clipped, (int)(sum / (int64_t)n));
}

static int rec_get(const struct shell *sh)
{
	const size_t bytes = rec_count * sizeof(int16_t);
	uint32_t crc;
	int sent;

	if (rec_count == 0) {
		shell_print(sh, "nothing recorded since boot: cb91ai rec [ms]");
		return -ENODATA;
	}
	crc = crc32_ieee((const uint8_t *)rec_buf, bytes);
	sent = remote_stream(rec_buf, bytes);
	shell_print(sh, "rec get: %d of %u bytes, crc32 %08x, %d Hz, 16-bit, mono", sent,
		    (unsigned int)bytes, crc, MIC_SAMPLE_RATE_HZ);
	return sent < 0 ? sent : 0;
}

/*
 * While the capture runs: "rEC" on the big digits (an R draws whole on the
 * weekday only) and the red LED blinking, 100 ms every 500 ms. That is the cue
 * asked for on 2026-09-23, the beeps used until then being hard to hear
 * through the case. The main loop is held in the capture, so a timer toggles
 * the LED as a GPIO, which it is again once the PWM of the LEDs has stopped.
 * A take in the storage flash adds its number on the units of the day of
 * month, a full digit, where the armed list showed it before the press.
 */
#define REC_BLINK_ON_MS  100
#define REC_BLINK_OFF_MS 400

static const struct gpio_dt_spec rec_led = GPIO_DT_SPEC_GET(DT_NODELABEL(led_red), gpios);
static bool rec_led_lit;

static void rec_blink(struct k_timer *timer)
{
	rec_led_lit = !rec_led_lit;
	(void)gpio_pin_set_dt(&rec_led, rec_led_lit);
	k_timer_start(timer, K_MSEC(rec_led_lit ? REC_BLINK_ON_MS : REC_BLINK_OFF_MS), K_NO_WAIT);
}

static K_TIMER_DEFINE(rec_blink_timer, rec_blink, NULL);

static void rec_cue(bool on, unsigned int take)
{
	lcd_all_pixels(false);
	lcd_clear();
	if (on) {
		lcd_display_string("rEC", 5);
	}
	if (on && take > 0) {
		const char digit[2] = { (char)('0' + take % 10U), '\0' };

		lcd_display_string(digit, 3);
	}
	(void)lcd_flush();
	if (!on) {
		k_timer_stop(&rec_blink_timer);
		(void)gpio_pin_set_dt(&rec_led, 0);
		return;
	}
	if (!lcd_is_on()) {
		(void)lcd_display_power(true); /* the main loop puts it back to sleep */
	}
	(void)pwm_set_pulse_dt(&pwm_leds[0], 0);
	rec_led_lit = true;
	(void)gpio_pin_configure_dt(&rec_led, GPIO_OUTPUT_ACTIVE);
	k_timer_start(&rec_blink_timer, K_MSEC(REC_BLINK_ON_MS), K_NO_WAIT);
}

int hwtest_cmd_rec(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t ms;
	int got;

	if (argc >= 2 && strcmp(argv[1], "get") == 0) {
		return rec_get(sh);
	}
	if (argc >= 2 && strcmp(argv[1], "info") == 0) {
		shell_print(sh, "%u samples held (%u ms), gain %+d dB", (unsigned int)rec_count,
			    (unsigned int)(rec_count * 1000 / MIC_SAMPLE_RATE_HZ), mic_gain_db());
		return 0;
	}
	ms = argc >= 2 ? strtoul(argv[1], NULL, 10) : CONFIG_CB91AI_REC_MS;
	if (ms < 100 || ms > CONFIG_CB91AI_REC_MS) {
		shell_print(sh, "usage: cb91ai rec [100 to %d ms] | rec get | rec info",
			    CONFIG_CB91AI_REC_MS);
		return -EINVAL;
	}
	/* The cue from the start: the microphone and the PDM filter settle for a
	 * quarter of a second before the first sample kept, about the time a
	 * wearer takes to start speaking */
	rec_cue(true, 0);
	selftest_wdt_feed();
	got = mic_capture(rec_buf, ms * (MIC_SAMPLE_RATE_HZ / 1000), true);
	rec_cue(false, 0);
	selftest_wdt_feed();
	if (got < 0) {
		rec_count = 0;
		shell_print(sh, "capture failed (%d)", got);
		return got;
	}
	rec_count = (size_t)got;
	shell_print(sh, "rec: %u samples at %d Hz (%u ms), gain %+d dB, high-pass on",
		    (unsigned int)rec_count, MIC_SAMPLE_RATE_HZ, ms, mic_gain_db());
	for (size_t at = 0; at < rec_count; at += REC_WINDOW) {
		char what[32];
		unsigned int t10 = at * 10U / MIC_SAMPLE_RATE_HZ;

		snprintk(what, sizeof(what), "  %u.%u to %u.%u s", t10 / 10, t10 % 10,
			 (t10 + 4) / 10, (t10 + 4) % 10);
		rec_level(sh, &rec_buf[at], MIN(REC_WINDOW, rec_count - at), what);
	}
	rec_level(sh, rec_buf, rec_count, "whole");
	return 0;
}

/* ---- Voice takes in the storage flash (0.1.52) ------------------------------- */

/*
 * Longer than the RAM holds, for the reference sentences of lots K1 and K3:
 * a take of up to 6 s goes from the microphone straight to the QSPI flash,
 * into one of five slots of 192 KB, so that the wearer reads five sentences at
 * his own pace, one per press of ALARM (`arm`), and the PC fetches them all
 * afterwards (`take get`, tools/ble_shell.py --wav). The way lot E1 will
 * record a note, less the codec. A slot is erased just before its take; its
 * first page holds a header, written once the take is over, and the samples
 * follow. The last 64 KB block, where `qspi` and the boot self-test write
 * their pattern, stays out of the slots.
 */
#define TAKE_SLOTS      5U
#define TAKE_SLOT_SIZE  (192U * 1024U)
#define TAKE_ERASE_SIZE (64U * 1024U)
#define TAKE_PAGE       256U
#define TAKE_BLOCK_MS   100U /* the blocks of the microphone: whole ones only */
#define TAKE_MS_DEFAULT 6000U
#define TAKE_MS_MAX                                                                                \
	(((TAKE_SLOT_SIZE - TAKE_PAGE) / sizeof(int16_t)) * 1000U / MIC_SAMPLE_RATE_HZ /           \
	 TAKE_BLOCK_MS * TAKE_BLOCK_MS)
#define TAKE_MAGIC   0x4b544243U /* "CBTK" */
#define TAKE_VERSION 1U

BUILD_ASSERT(TAKE_SLOTS * TAKE_SLOT_SIZE <= 0x100000U - TAKE_ERASE_SIZE,
	     "the takes leave the last 64 KB block of the flash to its checks");

/* The first page of a slot; levels in tenths of a dB below full scale */
struct take_header {
	uint32_t magic;
	uint8_t version;
	uint8_t slot;
	int8_t gain_db;
	uint8_t high_pass_hz;
	uint32_t rate_hz;
	uint32_t samples;
	uint32_t crc32;      /* of the samples, as the microphone gave them */
	int16_t tz_minutes;
	uint16_t erase_ms;
	int64_t utc_ms;      /* start of the take, 0 if the clock was not set */
	int16_t rms_db10;    /* the whole take */
	int16_t loud_db10;   /* the loudest 100 ms block: the voice */
	int16_t quiet_db10;  /* the quietest: the floor */
	int16_t peak_db10;
	uint32_t clipped;
	uint32_t reserved;
};

struct take_run {
	off_t at;          /* flash offset of the next sample */
	uint32_t crc;
	uint64_t sum_sq;
	uint64_t loud_sq;  /* mean squares of the loudest and quietest blocks */
	uint64_t quiet_sq;
	uint32_t blocks;
	uint32_t peak;
	uint32_t clipped;
};

static int16_t db10(double amplitude)
{
	return (int16_t)(amplitude > 0.5 ? CLAMP(200.0 * log10(amplitude / 32768.0) - 0.5, -999.0, 0.0)
					 : -999.0);
}

/* Never across a 256-byte page of the flash: one page program per write */
static int take_write(off_t at, const uint8_t *data, size_t len)
{
	while (len > 0) {
		const size_t n = MIN(len, TAKE_PAGE - (size_t)(at % TAKE_PAGE));
		const int ret = flash_write(qspi_flash, at, data, n);

		if (ret) {
			return ret;
		}
		at += (off_t)n;
		data += n;
		len -= n;
	}
	return 0;
}

/* From mic_stream(), every 100 ms: levels, CRC, and the block to the flash */
static int take_sink(const int16_t *samples, size_t count, void *ctx)
{
	struct take_run *run = ctx;
	uint64_t block_sq = 0;
	int ret;

	for (size_t i = 0; i < count; i++) {
		const int32_t v = samples[i];
		const uint32_t a = (uint32_t)abs(v);

		block_sq += (uint64_t)((int64_t)v * v);
		run->peak = MAX(run->peak, a);
		if (a >= 32767) {
			run->clipped++;
		}
	}
	if (count > 0) {
		const uint64_t mean_sq = block_sq / count;

		run->sum_sq += block_sq;
		run->loud_sq = MAX(run->loud_sq, mean_sq);
		run->quiet_sq = run->blocks == 0 ? mean_sq : MIN(run->quiet_sq, mean_sq);
		run->blocks++;
	}
	run->crc = crc32_ieee_update(run->crc, (const uint8_t *)samples, count * sizeof(int16_t));
	ret = take_write(run->at, (const uint8_t *)samples, count * sizeof(int16_t));
	run->at += (off_t)(count * sizeof(int16_t));
	selftest_wdt_feed();
	return ret;
}

/* "-40.1" from -401 */
static const char *db10_text(char buf[8], int16_t db10)
{
	const unsigned int below = (unsigned int)abs(db10);

	snprintk(buf, 8, "%s%u.%u", db10 < 0 ? "-" : "", below / 10U, below % 10U);
	return buf;
}

static void take_print(const struct shell *sh, const struct take_header *hdr)
{
	char when[32] = "clock not set";
	char rms[8], loud[8], quiet[8], peak[8];

	if (hdr->utc_ms != 0) {
		const time_t local = (time_t)(hdr->utc_ms / 1000) + hdr->tz_minutes * 60;
		struct tm tm;

		if (gmtime_r(&local, &tm) != NULL) {
			/* Bounded, so that the compiler sees the text fit */
			snprintk(when, sizeof(when), "%04u-%02u-%02u %02u:%02u:%02u",
				 (unsigned int)(tm.tm_year + 1900) % 10000U,
				 (unsigned int)(tm.tm_mon + 1) % 100U, (unsigned int)tm.tm_mday % 100U,
				 (unsigned int)tm.tm_hour % 100U, (unsigned int)tm.tm_min % 100U,
				 (unsigned int)tm.tm_sec % 100U);
		}
	}
	shell_print(sh, "take %u: %u ms at %u Hz, gain %+d dB, high-pass %u Hz, %s; rms %s, voice %s, "
		    "floor %s, peak %s dBFS, %u clipped; erase %u ms", hdr->slot,
		    (unsigned int)(hdr->samples * 1000ULL / hdr->rate_hz), hdr->rate_hz, hdr->gain_db,
		    hdr->high_pass_hz, when, db10_text(rms, hdr->rms_db10),
		    db10_text(loud, hdr->loud_db10), db10_text(quiet, hdr->quiet_db10),
		    db10_text(peak, hdr->peak_db10), hdr->clipped, hdr->erase_ms);
}

static int take_record(const struct shell *sh, unsigned int slot, uint32_t ms)
{
	static struct take_header hdr;
	const off_t base = (off_t)(slot - 1U) * TAKE_SLOT_SIZE;
	struct take_run run = { .at = base + TAKE_PAGE };
	const int64_t utc_ms = clock_now_ms();
	const int64_t t0 = k_uptime_get();
	int got, ret = 0;

	/* A block at a time, the watchdog fed in between */
	for (uint32_t done = 0; done < TAKE_SLOT_SIZE && ret == 0; done += TAKE_ERASE_SIZE) {
		ret = flash_erase(qspi_flash, base + (off_t)done, TAKE_ERASE_SIZE);
		selftest_wdt_feed();
	}
	if (ret) {
		shell_print(sh, "take %u: erase failed (%d)", slot, ret);
		return ret;
	}
	memset(&hdr, 0, sizeof(hdr));
	hdr.erase_ms = (uint16_t)MIN(k_uptime_get() - t0, UINT16_MAX);
	rec_cue(true, slot);
	got = mic_stream(ms * (MIC_SAMPLE_RATE_HZ / 1000U), true, take_sink, &run);
	rec_cue(false, 0);
	selftest_wdt_feed();
	if (got <= 0) {
		shell_print(sh, "take %u: capture failed (%d)", slot, got);
		return got < 0 ? got : -EIO;
	}
	hdr.magic = TAKE_MAGIC;
	hdr.version = TAKE_VERSION;
	hdr.slot = (uint8_t)slot;
	hdr.gain_db = (int8_t)mic_gain_db();
	hdr.high_pass_hz = CONFIG_CB91AI_MIC_HIGH_PASS_HZ;
	hdr.rate_hz = MIC_SAMPLE_RATE_HZ;
	hdr.samples = (uint32_t)got;
	hdr.crc32 = run.crc;
	hdr.tz_minutes = clock_tz_minutes();
	hdr.utc_ms = utc_ms;
	hdr.rms_db10 = db10(sqrt((double)run.sum_sq / (double)got));
	hdr.loud_db10 = db10(sqrt((double)run.loud_sq));
	hdr.quiet_db10 = db10(sqrt((double)run.quiet_sq));
	hdr.peak_db10 = db10((double)run.peak);
	hdr.clipped = run.clipped;
	ret = take_write(base, (const uint8_t *)&hdr, sizeof(hdr));
	if (ret) {
		shell_print(sh, "take %u: header not written (%d)", slot, ret);
		return ret;
	}
	take_print(sh, &hdr);
	return 0;
}

static int take_header_read(unsigned int slot, struct take_header *hdr)
{
	const int ret = flash_read(qspi_flash, (off_t)(slot - 1U) * TAKE_SLOT_SIZE, hdr, sizeof(*hdr));

	if (ret) {
		return ret;
	}
	if (hdr->magic != TAKE_MAGIC || hdr->version != TAKE_VERSION || hdr->slot != slot ||
	    hdr->rate_hz != MIC_SAMPLE_RATE_HZ || hdr->samples == 0 ||
	    hdr->samples > (TAKE_SLOT_SIZE - TAKE_PAGE) / sizeof(int16_t)) {
		return -ENODATA;
	}
	return 0;
}

/* Read from the flash a kilobyte at a time and streamed to the host; the CRC of
 * what the flash gave back is checked against the one of the capture */
static int take_get(const struct shell *sh, unsigned int slot)
{
	static uint8_t buf[1024] __aligned(4);
	const off_t base = (off_t)(slot - 1U) * TAKE_SLOT_SIZE + TAKE_PAGE;
	struct take_header hdr;
	uint32_t bytes, at, crc = 0;
	int ret = take_header_read(slot, &hdr);

	if (ret) {
		shell_print(sh, "take %u: nothing recorded (%d): cb91ai take %u [ms]", slot, ret, slot);
		return ret;
	}
	bytes = hdr.samples * sizeof(int16_t);
	for (at = 0; at < bytes && ret == 0;) {
		const size_t n = MIN(sizeof(buf), bytes - at);
		int sent;

		ret = flash_read(qspi_flash, base + (off_t)at, buf, n);
		if (ret) {
			break;
		}
		crc = crc32_ieee_update(crc, buf, n);
		sent = remote_stream_at(buf, n, at);
		if (sent < 0) {
			ret = sent;
			break;
		}
		at += (uint32_t)sent;
	}
	shell_print(sh, "take get %u: %u of %u bytes, crc32 %08x, %d Hz, 16-bit, mono (flash %s)", slot,
		    at, bytes, hdr.crc32, MIC_SAMPLE_RATE_HZ,
		    at < bytes ? "not read through" : crc == hdr.crc32 ? "matches the capture" :
		    "DIFFERS FROM THE CAPTURE");
	take_print(sh, &hdr);
	return ret;
}

int hwtest_cmd_take(const struct shell *sh, size_t argc, char **argv)
{
	const bool get = argc >= 2 && strcmp(argv[1], "get") == 0;
	unsigned int slot;
	uint32_t ms;

	if (!device_is_ready(qspi_flash)) {
		shell_print(sh, "QSPI flash not ready");
		return -ENODEV;
	}
	if (argc == 2 && strcmp(argv[1], "info") == 0) {
		for (slot = 1; slot <= TAKE_SLOTS; slot++) {
			struct take_header hdr;
			const int ret = take_header_read(slot, &hdr);

			if (ret) {
				shell_print(sh, "take %u: nothing recorded (%d)", slot, ret);
			} else {
				take_print(sh, &hdr);
			}
		}
		return 0;
	}
	slot = argc >= 2 + get ? strtoul(argv[1 + get], NULL, 10) : 0;
	ms = argc >= 3 && !get ? strtoul(argv[2], NULL, 10) : TAKE_MS_DEFAULT;
	if (slot < 1 || slot > TAKE_SLOTS || ms < TAKE_BLOCK_MS || ms > TAKE_MS_MAX ||
	    ms % TAKE_BLOCK_MS != 0) {
		shell_print(sh, "usage: cb91ai take <1 to %u> [ms, in steps of %u, up to %u] | take get <1 "
			    "to %u> | take info", TAKE_SLOTS, TAKE_BLOCK_MS, TAKE_MS_MAX, TAKE_SLOTS);
		return -EINVAL;
	}
	return get ? take_get(sh, slot) : take_record(sh, slot, ms);
}

#else

int hwtest_cmd_rec(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "not in this image (CONFIG_CB91AI_REC_MS = 0): see cb91ai codec");
	return -ENOTSUP;
}

int hwtest_cmd_take(const struct shell *sh, size_t argc, char **argv)
{
	return hwtest_cmd_rec(sh, argc, argv);
}

#endif /* CONFIG_CB91AI_REC_MS > 0 */

/* ---- Accelerometer interrupt lines ---------------------------------------------- */

/*
 * BMA400 registers used here, from its datasheet (BST-BMA400-DS000, revision
 * 2.3, register map p.47 and p.48, interrupt behaviour p.26 and p.35). The
 * data ready interrupt fires at every sample (100 Hz, as accel_init() sets
 * it); latched, it holds its pin up until INT_STAT0 is read, which a GPIO read
 * sees without any timing care.
 */
#define BMA400_INT_STAT0   0x0e /* reading it releases a latched data ready */
#define BMA400_ACC_CONFIG0 0x19 /* power_mode<1:0>: 0 sleep, 2 normal */
#define BMA400_INT_CONFIG0 0x1f /* bit 7: data ready interrupt enabled */
#define BMA400_INT_CONFIG1 0x20 /* bit 7: latched interrupts */
#define BMA400_INT1_MAP    0x21 /* bit 7: data ready on INT1 */
#define BMA400_INT2_MAP    0x22 /* bit 7: data ready on INT2 */
#define BMA400_INT12_IO    0x24 /* reset 0x22: both pins push-pull, active high */
#define BMA400_BIT7        0x80
#define BMA400_NORMAL      0x02
#define BMA400_SLEEP       0x00

static const struct i2c_dt_spec bma400 = I2C_DT_SPEC_GET(DT_NODELABEL(bma400));
static const struct gpio_dt_spec acc_int[] = {
	GPIO_DT_SPEC_GET(DT_NODELABEL(bma400), int1_gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(bma400), int2_gpios),
};

struct line_result {
	int rest;     /* level with nothing mapped: 0 expected */
	int raised;   /* level with data ready mapped and latched: 1 expected */
	int other;    /* the other line meanwhile: 0 expected */
	int released; /* reads at 0 right after INT_STAT0, out of LINE_TRIES */
};
#define LINE_TRIES 5

static int line_check(int line, struct line_result *r)
{
	uint8_t stat;
	int ret;

	ret = i2c_reg_write_byte_dt(&bma400, BMA400_INT1_MAP, line == 0 ? BMA400_BIT7 : 0);
	ret = ret ? ret : i2c_reg_write_byte_dt(&bma400, BMA400_INT2_MAP, line == 1 ? BMA400_BIT7 : 0);
	ret = ret ? ret : i2c_reg_read_byte_dt(&bma400, BMA400_INT_STAT0, &stat);
	if (ret) {
		return ret;
	}
	k_msleep(35); /* three samples or so: the latch is up */
	r->raised = gpio_pin_get_raw(acc_int[line].port, acc_int[line].pin);
	r->other = gpio_pin_get_raw(acc_int[1 - line].port, acc_int[1 - line].pin);
	r->released = 0;
	for (int i = 0; i < LINE_TRIES; i++) {
		/* Released by the read, raised again by the next sample 10 ms later:
		 * the pin is read at once, then the latch is left to rise again */
		ret = i2c_reg_read_byte_dt(&bma400, BMA400_INT_STAT0, &stat);
		if (ret) {
			return ret;
		}
		r->released += gpio_pin_get_raw(acc_int[line].port, acc_int[line].pin) == 0;
		k_msleep(25);
	}
	return 0;
}

int hwtest_accel_int(const struct shell *sh)
{
	struct line_result lines[2];
	uint8_t stat;
	int ret;

	if (!i2c_is_ready_dt(&bma400) || !gpio_is_ready_dt(&acc_int[0]) ||
	    !gpio_is_ready_dt(&acc_int[1])) {
		shell_print(sh, "I2C or GPIO not ready");
		return -ENODEV;
	}
	/* Pull-down on the nRF side: a line open between the chips reads 0 */
	for (int i = 0; i < 2; i++) {
		(void)gpio_pin_configure_dt(&acc_int[i], GPIO_INPUT | GPIO_PULL_DOWN);
	}
	ret = accel_init(); /* soft reset: pins back to push-pull, active high */
	k_busy_wait(100);
	lines[0].rest = gpio_pin_get_raw(acc_int[0].port, acc_int[0].pin);
	lines[1].rest = gpio_pin_get_raw(acc_int[1].port, acc_int[1].pin);
	ret = ret ? ret : i2c_reg_write_byte_dt(&bma400, BMA400_INT_CONFIG1, BMA400_BIT7);
	ret = ret ? ret : i2c_reg_write_byte_dt(&bma400, BMA400_INT_CONFIG0, BMA400_BIT7);
	ret = ret ? ret : i2c_reg_write_byte_dt(&bma400, BMA400_ACC_CONFIG0, BMA400_NORMAL);
	k_msleep(2); /* 1.5 ms from sleep to normal mode (datasheet p.13) */
	ret = ret ? ret : line_check(0, &lines[0]);
	ret = ret ? ret : line_check(1, &lines[1]);
	/* Back to the rest state of the self-test: nothing mapped, sensor asleep */
	(void)i2c_reg_write_byte_dt(&bma400, BMA400_INT_CONFIG0, 0);
	(void)i2c_reg_write_byte_dt(&bma400, BMA400_INT1_MAP, 0);
	(void)i2c_reg_write_byte_dt(&bma400, BMA400_INT2_MAP, 0);
	(void)i2c_reg_write_byte_dt(&bma400, BMA400_INT_CONFIG1, 0);
	(void)i2c_reg_write_byte_dt(&bma400, BMA400_ACC_CONFIG0, BMA400_SLEEP);
	(void)i2c_reg_read_byte_dt(&bma400, BMA400_INT_STAT0, &stat);
	for (int i = 0; i < 2; i++) {
		(void)gpio_pin_configure_dt(&acc_int[i], GPIO_DISCONNECTED);
	}
	if (ret) {
		shell_print(sh, "BMA400 did not answer (%d)", ret);
		return ret;
	}
	for (int i = 0; i < 2; i++) {
		/* A sample may land between the release and the read: one miss allowed */
		const bool ok = lines[i].rest == 0 && lines[i].raised == 1 && lines[i].other == 0 &&
				lines[i].released >= LINE_TRIES - 1;

		shell_print(sh, "INT%d (P1.%02d): at rest %d, data ready latched %d, other line %d, "
			    "released %d/%d: %s", i + 1, acc_int[i].pin, lines[i].rest,
			    lines[i].raised, lines[i].other, lines[i].released, LINE_TRIES,
			    ok ? "OK" : "FAULT");
	}
	return 0;
}

/* ---- Commands armed on ALARM ------------------------------------------------- */

/*
 * A list of steps, each a command and a count: every press of ALARM runs the
 * first step with runs left, so that the wearer walks through a whole test at
 * his own pace. `arm <n> <command>` starts a new list, `arm add` extends it.
 */
#define ARM_STEPS    8
#define ARM_LINE     72
#define ARM_RESULTS  1400
#define ARM_MAX      20
#define ARM_QUIET_MS 1000 /* bounces, and the tail of the press */

static struct {
	struct {
		char line[ARM_LINE];
		int left;
	} steps[ARM_STEPS];
	size_t count;
	unsigned int runs;
	int64_t quiet_until;
} armed;

/* What the armed commands printed, for `cb91ai arm` */
static char arm_results[ARM_RESULTS];
static size_t arm_used;

static size_t arm_next_step(void)
{
	size_t step;

	for (step = 0; step < armed.count && armed.steps[step].left <= 0; step++) {
	}
	return step;
}

/*
 * Where the wearer is, on the glass (lesson of 2026-09-22): the number of the
 * next press on the units of the day of month, a full digit, and "PrESS", or
 * "End" once the list is done. Commands that draw on the glass themselves
 * (`lcd ...`) are left alone. The clock stays off until `arm off`.
 */
static void arm_show(void)
{
	char text[11];

	if (arm_next_step() < armed.count) {
		snprintk(text, sizeof(text), "   %u PrESS", (armed.runs + 1) % 10);
	} else {
		snprintk(text, sizeof(text), "       End");
	}
	selftest_clock_enable(false);
	selftest_display_hold(true);
	lcd_all_pixels(false);
	lcd_clear();
	lcd_display_string(text, 0);
	(void)lcd_flush();
}

static bool draws_on_glass(const char *line)
{
	return strncmp(line, "lcd", 3) == 0;
}

void hwtest_alarm_pressed(void)
{
	const int64_t now = k_uptime_get();
	const char *out;
	size_t len, step;
	int ret, n;

	if (now < armed.quiet_until) {
		return;
	}
	step = arm_next_step();
	if (step == armed.count) {
		return;
	}
	armed.steps[step].left--;
	armed.runs++;
	out = remote_run(armed.steps[step].line, &ret, &len);
	n = snprintk(&arm_results[arm_used], sizeof(arm_results) - arm_used,
		     "#%u at %u.%u s, %s (%d): %s", armed.runs, (unsigned int)(now / 1000),
		     (unsigned int)(now % 1000 / 100), armed.steps[step].line, ret, out);
	if (n > 0) {
		arm_used = MIN(arm_used + (size_t)n, sizeof(arm_results) - 1);
	}
	if (!draws_on_glass(armed.steps[step].line)) {
		arm_show();
	}
	armed.quiet_until = k_uptime_get() + ARM_QUIET_MS;
}

static int arm_step(const struct shell *sh, size_t argc, char **argv, size_t first)
{
	long count = strtol(argv[first], NULL, 10);
	char *line;
	size_t used = 0;

	if (count < 1 || count > ARM_MAX || strcmp(argv[first + 1], "arm") == 0) {
		shell_print(sh, "usage: cb91ai arm [add] <1 to %d> <command...>", ARM_MAX);
		return -EINVAL;
	}
	if (armed.count == ARM_STEPS) {
		shell_print(sh, "refused: %d steps at most", ARM_STEPS);
		return -ENOMEM;
	}
	line = armed.steps[armed.count].line;
	for (size_t i = first + 1; i < argc; i++) {
		int n = snprintk(&line[used], ARM_LINE - used, "%s%s", i > first + 1 ? " " : "",
				 argv[i]);

		if (n < 0 || used + (size_t)n >= ARM_LINE) {
			shell_print(sh, "command too long");
			return -EINVAL;
		}
		used += (size_t)n;
	}
	armed.steps[armed.count].left = (int)count;
	armed.count++;
	shell_print(sh, "step %u armed: \"%s\" at %ld press(es) of ALARM", (unsigned int)armed.count,
		    line, count);
	return 0;
}

int hwtest_cmd_arm(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (argc >= 2 && strcmp(argv[1], "off") == 0) {
		armed.count = 0;
		/* Back to the clock, on demand */
		lcd_all_pixels(false);
		selftest_display_hold(false);
		selftest_clock_enable(true);
		shell_print(sh, "disarmed, clock back on the glass");
		return 0;
	}
	if (argc >= 4 && strcmp(argv[1], "add") == 0) {
		ret = arm_step(sh, argc, argv, 2);
		if (ret == 0) {
			arm_show();
		}
		return ret;
	}
	if (argc >= 3 && strcmp(argv[1], "add") != 0) {
		/* A new list */
		armed.count = 0;
		armed.runs = 0;
		arm_used = 0;
		arm_results[0] = '\0';
		armed.quiet_until = 0;
		ret = arm_step(sh, argc, argv, 1);
		if (ret == 0) {
			arm_show();
		}
		return ret;
	}
	if (argc >= 2) {
		shell_print(sh, "usage: cb91ai arm <n> <command...> | arm add <n> <command...> | "
			    "arm off | arm");
		return -EINVAL;
	}
	for (size_t i = 0; i < armed.count; i++) {
		shell_print(sh, "step %u: \"%s\", %d run(s) left", (unsigned int)(i + 1),
			    armed.steps[i].line, armed.steps[i].left);
	}
	shell_print(sh, "%u run(s) done%s", armed.runs, armed.count ? "" : ", nothing armed");
	if (arm_used > 0) {
		shell_print(sh, "%s", arm_results);
	}
	return 0;
}

/* ---- Storage flash -------------------------------------------------------------- */

/*
 * The notes will be written straight to the QSPI flash (EF-50): erase, program
 * and read back its last 4 KB sector on the coin cell, timed, with the pattern
 * of the boot self-test (which only reads it back when off USB). That sector
 * holds nothing else.
 */
#define QSPI_TEST_OFFSET (0x100000 - 4096)

int hwtest_cmd_qspi(const struct shell *sh, size_t argc, char **argv)
{
	static uint8_t pattern[256], back[256];
	uint8_t id[3] = { 0 };
	uint32_t t0, t1, t2, t3, t4;
	int32_t before = -1, after = -1;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!device_is_ready(qspi_flash)) {
		shell_print(sh, "QSPI flash not ready");
		return -ENODEV;
	}
	/* A different pattern at each run: with the same one, a missed erase would
	 * go unseen, the bits already being where they should */
	const uint8_t seed = (uint8_t)k_cycle_get_32();
	bool blank = true;

	for (size_t i = 0; i < sizeof(pattern); i++) {
		pattern[i] = (uint8_t)(i * 7 + seed);
	}
	memset(back, 0, sizeof(back));
	ret = flash_read_jedec_id(qspi_flash, id);
	(void)hwtest_rail_mv(&before);
	t0 = k_cycle_get_32();
	ret = ret ? ret : flash_erase(qspi_flash, QSPI_TEST_OFFSET, 4096);
	t1 = k_cycle_get_32();
	/* The erase is proven by reading the sector back blank */
	ret = ret ? ret : flash_read(qspi_flash, QSPI_TEST_OFFSET, back, sizeof(back));
	for (size_t i = 0; i < sizeof(back) && ret == 0; i++) {
		blank = blank && back[i] == 0xff;
	}
	shell_print(sh, "erase %s", ret ? "failed" : blank ? "verified: sector read back blank" :
		    "NOT VERIFIED: sector not blank after the erase");
	if (!blank) {
		return -EIO;
	}
	t2 = k_cycle_get_32();
	ret = ret ? ret : flash_write(qspi_flash, QSPI_TEST_OFFSET, pattern, sizeof(pattern));
	t3 = k_cycle_get_32();
	ret = ret ? ret : flash_read(qspi_flash, QSPI_TEST_OFFSET, back, sizeof(back));
	t4 = k_cycle_get_32();
	(void)hwtest_rail_mv(&after);
	selftest_wdt_feed();
	shell_print(sh, "QSPI flash, JEDEC %02x %02x %02x: 4 KB erased in %u ms, 256 B written in %u us, "
		    "read in %u us, %s (%d); rail %d mV before, %d mV after", id[0], id[1], id[2],
		    k_cyc_to_ms_floor32(t1 - t0), k_cyc_to_us_floor32(t3 - t2),
		    k_cyc_to_us_floor32(t4 - t3),
		    ret ? "failed" : memcmp(pattern, back, sizeof(back)) == 0 ? "read back OK" :
							    "READ BACK MISMATCH",
		    ret, before, after);
	return ret ? ret : memcmp(pattern, back, sizeof(back)) == 0 ? 0 : -EIO;
}

/* ---- Link --------------------------------------------------------------------- */

int hwtest_cmd_rssi(const struct shell *sh, size_t argc, char **argv)
{
	int8_t rssi;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ret = ble_conn_rssi(&rssi);
	if (ret) {
		shell_print(sh, "no RSSI (%d): no connection", ret);
		return ret;
	}
	shell_print(sh, "rssi %d dBm: the host as the watch hears it; watch tx power %d dBm", rssi,
		    ble_tx_power());
	return 0;
}

/* ---- Advertising trials (lot N1a) ---------------------------------------------- */

/*
 * How long a phone keeping a pending connection takes to get in once the watch
 * advertises (risk R11, ble.c). On the glass, the stopwatch of the F-91W:
 * minutes on the hours, the colon, seconds on the minutes, hundredths on the
 * seconds. While a burst runs it counts from the press, with "Ad" and the step
 * of the burst (1 to 3) on the day of month; once the phone is in it stops on
 * the delay, "Co" and the step the phone answered; "nO" and dashes when
 * nothing came in the window; "tr PrESS" before the first trial. A green pulse
 * greets a connection, a red one a trial without one: no beep, which the
 * wearer can hardly hear (2026-09-23).
 */
#define TRIAL_CUE_MS          30
#define TRIAL_MINUTES_DEFAULT 30
#define TRIAL_MINUTES_MAX     120

/* Defined in main.c */
void selftest_display_wake(void);

static const struct gpio_dt_spec cue_leds[] = {
	GPIO_DT_SPEC_GET(DT_NODELABEL(led_red), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(led_green), gpios),
};

static void trial_stopwatch(uint32_t ms, bool hundredths)
{
	char text[7];

	/* The hour tens of the V1 draws a "1" and nothing else: 19 min at most */
	snprintk(text, sizeof(text), "%2u%02u", MIN(ms / 60000U, 19U), ms / 1000U % 60U);
	lcd_display_string(text, 4);
	if (hundredths) {
		snprintk(text, sizeof(text), "%02u", ms / 10U % 100U);
		lcd_display_string(text, 8);
	}
	lcd_set_colon(true);
}

void hwtest_trial_show(const struct ble_trial_view *view)
{
	char step[2] = { 0 };

	lcd_clear();
	if (view->running) {
		lcd_display_string("Ad", 0);
		step[0] = (char)('1' + view->step);
		lcd_display_string(step, 3);
		trial_stopwatch(view->elapsed_ms, false);
	} else if (view->last.number == 0) {
		lcd_display_string("tr", 0);
		lcd_display_string("PrESS", 5);
	} else if (view->last.connect_ms != BLE_TRIAL_NONE) {
		lcd_display_string("Co", 0);
		step[0] = (char)('1' + view->last.step);
		lcd_display_string(step, 3);
		trial_stopwatch((uint32_t)view->last.connect_ms, true);
	} else {
		lcd_display_string("nO", 0);
		lcd_display_string("-----", 5);
	}
	(void)lcd_flush();
}

void hwtest_trial_cue(bool connected)
{
	const size_t led = connected ? 1 : 0;

	/* The PWM of the LEDs stops at a zero pulse and leaves the pin to the GPIO */
	(void)pwm_set_pulse_dt(&pwm_leds[led], 0);
	(void)gpio_pin_configure_dt(&cue_leds[led], GPIO_OUTPUT_ACTIVE);
	k_msleep(TRIAL_CUE_MS);
	(void)gpio_pin_set_dt(&cue_leds[led], 0);
}

/* One line per trial, short enough for the whole log to fit the 1600 bytes
 * that a command may send back over Bluetooth (remote.c) */
static void trial_print(const struct shell *sh, const struct ble_trial_entry *e, bool running)
{
	const uint16_t step_iv = ble_trial_step_interval(e->profile, e->step);
	char first_read[28];

	if (running || e->connect_ms == BLE_TRIAL_NONE) {
		shell_print(sh, "#%u at %u s %s: %s", e->number, e->at_s,
			    ble_trial_profile_name(e->profile),
			    running ? "burst running" : "no connection");
		return;
	}
	if (e->read_ms == BLE_TRIAL_NONE) {
		snprintk(first_read, sizeof(first_read), "no read");
	} else {
		snprintk(first_read, sizeof(first_read), "read +%u.%03u s",
			 (uint32_t)e->read_ms / 1000U, (uint32_t)e->read_ms % 1000U);
	}
	shell_print(sh, "#%u at %u s %s: in %d.%03d s, step %u (%u.%u ms), %s, link %u.%02u ms/%u/"
		    "%u ms, peer %02X:%02X:%02X%s",
		    e->number, e->at_s, ble_trial_profile_name(e->profile), e->connect_ms / 1000,
		    e->connect_ms % 1000, e->step + 1U, step_iv * 625U / 1000U,
		    step_iv * 625U / 100U % 10U, first_read, e->interval * 125U / 100U,
		    e->interval * 125U % 100U, e->latency, e->timeout * 10U, e->peer[0], e->peer[1],
		    e->peer[2], e->peer_random ? " random" : "");
}

static int trial_cmd(const struct shell *sh, size_t argc, char **argv)
{
	static struct ble_trial_entry log[BLE_TRIAL_LOG];
	struct ble_trial_view view;
	size_t n;

	/* argv[0] is "trial" */
	if (argc >= 2 && (strcmp(argv[1], "apple") == 0 || strcmp(argv[1], "link") == 0)) {
		const enum ble_trial_profile profile =
			strcmp(argv[1], "apple") == 0 ? BLE_TRIAL_APPLE : BLE_TRIAL_LINK;
		uint32_t minutes = argc >= 3 ? strtoul(argv[2], NULL, 10) : TRIAL_MINUTES_DEFAULT;

		if (minutes == 0 || minutes > TRIAL_MINUTES_MAX) {
			shell_print(sh, "refused: 1 to %u min", TRIAL_MINUTES_MAX);
			return -EINVAL;
		}
		/* The bursts start at a press: nothing may hold the buttons meanwhile */
		if (gest.running || probe.running || lcdwalk_active()) {
			shell_print(sh, "refused: the gestures, the probe or the segment walk hold the "
				    "buttons (keys gesture stop, keys stop, walk stop)");
			return -EBUSY;
		}
		ble_adv_hold_fast(0);
		(void)ble_trial_start(profile, minutes * 60U);
		lcd_all_pixels(false);
		selftest_clock_enable(true);
		selftest_display_wake();
		shell_print(sh, "trial mode, profile %s: the radio is silent once this host leaves, a "
			    "press on a case button starts a burst, the glass times it; back to normal "
			    "after %u min without a trial, or `adv trial off`", argv[1], minutes);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "go") == 0) {
		if (ble_trial_go_on_disconnect()) {
			shell_print(sh, "refused: trial mode is off (adv trial apple|link)");
			return -EPERM;
		}
		shell_print(sh, "a burst starts as soon as this host disconnects");
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "off") == 0) {
		ble_trial_stop();
		shell_print(sh, "trial mode off: normal advertising, clock back on the glass");
		return 0;
	}
	if (argc >= 2) {
		shell_print(sh, "usage: cb91ai adv trial apple|link [minutes] | adv trial go | "
			    "adv trial off | adv trial");
		return -EINVAL;
	}
	const bool on = ble_trial_view(&view);

	n = ble_trial_log(log, ARRAY_SIZE(log));
	shell_print(sh, "trial mode %s, advertising %s; %u trial(s) kept", on ? "on" : "off",
		    ble_adv_mode(), (unsigned int)n);
	for (size_t i = 0; i < n; i++) {
		trial_print(sh, &log[i], on && view.running && i == n - 1);
	}
	return 0;
}

int hwtest_cmd_adv(const struct shell *sh, size_t argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "trial") == 0) {
		return trial_cmd(sh, argc - 1, argv + 1);
	}
	if (argc >= 3 && strcmp(argv[1], "fast") == 0) {
		uint32_t minutes = strtoul(argv[2], NULL, 10);

		if (minutes == 0 || minutes > 120) {
			shell_print(sh, "refused: 1 to 120 min");
			return -EINVAL;
		}
		ble_trial_stop();
		ble_adv_hold_fast(minutes * 60U);
		shell_print(sh, "fast advertising (100 to 150 ms) held for %u min whenever no host is "
			    "connected", minutes);
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "normal") == 0) {
		ble_trial_stop();
		ble_adv_hold_fast(0);
		shell_print(sh, "advertising back to normal: fast for 30 s after a press or a "
			    "disconnection, slow otherwise");
		return 0;
	}
	shell_print(sh, "usage: cb91ai adv fast <minutes> | adv normal | adv trial ... (now %s)",
		    ble_adv_mode());
	return argc >= 2 ? -EINVAL : 0;
}
