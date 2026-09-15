/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Self-calibrating wheel rotation detector.
 *
 * Nothing has to be configured about the mount: the board may sit in any
 * orientation and the axes, signs and the sensor-to-axis distance are found
 * from the data. Samples are raw XYZ accelerations in m/s^2.
 *
 *   IDLE        the wheel is still - noise floor and gravity reference.
 *   CALIBRATING the wheel turns; a PCA of the sample window yields the
 *               rotation plane (basis + normal + centre) and revolutions are
 *               counted from the unwrapped atan2 phase inside that plane.
 *   LOCKED      the plane is stable and revolutions keep coming; the
 *               revolution mean at different speeds is fitted to
 *               c(w) = b + r*w^2*e_r, which gives the radial direction and
 *               the sensor-to-axis distance r (refined slowly, never from a
 *               single bad cycle).
 *
 * A revolution whose samples reach the full scale still counts, but never
 * feeds the radius fit (clipped samples would corrupt it).
 */

#ifndef SPEED_METER_WHEEL_DETECTOR_H_
#define SPEED_METER_WHEEL_DETECTOR_H_

#include <stdbool.h>
#include <stdint.h>

#define WHEEL_PLANE_MAX 256 /* calibration window, samples */
#define WHEEL_SPEED_MAX 16  /* (speed, centre) pairs kept for the fit */

enum wheel_detector_state {
	WHEEL_STATE_IDLE = 0,
	WHEEL_STATE_CALIBRATING,
	WHEEL_STATE_LOCKED,
};

struct wheel_detector_config {
	float circumference_m;  /* wheel circumference, user provided */
	float full_scale_ms2;   /* sensor range, for saturation detection */
	float nominal_radius_m; /* fallback sensor-to-axis distance */
	uint32_t odr_hz;        /* nominal sample rate */
	float rpm_max;          /* plausibility limit */
	float still_gate_ms2;   /* movement gate while the wheel is still */
};

struct wheel_detector_plane {
	bool valid;
	float normal[3]; /* wheel axis in the sensor frame */
	float e1[3];     /* in-plane basis */
	float e2[3];
	float center[3]; /* centre of the traced circle */
	float quality;   /* 0..1, planarity of the fit */
};

struct wheel_detector_result {
	bool new_revolution;
	bool high_quality;   /* clean revolution, usable for the radius fit */
	enum wheel_detector_state state;
	uint32_t revolutions;
	int8_t direction;    /* +1 / -1 of the last revolution */
	float rpm;
	float speed_kmh;
	float radius_m;      /* 0 until the fit converged */
	float radial_dir[3]; /* outward radial direction, sensor frame */
	float noise_ms2;
	float plane_quality;
	float confidence;    /* 0..1, quality of the current estimate */
};

struct wheel_detector {
	struct wheel_detector_config cfg;

	bool prev_valid;
	double t_prev;
	float prev[3];

	/* standstill estimate (IDLE only) */
	uint32_t still_n;
	float still_min[3];
	float still_max[3];
	float still_mean[3];
	float noise_ms2;
	uint32_t move_n;
	double last_move_t;

	/* calibration window and the fitted plane */
	uint32_t win_len;
	float win[3][WHEEL_PLANE_MAX];
	float win_t[WHEEL_PLANE_MAX]; /* seconds since win_t0 */
	double win_t0;
	struct wheel_detector_plane plane;

	/* phase tracking */
	bool phase_valid;
	double phase_prev;
	double phase_acc;
	double rev_t0;
	uint32_t revolutions;
	int8_t direction;

	/* per-revolution accumulation (circle centre at the current speed) */
	double acc[3];
	uint32_t acc_n;
	bool acc_saturated;

	/* centre(w^2) samples for the radius fit */
	double fit_w2[WHEEL_SPEED_MAX];
	double fit_c[3][WHEEL_SPEED_MAX];
	uint32_t fit_n;
	uint32_t fit_head;

	float radius_m;
	float radial_dir[3];
	bool radial_valid;

	enum wheel_detector_state state;
	float rpm;
	float confidence;
	double period_hist[4]; /* recent periods (newest first), median filtered */
	uint32_t period_n;
};

/**
 * @brief Initialise the detector state.
 *
 * @return true on success, false on invalid configuration.
 */
bool wheel_detector_init(struct wheel_detector *det,
			 const struct wheel_detector_config *cfg);

/* Call after losing samples / FIFO overrun: keeps the plane, drops phase. */
void wheel_detector_samples_lost(struct wheel_detector *det);

/**
 * @brief Feed one raw XYZ sample (m/s^2) and get the result.
 */
struct wheel_detector_result wheel_detector_update(struct wheel_detector *det,
						   double time_s, float x, float y,
						   float z);

/* Fitted plane, or NULL when the detector has not locked one yet. */
const struct wheel_detector_plane *wheel_detector_plane_get(
	const struct wheel_detector *det);

#endif /* SPEED_METER_WHEEL_DETECTOR_H_ */
