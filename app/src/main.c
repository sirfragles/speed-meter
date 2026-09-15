/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Speed Meter - Bluetooth cycling speed sensor (CSCS, service UUID 0x1816).
 *
 * Apple Watch (watchOS 10+) and iPhone discover cycling sensors by the CSCS
 * UUID in the advertising data. This skeleton advertises the service and
 * optionally simulates wheel revolutions so it can be paired and tested
 * before the real wheel sensor hardware is attached.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "bt.h"
#include "csc.h"
#include "wheel_config.h"

#if IS_ENABLED(CONFIG_CSC_BATTERY)
#include "battery.h"
#endif

#if IS_ENABLED(CONFIG_CSC_WHEEL_SENSOR_GPIO)
#include "wheel_source_gpio.h"
#endif

#if IS_ENABLED(CONFIG_CSC_WHEEL_SENSOR_ACCEL)
#include "wheel_source_accel.h"
#endif

#if IS_ENABLED(CONFIG_CSC_DFU)
#include "dfu_mode.h"
#include "dfu_button.h"
#endif

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
#include "boot.h"
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/*
 * The Bluetooth advertising, status LED, boot confirmation and simulation
 * now live in their own modules: bt.c, boot.c and sim.c.
 */

int main(void)
{
	int err;

	LOG_INF("Speed Meter (CSCS, 0x1816) starting");

	/*
	 * Wheel configuration first: it seeds the defaults and registers the
	 * settings handler, so the settings_load() below also restores the wheel
	 * circumference and detector tuning.
	 */
	err = wheel_config_init();
	if (err != 0) {
		LOG_ERR("Wheel config init failed: %d", err);
	}

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("Bluetooth init failed: %d", err);
		return 0;
	}

#if IS_ENABLED(CONFIG_SETTINGS)
	/*
	 * Restores the identities/bonds stored over BT_SETTINGS plus the wheel
	 * configuration. With CONFIG_BT_SETTINGS the stack has no identity
	 * address until this runs, so paired clients could not reconnect.
	 */
	err = settings_load();
	if (err != 0) {
		LOG_ERR("settings_load failed: %d", err);
	}
#endif

	(void)bt_init();

#if IS_ENABLED(CONFIG_BT_BAS)
	if (!IS_ENABLED(CONFIG_CSC_BATTERY)) {
		/*
		 * No measurement path: publish the fixed fallback level so the
		 * Battery Service still reports something sensible. With
		 * CONFIG_CSC_BATTERY the SAADC reading takes over below.
		 */
		bt_bas_set_battery_level(CONFIG_CSC_BAS_LEVEL);
	}
#endif

#if IS_ENABLED(CONFIG_CSC_BATTERY)
	/*
	 * Needs the Bluetooth stack up (it publishes through the Battery
	 * Service). Runs before advertising starts, so the first client to
	 * connect already reads a real level instead of a placeholder.
	 */
	err = battery_init();
	if (err != 0) {
		LOG_ERR("Battery measurement init failed: %d", err);
	}
#endif

	LOG_INF("Bluetooth initialized, advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
	bt_start_advertising();

#if IS_ENABLED(CONFIG_CSC_DFU)
	err = dfu_mode_init();
	if (err != 0) {
		LOG_ERR("DFU mode init failed: %d", err);
		return 0;
	}

	err = dfu_button_init();
	if (err != 0) {
		LOG_ERR("DFU button init failed: %d", err);
	}
#endif /* CONFIG_CSC_DFU */

#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
	boot_init();
#endif

#if IS_ENABLED(CONFIG_CSC_WHEEL_SENSOR_ACCEL)
	/*
	 * Magnetless source: its own thread samples the accelerometer, counts
	 * revolutions and drives the sleep/wake state machine.
	 */
	err = wheel_source_accel_init();
	if (err != 0) {
		LOG_ERR("Accelerometer wheel source init failed: %d", err);
	}
#endif /* CONFIG_CSC_WHEEL_SENSOR_ACCEL */

#if IS_ENABLED(CONFIG_CSC_WHEEL_SENSOR_GPIO)
	err = wheel_source_gpio_init();
	if (err != 0) {
		LOG_ERR("Wheel sensor init failed: %d", err);
	}

	LOG_INF("Wheel source: GPIO wheel sensor - waiting for revolutions");

	for (;;) {
		struct wheel_revolution_data wheel;

		k_sleep(K_SECONDS(1));
		wheel_source_gpio_read(&wheel);
		if (wheel.has_new_revolution) {
			LOG_INF("Wheel revolution %u (t=%u)", wheel.revolutions,
				wheel.last_event_time);
			csc_publish_wheel(wheel.revolutions, wheel.last_event_time);
		}
	}
#endif /* CONFIG_CSC_WHEEL_SENSOR_GPIO */

	return 0;
}
