/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CB91AI_HWTEST_H
#define CB91AI_HWTEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/shell/shell.h>

/* The 3V rail around a load: before it, lowest while it lasts, after it */
struct hwtest_rail {
	int32_t before_mv;
	int32_t lowest_mv;
	int32_t after_mv;
	uint32_t readings;
};

/* Read the rail (SAADC, VDD input) as fast as it goes for `ms` milliseconds,
 * keeping the lowest reading: the load is switched on by the caller before,
 * and off after. Fills `lowest_mv` and `readings` only.
 */
void hwtest_rail_watch(struct hwtest_rail *rail, uint32_t ms);
int hwtest_rail_mv(int32_t *mv);

/* USB power present: the bench, where the limits of a coin cell do not apply */
bool hwtest_on_usb(void);

/* One turn of the colour wheel on the RGB LED in 1.5 s, the LEDs at boot and
 * `cb91ai led hue`; the rail is watched meanwhile when `rail` is not NULL */
int hwtest_led_hue(struct hwtest_rail *rail);

/* From the main loop: the ALARM button was pressed (runs an armed command) */
void hwtest_alarm_pressed(void);

/* From the main loop, in trial mode (lot N1a): the glass as a stopwatch of the
 * burst, then its result; a short pulse of the green LED for a connection, of
 * the red one for a trial without */
struct ble_trial_view;
void hwtest_trial_show(const struct ble_trial_view *view);
void hwtest_trial_cue(bool connected);

/* Shell commands, registered in cmds.c */
int hwtest_cmd_keys(const struct shell *sh, size_t argc, char **argv);
int hwtest_cmd_beep(const struct shell *sh, size_t argc, char **argv);
int hwtest_cmd_rec(const struct shell *sh, size_t argc, char **argv);
int hwtest_cmd_take(const struct shell *sh, size_t argc, char **argv);
int hwtest_cmd_arm(const struct shell *sh, size_t argc, char **argv);
int hwtest_cmd_rssi(const struct shell *sh, size_t argc, char **argv);
int hwtest_cmd_adv(const struct shell *sh, size_t argc, char **argv);

/* `cb91ai accel int`: the INT1 and INT2 lines of the BMA400 */
int hwtest_accel_int(const struct shell *sh);

/* `cb91ai qspi`: erase, write and read back the last sector of the storage flash */
int hwtest_cmd_qspi(const struct shell *sh, size_t argc, char **argv);

#endif /* CB91AI_HWTEST_H */
