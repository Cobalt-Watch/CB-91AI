# Copyright (c) 2026 COBALT Technologies
# SPDX-License-Identifier: Apache-2.0
#
# Shared library of the CB-91AI (lot D1): the drivers and the wall clock, with
# no policy of their own. Each piece is enabled by its own Kconfig option, so
# an application only carries what it uses. The pure logic among them (gesture,
# calendar, link_proto) is also built for the PC by firmware/tests/host.

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(cb91ai_lib_sources)
foreach(piece LCD MIC ACCEL CLOCK GESTURE CALENDAR LINK_PROTO VAD NOTE)
  string(TOLOWER ${piece} file)
  if(CONFIG_CB91AI_LIB_${piece})
    list(APPEND cb91ai_lib_sources ${file}.c)
  endif()
endforeach()

if(cb91ai_lib_sources)
  zephyr_library_named(cb91ai_lib)
  zephyr_library_sources(${cb91ai_lib_sources})
endif()
