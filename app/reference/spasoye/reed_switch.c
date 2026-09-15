// REFERENCE COPY (commented out) - spasoye/nrf52840_zephyr_CSC_sensor (MIT, see LICENSE-MIT.txt)
// Original file: reed_switch.c - https://github.com/spasoye/nrf52840_zephyr_CSC_sensor
// #include "reed_switch.h"
// 
// #include <zephyr/kernel.h>
// #include <zephyr/device.h>
// #include <zephyr/drivers/gpio.h>
// #include <zephyr/sys/printk.h>
// #include <zephyr/sys/atomic_types.h>
// #include <zephyr/sys/atomic.h>
// 
// 
// /*
//  * Define the devicetree node for the reed switch
//  */
// 
// #define REED_SWITCH_NODE DT_ALIAS(reed_switch)
// #if !DT_NODE_HAS_STATUS(REED_SWITCH_NODE, okay)
// #error "Unsupported board: reed_switch devicetree alias is not defined"
// #endif
// 
// static struct gpio_dt_spec reed_switch_gpio = GPIO_DT_SPEC_GET_OR(REED_SWITCH_NODE, gpios, {0});
// static struct gpio_callback reed_switch_cb;
// 
// static atomic_t reed_switch_event_count = ATOMIC_INIT(0);
// 
// /*
//  * Debounce timestamp - stores last valid event time in milliseconds
//  * Using volatile ensures visibility across sleep/wake cycles
//  */
// static volatile int64_t last_event_time_ms;
// 
// /*
//  * Track last count returned to detect new events
//  */
// static uint32_t last_reported_count;
// 
// /**
//  * GPIO interrupt callback - runs in ISR context
//  *
//  * LOW-POWER OPTIMIZED DEBOUNCE:
//  * - This ISR will wake the system from sleep
//  * - We do minimal work here: timestamp check and atomic increment
//  * - No work queues or timers needed (they don't work well in sleep)
//  * - System can return to sleep immediately after ISR
//  *
//  * The timestamp-based approach works well with sleep because:
//  * 1. k_uptime_get() works across sleep/wake cycles
//  * 2. No dependency on running timers during sleep
//  * 3. Fast execution allows quick return to sleep
//  */
// static void reed_switch_callback_handler(const struct device *port,
//                                          struct gpio_callback *cb,
//                                          gpio_port_pins_t pins)
// {
//     int64_t current_time = k_uptime_get();
//     int64_t time_since_last = current_time - last_event_time_ms;
// 
//     /*
//      * Debounce check: only count if enough time passed since last event.
//      * This works correctly even if system was sleeping between events,
//      * because k_uptime_get() continues counting during sleep.
//      */
//     if (time_since_last >= REED_SWITCH_DEBOUNCE_MS) {
//         last_event_time_ms = current_time;
//         atomic_inc(&reed_switch_event_count);
//         printk("Reed switch event detected! Total count: %d\n",
//                (int)atomic_get(&reed_switch_event_count));
//     }
//     /* Else: bounce detected and ignored - return to sleep quickly */
// }
// 
// int reed_switch_init(void)
// {
//     int ret;
// 
//     if (!device_is_ready(reed_switch_gpio.port)) {
//         printk("Error: Reed switch GPIO device not ready\n");
//         return 0;
//     }
// 
//     ret = gpio_pin_configure_dt(&reed_switch_gpio, GPIO_INPUT);
//     if (ret != 0) {
//         printk("Error %d: failed to configure %s pin %d pin\n", ret,
//                reed_switch_gpio.port->name, reed_switch_gpio.pin);
//         return ret;
//     }
// 
//     /*
//      * Configure falling edge interrupt.
//      * This interrupt will wake the system from sleep modes,
//      * allowing low-power operation with GPIO wakeup.
//      */
//     ret = gpio_pin_interrupt_configure_dt(&reed_switch_gpio,
//                                          GPIO_INT_EDGE_FALLING);
//     if (ret != 0) {
//         printk("Error %d: failed to configure interrupt on %s pin %d\n", ret,
//                reed_switch_gpio.port->name, reed_switch_gpio.pin);
//         return ret;
//     }
// 
//     /* Initialize timestamp to allow first event immediately */
//     last_event_time_ms = k_uptime_get() - REED_SWITCH_DEBOUNCE_MS;
// 
//     gpio_init_callback(&reed_switch_cb, reed_switch_callback_handler,
//                       BIT(reed_switch_gpio.pin));
//     gpio_add_callback(reed_switch_gpio.port, &reed_switch_cb);
// 
//     printk("Reed switch initialized on %s pin %d with %d ms debounce\n",
//            reed_switch_gpio.port->name, reed_switch_gpio.pin,
//            REED_SWITCH_DEBOUNCE_MS);
//     printk("GPIO interrupt will wake system from sleep\n");
// 
//     return 0;
// }
// 
// void reed_switch_get_event_count(int32_t *count)
// {
//     *count = atomic_get(&reed_switch_event_count);
// }
// 
// void reed_switch_reset_event_count(void)
// {
//     atomic_set(&reed_switch_event_count, 0);
//     last_reported_count = 0;
// }
// 
// void reed_switch_get_csc_data(struct reed_switch_event_data *data)
// {
//     uint32_t current_count = atomic_get(&reed_switch_event_count);
//     int64_t event_time_ms = last_event_time_ms;
// 
//     data->count = current_count;
// 
//     /*
//      * Convert milliseconds to CSC time format (1/1024 second units).
//      * CSC spec: time = milliseconds * 1024 / 1000 = milliseconds * 1.024
//      *
//      * Using integer math: (ms * 1024) / 1000
//      * This gives ~0.977ms resolution, matching CSC spec requirement.
//      *
//      * The 16-bit value rolls over every 64 seconds (65536 / 1024 = 64),
//      * which is expected by the CSC protocol.
//      */
//     data->last_event_time = (uint16_t)((event_time_ms * 1024) / 1000);
// 
//     /* Check if there's a new event since last read */
//     data->has_new_event = (current_count != last_reported_count);
//     last_reported_count = current_count;
// }
// 
// void reed_switch_disable_interrupt(void)
// {
//     gpio_pin_interrupt_configure_dt(&reed_switch_gpio, GPIO_INT_DISABLE);
// }