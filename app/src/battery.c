/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CR2032 battery measurement - see battery.h.
 *
 * Zephyr provides the whole measurement path (SAADC driver, devicetree
 * channel, raw-to-millivolts conversion). The one thing it does not provide is
 * a voltage-to-percentage curve for a coin cell, because that depends on the
 * cell chemistry and the load profile - so the curve below is ours, and the
 * absolute accuracy can be trimmed against a multimeter with
 * CONFIG_CSC_BATTERY_CAL_PPM.
 */

#include "battery.h"

#include <errno.h>

#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_CSC_POWER_SAVE)
#include "wheel_power.h"
#endif

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

#define BATTERY_NODE DT_PATH(zephyr_user)

#if !DT_NODE_EXISTS(BATTERY_NODE)
#error "CONFIG_CSC_BATTERY needs the zephyr,user node with io-channels (see the board overlay)."
#endif

static const struct adc_dt_spec battery_adc =
	ADC_DT_SPEC_GET_BY_IDX(BATTERY_NODE, 0);

static struct k_work_delayable battery_work;

static uint8_t last_percent;
static bool adc_ready;

/* Offset calibration only has to run once after reset. */
static bool adc_calibrated;

/*
 * CR2032 discharge curve, no-load and at a low duty cycle. Interpolated rather
 * than scaled, because a coin cell holds a plateau around 2.9-3.0 V and then
 * falls off quickly.
 */
struct battery_point {
	uint16_t mv;
	uint8_t percent;
};

static const struct battery_point cr2032_curve[] = {
	{ 3200, 100 },
	{ 3050, 95 },
	{ 3000, 90 },
	{ 2950, 80 },
	{ 2900, 65 },
	{ 2850, 50 },
	{ 2800, 35 },
	{ 2700, 18 },
	{ 2600, 7 },
	{ 2500, 0 },
};

static bool standby(void)
{
#if IS_ENABLED(CONFIG_CSC_POWER_SAVE)
	/* wheel_power only leaves standby when the wheel is turning. */
	return wheel_power_is_standby();
#else
	return false;
#endif
}

static void schedule_next(void)
{
	uint32_t interval_s = standby() ? CONFIG_CSC_BATTERY_INTERVAL_IDLE_S
					: CONFIG_CSC_BATTERY_INTERVAL_RIDING_S;

	(void)k_work_reschedule(&battery_work, K_SECONDS(interval_s));
}

static void battery_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	(void)battery_update();
	schedule_next();
}

int battery_read_mv(int32_t *voltage_mv)
{
	int16_t raw;
	int err;
	struct adc_sequence sequence = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};

	if (!adc_ready) {
		return -ENODEV;
	}

	err = adc_sequence_init_dt(&battery_adc, &sequence);
	if (err != 0) {
		return err;
	}

	/* The offset calibration is only valid once per power cycle. */
	sequence.calibrate = !adc_calibrated;

	err = adc_read_dt(&battery_adc, &sequence);
	if (err != 0) {
		return err;
	}
	adc_calibrated = true;

	/*
	 * adc_raw_to_millivolts_dt() takes the raw value in *voltage_mv and
	 * converts it in place, using the gain/reference/resolution from
	 * devicetree and the driver's internal reference value.
	 */
	*voltage_mv = raw;
	err = adc_raw_to_millivolts_dt(&battery_adc, voltage_mv);
	if (err != 0) {
		return err;
	}

#if CONFIG_CSC_BATTERY_CAL_PPM != 0
	*voltage_mv = (int32_t)(((int64_t)*voltage_mv *
				 (1000000 + CONFIG_CSC_BATTERY_CAL_PPM)) /
				1000000);
#endif

	return 0;
}

uint8_t battery_percent_from_mv(int32_t voltage_mv)
{
	if (voltage_mv >= cr2032_curve[0].mv) {
		return 100U;
	}

	for (size_t i = 1; i < ARRAY_SIZE(cr2032_curve); i++) {
		const struct battery_point *high = &cr2032_curve[i - 1];
		const struct battery_point *low = &cr2032_curve[i];

		if (voltage_mv >= low->mv) {
			int32_t span_mv = high->mv - low->mv;
			int32_t span_pct = high->percent - low->percent;

			return (uint8_t)(low->percent +
					 ((voltage_mv - low->mv) * span_pct) / span_mv);
		}
	}

	return 0U;
}

int battery_update(void)
{
	int32_t mv = 0;
	int err = battery_read_mv(&mv);

	if (err != 0) {
		LOG_WRN("battery read failed: %d", err);
		return err;
	}

	last_percent = battery_percent_from_mv(mv);
	LOG_INF("battery: %d mV (%u%%)", (int)mv, last_percent);

#if IS_ENABLED(CONFIG_BT_BAS)
	bt_bas_set_battery_level(last_percent);
#endif

	return 0;
}

uint8_t battery_percent(void)
{
	return last_percent;
}

void battery_request_update(void)
{
	if (adc_ready) {
		(void)k_work_reschedule(&battery_work, K_SECONDS(1));
	}
}

int battery_init(void)
{
	int err;

	k_work_init_delayable(&battery_work, battery_work_handler);

	if (!adc_is_ready_dt(&battery_adc)) {
		LOG_ERR("SAADC not ready");
		return -ENODEV;
	}

	err = adc_channel_setup_dt(&battery_adc);
	if (err != 0) {
		LOG_ERR("ADC channel setup failed: %d", err);
		return err;
	}

	adc_ready = true;

	/* First measurement right away, so the Battery Service is never stale. */
	err = battery_update();
	if (err != 0) {
		return err;
	}

	schedule_next();
	return 0;
}
