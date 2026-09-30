# Copyright (c) 2026 COBALT Technologies
# SPDX-License-Identifier: Apache-2.0
#
# Say which key signs this build (Kconfig.sysbuild): an image signed with the
# wrong one uploads fine and is then refused by MCUboot at the swap, and a
# bootloader built with the development key accepts images signed by anyone.

if(SB_CONFIG_BOOT_SIGNATURE_KEY_FILE MATCHES "root-(rsa|ec|ed)[^/]*\\.pem$")
  message(WARNING "cb91ai: CB91AI_SIGNING_KEY is not set, MCUboot and the image use the public "
                  "development key ${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}. Never flash this "
                  "bootloader on a board that goes into a watch.")
elseif(NOT EXISTS "${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}")
  message(FATAL_ERROR "cb91ai: signing key not found: ${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE} "
                      "(CB91AI_SIGNING_KEY, forward slashes)")
else()
  message(STATUS "cb91ai: MCUboot and the image use the signing key "
                 "${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}")
endif()
