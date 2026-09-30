# Copyright (c) 2026 COBALT Technologies
# SPDX-License-Identifier: Apache-2.0
#
# The watch: the product application. The bench
# self-test lives next door in firmware/app and stays a bench tool.

cmake_minimum_required(VERSION 3.20.0)

# Board definition and shared drivers, both shared with the self-test
get_filename_component(CB91AI_FIRMWARE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
list(APPEND BOARD_ROOT ${CB91AI_FIRMWARE_DIR})
list(APPEND ZEPHYR_EXTRA_MODULES ${CB91AI_FIRMWARE_DIR}/lib)
# The bindings of the board's parts live with their drivers (firmware/lib/dts)
list(APPEND DTS_ROOT ${CB91AI_FIRMWARE_DIR}/lib)

# The VERSION file drives the version MCUboot compares, and Zephyr turns it into
# APP_VERSION_STRING (app_version.h). The Firmware Revision of the Device
# Information Service takes it too (EF-64): a Kconfig fragment written at
# configure time, as in the self-test, so that a phone reads over Bluetooth the
# very version MCUboot and MCUmgr report. The two applications share one line of
# versions on a board: MCUboot only compares numbers.
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

# Images signed with the address of their slot in the header (direct execution)
set(SIGNING_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sign.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cb91ai_watch LANGUAGES C)

target_sources(app PRIVATE src/main.c src/events.c src/buttons.c src/ui.c src/radio.c
			   src/link.c src/session.c src/update.c src/power.c src/light.c src/led.c
			   src/store.c src/tlog.c src/notes.c src/recorder.c src/take.c src/pair.c
			   src/pairing.c src/recall.c src/clicks.c src/wrist.c src/stuck.c src/cell.c
			   src/counts.c src/counters.c src/daily.c src/look.c)
# update.c reads the upload's offset and size from the image group's structures
# (img_mgmt.h), whose MCUboot headers need its configuration, as Zephyr's own
# libraries that use them link it
if(TARGET MCUBOOT_BOOTUTIL)
  target_link_libraries(app PRIVATE MCUBOOT_BOOTUTIL)
endif()
# The status line and the time of the bench tools, development build only
target_sources_ifdef(CONFIG_CB91AI_WATCH_DEBUG_SERVICE app PRIVATE src/debug.c)
