# Copyright (c) 2026 Mateusz Żerebecki
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wheel_detector_replay)

# wheel_detector.c makes no Zephyr calls on purpose, so the test compiles the
# production file itself rather than a copy that could drift away from it.
set(DETECTOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src)

file(GLOB FIXTURE_CSVS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/*.csv")

set(FIXTURES_HEADER ${CMAKE_CURRENT_BINARY_DIR}/fixtures.h)

add_custom_command(
  OUTPUT ${FIXTURES_HEADER}
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/csv_to_header.py
          ${FIXTURES_HEADER} ${FIXTURE_CSVS}
  DEPENDS ${FIXTURE_CSVS}
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/csv_to_header.py
  COMMENT "Embedding wheel captures into fixtures.h"
  VERBATIM
)

add_custom_target(fixtures_header DEPENDS ${FIXTURES_HEADER})

target_sources(app PRIVATE
  src/main.c
  ${DETECTOR_DIR}/wheel_detector.c
)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}
  ${DETECTOR_DIR}
)

add_dependencies(app fixtures_header)
