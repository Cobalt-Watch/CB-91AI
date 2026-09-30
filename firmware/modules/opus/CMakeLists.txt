# Copyright (c) 2026 COBALT Technologies
# SPDX-License-Identifier: Apache-2.0
#
# libopus as a Zephyr library, for the codec bench of lot K2. Its sources stay
# out of this repository (Kconfig): they are found through CB91AI_OPUS_DIR, from
# the environment or the CMake command line, else in modules/lib/opus of the
# west workspace.

if(CONFIG_CB91AI_OPUS)

if(NOT DEFINED CB91AI_OPUS_DIR AND DEFINED ENV{CB91AI_OPUS_DIR})
  set(CB91AI_OPUS_DIR $ENV{CB91AI_OPUS_DIR})
endif()
if(NOT DEFINED CB91AI_OPUS_DIR)
  set(CB91AI_OPUS_DIR ${ZEPHYR_BASE}/../modules/lib/opus)
endif()
get_filename_component(CB91AI_OPUS_DIR ${CB91AI_OPUS_DIR} ABSOLUTE)
if(NOT EXISTS ${CB91AI_OPUS_DIR}/include/opus.h)
  message(FATAL_ERROR "cb91ai: libopus not found in ${CB91AI_OPUS_DIR}. Clone "
                      "https://github.com/xiph/opus (tag v1.5.2) there, or set CB91AI_OPUS_DIR "
                      "(forward slashes).")
endif()
message(STATUS "cb91ai: libopus from ${CB91AI_OPUS_DIR}")

# libopus lists its sources in makefile fragments: NAME = \ a.c \ b.c, closed by
# an empty line.
function(cb91ai_opus_sources fragment name out)
  file(READ ${CB91AI_OPUS_DIR}/${fragment} text)
  # A Windows checkout may carry CRLF, and "\r" alone would not be an empty line
  string(REPLACE "\r" "" text "${text}")
  string(REGEX MATCH "(^|\n)${name} =[^\n]*(\n[^\n]+)*" block "${text}")
  string(REGEX MATCHALL "[A-Za-z0-9_/]+\\.c" files "${block}")
  if(NOT files)
    message(FATAL_ERROR "cb91ai: no ${name} in ${CB91AI_OPUS_DIR}/${fragment}")
  endif()
  list(TRANSFORM files PREPEND ${CB91AI_OPUS_DIR}/)
  set(${out} ${files} PARENT_SCOPE)
endfunction()

cb91ai_opus_sources(celt_sources.mk CELT_SOURCES celt_sources)
cb91ai_opus_sources(silk_sources.mk SILK_SOURCES silk_sources)
cb91ai_opus_sources(silk_sources.mk SILK_SOURCES_FIXED silk_fixed_sources)
cb91ai_opus_sources(opus_sources.mk OPUS_SOURCES opus_sources)

zephyr_library_named(cb91ai_opus)
zephyr_library_sources(${celt_sources} ${silk_sources} ${silk_fixed_sources} ${opus_sources})

zephyr_include_directories(${CB91AI_OPUS_DIR}/include)
zephyr_library_include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CB91AI_OPUS_DIR}
  ${CB91AI_OPUS_DIR}/celt
  ${CB91AI_OPUS_DIR}/silk
  ${CB91AI_OPUS_DIR}/silk/fixed
)
# config.h of this directory
zephyr_library_compile_definitions(HAVE_CONFIG_H)

if(CONFIG_CB91AI_OPUS_OS)
  zephyr_library_compile_options(-Os)
else()
  zephyr_library_compile_options(-O2)
endif()
# Third-party code, built as its authors do: their warnings are not ours
zephyr_library_compile_options(-w)

endif()
