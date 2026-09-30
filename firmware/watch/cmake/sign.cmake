# Copyright (c) 2026 COBALT Technologies
# SPDX-License-Identifier: Apache-2.0
#
# Signing step of the CB-91AI images: the one of Zephyr, plus the address of the
# slot the image is linked for.
#
# MCUboot executes the images in place (sysbuild.conf), so sysbuild builds one
# variant per slot. imgtool writes the slot address in the image header
# (--rom-fixed): MCUmgr then refuses an upload meant for the other slot, and
# MCUboot refuses to start it. The address cannot come from a Kconfig value: the
# slot-1 variant inherits the configuration of the application, so it is taken
# here from the load offset, which follows the code partition of each variant.

set(CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS
    "${CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS} --rom-fixed ${CONFIG_FLASH_LOAD_OFFSET}")
include(${ZEPHYR_BASE}/cmake/mcuboot.cmake)
