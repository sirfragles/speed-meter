# Copyright (c) 2026 Mateusz Żerebecki
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wheel_reconfig_test)

# wheel_reconfig.c has no Zephyr dependencies, so the fail-closed decision can
# be exercised without a sensor, a kernel clock or a thread.
target_sources(app PRIVATE
  src/main.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/wheel_reconfig.c
)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src
)
