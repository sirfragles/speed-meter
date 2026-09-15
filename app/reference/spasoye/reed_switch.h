// REFERENCE COPY (commented out) - spasoye/nrf52840_zephyr_CSC_sensor (MIT, see LICENSE-MIT.txt)
// Original file: reed_switch.h - https://github.com/spasoye/nrf52840_zephyr_CSC_sensor
// #ifndef REED_SWITCH_H_
// #define REED_SWITCH_H_
// 
// #include <zephyr/types.h>
// #include <stdbool.h>
// 
// /* Debounce time in milliseconds (adjust based on your reed switch characteristics) */
// #define REED_SWITCH_DEBOUNCE_MS 20
// 
// /**
//  * @brief Reed switch event data for CSC integration
//  *
//  * Contains all data needed for Bluetooth CSC measurements
//  */
// struct reed_switch_event_data {
//     uint32_t count;           /* Cumulative revolution count */
//     uint16_t last_event_time; /* Last event time in 1/1024 sec units (CSC format) */
//     bool has_new_event;       /* True if new event since last read */
// };
// 
// /**
//  * @brief Initialize the reed switch
//  * @return 0 on success, negative error code otherwise
//  */
// int reed_switch_init(void);
// 
// /**
//  * @brief Get current event count
//  * @param count Pointer to store count value
//  */
// void reed_switch_get_event_count(int32_t *count);
// 
// /**
//  * @brief Reset event count to zero
//  */
// void reed_switch_reset_event_count(void);
// 
// /**
//  * @brief Get event data for CSC measurements
//  *
//  * Returns cumulative count and last event time in CSC-compatible format.
//  * The last_event_time is in 1/1024 second units as required by Bluetooth CSC spec.
//  *
//  * @param data Pointer to struct to fill with event data
//  */
// void reed_switch_get_csc_data(struct reed_switch_event_data *data);
// 
// void reed_switch_disable_interrupt(void);
// 
// #endif /* REED_SWITCH_H_ */