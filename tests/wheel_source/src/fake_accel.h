/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPEED_METER_TEST_FAKE_ACCEL_H_
#define SPEED_METER_TEST_FAKE_ACCEL_H_

#include <stdbool.h>

/* Make attr_set() reject one half of a configuration, so the test can build
 * the partial success the fail-closed gate exists for. */
void fake_accel_fail_frequency(bool fail);
void fake_accel_fail_full_scale(bool fail);

/* What the driver will return from the next sample_fetch(). */
void fake_accel_set_sample(float x, float y, float z);
void fake_accel_report_no_data(bool no_data);
void fake_accel_fail_fetch(int err);

/* What the wheel source asked for. */
unsigned int fake_accel_attr_set_calls(void);
unsigned int fake_accel_frequency_sets(void);
unsigned int fake_accel_full_scale_sets(void);
void fake_accel_reset_counters(void);

#endif /* SPEED_METER_TEST_FAKE_ACCEL_H_ */
