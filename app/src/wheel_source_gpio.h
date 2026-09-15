/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * Wheel revolutions from a GPIO pulse input (Hall sensor, reed contact or any
 * switch to GND) - one of the mutually exclusive wheel sources selected by
 * `choice CSC_WHEEL_SOURCE`. The alternatives are wheel_source_accel.c (the
 * magnetless accelerometer source) and wheel_source_sim.c (the simulator).
 *
 * This implementation is derived from the
 * spasoye/nrf52840_zephyr_CSC_sensor project (Copyright (c) 2026 Ivan Spasic,
 * MIT licence - full text in reference/spasoye/LICENSE-MIT.txt) and adapted
 * for HOLYIOT-25008 / nRF54L15.
 *
 * Unlike the other two sources this one is polled from main(): it does not run
 * a thread of its own.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPEED_METER_WHEEL_SOURCE_GPIO_H_
#define SPEED_METER_WHEEL_SOURCE_GPIO_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Wheel revolution data in CSC format.
 */
struct wheel_revolution_data {
        /** Cumulative wheel revolution count. */
        uint32_t revolutions;
        /** Time of the last revolution in 1/1024 s units (CSC format). */
        uint16_t last_event_time;
        /** True when a new revolution arrived since the previous read. */
        bool has_new_revolution;
};

/**
 * @brief Initialise the GPIO pulse source.
 *
 * Uses the `wheel-sensor` devicetree alias (see the board overlay) and the
 * CONFIG_CSC_WHEEL_SENSOR_GPIO / CONFIG_CSC_WHEEL_SENSOR_DEBOUNCE_MS options.
 *
 * @return 0 on success, negative errno otherwise.
 */
int wheel_source_gpio_init(void);

/**
 * @brief Read the current revolution data (call periodically, e.g. every 1 s).
 *
 * @param data Filled with the cumulative count, last event time in 1/1024 s
 *             units and a "new revolution" flag.
 */
void wheel_source_gpio_read(struct wheel_revolution_data *data);

#endif /* SPEED_METER_WHEEL_SOURCE_GPIO_H_ */
