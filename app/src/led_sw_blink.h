/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software LED blink.
 *
 * The hardware blink in the LED API (`led_blink()`) is optional, and the GPIO
 * LED driver used on HOLYIOT-25008 does not implement it: `led_gpio_api` only
 * provides `set_brightness`, so `led_blink()` returns -ENOSYS and silently
 * does nothing. `led_on()`/`led_off()` are emulated on top of
 * `set_brightness()`, which is why plain on/off works there.
 *
 * This helper provides the missing piece for any LED driver: a delayable work
 * that toggles the LED and reschedules itself. It never blocks, so it is safe
 * on the system workqueue and does not stall other work items, and it can be
 * stopped at any time - which the status signalling needs, since the DFU LED
 * changes rate while the button is held.
 *
 * Each caller owns a context, so several LEDs can blink independently without
 * any shared registry.
 */

#ifndef SPEED_METER_LED_SW_BLINK_H_
#define SPEED_METER_LED_SW_BLINK_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>

struct led_sw_blink {
	const struct led_dt_spec *spec;
	struct k_work_delayable work;
	uint32_t on_ms;
	uint32_t off_ms;
	uint32_t remaining; /* half-periods left; 0 means "until stopped" */
	bool lit;
};

/**
 * @brief Prepare a blink context (before any start/stop call).
 *
 * @return 0 on success, negative errno otherwise.
 */
int led_sw_blink_init(struct led_sw_blink *ctx, const struct led_dt_spec *spec);

/**
 * @brief Start blinking.
 *
 * @param on_ms      time the LED stays on per cycle
 * @param off_ms     time the LED stays off per cycle
 * @param cycles     0 to blink until stopped, otherwise the number of full
 *                   on+off cycles before the LED is left off
 *
 * Restarting an already blinking LED simply switches to the new rate, and the
 * LED is always left off when the blink ends.
 *
 * @return 0 on success, negative errno otherwise.
 */
int led_sw_blink_start(struct led_sw_blink *ctx, uint32_t on_ms, uint32_t off_ms,
		       uint32_t cycles);

/** @brief Stop blinking and turn the LED off. Safe to call when not blinking. */
void led_sw_blink_stop(struct led_sw_blink *ctx);

/** @brief True while a blink is running. */
bool led_sw_blink_is_running(const struct led_sw_blink *ctx);

#endif /* SPEED_METER_LED_SW_BLINK_H_ */
