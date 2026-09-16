# Copyright (c) 2026 Mateusz Żerebecki
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wheel_source_test)

set(APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src)

# The source under test, plus everything it needs from the application. The
# sensor is a fake driver defined by this test; the configuration and the CSCS
# publisher are stubs, because neither is what is being tested here.
target_sources(app PRIVATE
  src/main.c
  src/fake_accel.c
  src/stubs.c
  ${APP_SRC}/wheel_source_accel.c
  ${APP_SRC}/wheel_detector.c
  ${APP_SRC}/wheel_csc.c
  ${APP_SRC}/wheel_reconfig.c
)

target_include_directories(app PRIVATE
  src
  ${APP_SRC}
)
