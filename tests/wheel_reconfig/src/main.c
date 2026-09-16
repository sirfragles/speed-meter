/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The fail-closed contract, tested without a sensor or a kernel clock.
 */

#include <zephyr/ztest.h>

#include "wheel_reconfig.h"

#define RETRY_MS 2000U

static const struct wheel_reconfig_keys wanted = {
	.odr_hz = 100,
	.range_g = 16,
	.circ_mm = 2100,
	.rpm_max = 1200,
};

ZTEST(wheel_reconfig, test_nothing_is_forwarded_before_the_first_apply)
{
	struct wheel_reconfig rc;

	wheel_reconfig_init(&rc);

	zassert_false(wheel_reconfig_ready(&rc),
		      "no configuration has been applied yet");
	zassert_true(wheel_reconfig_needed(&rc, &wanted),
		     "an unconfigured gate always needs an attempt");
	zassert_true(wheel_reconfig_may_retry(&rc, 0),
		     "the first attempt must not be delayed");
}

ZTEST(wheel_reconfig, test_failure_blocks_everything_until_the_delay)
{
	struct wheel_reconfig rc;

	wheel_reconfig_init(&rc);
	wheel_reconfig_failed(&rc, 1000, RETRY_MS);

	zassert_false(wheel_reconfig_ready(&rc),
		      "a failed attempt must keep the gate closed");
	zassert_false(wheel_reconfig_may_retry(&rc, 2999),
		      "a retry before the delay would be a busy-loop on the bus");
	zassert_true(wheel_reconfig_may_retry(&rc, 3000),
		     "the retry must be allowed once the delay expired");
}

ZTEST(wheel_reconfig, test_a_failure_does_not_make_the_keys_look_applied)
{
	struct wheel_reconfig rc;

	wheel_reconfig_init(&rc);
	wheel_reconfig_failed(&rc, 1000, RETRY_MS);

	zassert_true(wheel_reconfig_needed(&rc, &wanted),
		     "a failure must not be mistaken for an applied config");
}

ZTEST(wheel_reconfig, test_success_opens_the_gate)
{
	struct wheel_reconfig rc;

	wheel_reconfig_init(&rc);
	wheel_reconfig_applied(&rc, &wanted);

	zassert_true(wheel_reconfig_ready(&rc), "an applied config opens the gate");
	zassert_false(wheel_reconfig_needed(&rc, &wanted),
		      "an applied config does not need re-applying");
	zassert_true(wheel_reconfig_may_retry(&rc, 0),
		     "with nothing pending there is nothing to delay");
}

/*
 * Defect 1.6: circumference and rpm_max were not part of the reconfiguration
 * check, so changing either left the detector computing with the old values.
 * Every field the detector copies must be watched.
 */
ZTEST(wheel_reconfig, test_every_detector_field_is_watched)
{
	static const struct wheel_reconfig_keys variants[] = {
		{ .odr_hz = 200, .range_g = 16, .circ_mm = 2100, .rpm_max = 1200 },
		{ .odr_hz = 100, .range_g = 2,  .circ_mm = 2100, .rpm_max = 1200 },
		{ .odr_hz = 100, .range_g = 16, .circ_mm = 2000, .rpm_max = 1200 },
		{ .odr_hz = 100, .range_g = 16, .circ_mm = 2100, .rpm_max = 900 },
	};
	static const char *const names[] = {
		"odr_hz", "range_g", "circ_mm", "rpm_max",
	};
	struct wheel_reconfig rc;

	wheel_reconfig_init(&rc);
	wheel_reconfig_applied(&rc, &wanted);

	for (size_t i = 0; i < ARRAY_SIZE(variants); i++) {
		zassert_true(wheel_reconfig_needed(&rc, &variants[i]),
			     "a change of %s must trigger reconfiguration",
			     names[i]);
	}
}

ZTEST(wheel_reconfig, test_a_retry_after_failure_can_succeed)
{
	struct wheel_reconfig rc;

	wheel_reconfig_init(&rc);
	wheel_reconfig_failed(&rc, 1000, RETRY_MS);

	zassert_true(wheel_reconfig_may_retry(&rc, 3000), "delay expired");

	wheel_reconfig_applied(&rc, &wanted);

	zassert_true(wheel_reconfig_ready(&rc), "the retry succeeded");
	zassert_false(wheel_reconfig_needed(&rc, &wanted), "and is settled");
}

ZTEST_SUITE(wheel_reconfig, NULL, NULL, NULL, NULL, NULL);
