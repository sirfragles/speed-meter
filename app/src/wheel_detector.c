/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Self-calibrating wheel rotation detector - see wheel_detector.h for the
 * state machine and the calibration model.
 */

#include "wheel_detector.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

#define DET_PI 3.14159265358979323846
#define DET_TWO_PI (2.0 * DET_PI)

#define DET_STILL_WINDOW 50     /* samples between noise re-estimates */
#define DET_MOVE_SAMPLES 8      /* consecutive moves that start calibration */
#define DET_STILL_TIMEOUT_S 1.5 /* quiet time that returns the detector to IDLE */
#define DET_MIN_PLANE_SAMPLES 32
#define DET_MIN_PLANE_QUALITY 0.55f
#define DET_MIN_FIT_SPAN 2.0    /* rad^2/s^2: spread needed by the radius fit */
#define DET_RPM_MIN 4.0f
#define DET_LOCK_REVS 3         /* clean revolutions that mark the lock */
#define DET_MAX_GAP_FACTOR 3.0  /* gaps longer than this break the phase */
#define DET_CLIP_FRACTION 0.9f  /* samples this close to full scale are clipped */

static void detector_filter_period(struct wheel_detector *det, double period);
static void detector_confidence_update(struct wheel_detector *det);

static double dot3(const float *a, const float *b)
{
	return (double)a[0] * (double)b[0] + (double)a[1] * (double)b[1] +
	       (double)a[2] * (double)b[2];
}

static void clear_revolution(struct wheel_detector *det)
{
	det->acc[0] = 0.0;
	det->acc[1] = 0.0;
	det->acc[2] = 0.0;
	det->acc_n = 0;
	det->acc_saturated = false;
}

static void reset_phase(struct wheel_detector *det)
{
	det->phase_valid = false;
	det->phase_acc = 0.0;
	clear_revolution(det);
}

static void reset_still_window(struct wheel_detector *det)
{
	for (int k = 0; k < 3; k++) {
		det->still_min[k] = FLT_MAX;
		det->still_max[k] = -FLT_MAX;
	}
	det->still_n = 0;
}

static void update_still(struct wheel_detector *det, const float *v)
{
	uint32_t n = det->still_n + 1;

	for (int k = 0; k < 3; k++) {
		if (v[k] < det->still_min[k]) {
			det->still_min[k] = v[k];
		}
		if (v[k] > det->still_max[k]) {
			det->still_max[k] = v[k];
		}
		det->still_mean[k] += (v[k] - det->still_mean[k]) / (float)n;
	}
	det->still_n = n;

	if (n >= DET_STILL_WINDOW) {
		float peak = 0.0f;

		for (int k = 0; k < 3; k++) {
			peak = fmaxf(peak, det->still_max[k] - det->still_min[k]);
		}
		det->noise_ms2 = peak;
		reset_still_window(det);
	}
}

static float offset_from_gravity(const struct wheel_detector *det, const float *v)
{
	float dx = v[0] - det->still_mean[0];
	float dy = v[1] - det->still_mean[1];
	float dz = v[2] - det->still_mean[2];

	return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void window_add(struct wheel_detector *det, double time_s, const float *v)
{
	if (det->win_len == 0) {
		det->win_t0 = time_s;
	}
	if (det->win_len >= WHEEL_PLANE_MAX) {
		return;
	}

	det->win_t[det->win_len] = (float)(time_s - det->win_t0);
	for (int k = 0; k < 3; k++) {
		det->win[k][det->win_len] = v[k];
	}
	det->win_len++;
}

/* Jacobi eigen decomposition of a symmetric 3x3 matrix; eigenvectors end up
 * in the columns of v, eigenvalues on the diagonal of a. */
static void jacobi3(double a[3][3], double v[3][3])
{
	for (int sweep = 0; sweep < 16; sweep++) {
		double off = fabs(a[0][1]) + fabs(a[0][2]) + fabs(a[1][2]);

		if (off < 1e-12) {
			break;
		}

		for (int p = 0; p < 2; p++) {
			for (int q = p + 1; q < 3; q++) {
				if (fabs(a[p][q]) < 1e-15) {
					continue;
				}

				double theta = 0.5 * (a[q][q] - a[p][p]) / a[p][q];
				double t = (theta >= 0.0 ? 1.0 : -1.0) /
					   (fabs(theta) + sqrt(theta * theta + 1.0));
				double c = 1.0 / sqrt(t * t + 1.0);
				double s = t * c;

				for (int k = 0; k < 3; k++) {
					double akp = a[k][p], akq = a[k][q];

					a[k][p] = c * akp - s * akq;
					a[k][q] = s * akp + c * akq;
				}
				for (int k = 0; k < 3; k++) {
					double apk = a[p][k], aqk = a[q][k];

					a[p][k] = c * apk - s * aqk;
					a[q][k] = s * apk + c * aqk;
				}
				for (int k = 0; k < 3; k++) {
					double vkp = v[k][p], vkq = v[k][q];

					v[k][p] = c * vkp - s * vkq;
					v[k][q] = s * vkp + c * vkq;
				}
			}
		}
	}
}

static bool plane_solve(struct wheel_detector *det)
{
	if (det->win_len < DET_MIN_PLANE_SAMPLES) {
		return false;
	}

	double mean[3] = { 0.0, 0.0, 0.0 };
	double cov[3][3] = { { 0.0 } };
	double eigvec[3][3] = { { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 } };

	for (uint32_t i = 0; i < det->win_len; i++) {
		for (int k = 0; k < 3; k++) {
			mean[k] += (double)det->win[k][i];
		}
	}

	for (int k = 0; k < 3; k++) {
		mean[k] /= (double)det->win_len;
	}

	for (uint32_t i = 0; i < det->win_len; i++) {
		double d[3];

		for (int k = 0; k < 3; k++) {
			d[k] = (double)det->win[k][i] - mean[k];
		}
		for (int p = 0; p < 3; p++) {
			for (int q = 0; q < 3; q++) {
				cov[p][q] += d[p] * d[q];
			}
		}
	}

	for (int p = 0; p < 3; p++) {
		for (int q = 0; q < 3; q++) {
			cov[p][q] /= (double)det->win_len;
		}
	}

	jacobi3(cov, eigvec);

	double lam[3] = { cov[0][0], cov[1][1], cov[2][2] };
	int imin = 0;
	int imax = 0;

	for (int k = 1; k < 3; k++) {
		if (lam[k] < lam[imin]) {
			imin = k;
		}
		if (lam[k] > lam[imax]) {
			imax = k;
		}
	}

	if (lam[imax] <= 0.0) {
		return false;
	}

	float quality = (float)(1.0 - lam[imin] / lam[imax]);

	if (!isfinite(quality) || quality < DET_MIN_PLANE_QUALITY) {
		return false;
	}

	float normal[3];
	float e1[3];
	float e2[3];
	float norm = 0.0f;

	for (int k = 0; k < 3; k++) {
		normal[k] = (float)eigvec[k][imin];
		e1[k] = (float)eigvec[k][imax];
		norm += e1[k] * e1[k];
	}

	norm = sqrtf(norm);
	if (norm < 1e-6f) {
		return false;
	}
	for (int k = 0; k < 3; k++) {
		e1[k] /= norm;
	}

	norm = 0.0f;
	for (int k = 0; k < 3; k++) {
		norm += normal[k] * normal[k];
	}
	norm = sqrtf(norm);
	if (norm < 1e-6f) {
		return false;
	}
	for (int k = 0; k < 3; k++) {
		normal[k] /= norm;
	}

	e2[0] = normal[1] * e1[2] - normal[2] * e1[1];
	e2[1] = normal[2] * e1[0] - normal[0] * e1[2];
	e2[2] = normal[0] * e1[1] - normal[1] * e1[0];

	for (int k = 0; k < 3; k++) {
		det->plane.center[k] = (float)mean[k];
		det->plane.normal[k] = normal[k];
		det->plane.e1[k] = e1[k];
		det->plane.e2[k] = e2[k];
	}
	det->plane.quality = quality;
	det->plane.valid = true;

	return true;
}

static void fit_add(struct wheel_detector *det, double w2, const double *c)
{
	uint32_t slot = det->fit_head;

	det->fit_w2[slot] = w2;
	for (int k = 0; k < 3; k++) {
		det->fit_c[k][slot] = c[k];
	}
	det->fit_head = (slot + 1U) % WHEEL_SPEED_MAX;
	if (det->fit_n < WHEEL_SPEED_MAX) {
		det->fit_n++;
	}
}

/* c(w) = b + r*w^2*e_r: the slope of the revolution mean against w^2 is the
 * radial direction scaled by the sensor-to-axis distance. */
static void fit_solve(struct wheel_detector *det)
{
	if (det->fit_n < 3) {
		return;
	}

	double sw = 0.0;
	double sc[3] = { 0.0, 0.0, 0.0 };
	double wmin = DBL_MAX;
	double wmax = -DBL_MAX;

	for (uint32_t i = 0; i < det->fit_n; i++) {
		double w2 = det->fit_w2[i];

		sw += w2;
		if (w2 < wmin) {
			wmin = w2;
		}
		if (w2 > wmax) {
			wmax = w2;
		}
		for (int k = 0; k < 3; k++) {
			sc[k] += det->fit_c[k][i];
		}
	}

	if ((wmax - wmin) < DET_MIN_FIT_SPAN) {
		return;
	}

	double mw = sw / (double)det->fit_n;
	double mc[3];
	double den = 0.0;
	double num[3] = { 0.0, 0.0, 0.0 };

	for (int k = 0; k < 3; k++) {
		mc[k] = sc[k] / (double)det->fit_n;
	}

	for (uint32_t i = 0; i < det->fit_n; i++) {
		double dw = det->fit_w2[i] - mw;

		den += dw * dw;
		for (int k = 0; k < 3; k++) {
			num[k] += dw * (det->fit_c[k][i] - mc[k]);
		}
	}

	if (den < 1e-9) {
		return;
	}

	double slope[3];
	double r2 = 0.0;

	for (int k = 0; k < 3; k++) {
		slope[k] = num[k] / den;
		r2 += slope[k] * slope[k];
	}

	double r = sqrt(r2);

	if (r < 1e-4 || r > 0.5) {
		return; /* implausible for a hub-mounted sensor */
	}

	if (det->radial_valid) {
		double ratio = r / (double)det->radius_m;

		if (ratio < 0.5 || ratio > 2.0) {
			return; /* never jump on one bad fit */
		}
		det->radius_m += 0.05f * (float)(r - (double)det->radius_m);
	} else {
		det->radius_m = (float)r;
	}

	for (int k = 0; k < 3; k++) {
		det->radial_dir[k] = (float)(slope[k] / r);
	}
	det->radial_valid = true;
}

static void phase_process(struct wheel_detector *det, double time_s,
			  const float *v, struct wheel_detector_result *out)
{
	float d[3];
	double u;
	double w;
	double phase;

	for (int k = 0; k < 3; k++) {
		d[k] = v[k] - det->plane.center[k];
	}

	u = dot3(d, det->plane.e1);
	w = dot3(d, det->plane.e2);
	phase = atan2(w, u);

	for (int k = 0; k < 3; k++) {
		det->acc[k] += (double)v[k];
	}
	det->acc_n++;

	if (det->cfg.full_scale_ms2 > 0.0f) {
		float clip = DET_CLIP_FRACTION * det->cfg.full_scale_ms2;

		if (fabsf(v[0]) >= clip || fabsf(v[1]) >= clip ||
		    fabsf(v[2]) >= clip) {
			det->acc_saturated = true;
		}
	}

	if (!det->phase_valid) {
		det->phase_valid = true;
		det->phase_prev = phase;
		det->rev_t0 = time_s;
		return;
	}

	double dphi = phase - det->phase_prev;

	det->phase_prev = phase;
	if (dphi > DET_PI) {
		dphi -= DET_TWO_PI;
	}
	if (dphi < -DET_PI) {
		dphi += DET_TWO_PI;
	}

	double rate = det->cfg.odr_hz ? (double)det->cfg.odr_hz : 100.0;
	double max_step = DET_TWO_PI * ((double)det->cfg.rpm_max / 60.0) / rate * 2.0;

	if (max_step > DET_PI) {
		max_step = DET_PI;
	}

	/* A jump means a glitch or a lost sample: restart the phase, keep the
	 * plane and the revolution count. */
	if (fabs(dphi) > max_step) {
		det->phase_valid = false;
		clear_revolution(det);
		return;
	}

	det->phase_acc += dphi;

	if (fabs(det->phase_acc) < DET_TWO_PI) {
		return;
	}

	int8_t dir = det->phase_acc > 0.0 ? 1 : -1;

	det->phase_acc -= (double)dir * DET_TWO_PI;
	det->direction = dir;
	det->revolutions++;
	out->new_revolution = true;
	out->revolutions = det->revolutions;
	out->direction = dir;

	double period = time_s - det->rev_t0;

	det->rev_t0 = time_s;

	if (period > 0.0) {
		double rpm = 60.0 / period;

		if (rpm >= (double)DET_RPM_MIN && rpm <= (double)det->cfg.rpm_max) {
			detector_filter_period(det, period);
			out->rpm = det->rpm;
			out->speed_kmh = det->cfg.circumference_m * det->rpm / 60.0f * 3.6f;

			if (det->acc_n > 0 && !det->acc_saturated) {
				double c[3];
				double omega = DET_TWO_PI / period;

				for (int k = 0; k < 3; k++) {
					c[k] = det->acc[k] / (double)det->acc_n;
				}
				fit_add(det, omega * omega, c);
				fit_solve(det);
				out->high_quality = true;
			}
		}
	}

	clear_revolution(det);
}

static void detector_confidence_update(struct wheel_detector *det)
{
	uint32_t n = det->period_n;
	float conf = det->plane.quality;

	if (n >= 2U) {
		double sum = 0.0;

		for (uint32_t i = 0; i < n; i++) {
			sum += det->period_hist[i];
		}
		double mean = sum / (double)n;
		double var = 0.0;

		for (uint32_t i = 0; i < n; i++) {
			double d = det->period_hist[i] - mean;

			var += d * d;
		}
		var /= (double)n;
		double cv = mean > 0.0 ? sqrt(var) / mean : 1.0;
		float spread = (float)fmax(0.0, 1.0 - 3.0 * cv);

		conf *= spread;
	} else {
		conf *= 0.5f;
	}

	det->confidence = fmaxf(0.0f, fminf(1.0f, conf));
}

/* Median of the last up-to-4 revolution periods filters a single bad cycle. */
static void detector_filter_period(struct wheel_detector *det, double period)
{
	double p[4];

	for (int i = 3; i > 0; i--) {
		det->period_hist[i] = det->period_hist[i - 1];
	}
	det->period_hist[0] = period;
	if (det->period_n < 4U) {
		det->period_n++;
	}

	for (uint32_t i = 0; i < det->period_n; i++) {
		p[i] = det->period_hist[i];
	}
	for (uint32_t i = 0; i < det->period_n; i++) {
		for (uint32_t j = i + 1U; j < det->period_n; j++) {
			if (p[j] < p[i]) {
				double t = p[i];

				p[i] = p[j];
				p[j] = t;
			}
		}
	}

	det->rpm = (float)(60.0 / p[det->period_n / 2U]);
	detector_confidence_update(det);
}

bool wheel_detector_init(struct wheel_detector *det,
			 const struct wheel_detector_config *cfg)
{
	if (det == NULL || cfg == NULL ||
	    !isfinite(cfg->circumference_m) || cfg->circumference_m <= 0.0f ||
	    !isfinite(cfg->full_scale_ms2) || cfg->full_scale_ms2 <= 0.0f ||
	    !isfinite(cfg->nominal_radius_m) || cfg->nominal_radius_m <= 0.0f ||
	    cfg->odr_hz == 0U ||
	    !isfinite(cfg->rpm_max) || cfg->rpm_max <= 0.0f ||
	    !isfinite(cfg->still_gate_ms2) || cfg->still_gate_ms2 <= 0.0f) {
		return false;
	}

	*det = (struct wheel_detector){ .cfg = *cfg };
	reset_still_window(det);
	det->state = WHEEL_STATE_IDLE;

	return true;
}

void wheel_detector_samples_lost(struct wheel_detector *det)
{
	reset_phase(det);
}

const struct wheel_detector_plane *wheel_detector_plane_get(
	const struct wheel_detector *det)
{
	if (det == NULL || !det->plane.valid) {
		return NULL;
	}

	return &det->plane;
}

struct wheel_detector_result wheel_detector_update(struct wheel_detector *det,
						   double time_s, float x, float y,
						   float z)
{
	struct wheel_detector_result out = {
		.state = det->state,
		.revolutions = det->revolutions,
		.direction = det->direction,
		.rpm = det->rpm,
		.noise_ms2 = det->noise_ms2,
		.plane_quality = det->plane.quality,
		.radius_m = det->radial_valid ? det->radius_m : 0.0f,
		.confidence = det->confidence,
	};
	float v[3] = { x, y, z };

	if (!isfinite(time_s) || !isfinite(x) || !isfinite(y) || !isfinite(z)) {
		wheel_detector_samples_lost(det);
		return out;
	}

	double nominal = det->cfg.odr_hz ? 1.0 / (double)det->cfg.odr_hz : 0.01;
	double dt = time_s - det->t_prev;

	if (det->prev_valid && (dt <= 0.0 || dt > DET_MAX_GAP_FACTOR * nominal)) {
		reset_phase(det);
	}

	/* The very first sample only seeds the gravity reference. */
	if (!det->prev_valid) {
		for (int k = 0; k < 3; k++) {
			det->still_mean[k] = v[k];
		}
		reset_still_window(det);
		det->prev_valid = true;
		det->t_prev = time_s;
		return out;
	}

	float gate = fmaxf(4.0f * det->noise_ms2, det->cfg.still_gate_ms2);
	bool moving = offset_from_gravity(det, v) > gate;
	bool added = false;

	if (moving) {
		det->move_n++;
		det->last_move_t = time_s;
	} else {
		det->move_n = 0;
	}

	if (det->state == WHEEL_STATE_IDLE) {
		if (!moving) {
			update_still(det, v);
		}
		if (det->move_n >= DET_MOVE_SAMPLES) {
			det->state = WHEEL_STATE_CALIBRATING;
			det->win_len = 0;
			reset_phase(det);
		}
	} else if (det->move_n == 0U &&
		   (time_s - det->last_move_t) > DET_STILL_TIMEOUT_S) {
		det->state = WHEEL_STATE_IDLE;
		det->rpm = 0.0f;
		det->confidence = 0.0f;
		det->period_n = 0U;
		det->win_len = 0;
		reset_phase(det);
		reset_still_window(det);
	}

	if (det->state != WHEEL_STATE_IDLE) {
		if (!det->plane.valid) {
			if (moving) {
				window_add(det, time_s, v);
				added = true;
			}
			if (det->win_len >= WHEEL_PLANE_MAX) {
				if (plane_solve(det)) {
					/* Replay the window so the first revolutions
					 * of this spin are not lost. */
					for (uint32_t i = 0; i < det->win_len; i++) {
						float s[3];

						for (int k = 0; k < 3; k++) {
							s[k] = det->win[k][i];
						}
						phase_process(det,
							      det->win_t0 + (double)det->win_t[i],
							      s, &out);
					}
				} else {
					/* Keep the newer half and learn on.
					 *
					 * The surviving samples must be
					 * rebased onto the new origin:
					 * window_add() stamps everything
					 * relative to win_t0, so leaving
					 * them on the old base would put two
					 * time origins in one window and
					 * stamp the replayed samples into
					 * the future.
					 */
					uint32_t half = det->win_len / 2U;
					double delta =
						(double)det->win_t[half];

					for (uint32_t i = 0; i < half; i++) {
						for (int k = 0; k < 3; k++) {
							det->win[k][i] =
								det->win[k][i + half];
						}
						det->win_t[i] =
							det->win_t[i + half] -
							(float)delta;
					}
					det->win_t0 += delta;
					det->win_len = half;
				}
			}
		}

		if (det->plane.valid && !added) {
			phase_process(det, time_s, v, &out);
		}
	}

	if (det->plane.valid && det->state == WHEEL_STATE_CALIBRATING &&
	    det->revolutions >= DET_LOCK_REVS) {
		det->state = WHEEL_STATE_LOCKED;
	}

	det->t_prev = time_s;
	det->prev[0] = v[0];
	det->prev[1] = v[1];
	det->prev[2] = v[2];

	out.state = det->state;
	out.revolutions = det->revolutions;
	out.direction = det->direction;
	out.rpm = det->rpm;
	out.speed_kmh = det->cfg.circumference_m * det->rpm / 60.0f * 3.6f;
	out.noise_ms2 = det->noise_ms2;
	out.plane_quality = det->plane.quality;
	out.radius_m = det->radial_valid ? det->radius_m : 0.0f;
	out.confidence = det->confidence;
	for (int k = 0; k < 3; k++) {
		out.radial_dir[k] = det->radial_dir[k];
	}

	return out;
}
