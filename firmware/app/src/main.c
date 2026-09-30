/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bring-up self-test for the COBALT CB-91AI.
 *
 * At boot the firmware reports the chip identity and the UICR state, then
 * exercises each peripheral in turn and reports the result on the RTT shell:
 * LEDs, buzzer, battery ADC, BMA400 accelerometer (I2C), QSPI flash (JEDEC ID)
 * and the three case buttons. It then logs the battery voltage and button
 * levels periodically. The green heartbeat blink is off by default so that it
 * does not disturb visual tests (`cb91ai heartbeat on` turns it on).
 *
 * The same image runs on the bench board (USB, ST-Link) and on a board sealed
 * in the watch on a CR2016, updated over Bluetooth. Two things follow:
 * - power profile picked at boot from VBUS: with USB the full self-test runs;
 *   without it the battery-safe one does (specification 4.7: 30 ms beep, at
 *   50 % since 2026-09-23 where 4.7 asked 25 %, short LED pulses but for the
 *   colour wheel of the boot, no flash erase, no 3 s receiver check);
 * - the idle state is frugal on both: display on demand (on for 30 s after
 *   boot, then 10 s per button press, BU97930 in software standby otherwise),
 *   event-driven main loop, radio policy in ble.c.
 *
 * On the sealed watch the `cb91ai` commands come over Bluetooth instead of RTT
 * or USB (remote.c, tools/ble_shell.py), and the main loop runs them, as well
 * as the checks armed on the ALARM button (hwtest.c).
 *
 * Read the output with: pyocd rtt -t nrf52840
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel_version.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <nrfx.h>
#include <zephyr/app_version.h>
#include <zephyr/task_wdt/task_wdt.h>
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#endif
#if defined(CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS)
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_callbacks.h>
#endif

#include "ble.h"
#include "lcd.h"
#include "mic.h"
#include "accel.h"
#include "clock.h"
#include "hwtest.h"
#include "remote.h"
#include "update.h"

bool cb91ai_usb_configured(void);

static bool clock_enabled = true;
static bool heartbeat_enabled; /* off by default, `cb91ai heartbeat on` */

/* Shell command hook: stop or resume the green heartbeat blink, which shares
 * P0.04 with the green PWM LED and would override it
 */
void selftest_heartbeat_enable(bool enable)
{
	heartbeat_enabled = enable;
}

/*
 * Display on demand (first step of EF-44): the glass is lit for a while after
 * boot and after a button press, and the driver sleeps otherwise. Diagnostics
 * that need a steady display (shell lcd commands, segment walk) hold it on.
 */
#define DISPLAY_BOOT_MS (30 * 1000)
#define DISPLAY_WAKE_MS (10 * 1000)
/* Main loop period while nothing is displayed: the watchdog channel is 10 s */
#define IDLE_PERIOD_MS  5000

static int64_t display_until;
static atomic_t display_hold;
static atomic_t button_event;
/* ALARM pressed: runs the command armed on it (hwtest.c), the only case button
 * that reaches the firmware in the module since 2026-09-22 (V2-22) */
static atomic_t alarm_event;
#define BUTTON_ALARM 2
/* Button interrupts since boot per button (LIGHT, MODE, ALARM), bounces
 * included: `btn=` in the status line, so that a press can be checked on a
 * sealed watch */
static atomic_t button_irqs[3];
static K_SEM_DEFINE(wake_sem, 0, 1);

/* Shell command hook: stop or resume the uptime clock on the LCD */
void selftest_clock_enable(bool enable)
{
	clock_enabled = enable;
	k_sem_give(&wake_sem);
}

/* Shell and walk hook: keep the display on (true) or back on demand (false) */
void selftest_display_hold(bool hold)
{
	atomic_set(&display_hold, hold);
	k_sem_give(&wake_sem);
}

bool selftest_display_held(void)
{
	return atomic_get(&display_hold);
}

/* remote.c hook: a host wrote a command, the loop runs it; ble.c hook: a trial
 * began or ended, the loop redraws the glass */
void selftest_wake(void)
{
	k_sem_give(&wake_sem);
}

/* hwtest.c hook: light the display for a while, as a press does */
static atomic_t display_wake;

void selftest_display_wake(void)
{
	atomic_set(&display_wake, 1);
	k_sem_give(&wake_sem);
}

/*
 * EF-62: a freshly updated image boots "in test". It is confirmed after a grace
 * period spent running without a watchdog fault, or as soon as the host reads
 * the BLE debug/status characteristic (the accelerator below). Until it is
 * confirmed, MCUboot reverts to the previous image at the next reset.
 */
#define CONFIRM_GRACE_MS (120 * 1000)
static atomic_t confirm_requested;

/* Called from the BLE debug-characteristic read: confirm the pending image. */
void selftest_request_confirm(void)
{
	atomic_set(&confirm_requested, 1);
}

LOG_MODULE_REGISTER(cb91ai, LOG_LEVEL_INF);

/*
 * Task watchdog on the nRF WDT. It is started first thing in main(): the nRF
 * watchdog survives a soft reset, so after an update the one armed by the
 * previous image is still counting when the new image starts. Nobody feeds it
 * while MCUboot checks the image, nor until this image takes over - and the
 * boot self-test lasts longer than its window. Started late, the watchdog bites
 * during the first boot of every update, and MCUboot gives up an image that
 * was fine (seen on 2026-09-19, 0.1.20 to 0.1.21).
 */
static int wdt_channel = -1;

/* Reset cause of this boot (hwinfo RESET_* bits), kept for the BLE status line:
 * on a sealed watch it is the only way to tell a watchdog bite from a brown-out.
 */
static uint32_t boot_reset_cause;

static void wdt_kick(void)
{
	if (wdt_channel >= 0) {
		task_wdt_feed(wdt_channel);
	}
}

/* For the commands that hold the loop for seconds (hwtest.c, remote.c) */
void selftest_wdt_feed(void)
{
	wdt_kick();
}

/*
 * Window of the hardware watchdog really in force, in ms (0: not running).
 * A running nRF watchdog ignores writes to its reload value, and only a
 * hardware reset stops it: after an update over Bluetooth the window is still
 * the one of the image that started it, whatever this image asks for. An image
 * delivered over Bluetooth must therefore never feed less often than the
 * shortest window ever shipped. Shown in the status line: on a sealed watch
 * nothing else tells. Same arithmetic as nrfx_wdt (reload = ms * 32768 / 1000).
 */
#define WDT_WINDOW_WANTED_MS (CONFIG_TASK_WDT_MIN_TIMEOUT + CONFIG_TASK_WDT_HW_FALLBACK_DELAY)

static uint32_t wdt_window_ms(void)
{
	if ((NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk) == 0) {
		return 0;
	}
	return (uint32_t)(((uint64_t)NRF_WDT->CRV * 1000U) / 32768U);
}

/* ---- Devices from the board devicetree ---------------------------------- */

/* The heartbeat blinks the green LED as a GPIO; the boot drives all three
 * through their PWM channels (hwtest.c) */
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_NODELABEL(led_green), gpios);

static const struct gpio_dt_spec buttons[] = {
	GPIO_DT_SPEC_GET(DT_NODELABEL(button_light), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(button_mode), gpios),
	GPIO_DT_SPEC_GET(DT_NODELABEL(button_alarm), gpios),
};
static const char *const button_names[] = { "LIGHT", "MODE", "ALARM" };

static const struct pwm_dt_spec buzzer = PWM_DT_SPEC_GET(DT_NODELABEL(buzzer));

/* Internal SAADC inputs: VDD (the 3V rail) and VDDH / 5 (USB 5 V). Channel 0,
 * AIN6 behind the battery divider, is left unread: see test_battery() */
static const struct adc_dt_spec vdd_adc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 1);
static const struct adc_dt_spec vddh_adc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 2);

static const struct i2c_dt_spec bma400 = I2C_DT_SPEC_GET(DT_NODELABEL(bma400));
#define BMA400_REG_CHIPID 0x00
#define BMA400_CHIPID     0x90

static const struct device *const qspi_flash = DEVICE_DT_GET(DT_NODELABEL(zd25wq80c));

/* ---- Helpers ------------------------------------------------------------ */

static void report_chip(void)
{
	uint32_t cause = 0;
	char variant[5];

	/* FICR.INFO.VARIANT holds the build code as ASCII, e.g. "AAF0" */
	uint32_t v = NRF_FICR->INFO.VARIANT;
	variant[0] = (v >> 24) & 0xff;
	variant[1] = (v >> 16) & 0xff;
	variant[2] = (v >> 8) & 0xff;
	variant[3] = v & 0xff;
	variant[4] = '\0';

	uint32_t kv = sys_kernel_version_get();

	LOG_INF("COBALT CB-91AI self-test %s, Zephyr %u.%u.%u", APP_VERSION_STRING,
		SYS_KERNEL_VER_MAJOR(kv), SYS_KERNEL_VER_MINOR(kv), SYS_KERNEL_VER_PATCHLEVEL(kv));
	LOG_INF("nRF52840 variant %s, package 0x%08x, RAM %u kB, flash %u kB",
		variant, NRF_FICR->INFO.PACKAGE, NRF_FICR->INFO.RAM, NRF_FICR->INFO.FLASH);
	LOG_INF("device id %08x%08x", NRF_FICR->DEVICEID[1], NRF_FICR->DEVICEID[0]);

	uint32_t regout0 = NRF_UICR->REGOUT0 & UICR_REGOUT0_VOUT_Msk;
	static const char *const regout_names[] = {
		"1.8 V", "2.1 V", "2.4 V", "2.7 V", "3.0 V", "3.3 V", "reserved", "default (1.8 V)"
	};
	LOG_INF("UICR REGOUT0 = %s, APPROTECT = 0x%08x, PSELRESET = 0x%08x / 0x%08x, NFCPINS = 0x%08x",
		regout_names[regout0 & 0x7], NRF_UICR->APPROTECT,
		NRF_UICR->PSELRESET[0], NRF_UICR->PSELRESET[1], NRF_UICR->NFCPINS);

	LOG_INF("power: %s voltage mode, VBUS %s, USB regulator %s",
		(NRF_POWER->MAINREGSTATUS & POWER_MAINREGSTATUS_MAINREGSTATUS_Msk) ? "high" : "normal",
		(NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) ? "detected" : "absent",
		(NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_OUTPUTRDY_Msk) ? "ready" : "off");

	if (hwinfo_get_reset_cause(&cause) == 0) {
		boot_reset_cause = cause;
		LOG_INF("reset cause 0x%08x%s%s%s%s", cause,
			(cause & RESET_PIN) ? " [pin]" : "",
			(cause & RESET_SOFTWARE) ? " [software]" : "",
			(cause & RESET_POR) ? " [power-on]" : "",
			(cause & RESET_DEBUG) ? " [debug]" : "");
		hwinfo_clear_reset_cause();
	}
}

/* USB present: bench board, full self-test. Absent: possibly a coin cell. */
static bool on_usb(void)
{
	return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}

/*
 * LEDs at boot: one turn of the colour wheel in 1.5 s, over the boot screen,
 * the same on USB and on the coin cell (2026-09-23; until then red,
 * green and blue one after the other, 40 ms each on the cell). Green and blue
 * have no series resistor: their channels stay in standard drive. The green
 * LED is then set up as a GPIO output for the heartbeat.
 */
static int test_leds(void)
{
	int ret = hwtest_led_hue(NULL);

	if (ret) {
		LOG_ERR("LEDs: colour wheel failed (%d)", ret);
		return ret;
	}
	LOG_INF("LEDs: one turn of the colour wheel");
	if (!gpio_is_ready_dt(&led_green)) {
		LOG_ERR("LED green: GPIO not ready");
		return -ENODEV;
	}
	return gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
}

/*
 * Off USB the beep is 30 ms long, at 50 % duty since the decision of
 * 2026-09-23: specification 4.7 (EF-47) asked for 25 %, for fear of what a
 * CR2016 makes of the buzzer, and the cell in the watch held 500 ms at 50 %
 * with a quarter of a volt of sag and no reset. The same
 * beep whatever the cell voltage (2026-09-22).
 */
static int test_buzzer(bool bench)
{
	const int on_ms = bench ? 150 : 30;
	const uint32_t pulse = buzzer.period / 2;

	if (!pwm_is_ready_dt(&buzzer)) {
		LOG_ERR("buzzer: PWM not ready");
		return -ENODEV;
	}
	/* 4 kHz square wave (period from the devicetree) */
	LOG_INF("buzzer: 4 kHz for %d ms at 50 %%", on_ms);
	int ret = pwm_set_dt(&buzzer, buzzer.period, pulse);
	if (ret) {
		LOG_ERR("buzzer: pwm_set failed (%d)", ret);
		return ret;
	}
	k_msleep(on_ms);
	pwm_set_dt(&buzzer, buzzer.period, 0);
	return 0;
}

static int read_channel_mv(const struct adc_dt_spec *spec, int32_t *mv_out)
{
	int16_t sample;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};
	int ret;

	ret = adc_sequence_init_dt(spec, &seq);
	if (ret) {
		return ret;
	}
	ret = adc_read_dt(spec, &seq);
	if (ret) {
		return ret;
	}
	int32_t mv = sample;
	ret = adc_raw_to_millivolts_dt(spec, &mv);
	if (ret) {
		return ret;
	}
	*mv_out = mv;
	return 0;
}

/* VDD rail and VDDH (USB 5 V, measured through the internal /5 divider) */
static int read_supplies_mv(int32_t *vdd_mv, int32_t *vddh_mv)
{
	int ret = read_channel_mv(&vdd_adc, vdd_mv);

	if (ret) {
		return ret;
	}
	ret = read_channel_mv(&vddh_adc, vddh_mv);
	if (ret) {
		return ret;
	}
	*vddh_mv *= 5;
	return 0;
}

/*
 * The cell is read on the VDD input of the SAADC, with no divider: on the
 * bench board Q1 and the AIN6 divider (R2, R3) are gone since 2026-09-23 and
 * the cell sits on the rail, and the V2 has neither (V2-34, V2-41). AIN6 now
 * floats there and is no longer read. On a V1 board left untouched, VDD is the
 * cell less the drop in Q1, a quarter of a volt: a pessimistic reading.
 */
static int test_battery(void)
{
	const struct adc_dt_spec *channels[] = { &vdd_adc, &vddh_adc };
	int32_t vdd_mv, vddh_mv;

	if (!adc_is_ready_dt(&vdd_adc)) {
		LOG_ERR("battery: ADC not ready");
		return -ENODEV;
	}
	for (size_t i = 0; i < ARRAY_SIZE(channels); i++) {
		int ret = adc_channel_setup_dt(channels[i]);

		if (ret) {
			LOG_ERR("battery: channel %d setup failed (%d)", channels[i]->channel_id, ret);
			return ret;
		}
	}
	int ret = read_supplies_mv(&vdd_mv, &vddh_mv);
	if (ret) {
		LOG_ERR("supplies: read failed (%d)", ret);
		return ret;
	}
	LOG_INF("supplies: VDD rail = %d mV (the cell, or REG0 on USB), VDDH (USB 5 V) = %d mV",
		vdd_mv, vddh_mv);
	return 0;
}

static int test_accelerometer(void)
{
	uint8_t id = 0;

	if (!i2c_is_ready_dt(&bma400)) {
		LOG_ERR("BMA400: I2C bus not ready");
		return -ENODEV;
	}
	int ret = i2c_reg_read_byte_dt(&bma400, BMA400_REG_CHIPID, &id);
	if (ret) {
		LOG_ERR("BMA400: no answer at 0x%02x (%d)", bma400.addr, ret);
		return ret;
	}
	if (id == BMA400_CHIPID) {
		LOG_INF("BMA400: chip id 0x%02x OK", id);
	} else {
		LOG_WRN("BMA400: unexpected chip id 0x%02x (expected 0x%02x)", id, BMA400_CHIPID);
	}
	return 0;
}

static int test_flash(bool bench)
{
	uint8_t id[3] = { 0 };

	if (!device_is_ready(qspi_flash)) {
		LOG_ERR("QSPI flash: driver not ready (JEDEC ID mismatch or no answer)");
		return -ENODEV;
	}
	int ret = flash_read_jedec_id(qspi_flash, id);
	if (ret) {
		LOG_ERR("QSPI flash: JEDEC ID read failed (%d)", ret);
		return ret;
	}
	LOG_INF("QSPI flash: JEDEC ID %02x %02x %02x", id[0], id[1], id[2]);

	/* Erase, program and read back the last 4 kB sector, then time a read */
	static uint8_t pattern[256], readback[256];
	const off_t test_offset = 0x100000 - 4096;
	uint32_t t0, t1;

	for (size_t i = 0; i < sizeof(pattern); i++) {
		pattern[i] = (uint8_t)(i * 7 + 3);
	}
	if (!bench) {
		/* A sector erase draws several mA for tens of ms at every boot, for
		 * nothing on a watch: off USB, only read back what the bench wrote.
		 */
		ret = flash_read(qspi_flash, test_offset, readback, sizeof(readback));
		if (ret) {
			LOG_ERR("QSPI flash: read failed (%d)", ret);
			return ret;
		}
		LOG_INF("QSPI flash: read of 256 B at 0x%lx OK, %s (battery-safe: no erase)",
			(unsigned long)test_offset,
			memcmp(pattern, readback, sizeof(pattern)) == 0 ? "bench pattern found"
									 : "no bench pattern");
		return 0;
	}
	ret = flash_erase(qspi_flash, test_offset, 4096);
	if (ret) {
		LOG_ERR("QSPI flash: erase failed (%d)", ret);
		return ret;
	}
	ret = flash_write(qspi_flash, test_offset, pattern, sizeof(pattern));
	if (ret) {
		LOG_ERR("QSPI flash: write failed (%d)", ret);
		return ret;
	}
	t0 = k_cycle_get_32();
	ret = flash_read(qspi_flash, test_offset, readback, sizeof(readback));
	t1 = k_cycle_get_32();
	if (ret) {
		LOG_ERR("QSPI flash: read failed (%d)", ret);
		return ret;
	}
	if (memcmp(pattern, readback, sizeof(pattern)) != 0) {
		LOG_ERR("QSPI flash: read back mismatch at sector 0x%lx", (unsigned long)test_offset);
		return -EIO;
	}
	LOG_INF("QSPI flash: erase, program and read back of 256 B at 0x%lx OK, read in %u us (%s)",
		(unsigned long)test_offset, (unsigned int)k_cyc_to_us_floor32(t1 - t0),
		IS_ENABLED(CONFIG_NORDIC_QSPI_NOR) ? "quad I/O" : "single");
	return 0;
}

static int buttons_probe_configure(gpio_flags_t pull)
{
	/* Raw configuration, ignoring the devicetree flags, for the polarity probe */
	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
		if (!gpio_is_ready_dt(&buttons[i])) {
			LOG_ERR("button %s: GPIO not ready", button_names[i]);
			return -ENODEV;
		}
		int ret = gpio_pin_configure(buttons[i].port, buttons[i].pin, GPIO_INPUT | pull);
		if (ret) {
			LOG_ERR("button %s: configure failed (%d)", button_names[i], ret);
			return ret;
		}
	}
	return 0;
}

static int buttons_configure_from_dt(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
		int ret = gpio_pin_configure_dt(&buttons[i], GPIO_INPUT);
		if (ret) {
			LOG_ERR("button %s: configure failed (%d)", button_names[i], ret);
			return ret;
		}
	}
	return 0;
}

/*
 * A button press wakes the main loop: display on, fast advertising again. The
 * edges are detected through the GPIO SENSE mechanism (sense-edge-mask in the
 * board devicetree), which needs no running clock, unlike a GPIOTE IN channel.
 */
static struct gpio_callback button_cbs[ARRAY_SIZE(buttons)];
BUILD_ASSERT(ARRAY_SIZE(button_irqs) == ARRAY_SIZE(buttons));

static void button_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	const size_t index = cb - button_cbs;

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	atomic_set(&button_event, 1);
	atomic_inc(&button_irqs[index]);
	if (index == BUTTON_ALARM) {
		atomic_set(&alarm_event, 1);
	}
	k_sem_give(&wake_sem);
}

/* Also a hook for the segment walk, which polls the buttons with both pulls */
void selftest_buttons_irq(bool enable)
{
	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
		gpio_pin_interrupt_configure_dt(&buttons[i], enable ? GPIO_INT_EDGE_TO_ACTIVE
								     : GPIO_INT_DISABLE);
	}
}

static void buttons_irq_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
		gpio_init_callback(&button_cbs[i], button_isr, BIT(buttons[i].pin));
		gpio_add_callback(buttons[i].port, &button_cbs[i]);
	}
	selftest_buttons_irq(true);
}

static void buttons_report(const char *context)
{
	int raw[ARRAY_SIZE(buttons)];

	k_msleep(5);
	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
		raw[i] = gpio_pin_get_raw(buttons[i].port, buttons[i].pin);
	}
	LOG_INF("buttons %s: LIGHT=%d MODE=%d ALARM=%d (raw levels)", context, raw[0], raw[1], raw[2]);
}

/*
 * Battery Service level: a CR2016 reads about 3.0 V fresh and 2.0 V exhausted.
 * Without a cell (USB power), report the rail instead.
 */
static uint8_t battery_percent(int32_t vdd_mv)
{
	return (uint8_t)CLAMP((vdd_mv - 2000) / 10, 0, 100);
}

/*
 * LCD: reset the BU97930, light every segment for a moment, then the boot
 * screen chosen on 2026-09-23: "CB" on the weekday, "91" on the minutes,
 * "AI" on the seconds, nothing else. The 91 cannot go on the day of month next
 * to CB: its tens has no top left bar, and a 9 there reads 3. The LEDs sweep
 * the colour wheel over it, then the main loop takes the glass for the clock.
 */
static int test_lcd(void)
{
	int ret = lcd_init();

	if (ret) {
		LOG_ERR("LCD: init failed (%d)", ret);
		return ret;
	}
	lcd_all_pixels(true);
	LOG_INF("LCD: all segments on");
	k_msleep(1500);
	lcd_all_pixels(false);
	lcd_clear();
	lcd_display_string("CB", 0);
	lcd_display_string("91AI", 6);
	ret = lcd_flush();
	if (ret) {
		LOG_ERR("LCD: write failed (%d)", ret);
		return ret;
	}
	LOG_INF("LCD: boot screen, CB top left, 91 AI on the minutes and seconds");
	return 0;
}

static int test_buttons(void)
{
	/*
	 * The polarity of the case buttons is not known yet (V2-22): read the
	 * raw level with both pulls so that a pressed button can be recognised
	 * whatever the frame potential is. The devicetree flags are restored
	 * at the end.
	 */
	int ret = buttons_probe_configure(GPIO_PULL_DOWN);
	if (ret) {
		return ret;
	}
	buttons_report("with pull-down");
	ret = buttons_probe_configure(GPIO_PULL_UP);
	if (ret) {
		return ret;
	}
	buttons_report("with pull-up");
	return buttons_configure_from_dt();
}

#if defined(CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS)
/*
 * EF-63: show "UPd" on the glass just before the reset that starts the new
 * image. An upload is accepted whatever the cell voltage: the watch behaves the
 * same over the whole range of the CR2016 (2026-09-22), and a brown-out
 * during an upload leaves the running image untouched (MCUboot runs it in
 * place, the upload only writes the spare slot).
 */
static enum mgmt_cb_return update_mgmt_cb(uint32_t event, enum mgmt_cb_return prev_status,
					  int32_t *rc, uint16_t *group, bool *abort_more,
					  void *data, size_t data_size)
{
	ARG_UNUSED(prev_status);
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(data_size);

	if (event == MGMT_EVT_OP_CMD_RECV) {
		/* Any SMP command counts as host activity (idle disconnect, EF-33) */
		ble_activity();
		return MGMT_CB_OK;
	}
	if (event == MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK || event == MGMT_EVT_OP_IMG_MGMT_DFU_STARTED ||
	    event == MGMT_EVT_OP_IMG_MGMT_DFU_PENDING) {
		/* A host is filling the spare slot, or has marked what it holds: it is
		 * no longer ours to erase (update.c). Confirming the running image is
		 * not such an event: that is when the old one becomes useless. */
		update_spare_cancel();
	}
	if (event == MGMT_EVT_OP_OS_MGMT_RESET) {
		clock_retain(); /* freshest time in retained RAM across the reset */
		/* A reset over SMP is the last step of an update: MCUboot checks the
		 * new image without a display driver, so paint "UPd" now and let it stay
		 * on the glass until the new image boots (EF-63). Painting the shared
		 * framebuffer from this (SMP) thread is safe because clock_enabled is
		 * cleared first and the higher-priority main loop never redraws the
		 * clock again, and every lcd edit is a non-yielding CPU snapshot before
		 * the SPI transfer - see the note on the framebuffer in lcd.c. */
		selftest_clock_enable(false);
		lcd_clear();
		/* On the last three digits, the only run of positions where U, P and d
		 * all draw whole: the hour tens of the V1 draws a "1" and nothing else,
		 * and the minute tens shares its top and bottom bars (2026-09-23) */
		lcd_display_string("UPd", 7);
		(void)lcd_flush();
		/* The display may be asleep (on demand). Hold it on so that the main
		 * loop does not switch it back off before the reset; the driver
		 * then keeps showing its RAM on its own until the new image starts. */
		selftest_display_hold(true);
		(void)lcd_display_power(true);
		/* Last thing before the reset: restart the watchdog window. It keeps
		 * running across a software reset, and MCUboot checks the incoming
		 * image without feeding it (its BOOT_WATCHDOG_FEED only covers flash
		 * erases, not the hash and the signature). Measured on the bench:
		 * 1.54 s from here to the first feed of the new image, for an image of
		 * 408 KB. Left to itself the window could already be 3 s old, the age
		 * at which the task watchdog feeds it on its own, and 3 + 1.54 s is
		 * past the 4 s window: the new image would be bitten before it could
		 * confirm itself, and MCUboot would roll it back at every try. Feeding
		 * here makes the whole 4 s the bootloader's. */
		wdt_kick();
	}
	return MGMT_CB_OK;
}

static struct mgmt_callback update_upload_cb = {
	.callback = update_mgmt_cb,
	.event_id = MGMT_EVT_OP_IMG_MGMT_ALL,
};
static struct mgmt_callback update_reset_cb = {
	.callback = update_mgmt_cb,
	.event_id = MGMT_EVT_OP_OS_MGMT_RESET,
};
static struct mgmt_callback update_activity_cb = {
	.callback = update_mgmt_cb,
	.event_id = MGMT_EVT_OP_CMD_RECV,
};
#endif /* CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS */

int main(void)
{
	int failures = 0;

	/* Before anything else, see wdt_channel above. The hardware watchdog is
	 * then fed from the task watchdog timer; the 10 s software channel is fed
	 * between the self-test steps and by the loop. A hang resets the chip, and
	 * MCUboot reverts an unconfirmed image at the next boot (EF-62). */
	const struct device *hw_wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

	if (device_is_ready(hw_wdt) && task_wdt_init(hw_wdt) == 0) {
		wdt_channel = task_wdt_add(10000, NULL, NULL);
	}

	k_msleep(200);
	report_chip();
	if (wdt_channel < 0) {
		LOG_WRN("watchdog not started (%d)", wdt_channel);
	} else if (wdt_window_ms() != WDT_WINDOW_WANTED_MS) {
		LOG_WRN("hardware watchdog: %u ms window in force, kept from the image that started "
			"it; the %u ms of this image wait for a hardware reset",
			wdt_window_ms(), WDT_WINDOW_WANTED_MS);
	} else {
		LOG_INF("hardware watchdog: %u ms window", wdt_window_ms());
	}
	/* Early: the time kept in retained RAM is late by what runs before this */
	(void)clock_init();

	/*
	 * Power profile, from VBUS at boot. With USB this is the bench board and
	 * the full self-test runs. Without it the board may sit on a CR2016 in the
	 * watch (or on the ST-Link 3.3 V, harmless): the battery-safe variant runs,
	 * specification 4.7. The supplies are read first: the beep needs the rail.
	 */
	const bool bench = on_usb();

	LOG_INF("power profile: %s", bench ? "bench (USB present), full self-test"
					   : "battery-safe (no USB): short beep, no flash erase, "
					     "no receiver check");

	failures += test_battery() ? 1 : 0;
	failures += test_buzzer(bench) ? 1 : 0;
	wdt_kick();
	failures += test_accelerometer() ? 1 : 0;
	failures += test_flash(bench) ? 1 : 0;
	failures += test_buttons() ? 1 : 0;
	wdt_kick();
	failures += test_lcd() ? 1 : 0;
	wdt_kick();
	/* Over the boot screen */
	failures += test_leds() ? 1 : 0;
	failures += accel_test() ? 1 : 0;
	wdt_kick();
	failures += mic_test() ? 1 : 0;
	wdt_kick();
	failures += ble_start(bench) ? 1 : 0;
	wdt_kick();

	buttons_irq_init();

#if defined(CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS)
	mgmt_callback_register(&update_upload_cb);
	mgmt_callback_register(&update_reset_cb);
	mgmt_callback_register(&update_activity_cb);
#endif

	bool confirm_pending = false;

	if (failures == 0) {
		LOG_INF("self-test complete: all steps passed");
	} else {
		LOG_WRN("self-test complete: %d step(s) failed", failures);
	}
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
	/*
	 * EF-62: a freshly updated image boots "in test" and must be confirmed, or
	 * MCUboot reverts to the previous image at the next reset. Confirmation is
	 * driven by the loop below (a grace period without a watchdog fault, or a
	 * host read of the debug characteristic) and is deliberately NOT gated on
	 * the self-test result: the revert path is for broken firmware (a hang the
	 * watchdog catches, or an early reset), not for a board with a flaky
	 * peripheral, which a revert would not fix. The self-test outcome is
	 * reported above and in the BLE debug status for diagnosis instead.
	 */
	confirm_pending = !boot_is_img_confirmed();
#endif

	int64_t next_status = 0;
	/* Last advertising trial shown on the glass, and cued (lot N1a), and
	 * whether the glass shows trials */
	uint32_t trial_seen = 0;
	bool trial_drawn = false;

	update_spare_init();
	display_until = k_uptime_get() + DISPLAY_BOOT_MS;

	while (1) {
		wdt_kick();

		/* A command a host wrote over BLE, then one armed on ALARM (0.1.46),
		 * unless the presses start advertising trials (0.1.53). First, so
		 * that what follows sees the time after them: a capture or a stream
		 * holds the loop for seconds, and feeds the watchdog. */
		(void)remote_process();
		if (atomic_cas(&alarm_event, 1, 0) && !ble_trial_mode()) {
			hwtest_alarm_pressed();
		}

		int64_t now = k_uptime_get();
		uint32_t seconds = now / 1000U;
		char clock[8];

		/* A button press lights the display and brings fast advertising back,
		 * or, in trial mode, starts a burst */
		if (atomic_cas(&button_event, 1, 0)) {
			display_until = now + DISPLAY_WAKE_MS;
			ble_adv_kick();
		}
		if (atomic_cas(&display_wake, 1, 0)) {
			display_until = now + DISPLAY_WAKE_MS;
		}

		/* Trial mode: the glass follows the burst, and shows its result for a
		 * while, greeted by a pulse of the LED. A burst that trial mode left
		 * unfinished is only marked seen. */
		struct ble_trial_view trial;
		const bool trial_on = ble_trial_view(&trial);

		if (!trial_on) {
			trial_seen = trial.last.number;
		} else if (!trial.running && trial.last.number != trial_seen) {
			trial_seen = trial.last.number;
			display_until = now + DISPLAY_WAKE_MS;
			hwtest_trial_cue(trial.last.connect_ms != BLE_TRIAL_NONE);
		}
		/* In or out of trial mode: the words of the other screen leave the
		 * glass, unless something else holds it */
		if (trial_on != trial_drawn && clock_enabled) {
			trial_drawn = trial_on;
			lcd_clear();
		}

		/* Display on demand: BU97930 in software standby the rest of the time */
		const bool want_display = atomic_get(&display_hold) || now < display_until ||
					  (trial_on && trial.running);

		if (want_display != lcd_is_on()) {
			(void)lcd_display_power(want_display);
			LOG_INF("display %s", want_display ? "on" : "off (driver standby)");
		}

		if (heartbeat_enabled) {
			gpio_pin_set_dt(&led_green, 1);
			k_msleep(30);
			gpio_pin_set_dt(&led_green, 0);
		}

		clock_process();

		/* The spare slot is erased ahead of the next update, at rest */
		const bool spare_busy = update_spare_process();

		/* The time once a host has set it (EF-01): weekday, day of the month,
		 * HH:MM:SS local. Until then the uptime, the colon blinking. In trial
		 * mode the stopwatch of the trials instead. Neither while something
		 * else holds the glass: "UPd" before an update, a diagnostic. */
		if (want_display && clock_enabled && trial_on) {
			hwtest_trial_show(&trial);
		} else if (want_display && clock_enabled) {
			static const char *const weekdays[] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };
			struct tm tm;

			bool pm;

			/* No alarm, hourly chime nor stopwatch in the self-test: their icons
			 * stay off, whatever the boot pattern or a diagnostic left lit */
			lcd_set_indicator(LCD_INDICATOR_ALARM, false);
			lcd_set_indicator(LCD_INDICATOR_CHIME, false);
			lcd_set_indicator(LCD_INDICATOR_LAP, false);
			if (clock_display(clock, &pm, &tm)) {
				char date[4];

				snprintf(date, sizeof(date), "%2d", tm.tm_mday);
				lcd_display_string(weekdays[tm.tm_wday % 7], 0);
				lcd_display_string(date, 2);
				/* 12-hour with PM by default on the V1, 24-hour on request (EF-04) */
				lcd_set_indicator(LCD_INDICATOR_24H, clock_24h());
				lcd_set_indicator(LCD_INDICATOR_PM, pm);
				lcd_set_colon(true);
			} else {
				/* No leading zero: the hour tens of the V1 only draws "1" */
				snprintf(clock, sizeof(clock), "%2u%02u%02u",
					 (unsigned int)(seconds / 3600U % 100U),
					 (unsigned int)(seconds / 60U % 60U), (unsigned int)(seconds % 60U));
				lcd_set_colon(seconds & 1);
			}
			lcd_display_string(clock, 4);
			lcd_flush();
		}

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
		if (confirm_pending &&
		    (now > CONFIRM_GRACE_MS || atomic_get(&confirm_requested))) {
			bool by_read = atomic_get(&confirm_requested);
			int ret = boot_write_img_confirmed();

			LOG_INF("image %s (%s)", ret ? "confirmation failed" : "confirmed",
				by_read ? "host read status" : "grace elapsed");
			confirm_pending = false;
		}
#endif
		/* Supplies, Battery Service and status line: every 10 s while someone
		 * may be looking (USB, display on, connection), every 60 s otherwise. */
		if (now >= next_status) {
			int32_t vdd_mv = -1, vddh_mv = -1;
			int pressed[ARRAY_SIZE(buttons)];
			const bool watched = on_usb() || want_display ||
					     strcmp(ble_state(), "connected") == 0;

			next_status = now + (watched ? 10 : 60) * 1000;
			for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
				pressed[i] = gpio_pin_get_dt(&buttons[i]);
			}
			if (read_supplies_mv(&vdd_mv, &vddh_mv) == 0) {
				uint32_t usb = NRF_POWER->USBREGSTATUS;

				ble_set_battery_level(battery_percent(vdd_mv));
				LOG_INF("vdd %d mV, vddh %d mV, usb vbus=%u rdy=%u, ble %s (%u conn, adv %s, %d dBm), lcd %s, buttons LIGHT=%d MODE=%d ALARM=%d",
					vdd_mv, vddh_mv,
					(usb & POWER_USBREGSTATUS_VBUSDETECT_Msk) ? 1U : 0U,
					(usb & POWER_USBREGSTATUS_OUTPUTRDY_Msk) ? 1U : 0U,
					ble_state(), ble_connect_count(), ble_adv_mode(), ble_tx_power(),
					want_display ? "on" : "off",
					pressed[0], pressed[1], pressed[2]);

				char status[224];

				snprintf(status, sizeof(status),
					 "%s vdd=%d vbus=%u up=%us img=%s fails=%d conn=%u tx=%d "
					 "lcd=%u adv=%s/%d rst=%x t=%c ppb=%d wdt=%u slot=%d sp=%c "
					 "btn=%u,%u,%u keys=%d%d%d vddh=%d dis=%02x",
					 APP_VERSION_STRING, vdd_mv,
					 (usb & POWER_USBREGSTATUS_VBUSDETECT_Msk) ? 1U : 0U,
					 (unsigned int)seconds,
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
					 boot_is_img_confirmed() ? "confirmed" : "test",
#else
					 "n/a",
#endif
					 failures, ble_connect_count(), ble_tx_power(),
					 want_display ? 1U : 0U, ble_adv_mode(), ble_adv_last_error(),
					 (unsigned int)boot_reset_cause,
					 !clock_is_set() ? '-' :
					 (clock_flags() & CLOCK_FLAG_APPROXIMATE) ? '~' : 'y',
					 clock_ppb(), wdt_window_ms(), update_running_slot(),
					 update_spare_state(), (unsigned int)atomic_get(&button_irqs[0]),
					 (unsigned int)atomic_get(&button_irqs[1]),
					 (unsigned int)atomic_get(&button_irqs[2]),
					 pressed[0], pressed[1], pressed[2], vddh_mv,
					 ble_last_disconnect_reason());
				ble_dbg_set_status(status);
			}
		}

		/* Sleep to the next second while the clock shows, a few seconds
		 * otherwise; a button, the shell or the walk wake the loop early. */
		now = clock_is_set() ? clock_now_ms() : k_uptime_get();
		k_sem_take(&wake_sem, K_MSEC(spare_busy ? 50 :
					     want_display ? 1000 - (now % 1000) : IDLE_PERIOD_MS));
	}
	return 0;
}
