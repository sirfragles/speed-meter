/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The contract the CSCS host relies on, tested without a sensor or BLE.
 *
 * These tests are written against the *required* behaviour, not the current
 * one: while wheel_csc.c still carries the two defects inherited from
 * wheel_source_accel.c, they fail. That is the evidence that the defects are
 * real and that the fix changes something.
 */

#include <zephyr/ztest.h>

#include "wheel_csc.h"

/* 1/1024 s units */
#define SEC_UNITS 1024U
#define WRAP_UNITS 65536U

ZTEST(wheel_csc, test_event_time_is_zero_until_a_second_revolution)
{
	struct wheel_csc csc;
	struct wheel_csc_sample s;

	wheel_csc_init(&csc);

	s = wheel_csc_revolution(&csc, 1U, 1000);
	zassert_equal(s.event_time, 0U,
		      "the first revolution has no preceding time to measure from");
}

ZTEST(wheel_csc, test_event_time_wraps_instead_of_stalling)
{
	struct wheel_csc csc;
	struct wheel_csc_sample s;

	wheel_csc_init(&csc);
	(void)wheel_csc_revolution(&csc, 1U, 0);

	/* The wheel stands still for 90 s, then turns again. The 16-bit field
	 * wraps every 64 s by definition, so the expected value is the delta
	 * modulo 65536 - not a clamped 60000, which would tell the host that
	 * almost no time passed and produce a speed spike. */
	s = wheel_csc_revolution(&csc, 2U, 90000);

	zassert_equal(s.event_time, (uint16_t)(90U * SEC_UNITS % WRAP_UNITS),
		      "90 s must encode as %u units, got %u",
		      (unsigned int)(90U * SEC_UNITS % WRAP_UNITS),
		      s.event_time);
}

ZTEST(wheel_csc, test_event_time_advances_by_real_deltas)
{
	struct wheel_csc csc;
	struct wheel_csc_sample s;
	uint16_t previous = 0;

	wheel_csc_init(&csc);

	/* Revolutions 10 ms apart (100 Hz). The field only moves in whole
	 * 1/1024 s units, so each step contributes floor(10 * 1024 / 1000) = 10
	 * - the truncation is per event, not cumulative. */
	for (unsigned int i = 0; i < 10U; i++) {
		s = wheel_csc_revolution(&csc, i + 1U, (int64_t)i * 10);

		if (i > 0U) {
			zassert_true(s.event_time > previous,
				     "event time did not advance at step %u", i);
		}
		previous = s.event_time;
	}

	zassert_equal(s.event_time, 9U * 10U,
		      "nine 10 ms steps should add 90 units, got %u",
		      s.event_time);
}

ZTEST(wheel_csc, test_counter_survives_a_detector_restart)
{
	struct wheel_csc csc;
	struct wheel_csc_sample s;

	wheel_csc_init(&csc);

	for (uint32_t rev = 1U; rev <= 5U; rev++) {
		(void)wheel_csc_revolution(&csc, rev, (int64_t)rev * 1000);
	}

	/* wheel_detector_init() zeroes the detector's counter. The revolutions
	 * already counted are still real, so the published counter must carry
	 * on from where it was, not jump back to the detector's new epoch. */
	s = wheel_csc_revolution(&csc, 1U, 6000);

	zassert_equal(s.revolutions, 6U,
		      "a detector restart must not move the counter backwards; got %u",
		      s.revolutions);
}

ZTEST(wheel_csc, test_counter_never_decreases)
{
	struct wheel_csc csc;
	struct wheel_csc_sample s;
	uint32_t previous = 0;
	const uint32_t detector_sequence[] = { 1U, 2U, 3U, 1U, 2U, 3U, 4U };

	wheel_csc_init(&csc);

	for (size_t i = 0; i < ARRAY_SIZE(detector_sequence); i++) {
		s = wheel_csc_revolution(&csc, detector_sequence[i],
					 (int64_t)i * 1000);

		zassert_true(s.revolutions >= previous,
			     "counter went backwards at step %zu: %u after %u",
			     i, s.revolutions, previous);
		previous = s.revolutions;
	}
}

/*
 * What SET CUMULATIVE VALUE has to mean, not the code that serves it. The SC
 * Control Point is handled in csc.c, which re-bases at the publish point
 * because it has to work for every source - the GPIO source has no wheel_csc
 * to re-base. This test therefore covers the contract, not that offset; the
 * gap is recorded in the plan rather than papered over by a passing test.
 */
ZTEST(wheel_csc, test_set_cumulative_value_rebases_the_counter)
{
	struct wheel_csc csc;
	struct wheel_csc_sample s;

	wheel_csc_init(&csc);
	(void)wheel_csc_revolution(&csc, 1U, 0);

	/* The host re-based the counter over the SC Control Point. */
	wheel_csc_set_cumulative(&csc, 1000U);

	s = wheel_csc_revolution(&csc, 2U, 1000);
	zassert_equal(s.revolutions, 1001U,
		      "the next revolution after SET CUMULATIVE VALUE 1000 must be 1001, got %u",
		      s.revolutions);
}

ZTEST_SUITE(wheel_csc, NULL, NULL, NULL, NULL, NULL);
