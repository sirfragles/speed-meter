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

/*
 * The shell has a detector of its own for its own recording, so `wheel status`
 * would happily describe that one and never the detector that counts the wheel
 * - healthy-looking output for a wheel that is not being counted at all. These
 * counters are the only view of the production detector from outside the
 * sampling thread, and they are what the plan's hardware session reads to tell
 * "the calibration rejected the data" from "the calibration never started".
 *
 * Numbers here are deterministic, not sampled: the fake sensor is constant, so
 * the movement gate never opens and the still window closes exactly once.
 */
ZTEST(wheel_source, test_the_production_detectors_counters_are_visible)
{
	struct wheel_detector_stats st;
	struct wheel_detector_result last;

	zassert_true(wheel_source_accel_detector_stats(&st, &last),
		     "the source reports no detector after a successful init");

	wheel_source_accel_detector_stats_reset();
	zassert_true(wheel_source_accel_detector_stats(&st, &last), "...");
	zassert_equal(st.still_windows, 0U, "reset did not clear the counters");

	step(60);

	zassert_true(wheel_source_accel_detector_stats(&st, &last),
		     "the counters stopped being readable once sampling started");

	zassert_equal(st.still_windows, 1U,
		      "a constant sensor should close exactly one still window, "
		      "not %u", st.still_windows);
	zassert_equal(st.moving_samples, 0U,
		      "a still sensor was classified as moving %u times",
		      st.moving_samples);

	/*
	 * The gate must sit on its configured floor while the wheel is still.
	 * If it ever rises above the gravity circle - about 2 g - nothing can be
	 * moving again and the detector never leaves IDLE, which is the failure
	 * the replay found in one of the recordings. A still sensor is the case
	 * where that must be impossible.
	 */
	zassert_within(st.gate_last, 0.5f, 0.001f,
		       "the gate left its floor while the sensor was still: "
		       "%.3f m/s2", (double)st.gate_last);
	zassert_within(st.noise_last, 0.0f, 0.001f,
		       "a constant sensor produced %.3f m/s2 of noise",
		       (double)st.noise_last);
	zassert_equal(st.plane_attempts, 0U,
		      "a still wheel started a calibration");

	zassert_equal(last.state, WHEEL_STATE_IDLE, "the detector left IDLE");
	zassert_equal(last.revolutions, 0U, "a still wheel produced revolutions");
}

ZTEST_SUITE(wheel_source, NULL, suite_setup, before_each, NULL, NULL);
