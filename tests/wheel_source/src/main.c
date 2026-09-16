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

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
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

	/*
	 * wheel_source_accel_init() re-initialises the CSC state and the
	 * reconfiguration gate, which are static and would otherwise carry over
	 * between tests: a later test would find the sensor already configured
	 * and see no attr_set() calls at all.
	 */
	zassert_ok(wheel_source_accel_init(), "the source refused to restart");
	fake_accel_fail_frequency(false);
	fake_accel_fail_full_scale(false);
	fake_accel_fail_fetch(0);
	fake_accel_report_no_data(false);
	fake_accel_set_sample(0.0f, 0.0f, 9.80665f);
	stub_publish_reset();
	wheel_source_accel_stats_reset();
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

/*
 * A bus error means the samples in between never arrived. The detector only
 * sees that time passed, so it has to be told or it interpolates across a hole
 * it cannot see.
 */
ZTEST(wheel_source, test_a_bus_error_is_reported_to_the_detector)
{
	struct wheel_source_stats stats;

	fake_accel_fail_fetch(-EIO);

	step(1);

	wheel_source_accel_stats(&stats);
	zassert_equal(stats.lost, 1U,
		      "the detector was not told about the lost samples");
	zassert_equal(stats.no_data, 0U,
		      "a bus error is not the same as 'no new sample yet'");
}

/*
 * -ENODATA at a polling rate equal to the ODR is the normal case. Telling the
 * detector would drop a phase that is still perfectly valid - which is why the
 * first version of the plan, proposing exactly that, was wrong.
 */
ZTEST(wheel_source, test_no_new_sample_yet_is_not_reported_as_loss)
{
	struct wheel_source_stats stats;

	fake_accel_report_no_data(true);

	step(3);

	wheel_source_accel_stats(&stats);
	zassert_equal(stats.no_data, 3U, "-ENODATA was not counted as normal");
	zassert_equal(stats.lost, 0U,
		      "-ENODATA must not be treated as a discontinuity");
	zassert_equal(stub_published_count(), 0U, "nothing to publish");
}

/*
 * A thread held off for longer than a sampling period leaves the deadline in
 * the past. Advancing it by one period keeps it there, and the following
 * passes fetch back to back - reading the same sample over and over - until it
 * catches up.
 */
ZTEST(wheel_source, test_a_stale_deadline_is_rebased_not_chased)
{
	uint64_t now;

	step(1); /* settles period_us from the configured ODR */

	/* Let the kernel clock move on, then drop the deadline far behind. */
	k_sleep(K_MSEC(50));
	next_us = 0;

	step(1);

	now = k_ticks_to_us_floor64(k_uptime_ticks());
	zassert_true(next_us > now,
		     "the deadline is still in the past (next=%llu, now=%llu), so "
		     "the following passes would fetch back to back",
		     next_us, now);
}

ZTEST_SUITE(wheel_source, NULL, suite_setup, before_each, NULL, NULL);
