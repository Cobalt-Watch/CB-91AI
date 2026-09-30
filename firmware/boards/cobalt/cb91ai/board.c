/*
 * Copyright (c) 2026 COBALT Technologies
 * SPDX-License-Identifier: Apache-2.0
 *
 * First-boot provisioning of the UICR for the COBALT CB-91AI.
 *
 * - REGOUT0: VDD (the 3V rail) is fed by the coin cell, VDDH by USB. When USB
 *   is plugged the nRF52840 enters high voltage mode and REG0 drives the 3V
 *   rail at the voltage stored in UICR.REGOUT0. The factory value is 1.8 V,
 *   which would brown out every peripheral on the rail: set it to 3.0 V.
 * - APPROTECT: the meaning of UICR.APPROTECT depends on the die revision.
 *   Up to build code D (revisions 1 and 2, e.g. the "AAD0" dies fitted on the
 *   V1 boards) the legacy scheme applies: 0xFF keeps the debug port open and
 *   any other value locks it. From build code F (revision 3) the hardened
 *   scheme applies: the port only stays open when UICR.APPROTECT holds 0x5A
 *   (HwDisabled) and the startup code writes APPROTECT.DISABLE, which the
 *   Nordic MDK does when the UICR says so. The 0x5A word is therefore only
 *   written on hardened dies. Writing it on a legacy die locks the port, and
 *   the next pyOCD connection then mass erases the chip to recover (pyOCD
 *   option auto_unlock, enabled by default), which wipes the firmware.
 *
 * UICR words can only have bits cleared, so each value is written only when
 * the field is still in its erased state. A system reset is issued after
 * writing so that the new values take effect.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <cmsis_core.h>
#include <nrfx.h>

#define UICR_APPROTECT_HW_DISABLED 0x5AUL

/* First nRF52840 build code letter with the hardened APPROTECT scheme */
#define NRF52840_HARDENED_APPROTECT_BUILD_CODE 'F'

static void uicr_word_write(volatile uint32_t *reg, uint32_t value)
{
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos;
	while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
	}
	*reg = value;
	while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
	}
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
	while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
	}
}

/*
 * FICR.INFO.VARIANT holds the build code as four ASCII characters packed big
 * endian, e.g. 0x41414430 for "AAD0"; the third character is the build code
 * letter that distinguishes the die revisions.
 */
static bool cb91ai_has_hardened_approtect(void)
{
	char build_code = (char)((NRF_FICR->INFO.VARIANT >> 8) & 0xff);

	return build_code >= NRF52840_HARDENED_APPROTECT_BUILD_CODE;
}

static int cb91ai_uicr_provision(void)
{
	bool reset_needed = false;
	uint32_t regout0 = NRF_UICR->REGOUT0;
	uint32_t approtect = NRF_UICR->APPROTECT;

	if ((regout0 & UICR_REGOUT0_VOUT_Msk) ==
	    (UICR_REGOUT0_VOUT_DEFAULT << UICR_REGOUT0_VOUT_Pos)) {
		regout0 = (regout0 & ~UICR_REGOUT0_VOUT_Msk) |
			  (UICR_REGOUT0_VOUT_3V0 << UICR_REGOUT0_VOUT_Pos);
		uicr_word_write(&NRF_UICR->REGOUT0, regout0);
		reset_needed = true;
	}

	if (cb91ai_has_hardened_approtect() &&
	    (approtect & UICR_APPROTECT_PALL_Msk) == UICR_APPROTECT_PALL_Msk) {
		approtect = (approtect & ~UICR_APPROTECT_PALL_Msk) |
			    (UICR_APPROTECT_HW_DISABLED << UICR_APPROTECT_PALL_Pos);
		uicr_word_write(&NRF_UICR->APPROTECT, approtect);
		reset_needed = true;
	}

	if (reset_needed) {
		NVIC_SystemReset();
	}

	return 0;
}

#if defined(CONFIG_CB91AI_PROVISION_UICR)
SYS_INIT(cb91ai_uicr_provision, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
#endif
