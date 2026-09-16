# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Application-specific Kconfig options (CSC service, wheel simulation).
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(speed_meter)

target_sources(app PRIVATE
	src/main.c
	src/csc.c
	src/bt.c
	src/led_sw_blink.c
)

# Wheel configuration (circumference, ODR, range, detector thresholds) with
# persistence in NVS. Used by every wheel source and by the diagnostics shell.
target_sources(app PRIVATE src/wheel_config.c)

# CSCS wire state: the event-time clock and the cumulative revolution counter.
# Free of Zephyr dependencies so it can be unit-tested on the host.
target_sources(app PRIVATE src/wheel_csc.c)

# CR2032 measurement through the SAADC internal VDD input + Battery Service.
target_sources_ifdef(CONFIG_CSC_BATTERY app PRIVATE src/battery.c)

# Wheel revolution sources - `choice CSC_WHEEL_SOURCE` builds exactly one.
target_sources_ifdef(CONFIG_CSC_SIMULATE app PRIVATE src/wheel_source_sim.c)
target_sources_ifdef(CONFIG_CSC_WHEEL_SENSOR_GPIO app PRIVATE src/wheel_source_gpio.c)
target_sources_ifdef(CONFIG_CSC_WHEEL_SENSOR_ACCEL app PRIVATE src/wheel_source_accel.c)

# Self-calibrating revolution detector (accelerometer source and/or recorder).
target_sources_ifdef(CONFIG_CSC_WHEEL_DETECTOR app PRIVATE src/wheel_detector.c)

# Remote diagnostics (recording + calibration over the MCUmgr/SMP shell).
target_sources_ifdef(CONFIG_CSC_DIAG app PRIVATE src/wheel_diag.c)

# Low-power management (sleep on wheel standstill, wake on motion).
target_sources_ifdef(CONFIG_CSC_POWER_SAVE app PRIVATE src/wheel_power.c)

# On-demand firmware update (DFU) over BLE SMP.
target_sources_ifdef(CONFIG_CSC_DFU app PRIVATE src/dfu_mode.c)
target_sources_ifdef(CONFIG_CSC_DFU app PRIVATE src/dfu_button.c)

# Boot self-test and firmware confirmation (MCUboot).
target_sources_ifdef(CONFIG_BOOTLOADER_MCUBOOT app PRIVATE src/boot.c)
