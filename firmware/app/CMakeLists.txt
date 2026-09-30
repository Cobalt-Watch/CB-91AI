# Copyright (c) 2026 COBALT Technologies
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# The cb91ai board definition is shared with the watch application
# (firmware/boards), as are the drivers and the wall clock (firmware/lib)
get_filename_component(CB91AI_FIRMWARE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
list(APPEND BOARD_ROOT ${CB91AI_FIRMWARE_DIR})
list(APPEND ZEPHYR_EXTRA_MODULES ${CB91AI_FIRMWARE_DIR}/lib)
# The bindings of the board's parts live with their drivers (firmware/lib/dts)
list(APPEND DTS_ROOT ${CB91AI_FIRMWARE_DIR}/lib)
# Glue around libopus (lot K2): compiles nothing unless CONFIG_CB91AI_OPUS is
# set (bench_opus.conf)
list(APPEND ZEPHYR_EXTRA_MODULES ${CB91AI_FIRMWARE_DIR}/modules/opus)

# Firmware Revision of the Device Information Service = the VERSION file (EF-64):
# a Kconfig fragment written at configure time, so that a phone reads over
# Bluetooth the very version MCUboot and MCUmgr report.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/VERSION app_version_file)
string(REGEX MATCH "VERSION_MAJOR = ([0-9]+)" _ "${app_version_file}")
set(app_version_major ${CMAKE_MATCH_1})
string(REGEX MATCH "VERSION_MINOR = ([0-9]+)" _ "${app_version_file}")
set(app_version_minor ${CMAKE_MATCH_1})
string(REGEX MATCH "PATCHLEVEL = ([0-9]+)" _ "${app_version_file}")
set(app_version_patch ${CMAKE_MATCH_1})
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/version.conf
     "CONFIG_BT_DIS_FW_REV_STR=\"${app_version_major}.${app_version_minor}.${app_version_patch}\"\n")
list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_BINARY_DIR}/version.conf)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION)

# Images signed by Zephyr's script, with the address of their slot in the header
set(SIGNING_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sign.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cb91ai_firmware LANGUAGES C)

target_sources(app PRIVATE src/main.c src/ble.c src/usb.c src/cmds.c src/lcdwalk.c src/update.c
			   src/remote.c src/hwtest.c)
# Codec bench (lot K2), only in the bench image: -DEXTRA_CONF_FILE=bench_codec.conf
target_sources_ifdef(CONFIG_CB91AI_CODEC_BENCH app PRIVATE src/codec_bench.c)
