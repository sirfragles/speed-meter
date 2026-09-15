/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wheel revolutions from the accelerometer - the magnetless speed source.
 *
 * A spinning accelerometer sees gravity rotate once per wheel turn, so the
 * self-calibrating detector in wheel_detector.c can count revolutions without
 * any magnet, giving the wheel speed the CSCS service publishes. The board may
 * be mounted in any orientation: the detector finds the rotation plane, the
 * axis signs and the sensor-to-axis distance from the data itself.
 *
 * This module also drives the low-power state machine: it hands the detector's
 * "wheel is turning" flag to wheel_power.c, which puts the LIS2DH into a 1 Hz
 * motion-interrupt mode (and the SoC into System OFF when nobody is connected)
 * after the configured standstill timeout, and wakes everything back up when
 * the wheel moves again.
 *
 * Enable with CONFIG_CSC_WHEEL_SENSOR_ACCEL.
 */

#ifndef SPEED_METER_WHEEL_SOURCE_ACCEL_H_
#define SPEED_METER_WHEEL_SOURCE_ACCEL_H_

/**
 * @brief Start sampling the accelerometer and publishing wheel revolutions.
 *
 * @return 0 on success, negative errno if the sensor is not ready.
 */
int wheel_source_accel_init(void);

#endif /* SPEED_METER_WHEEL_SOURCE_ACCEL_H_ */
