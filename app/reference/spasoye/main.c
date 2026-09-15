// REFERENCE COPY (commented out) - spasoye/nrf52840_zephyr_CSC_sensor (MIT, see LICENSE-MIT.txt)
// Original file: main.c - https://github.com/spasoye/nrf52840_zephyr_CSC_sensor
// /* main.c - Application main entry point */
// 
// /*
//  * SPDX-License-Identifier: Apache-2.0
//  */
// 
// #include <zephyr/kernel.h>
// #include <zephyr/sys/printk.h>
// #include <zephyr/bluetooth/services/bas.h>
// 
// #include "bluetooth/ble_conn.h"
// #include "bluetooth/ble_csc.h"
// 
// #include "reed_switch/reed_switch.h"
// 
// #include "battery/battery.h"
// 
// #include "power/power_ctrl.h"
// 
// static void bas_notify(void)
// {
// 	uint8_t battery_level = battery_get_level_percent();
// 	uint8_t extr_battery_level = battery_get_extr_level_percent();
// 
// 	printk("Internal Battery level: %u%%\n", battery_level);
// 	printk("External Battery level: %u%%\n", extr_battery_level);
// 	printk("-------------------------\n");
// 	bt_bas_set_battery_level(extr_battery_level);
// }
// 
// int main(void)
// {
// 	int err;
// 	struct reed_switch_event_data wheel_data;
// 
// 	power_init();
// 	
// 	err = battery_init();
// 	if (err) {
// 		printk("Battery init failed (err %d)\n", err);
// 	}
//     else {
//         printk("Battery module initialized\n");
//     }
// 
// 	printk("Starting Cycling Speed and Cadence application\n");
// 
// 	/* Initialize Bluetooth */
// 	err = ble_conn_init();
// 	if (err) {
// 		printk("Failed to initialize Bluetooth (err %d)\n", err);
// 		return 0;
// 	}
// 
// 	/* Initialize reed switch */
// 	err = reed_switch_init();
// 	if (err) {
// 		printk("Failed to initialize reed switch (err %d)\n", err);
// 		return 0;
// 	}
// 
// 	printk("CSC sensor ready - reed switch events will be sent as wheel revolutions\n");
// 
// 	/* Main loop */
// 	while (1) {
// 		k_sleep(K_SECONDS(1));
// 
// 		/* Get real wheel revolution data from reed switch */
// 		reed_switch_get_csc_data(&wheel_data);
// 
// 		/* Send CSC measurement if BLE client is connected and subscribed */
// 		if (ble_csc_is_notify_enabled()) {
// 			ble_csc_update_wheel(wheel_data.count,
// 					     wheel_data.last_event_time,
// 					     wheel_data.has_new_event);
// 
// 			if (wheel_data.has_new_event) {
// 				printk("CSC notify: wheel_revs=%u, event_time=%u\n",
// 				       wheel_data.count, wheel_data.last_event_time);
// 			}
// 		}
// 
// 		bas_notify();
// 	}
// 
// 	return 0;
// }
