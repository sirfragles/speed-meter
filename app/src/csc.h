/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cycling Speed and Cadence Service (UUID 0x1816) - GATT server.
 *
 * Zephyr has no built-in CSCS server (no CONFIG_BT_CSCS, no
 * bt_cscs_measurement_send()); the service is implemented by this project
 * (src/csc.c), following the upstream samples/bluetooth/peripheral_csc
 * sample and the earlier wheel_speed_sensor project.
 */

#ifndef SPEED_METER_CSC_H_
#define SPEED_METER_CSC_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

/* CSC Feature bits (characteristic 0x2A5C, uint16, little-endian). */
#define CSC_FEAT_WHEEL_REV_DATA BIT(0)
#define CSC_FEAT_CRANK_REV_DATA BIT(1)
#define CSC_FEAT_MULTI_SENSORS  BIT(2)

/* CSC Measurement flags (characteristic 0x2A5B, first byte). */
#define CSC_MEAS_FLAG_WHEEL_REV_DATA BIT(0)
#define CSC_MEAS_FLAG_CRANK_REV_DATA BIT(1)

/**
 * @brief Publish a wheel revolution measurement (speed data).
 *
 * @param cumulative_revolutions  Total wheel revolutions (uint32, rolls over
 *                                after ~4.29 billion revolutions).
 * @param last_event_time_1024    Time of the last wheel revolution in
 *                                1/1024 s units. The value is uint16 and
 *                                rolls over every 64 s (per the CSCS spec);
 *                                receivers compute speed from the deltas.
 */
void csc_publish_wheel(uint32_t cumulative_revolutions, uint16_t last_event_time_1024);

/**
 * @brief Publish a crank revolution measurement (cadence data).
 *
 * Only available when CONFIG_CSC_CRANK_REV_DATA is enabled, otherwise the
 * call is a no-op.
 *
 * @param cumulative_revolutions  Total crank revolutions (uint16).
 * @param last_event_time_1024    Time of the last crank revolution in
 *                                1/1024 s units (rolls over every 64 s).
 */
void csc_publish_crank(uint16_t cumulative_revolutions, uint16_t last_event_time_1024);

/**
 * @brief Current uptime expressed in 1/1024 s units.
 *
 * Convenience helper for interrupt-driven sensors:
 *   csc_publish_wheel(++counter, csc_event_time_now());
 */
uint16_t csc_event_time_now(void);

/** @brief True once a client enabled CSC Measurement notifications (CCCD). */
bool csc_notifications_enabled(void);

#endif /* SPEED_METER_CSC_H_ */
