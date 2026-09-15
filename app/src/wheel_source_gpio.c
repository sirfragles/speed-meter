/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * Wheel revolution sensing over a GPIO pulse input (e.g. Hall sensor).
 * Derived from the spasoye/nrf52840_zephyr_CSC_sensor project
 * (Copyright (c) 2026 Ivan Spasic, MIT licence - full text in
 * reference/spasoye/LICENSE-MIT.txt); adapted for HOLYIOT-25008 / nRF54L15.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wheel_source_gpio.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(wheel_source_gpio, LOG_LEVEL_INF);

#define WHEEL_SENSOR_NODE DT_ALIAS(wheel_sensor)

#if !DT_HAS_ALIAS(wheel_sensor)
#error "Enable CONFIG_CSC_WHEEL_SENSOR_GPIO only on boards whose overlay defines the 'wheel-sensor' devicetree alias (see boards/holyiot_25008_nrf54l15_cpuapp.overlay)."
#endif

static const struct gpio_dt_spec wheel_sensor_gpio = GPIO_DT_SPEC_GET(WHEEL_SENSOR_NODE, gpios);

static struct gpio_callback wheel_sensor_cb;

/* Cumulative revolution counter, incremented from the ISR. */
static atomic_t wheel_sensor_event_count = ATOMIC_INIT(0);

/*
 * Timestamp of the last accepted edge (in milliseconds). Written from the
 * ISR, read from thread context. k_uptime_get() keeps counting through sleep,
 * so this debounce works with power management (unlike timers/work queues).
 */
static volatile int64_t last_event_time_ms;

/* Last count returned to the caller (detects "new event" without races). */
static uint32_t last_reported_count;

static void wheel_sensor_isr(const struct device *port, struct gpio_callback *cb,
			     gpio_port_pins_t pins)
{
	int64_t now = k_uptime_get();

	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* Bounce check: ignore edges that arrive too soon after the last one. */
	if ((now - last_event_time_ms) < CONFIG_CSC_WHEEL_SENSOR_DEBOUNCE_MS) {
		return;
	}

	last_event_time_ms = now;
	atomic_inc(&wheel_sensor_event_count);

	LOG_DBG("Revolution %d", (int)atomic_get(&wheel_sensor_event_count));
}

int wheel_source_gpio_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&wheel_sensor_gpio)) {
		LOG_ERR("Wheel sensor GPIO not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&wheel_sensor_gpio, GPIO_INPUT);
	if (err != 0) {
		LOG_ERR("Failed to configure wheel sensor pin: %d", err);
		return err;
	}

	/* A falling edge happens when the contact closes the input to GND. */
	err = gpio_pin_interrupt_configure_dt(&wheel_sensor_gpio, GPIO_INT_EDGE_FALLING);
	if (err != 0) {
		LOG_ERR("Failed to configure wheel sensor interrupt: %d", err);
		return err;
	}

	/* Allow the first event to be accepted immediately. */
	last_event_time_ms = k_uptime_get() - CONFIG_CSC_WHEEL_SENSOR_DEBOUNCE_MS;

	gpio_init_callback(&wheel_sensor_cb, wheel_sensor_isr, BIT(wheel_sensor_gpio.pin));
	err = gpio_add_callback_dt(&wheel_sensor_gpio, &wheel_sensor_cb);
	if (err != 0) {
		LOG_ERR("Failed to add wheel sensor callback: %d", err);
		return err;
	}

	LOG_INF("Wheel sensor ready: %s pin %u, debounce %u ms",
		wheel_sensor_gpio.port->name, wheel_sensor_gpio.pin,
		CONFIG_CSC_WHEEL_SENSOR_DEBOUNCE_MS);

	return 0;
}

void wheel_source_gpio_read(struct wheel_revolution_data *data)
{
	uint32_t count = (uint32_t)atomic_get(&wheel_sensor_event_count);
	int64_t event_time_ms = last_event_time_ms;

	data->revolutions = count;

	/* Convert ms to the CSC time format (1/1024 s units); the uint16
	 * rolls over every 64 s, which the CSCS specification expects.
	 */
	data->last_event_time = (uint16_t)((event_time_ms * 1024) / 1000);
	data->has_new_revolution = (count != last_reported_count);
	last_reported_count = count;
}
