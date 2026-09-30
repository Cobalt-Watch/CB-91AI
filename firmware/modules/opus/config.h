/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * Build configuration of libopus for the CB-91AI (HAVE_CONFIG_H), in place of
 * the config.h its configure script would write. Zephyr includes autoconf.h in
 * every compilation unit, so the CONFIG_ symbols are visible here.
 */

#ifndef CB91AI_OPUS_CONFIG_H
#define CB91AI_OPUS_CONFIG_H

#define OPUS_BUILD        1
#define PACKAGE_VERSION   "1.5.2-cb91ai"

/* Integer arithmetic only: no FPU needed, no float entry points */
#define FIXED_POINT       1
#define DISABLE_FLOAT_API 1

/* Scratch memory as C99 variable-length arrays on the caller's stack: no heap,
 * no global pseudo-stack. The codec bench measures how deep it goes.
 */
#define VAR_ARRAYS        1

#if defined(CONFIG_CB91AI_OPUS_ARM_INLINE_ASM)
/* Cortex-M4 (ARMv7E-M): SMULL, the DSP multiplies libopus calls "EDSP" (SMULWB,
 * SMLAWB, SMULBB) and the saturation it calls "media" (SSAT) all exist in
 * Thumb-2. Inline only: OPUS_ARM_ASM and the PRESUME/MAY_HAVE switches would
 * call the assembly files of libopus, written for A-profile cores.
 */
#define OPUS_ARM_INLINE_ASM   1
#define OPUS_ARM_INLINE_EDSP  1
#define OPUS_ARM_INLINE_MEDIA 1
#endif

#endif /* CB91AI_OPUS_CONFIG_H */
