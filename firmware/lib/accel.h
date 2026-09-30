/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CB91AI_ACCEL_H
#define CB91AI_ACCEL_H

#include <stdint.h>

/* Reset the BMA400 and set 100 Hz, +/-4 g; the sensor is left in sleep mode. */
int accel_init(void);

/* One reading in mg and the sensor temperature in tenths of a degree. The
 * sensor is woken for the measurement and put back to sleep (about 40 ms).
 */
int accel_read(int32_t mg[3], int32_t *temp_dc);

/* Sleep mode, 0.16 uA typ.; accel_read() does it by itself. */
int accel_sleep(void);

/* Self-test step: init, a few readings, norm close to 1 g expected at rest. */
int accel_test(void);

/*
 * The wrist (lot D4, CdC 4.8, EF-19, EF-44): the sensor runs as it is built to
 * on a coin cell. In low-power mode (25 Hz, 0.85 uA, datasheet p. 19) only its
 * wake-up interrupt runs: `wake_samples` samples in a row farther than
 * `wake_thres` from its reference (p. 21, 22, 74) switch it to normal mode (200 Hz, 3.5 uA), where the
 * advanced interrupts run (p. 38): the orientation change on INT1 (a new
 * orientation held `orient_dur` x 10 ms, `orient_thres` x 8 mg away from the
 * reference, p. 44) and the double tap on INT2 (p. 43); it goes back to low
 * power `lowpower_ms` after the wake-up (p. 23). Both pins latched:
 * accel_wrist_status() reads the status registers, which releases them.
 *
 * Measured on a still watch (26/09):
 * - the noise of the low-power mode, 11 mg rms on X and Y and 16 on Z with
 *   `osr_lp` 0 (p. 19), against a reference kept on 8 bits (p. 21): compared
 *   with the previous sample (ACCEL_WAKE_REF_EVERY), 62.5 mg over one sample
 *   woke the sensor every 10 to 30 s; compared with the orientation at rest
 *   (ACCEL_WAKE_REF_ONCE) over two samples, 62.5 or 94 mg, never in 5 min;
 * - a reference the sensor updates itself (ACCEL_ORIENT_REF_FILT2, _LP) is
 *   taken again at each wake-up from a filter not settled yet (Z 0.5 g for a
 *   glass at 1 g): every wake-up then brought an orientation change, 21 out
 *   of 21. Held by the watch (ACCEL_ORIENT_REF_HOST), none in 96 wake-ups:
 *   47 with the reference written again at each one, 49 with it written
 *   once, after ORIENTCH_CONFIG0 (accel_wrist_start(): before it, it was
 *   lost).
 */
struct accel_wrist {
	uint8_t wake_thres;      /* WKUP_INT_CONFIG1, 31.25 mg per LSB at +/-4 g (p. 75) */
	uint8_t wake_samples;    /* 1 to 8 samples of 40 ms over the threshold */
	uint8_t orient_thres;    /* 8 mg per LSB (p. 78), on Z (out of the glass) and Y (12 h) */
	uint8_t orient_dur;      /* 10 ms per LSB */
	uint8_t tap_sensitivity; /* 0 the most sensitive to 7 (p. 96) */
	uint16_t lowpower_ms;    /* 2.5 ms steps, 10 s at most (p. 72) */
	uint8_t wake_ref;        /* ACCEL_WAKE_REF_*: what a sample is compared with (p. 74) */
	uint8_t orient_ref;      /* ACCEL_ORIENT_REF_*: who moves the reference (p. 77) */
	uint8_t osr_lp;          /* 0 to 3: noise traded for current in low power (p. 19, 63) */
	uint8_t flags;           /* ACCEL_WRIST_* */
};

#define ACCEL_WAKE_REF_ONCE    1 /* the first sample once asleep: the orientation at rest */
#define ACCEL_WAKE_REF_EVERY   2 /* the previous sample */
#define ACCEL_ORIENT_REF_HOST  0 /* written by accel_orient_ref_now() and _again() */
#define ACCEL_ORIENT_REF_FILT2 1 /* set by the sensor after each change, from acc_filt2 */
#define ACCEL_ORIENT_REF_LP    2 /* the same from acc_filt_lp (1 Hz) */
#define ACCEL_WRIST_ORIENT_LP  0x01 /* orientation judged on acc_filt_lp, not acc_filt2 */
#define ACCEL_WRIST_WAKE_INT1  0x02 /* the wake-up interrupt on INT1 as well: each one seen */
/* The orientation judged on Z only, as up to 0.2.1+27: by default on Y (12 o'clock) as well,
 * since a look from the keyboard is a turn of the wrist that Z hardly sees (watch/src/look.h;
 * a change on any enabled axis raises the interrupt, p. 44) */
#define ACCEL_WRIST_ORIENT_Z_ONLY 0x04

#define ACCEL_WRIST_DEFAULT                                                                        \
	{ .wake_thres = 3, .wake_samples = 2, .orient_thres = 60, .orient_dur = 25,               \
	  .tap_sensitivity = 3, .lowpower_ms = 3000, .wake_ref = ACCEL_WAKE_REF_ONCE,             \
	  .orient_ref = ACCEL_ORIENT_REF_HOST, .osr_lp = 0, .flags = 0 }

/* Reset, configure as above, start in low power. Its orientation reference is
 * set to the acceleration now, so that the first gesture is not the start. */
int accel_wrist_start(const struct accel_wrist *w);

/* INT_STAT0 (wake-up: bit 0, orientation change: bit 1, engine overrun: bit 4)
 * and INT_STAT1 (double tap: bit 3), read together: the latched pins are
 * released */
#define ACCEL_STAT0_WAKEUP   0x01
#define ACCEL_STAT0_ORIENTCH 0x02
#define ACCEL_STAT0_OVERRUN  0x10
#define ACCEL_STAT1_D_TAP    0x08
int accel_wrist_status(uint8_t *stat0, uint8_t *stat1);

/* The acceleration now, in mg, in whatever mode the sensor runs (no wake-up) */
int accel_now(int32_t mg[3]);

/* ACCEL_ORIENT_REF_HOST: the acceleration now becomes the orientation
 * reference (and is returned in mg); _again() writes the last one back */
int accel_orient_ref_now(int32_t mg[3]);
int accel_orient_ref_again(void);

/* The orientation reference last written, in mg: the orientation before a change */
void accel_orient_ref_mg(int32_t mg[3]);

/* For the bench: registers 0x00 to 0x0C (identity, status, data, time), 0x19 to
 * 0x3E (configuration, wake-up and orientation references) and 0x57 to 0x58
 * (tap), none of which a read changes but the cmd_err bit of ERR_REG (0x02,
 * cleared on read, p. 50); the status registers are left alone */
#define ACCEL_DUMP_SIZE 53
int accel_dump(uint8_t out[ACCEL_DUMP_SIZE]);

#endif /* CB91AI_ACCEL_H */
