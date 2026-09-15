/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wheel sensor configuration - the single source of truth shared by the
 * revolution detector, the diagnostics shell (tools/wheel_cal.py) and the
 * `wheel cal set` command.
 *
 * Values are persisted in NVS (settings subtree "wheel") so a configured
 * device keeps its wheel circumference and detector tuning across reboots and
 * battery changes. See CONFIG_SETTINGS / CONFIG_SETTINGS_NVS in prj.conf.
 */

#ifndef SPEED_METER_WHEEL_CONFIG_H_
#define SPEED_METER_WHEEL_CONFIG_H_

#include <stdbool.h>
#include <stdint.h>

struct wheel_config {
	/* Accelerometer axes forming the wheel rotation plane (0=X, 1=Y, 2=Z).
	 * axis_a >= 3 means "detect the plane automatically". */
	uint8_t axis_a;
	uint8_t axis_b;
	bool invert_a;
	bool invert_b;

	/* Detector limits. */
	uint16_t rpm_min;     /* below this rate revolutions are ignored */
	uint16_t rpm_max;     /* above this rate revolutions are ignored */
	uint16_t amp_mg;      /* min rotating-vector amplitude [milli-g] */
	uint16_t step_mrad;   /* max plausible phase step [milli-rad] */
	uint16_t alpha_milli; /* gravity-centre filter alpha x1000 */
	uint16_t stop_ms;     /* no revolution for so long -> standstill */

	/* Wheel geometry: used to turn revolutions into a speed. */
	uint16_t circ_mm;

	/* LIS2DH configuration. */
	uint16_t odr_hz; /* output data rate for sampling */
	uint8_t range_g; /* range: 2, 4, 8 or 16 g */
};

/** @brief Current configuration (read-only for other modules). */
const struct wheel_config *wheel_config_get(void);

/**
 * @brief Update one setting, validating the value.
 *
 * Accepts the same keys as `wheel cal set`: axes, invert_a, invert_b, rpm_min,
 * rpm_max, amp_mg, step_mrad, alpha_milli, stop_ms, circ_mm, odr_hz, range_g.
 * A change is written back to NVS after a short debounce, so a burst of
 * `cal set` commands costs a single flash write.
 *
 * @return 0 on success, -EINVAL on an unknown key or an out-of-range value.
 */
int wheel_config_set(const char *key, const char *value);

/** @brief Drop the stored configuration and go back to the built-in defaults. */
int wheel_config_reset(void);

/**
 * @brief Initialise the module (settings backend + save worker).
 *
 * Must run before settings_load() so the stored values are applied.
 */
int wheel_config_init(void);

#endif /* SPEED_METER_WHEEL_CONFIG_H_ */
