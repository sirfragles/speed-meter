/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DFU entry trigger + status signalling on the red LED (led0, P2.09).
 *
 * Hold the board button (sw0, P1.13) for CONFIG_CSC_DFU_BUTTON_LONG_MS to
 * enter DFU mode. While holding, the LED blinks with an accelerating rate so
 * the user feels the countdown approaching; releasing early cancels.
 *
 * The LED is driven through the Zephyr LED driver (gpio-leds); the button is
 * sampled via a GPIO interrupt + debounce work. Everything is non-blocking.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_MCUMGR_GRP_IMG) && \
	IS_ENABLED(CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS)
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_callbacks.h>
#endif

#include "dfu_mode.h"
#include "led_sw_blink.h"

LOG_MODULE_REGISTER(dfu_button, LOG_LEVEL_INF);

#define DFU_HOLD_TIME_MS     ((uint32_t)CONFIG_CSC_DFU_BUTTON_LONG_MS)
#define BUTTON_DEBOUNCE_MS   30U

/* Accelerating arming blink: 600 ms -> 50 ms over the hold. */
#define BLINK_START_MS       600U
#define BLINK_END_MS         50U
/* DFU state blinks. */
#define DFU_ACTIVE_BLINK_MS  500U
#define DFU_UPLOAD_BLINK_MS  100U

#if !DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
#error "Missing sw0 devicetree alias"
#endif

static const struct gpio_dt_spec button =
	GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static const struct led_dt_spec led = LED_DT_SPEC_GET(DT_ALIAS(led0));

static struct led_sw_blink led_status_blink;

enum dfu_button_state {
	DFU_BUTTON_IDLE,
	DFU_BUTTON_ARMING,
	DFU_BUTTON_ACTIVE,
	DFU_BUTTON_UPLOADING,
	DFU_BUTTON_UPLOADED,
};

static enum dfu_button_state state = DFU_BUTTON_IDLE;

static struct gpio_callback button_callback;
static struct k_work_delayable debounce_work;
static struct k_work_delayable arm_work; /* accelerating arming blink */

static bool led_on_state;
static int64_t hold_started_ms;

/* Accelerating blink interval: 600 ms -> 50 ms over the hold. */
static uint32_t calculate_blink_interval(uint32_t elapsed_ms)
{
	uint32_t remaining;
	uint64_t scaled;

	if (elapsed_ms >= DFU_HOLD_TIME_MS) {
		return BLINK_END_MS;
	}

	remaining = DFU_HOLD_TIME_MS - elapsed_ms;
	scaled = (uint64_t)remaining * remaining *
		 (BLINK_START_MS - BLINK_END_MS);
	scaled /= (uint64_t)DFU_HOLD_TIME_MS * DFU_HOLD_TIME_MS;

	return BLINK_END_MS + (uint32_t)scaled;
}

static void dfu_led_error(void)
{
	/* Three quick blinks; the blinker turns the LED off by itself. */
	(void)led_sw_blink_start(&led_status_blink, 100, 100, 3);
}

static void cancel_dfu_arming(void)
{
	state = DFU_BUTTON_IDLE;

	(void)k_work_cancel_delayable(&arm_work);
	led_on_state = false;
	led_sw_blink_stop(&led_status_blink);

	LOG_INF("DFU entry cancelled");
}

static void arm_work_handler(struct k_work *work)
{
	uint32_t elapsed_ms;
	int err;

	ARG_UNUSED(work);

	/* The button must still be physically held. */
	err = gpio_pin_get_dt(&button);
	if (err <= 0) {
		cancel_dfu_arming();
		return;
	}

	elapsed_ms = (uint32_t)(k_uptime_get() - hold_started_ms);

	if (elapsed_ms >= DFU_HOLD_TIME_MS) {
		state = DFU_BUTTON_ACTIVE;

		/* Brief solid light marks the hold point. */
		(void)led_on_dt(&led);

		err = dfu_mode_enter();
		if (err != 0 && err != -EALREADY) {
			LOG_ERR("Cannot enter DFU mode: %d", err);
			state = DFU_BUTTON_IDLE;
			dfu_led_error();
			return;
		}

		LOG_INF("DFU mode enabled");

		/* Steady, regular blink while waiting for the phone. */
		(void)led_sw_blink_start(&led_status_blink, DFU_ACTIVE_BLINK_MS,
					 DFU_ACTIVE_BLINK_MS, 0U);
		return;
	}

	led_on_state = !led_on_state;
	if (led_on_state) {
		(void)led_on_dt(&led);
	} else {
		(void)led_off_dt(&led);
	}

	(void)k_work_reschedule(&arm_work,
				K_MSEC(calculate_blink_interval(elapsed_ms)));
}

#if IS_ENABLED(CONFIG_MCUMGR_GRP_IMG) && \
	IS_ENABLED(CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS)
static enum mgmt_cb_return dfu_img_mgmt_cb(uint32_t event,
					   enum mgmt_cb_return prev_status,
					   int32_t *rc, uint16_t *group,
					   bool *abort_more, void *data,
					   size_t data_size)
{
	ARG_UNUSED(prev_status);
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(data_size);

	switch (event) {
	case MGMT_EVT_OP_IMG_MGMT_DFU_STARTED:
		if (state == DFU_BUTTON_ACTIVE) {
			state = DFU_BUTTON_UPLOADING;
			(void)led_sw_blink_start(&led_status_blink, DFU_UPLOAD_BLINK_MS,
						 DFU_UPLOAD_BLINK_MS, 0U);
		}
		break;

	case MGMT_EVT_OP_IMG_MGMT_DFU_PENDING:
		/* Transfer finished: solid light until reset. Stop the blink first -
		 * led_on_dt() on its own would not cancel it, and the blinker
		 * would toggle the light away again on its next half period.
		 */
		state = DFU_BUTTON_UPLOADED;
		led_sw_blink_stop(&led_status_blink);
		(void)led_on_dt(&led);
		break;

	case MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED:
		/* Aborted mid-upload: back to waiting for the phone. */
		if (state == DFU_BUTTON_UPLOADING) {
			state = DFU_BUTTON_ACTIVE;
			(void)led_sw_blink_start(&led_status_blink, DFU_ACTIVE_BLINK_MS,
						 DFU_ACTIVE_BLINK_MS, 0U);
		}
		break;

	default:
		break;
	}

	return MGMT_CB_OK;
}

static struct mgmt_callback dfu_img_mgmt_callback = {
	.callback = dfu_img_mgmt_cb,
	.event_id = MGMT_EVT_OP_IMG_MGMT_DFU_STARTED |
		    MGMT_EVT_OP_IMG_MGMT_DFU_PENDING |
		    MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED,
};
#endif /* CONFIG_MCUMGR_GRP_IMG && CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS */

static void debounce_work_handler(struct k_work *work)
{
	int pressed;

	ARG_UNUSED(work);

	pressed = gpio_pin_get_dt(&button);
	if (pressed < 0) {
		LOG_ERR("Cannot read DFU button: %d", pressed);
		return;
	}

	if (pressed) {
		if (state != DFU_BUTTON_IDLE) {
			return;
		}

		state = DFU_BUTTON_ARMING;
		hold_started_ms = k_uptime_get();

		/* A previous state may have left a blink running. */
		led_sw_blink_stop(&led_status_blink);
		led_on_state = true;
		(void)led_on_dt(&led);
		(void)k_work_reschedule(&arm_work, K_MSEC(BLINK_START_MS));

		LOG_INF("Hold button for %u s to enter DFU",
			(unsigned int)(DFU_HOLD_TIME_MS / 1000U));
		return;
	}

	if (state == DFU_BUTTON_ARMING) {
		cancel_dfu_arming();
	}
}

static void button_isr(const struct device *port, struct gpio_callback *callback,
		       gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(callback);
	ARG_UNUSED(pins);

	/* No logic in the ISR: wait for the contacts to settle, then read. */
	(void)k_work_reschedule(&debounce_work, K_MSEC(BUTTON_DEBOUNCE_MS));
}

int dfu_button_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&button) || !led_is_ready_dt(&led)) {
		LOG_ERR("DFU button/LED not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (err != 0) {
		LOG_ERR("DFU button configure failed: %d", err);
		return err;
	}

	k_work_init_delayable(&debounce_work, debounce_work_handler);
	k_work_init_delayable(&arm_work, arm_work_handler);

	err = led_sw_blink_init(&led_status_blink, &led);
	if (err != 0) {
		LOG_ERR("LED blink init failed: %d", err);
		return err;
	}

	gpio_init_callback(&button_callback, button_isr, BIT(button.pin));

	err = gpio_add_callback_dt(&button, &button_callback);
	if (err != 0) {
		LOG_ERR("DFU button callback add failed: %d", err);
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
	if (err != 0) {
		LOG_ERR("DFU button interrupt config failed: %d", err);
		(void)gpio_remove_callback_dt(&button, &button_callback);
		return err;
	}

#if IS_ENABLED(CONFIG_MCUMGR_GRP_IMG) && \
	IS_ENABLED(CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS)
	mgmt_callback_register(&dfu_img_mgmt_callback);
#endif

	LOG_INF("DFU button initialized (hold %u s to enter DFU)",
		(unsigned int)(DFU_HOLD_TIME_MS / 1000U));
	return 0;
}
