// REFERENCE COPY (commented out) - spasoye/nrf52840_zephyr_CSC_sensor (MIT, see LICENSE-MIT.txt)
// Original file: ble_csc.h - https://github.com/spasoye/nrf52840_zephyr_CSC_sensor
// /* ble_csc.h - Bluetooth Cycling Speed and Cadence Service */
// 
// /*
//  * SPDX-License-Identifier: Apache-2.0
//  */
// 
// #ifndef BLE_CSC_H_
// #define BLE_CSC_H_
// 
// #include <zephyr/types.h>
// #include <stdbool.h>
// 
// /**
//  * @brief Initialize the CSC (Cycling Speed and Cadence) service
//  *
//  * Sets up the BLE GATT service for cycling speed and cadence measurements.
//  */
// void ble_csc_init(void);
// 
// /**
//  * @brief Check if CSC notifications are enabled
//  *
//  * @return true if a BLE client has enabled CSC measurement notifications
//  */
// bool ble_csc_is_notify_enabled(void);
// 
// /**
//  * @brief Check if CSC simulation is enabled (deprecated - use ble_csc_is_notify_enabled)
//  *
//  * @return true if notifications are enabled and simulation should run
//  */
// bool ble_csc_is_simulation_enabled(void);
// 
// /**
//  * @brief Run CSC simulation step
//  *
//  * Simulates wheel and crank revolutions and sends notifications.
//  * Should be called periodically (e.g., once per second).
//  */
// void ble_csc_simulate(void);
// 
// /**
//  * @brief Update CSC with real wheel revolution data from reed switch
//  *
//  * Call this function periodically (e.g., every 1-2 seconds) to send
//  * wheel revolution measurements to connected BLE clients.
//  *
//  * @param wheel_revs Cumulative wheel revolution count
//  * @param last_wheel_event_time Last wheel event time in 1/1024 sec units
//  * @param has_new_wheel_data True if wheel data has changed since last call
//  */
// void ble_csc_update_wheel(uint32_t wheel_revs, uint16_t last_wheel_event_time,
//                           bool has_new_wheel_data);
// 
// /**
//  * @brief Update CSC with real crank revolution data from reed switch
//  *
//  * Call this function periodically (e.g., every 1-2 seconds) to send
//  * crank revolution measurements to connected BLE clients.
//  *
//  * @param crank_revs Cumulative crank revolution count (16-bit)
//  * @param last_crank_event_time Last crank event time in 1/1024 sec units
//  * @param has_new_crank_data True if crank data has changed since last call
//  */
// void ble_csc_update_crank(uint16_t crank_revs, uint16_t last_crank_event_time,
//                           bool has_new_crank_data);
// 
// #endif /* BLE_CSC_H_ */
