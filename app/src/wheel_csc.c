/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CSCS wheel measurement state - see wheel_csc.h.
 *
 * The contract is the one the host is entitled to assume, and the tests in
 * tests/wheel_csc/src/main.c hold the module to it:
 *
 *   - the event time is the sensor's own clock. It is 16 bit in 1/1024 s units
 *     and wraps every 64 s *by definition*; the host handles the wrap, so a
 *     long standstill simply advances the clock by the real delta.
 *   - the cumulative counter only ever grows. It advances by the *difference*
 *     of the detector's counter, so re-initialising the detector (a wheel
 *     odr/range change, a lost-continuity reset) cannot move it backwards.
 *
 * Both were defects in the inline code this module replaces; the commit that
 * introduced the module records the four failing tests that demonstrated them.
 */

#include "wheel_csc.h"

#define CSCS_UNITS_PER_SEC 1024U

void wheel_csc_init(struct wheel_csc *csc)
{
	*csc = (struct wheel_csc){ 0 };
}

static void advance_event_time(struct wheel_csc *csc, int64_t now_ms)
{
	int64_t delta_ms = now_ms - csc->last_event_ms;

	if (csc->primed && delta_ms > 0) {
		uint64_t units =
			(uint64_t)delta_ms * CSCS_UNITS_PER_SEC / 1000U;

		/* No clamping. The field wraps every 64 s by definition and the host
		 * is required to cope; clamping made a long standstill look like
		 * almost no time had passed, which the host reads as a speed spike. */
		csc->event_time = (uint16_t)(csc->event_time + units);
	}

	csc->last_event_ms = now_ms;
	csc->primed = true;
}

struct wheel_csc_sample wheel_csc_revolution(struct wheel_csc *csc,
					     uint32_t detector_revolutions,
					     int64_t now_ms)
{
	/* Advance by the *difference*, never by the detector's absolute value:
	 * the detector zeroes its counter on wheel_detector_init(), and the host
	 * would see that as the distance counter jumping backwards. */
	int64_t delta = (int64_t)detector_revolutions -
			(int64_t)csc->last_detector;

	advance_event_time(csc, now_ms);

	if (delta < 0) {
		/* The detector restarted. Everything it counted before the restart
		 * is already in csc->revolutions, and this call carries the first
		 * revolution of the new run. */
		delta = 1;
	}

	csc->revolutions += (uint32_t)delta;
	csc->last_detector = detector_revolutions;

	struct wheel_csc_sample out = {
		.revolutions = csc->revolutions,
		.event_time = csc->event_time,
	};

	return out;
}

void wheel_csc_set_cumulative(struct wheel_csc *csc, uint32_t revolutions)
{
	csc->revolutions = revolutions;
}
