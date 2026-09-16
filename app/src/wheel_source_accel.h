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

#include <stdint.h>

/**
 * @brief How the source classified the outcomes of its bus accesses.
 *
 * This is not the detector's state. It is the distinction the sampling loop
 * has to make: "there is no new sample yet" is normal at a polling rate equal
 * to the ODR, while a bus error means samples were genuinely lost and the
 * detector has to be told.
 */
struct wheel_source_stats {
	uint32_t samples; /* forwarded to the detector */
	uint32_t no_data; /* -ENODATA: nothing new, nothing lost */
	uint32_t lost;    /* discontinuity signalled to the detector */
};

/**
 * @brief Start sampling the accelerometer and publishing wheel revolutions.
 *
 * @return 0 on success, negative errno if the sensor is not ready.
 */
int wheel_source_accel_init(void);

/** @brief Read the counters since the last reset. */
void wheel_source_accel_stats(struct wheel_source_stats *out);

/** @brief Zero the counters. */
void wheel_source_accel_stats_reset(void);

/**
 * @brief One pass of the sampling loop.
 *
 * The sampling thread is a loop around this. It is public so the integration
 * test in tests/wheel_source can drive it step by step: a thread racing the
 * scheduler and the kernel clock is not something a test can make assertions
 * about.
 *
 * @param next_us    cadence deadline, carried between passes
 * @param period_us  current sampling period, carried between passes
 */
void wheel_source_accel_step(uint64_t *next_us, uint32_t *period_us);

#endif /* SPEED_METER_WHEEL_SOURCE_ACCEL_H_ */
