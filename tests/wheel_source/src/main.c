/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Integration test for the accelerometer wheel source: the piece between the
 * sensor driver and the CSCS wire, which the unit tests on either side cannot
 * reach.
 *
 * The source's sampling thread is not started here (wheel_source_accel.c
 * omits K_THREAD_DEFINE under CONFIG_ZTEST); the test drives
 * wheel_source_accel_step() itself, so nothing depends on the kernel clock or
 * on how the scheduler happened to interleave.
 */

#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "fake_accel.h"
#include "stubs.h"
#include "wheel_config.h"
#include "wheel_source_accel.h"

/* Advanced by hand instead of by the kernel, so the cadence is deterministic. */
static uint64_t next_us;
static uint32_t period_us;

static void step(unsigned int times)
{
	for (unsigned int i = 0; i < times; i++) {
		wheel_source_accel_step(&next_us, &period_us);
	}
}

static void *suite_setup(void)
{
	zassert_ok(wheel_source_accel_init(), "the source refused to start");

	return NULL;
}

static void reset_harness(void)
{
	fake_accel_reset_counters();
	fake_accel_fail_frequency(false);
	fake_accel_fail_full_scale(false);
	fake_accel_fail_fetch(0);
	fake_accel_report_no_data(false);
	fake_accel_set_sample(0.0f, 0.0f, 9.80665f);
	stub_publish_reset();
	next_us = 0;
	period_us = 0;
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);

	reset_harness();
}

ZTEST(wheel_source, test_the_harness_is_wired_up)
{
	const struct device *accel = DEVICE_DT_GET(DT_ALIAS(accel0));

	zassert_true(device_is_ready(accel), "the fake accelerometer is not ready");
	zassert_not_null(wheel_config_get(), "the configuration stub is missing");
}

ZTEST(wheel_source, test_the_source_configures_the_sensor_before_measuring)
{
	reset_harness();

	step(1);

	zassert_equal(fake_accel_frequency_sets(), 1U,
		      "the source must push the ODR into the sensor");
	zassert_equal(fake_accel_full_scale_sets(), 1U,
		      "the source must push the full scale into the sensor");
}

ZTEST_SUITE(wheel_source, NULL, suite_setup, before_each, NULL, NULL);
