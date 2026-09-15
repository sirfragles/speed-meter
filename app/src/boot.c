/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Boot-time self-test and firmware confirmation - see boot.h.
 */

#include "boot.h"

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_CSC_POWER_SAVE)
#include "wheel_power.h"
#endif

LOG_MODULE_REGISTER(boot, LOG_LEVEL_INF);

static const struct led_dt_spec led_red = LED_DT_SPEC_GET(DT_ALIAS(led0));
static const struct led_dt_spec led_green = LED_DT_SPEC_GET(DT_ALIAS(led1));
static const struct led_dt_spec led_blue = LED_DT_SPEC_GET(DT_ALIAS(led2));

static bool self_test_ok(void)
{
#if IS_ENABLED(CONFIG_CSC_POWER_SAVE)
	/* Minimal self-test: the LIS2DH12 answers WHO_AM_I over SPI. */
	return wheel_power_selftest();
#else
	return true;
#endif
}

static void blink(const struct led_dt_spec *spec, unsigned int times,
		  uint32_t period_ms)
{
	if (spec == NULL || times == 0U || period_ms == 0U) {
		return;
	}
	if (!led_is_ready_dt(spec)) {
		return;
	}

	(void)led_off_dt(spec);

	for (unsigned int i = 0U; i < times; i++) {
		(void)led_on_dt(spec);
		k_msleep(period_ms);
		(void)led_off_dt(spec);
		k_msleep(period_ms);
	}
}

/*
 * Alternating two-colour blink: a, b, a, b ... (pairs * 2 flashes).
 * Total duration: pairs * 2 * period_ms.
 *
 * Each colour is switched off before the next one is lit: lighting both for
 * even a moment would spike the current drawn from the coin cell and mix the
 * two colours.
 *
 * Blocking (k_msleep), so it must run from thread context - never from an ISR
 * or the system workqueue, where it would stall every other work item for the
 * whole animation. For a longer animation use led_sw_blink.c instead, which
 * toggles from a delayable work and can be cancelled - that is what the 5 s
 * DFU hold in dfu_button.c needs, since it must keep sampling the button.
 */
static void blink_alt(const struct led_dt_spec *a, const struct led_dt_spec *b,
		      unsigned int pairs, uint32_t period_ms)
{
	if (a == NULL || b == NULL || pairs == 0U || period_ms == 0U) {
		return;
	}
	if (!led_is_ready_dt(a) || !led_is_ready_dt(b)) {
		return;
	}

	(void)led_off_dt(a);
	(void)led_off_dt(b);

	for (unsigned int i = 0U; i < pairs; i++) {
		/* A */
		(void)led_off_dt(b);
		(void)led_on_dt(a);
		k_msleep(period_ms);

		/* B */
		(void)led_off_dt(a);
		(void)led_on_dt(b);
		k_msleep(period_ms);
	}

	(void)led_off_dt(a);
	(void)led_off_dt(b);
}

static void confirm_firmware(void)
{
	if (!self_test_ok()) {
		LOG_ERR("Self-test failed - image will roll back on reboot");
		blink_alt(&led_blue, &led_red, 2, 150);
		return;
	}

	if (!boot_is_img_confirmed()) {
		int err = boot_write_img_confirmed();

		if (err != 0) {
			LOG_ERR("Image confirmation failed: %d", err);
			blink_alt(&led_blue, &led_red, 2, 150);
		} else {
			LOG_INF("Firmware image confirmed");
			blink(&led_green, 3, 150);
		}
	}
}

void boot_init(void)
{
	confirm_firmware();
}
