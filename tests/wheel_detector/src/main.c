/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Replay tests for the wheel detector.
 *
 * wheel_detector.c makes no Zephyr calls, so the honest way to test it is to
 * compile the production file as it ships and feed it samples captured from a
 * real board. The fixtures in fixtures/ were taken over BLE with
 * app/tools/wheel_cal.py; README.md explains how to record more.
 *
 * Each capture is replayed with the production geometry from
 * app/src/wheel_config.c, so the revolution counts printed below are the ones
 * the firmware produces for that data - not a model of them.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "fixtures.h"
#include "wheel_detector.h"

/*
 * Production defaults from app/src/wheel_config.c: 28" wheel, 10 mm nominal
 * sensor-to-axis distance, 1200 rpm plausibility limit. odr_hz and range_g are
 * taken from each capture's own header instead: a capture recorded at +/-16 g
 * would look saturated to a detector configured for +/-2 g.
 */
#define TEST_CIRCUMFERENCE_M  2.100f
#define TEST_NOMINAL_RADIUS_M 0.010f
#define TEST_RPM_MAX          1200.0f
#define TEST_STILL_GATE_MS2   0.5f

/* The detector holds a 256-sample calibration window (~4.5 kB). */
static struct wheel_detector det;

/*
 * What the detector does with each capture today.
 *
 * These are characterisation values, not targets: they were observed by
 * replaying this code against recorded rides, so a change here is a behaviour
 * change that has to be explained rather than a number to quietly update.
 *
 * verify10 is the exception worth staring at. The capture is real wheel motion
 * - 1.8 g peak-to-peak, and 61% of its samples move faster than the detector's
 * 0.5 m/s^2 gate - yet the detector finds nothing at all and ends in IDLE. The
 * other three lock within 400 samples. Recording it as "never locks" keeps the
 * defect visible instead of hiding it in a passing suite. See README.md.
 */
static const struct expectation {
	const char *name;
	uint32_t revolutions;
	bool locks;
} expectations[] = {
	{ "autocal_capture", 16, true },
	{ "ride1", 19, true },
	{ "verify10", 0, false },
	{ "verify_manual", 22, true },
};

static const struct expectation *find_expectation(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(expectations); i++) {
		if (strcmp(expectations[i].name, name) == 0) {
			return &expectations[i];
		}
	}

	return NULL;
}

struct replay {
	uint32_t revolutions;
	uint32_t locked_at;
	bool locked;
	float rpm;
	float speed_kmh;
	float radius_m;
	enum wheel_detector_state state;
};

static struct replay replay_capture(const struct fixture *fx)
{
	struct wheel_detector_config cfg = {
		.circumference_m = TEST_CIRCUMFERENCE_M,
		.full_scale_ms2 = 9.80665f * (float)fx->range_g,
		.nominal_radius_m = TEST_NOMINAL_RADIUS_M,
		.odr_hz = fx->odr_hz,
		.rpm_max = TEST_RPM_MAX,
		.still_gate_ms2 = TEST_STILL_GATE_MS2,
	};
	struct wheel_detector_result res = { 0 };
	struct replay out = { 0 };
	uint32_t previous = 0;

	zassert_true(wheel_detector_init(&det, &cfg),
		     "%s: wheel_detector_init rejected the production config",
		     fx->name);

	for (size_t i = 0; i < fx->count; i++) {
		const struct fixture_sample *s = &fx->samples[i];

		res = wheel_detector_update(&det, (double)s->t_s, s->x_ms2,
					    s->y_ms2, s->z_ms2);

		zassert_true(res.revolutions >= previous,
			     "%s: revolution count went backwards at sample %zu",
			     fx->name, i);
		previous = res.revolutions;

		if (!out.locked && res.state == WHEEL_STATE_LOCKED) {
			out.locked = true;
			out.locked_at = (uint32_t)i;
		}

		zassert_true(isfinite((double)res.rpm) &&
			     isfinite((double)res.speed_kmh),
			     "%s: non-finite speed at sample %zu", fx->name, i);
		zassert_true(res.rpm >= 0.0f && res.rpm <= TEST_RPM_MAX,
			     "%s: rpm %.1f outside 0..%d at sample %zu",
			     fx->name, (double)res.rpm, (int)TEST_RPM_MAX, i);
	}

	out.revolutions = res.revolutions;
	out.rpm = res.rpm;
	out.speed_kmh = res.speed_kmh;
	out.radius_m = res.radius_m;
	out.state = res.state;
	return out;
}

/* A wheel that never moves must never produce a revolution. */
ZTEST(wheel_detector, test_still_data_reports_nothing)
{
	struct wheel_detector_config cfg = {
		.circumference_m = TEST_CIRCUMFERENCE_M,
		.full_scale_ms2 = 9.80665f * 16.0f,
		.nominal_radius_m = TEST_NOMINAL_RADIUS_M,
		.odr_hz = 100,
		.rpm_max = TEST_RPM_MAX,
		.still_gate_ms2 = TEST_STILL_GATE_MS2,
	};
	/* Deterministic jitter: +/-0.02 m/s^2, far below the 0.5 m/s^2 gate. */
	uint32_t rng = 0x12345678U;
	struct wheel_detector_result res = { 0 };

	zassert_true(wheel_detector_init(&det, &cfg), "init rejected the config");

	for (unsigned int i = 0; i < 1000U; i++) {
		float jitter[3];

		for (int k = 0; k < 3; k++) {
			rng = rng * 1103515245U + 12345U;
			jitter[k] = ((float)((rng >> 16) & 0xFFU) / 255.0f - 0.5f) * 0.04f;
		}

		res = wheel_detector_update(&det, (double)i / 100.0,
					    jitter[0], jitter[1],
					    9.80665f + jitter[2]);
	}

	zassert_equal(res.revolutions, 0U,
		      "still wheel produced %u revolutions", res.revolutions);
	zassert_equal(res.state, WHEEL_STATE_IDLE,
		      "still wheel left the detector in state %d", res.state);
}

ZTEST(wheel_detector, test_replay_recorded_captures)
{
	for (size_t i = 0; i < FIXTURE_COUNT; i++) {
		const struct fixture *fx = &fixtures[i];
		const struct expectation *want = find_expectation(fx->name);
		struct replay r;

		zassert_not_null(want,
				 "%s: no recorded expectation - add a row to the "
				 "expectations table", fx->name);

		r = replay_capture(fx);

		printk("%-16s %5zu samples @%3u Hz  revs=%-4u locked=%s%-5u "
		       "rpm=%8.2f  speed=%7.2f km/h  radius=%.4f m  state=%d\n",
		       fx->name, fx->count, fx->odr_hz, r.revolutions,
		       r.locked ? "@" : "never", r.locked_at,
		       (double)r.rpm, (double)r.speed_kmh,
		       (double)r.radius_m, r.state);

		zassert_equal(r.locked, want->locks,
			      "%s: lock state changed (recorded %s)",
			      fx->name, want->locks ? "locked" : "never locked");
		zassert_equal(r.revolutions, want->revolutions,
			      "%s: counted %u revolutions, recorded %u",
			      fx->name, r.revolutions, want->revolutions);
	}
}

ZTEST_SUITE(wheel_detector, NULL, NULL, NULL, NULL, NULL);
