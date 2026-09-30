# Copyright (c) 2026 COBALT Technologies
# SPDX-License-Identifier: Apache-2.0

# Default runner: pyOCD, which supports the ST-Link V2 used on the bench.
board_runner_args(pyocd "--target=nrf52840" "--frequency=4000000")
board_runner_args(jlink "--device=nRF52840_xxAA" "--speed=4000")

include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/nrfutil.board.cmake)
include(${ZEPHYR_BASE}/boards/common/nrfjprog.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
