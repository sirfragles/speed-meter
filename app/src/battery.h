/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CR2032 battery measurement through the nRF54L15 SAADC's internal VDD input.
 *
 * The coin cell feeds VDD directly, and the SAADC can route VDD to a channel
 * internally, so there is no divider, no extra pin and nothing to solder. See
 * the board overlay for the channel configuration (gain 1/4 against the 0.9 V
 * internal reference, i.e. a 3.6 V full scale).
 *
 * The measured voltage is published as a percentage through the standard BLE
 * Battery Service. Enable with CONFIG_CSC_BATTERY.
 */

#ifndef SPEED_METER_BATTERY_H_
#define SPEED_METER_BATTERY_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Set the ADC channel up and take a first reading.
 *
 * @return 0 on success, negative errno otherwise. Failure is not fatal: the
 *         Battery Service then keeps reporting CONFIG_CSC_BAS_LEVEL.
 */
int battery_init(void);

/**
 * @brief Read the battery voltage in millivolts.
 *
 * @return 0 on success, negative errno otherwise.
 */
int battery_read_mv(int32_t *voltage_mv);

/**
 * @brief Convert a CR2032 voltage to an approximate state of charge.
 *
 * A coin cell does not discharge linearly, so this interpolates a discharge
 * curve rather than scaling the voltage.
 */
uint8_t battery_percent_from_mv(int32_t voltage_mv);

/**
 * @brief Measure and publish the level over the BLE Battery Service.
 *
 * @return 0 on success, negative errno otherwise.
 */
int battery_update(void);

/**
 * @brief Current state of charge as last published, 0 when never measured.
 */
uint8_t battery_percent(void);

/**
 * @brief Ask for a measurement soon (e.g. right after a burst of BLE traffic).
 *
 * Safe to call from any context; it only reschedules a work item.
 */
void battery_request_update(void);

#endif /* SPEED_METER_BATTERY_H_ */
