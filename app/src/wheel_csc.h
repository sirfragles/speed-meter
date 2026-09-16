/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CSCS wheel measurement state: the event-time clock and the cumulative
 * revolution counter.
 *
 * This module exists so the two things the Bluetooth Cycling Speed and Cadence
 * service actually transmits can be tested without a sensor, without BLE and
 * without the kernel clock. Everything it needs from the outside is a monotonic
 * timestamp in milliseconds and the detector's revolution count.
 *
 * The two counters behave differently and must not be confused:
 *
 *   revolutions  cumulative, counts every revolution since the device powered
 *                up. The host derives distance and speed from its deltas, so it
 *                must never move backwards - but it is free to wrap at 2^32,
 *                and the host is required to handle that.
 *
 *   event_time   1/1024 s units, 16 bit, derived from the sensor's own notion
 *                of time. It wraps every 64 s *by definition*; that is not an
 *                error, it is the encoding, and the host handles it.
 */

#ifndef SPEED_METER_WHEEL_CSC_H_
#define SPEED_METER_WHEEL_CSC_H_

#include <stdbool.h>
#include <stdint.h>

/** What goes on the wire for one revolution. */
struct wheel_csc_sample {
	uint32_t revolutions;
	uint16_t event_time;
};

struct wheel_csc {
	uint32_t revolutions;
	uint16_t event_time;
	uint32_t last_detector; /* the detector's counter as last seen */
	int64_t last_event_ms;
	bool primed;
};

/** Reset to "no revolution seen yet". */
void wheel_csc_init(struct wheel_csc *csc);

/**
 * @brief Record one detected revolution.
 *
 * @param csc                    state
 * @param detector_revolutions   the detector's own counter at this revolution
 * @param now_ms                 monotonic milliseconds
 */
struct wheel_csc_sample wheel_csc_revolution(struct wheel_csc *csc,
					     uint32_t detector_revolutions,
					     int64_t now_ms);

/**
 * @brief Re-base the counter, as SET CUMULATIVE VALUE asks.
 *
 * Afterwards the next revolution reports @p revolutions + 1. This is the
 * wire contract in the form this module owns it.
 *
 * It is NOT the running code path, and that is deliberate. The SC Control
 * Point is served by csc.c, which re-bases at the publish point as
 * `reported = raw + offset`. That layer has to be source agnostic: the GPIO
 * source publishes through the same function but has no wheel_csc state to
 * re-base. Applying the offset here as well as there would count it twice.
 *
 * So the test for this function pins what the request must mean; it does not
 * exercise the offset that actually runs.
 */
void wheel_csc_set_cumulative(struct wheel_csc *csc, uint32_t revolutions);

#endif /* SPEED_METER_WHEEL_CSC_H_ */
