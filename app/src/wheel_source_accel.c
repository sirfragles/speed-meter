/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Accelerometer wheel source - see wheel_source_accel.h.
 *
 * Sampling is a plain thread that reads the LIS2DH through the Zephyr sensor
 * driver at the configured ODR and feeds the detector. The detector itself is
 * timestamp-driven (it uses the real sample times, not the nominal rate), so
 * scheduler jitter cannot fake or hide a revolution.
 */

#include "wheel_source_accel.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "csc.h"
#include "wheel_config.h"
#include "wheel_csc.h"
#include "wheel_detector.h"
#include "wheel_reconfig.h"

#if IS_ENABLED(CONFIG_CSC_POWER_SAVE)
#include "wheel_power.h"
#endif

LOG_MODULE_REGISTER(wheel_source_accel, LOG_LEVEL_INF);

#define ACCEL_NODE DT_ALIAS(accel0)

#if !DT_NODE_EXISTS(ACCEL_NODE)
#error "CONFIG_CSC_WHEEL_SENSOR_ACCEL needs an accelerometer (devicetree alias accel0)."
#endif

static const struct device *const accel = DEVICE_DT_GET(ACCEL_NODE);

/*
 * The detector holds a 256-sample calibration window plus the radius fit
 * (~4.5 kB), so it lives in BSS instead of on the thread stack.
 */
static struct wheel_detector det;
static bool det_ready;

/* Most recent detector result - only the diagnostics shell reads it, but it is
 * the only way to see the production detector's speed and state from outside
 * the sampling thread. */
static struct wheel_detector_result last_result;

/* Poll period while wheel_power holds the sensor in its 1 Hz standby mode:
 * slow enough to cost nothing, fast enough to notice the wheel moving again. */
#define STANDBY_POLL_US 1000000U

/* How long to wait before retrying a configuration the sensor rejected. */
#define CONFIG_RETRY_MS 2000U

/* CSCS wire state: event-time clock and cumulative revolution counter. */
static struct wheel_csc csc_state;

/* Fail-closed gate: which configuration the sensor and detector agree on. */
static struct wheel_reconfig reconfig;

/* How the bus accesses went - see wheel_source_accel.h. */
static struct wheel_source_stats stats;

void wheel_source_accel_stats(struct wheel_source_stats *out)
{
	*out = stats;
}

void wheel_source_accel_stats_reset(void)
{
	stats = (struct wheel_source_stats){ 0 };
}

bool wheel_source_accel_detector_stats(struct wheel_detector_stats *stats_out,
				       struct wheel_detector_result *last)
{
	if (!det_ready) {
		return false;
	}

	*stats_out = det.stats;

	if (last != NULL) {
		*last = last_result;
	}

	return true;
}

void wheel_source_accel_detector_stats_reset(void)
{
	det.stats = (struct wheel_detector_stats){ 0 };
}

static uint64_t now_us(void)
{
	return k_ticks_to_us_floor64(k_uptime_ticks());
}

static bool power_standby(void)
{
#if IS_ENABLED(CONFIG_CSC_POWER_SAVE)
	return wheel_power_is_standby();
#else
	return false;
#endif
}

static void power_report(bool rotating)
{
#if IS_ENABLED(CONFIG_CSC_POWER_SAVE)
	wheel_power_report(rotating);
#else
	ARG_UNUSED(rotating);
#endif
}

static bool accel_apply_config(void)
{
	const struct wheel_config *cfg = wheel_config_get();
	struct wheel_detector_config det_cfg = {
		.circumference_m = cfg->circ_mm / 1000.0f,
		.full_scale_ms2 = 9.80665f * (float)cfg->range_g,
		.nominal_radius_m = 0.010f,
		.odr_hz = cfg->odr_hz,
		.rpm_max = (float)cfg->rpm_max,
		.still_gate_ms2 = 0.5f,
	};
	struct sensor_value odr;
	struct sensor_value full_scale;

	odr.val1 = cfg->odr_hz;
	odr.val2 = 0;
	sensor_g_to_ms2(cfg->range_g, &full_scale);

	/*
	 * The sensor first, and all-or-nothing. The ODR may be accepted while the
	 * full scale is rejected, and the sensor then holds neither the old values
	 * nor the new ones - so the detector must not be re-initialised until both
	 * took effect. Reporting a partial success would leave it classifying
	 * saturated samples as good, because its idea of full scale would
	 * disagree with the hardware's.
	 */
	if (sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ,
			    SENSOR_ATTR_SAMPLING_FREQUENCY, &odr) != 0) {
		LOG_WRN("cannot set ODR to %u Hz", cfg->odr_hz);
		return false;
	}
	if (sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_FULL_SCALE,
			    &full_scale) != 0) {
		LOG_WRN("cannot set full scale to +/-%u g", cfg->range_g);
		return false;
	}

	/*
	 * A different ODR, range or geometry invalidates the plane fit and the
	 * radius estimate, so the detector starts over. That zeroes its own
	 * revolution count, which is why the published counter lives in wheel_csc
	 * and advances by deltas: a re-initialisation is an internal event and
	 * must not be visible to the host.
	 */
	det_ready = wheel_detector_init(&det, &det_cfg);
	if (!det_ready) {
		LOG_ERR("wheel detector init failed");
		return false;
	}

	LOG_INF("Wheel source: accelerometer (circ %u mm, %u Hz, +/-%u g)", cfg->circ_mm,
		cfg->odr_hz, cfg->range_g);
	return true;
}

/** Publish one revolution, deriving the CSCS event time from real timestamps. */
static void publish_revolution(const struct wheel_detector_result *res)
{
	struct wheel_csc_sample sample =
		wheel_csc_revolution(&csc_state, res->revolutions, k_uptime_get());

	LOG_DBG("wheel rev %u: %d.%01d km/h (dir %d, r=%d mm)", res->revolutions,
		(int)res->speed_kmh, (int)(res->speed_kmh * 10.0f) % 10,
		(int)res->direction, (int)(res->radius_m * 1000.0f));

	csc_publish_wheel(sample.revolutions, sample.event_time);
}

void wheel_source_accel_step(uint64_t *next_us, uint32_t *period_us)
{
	const struct wheel_config *cfg = wheel_config_get();
	uint32_t want_us;
	struct sensor_value v[3];
	struct wheel_detector_result res;
	uint64_t t;
	int err;

	/*
	 * K_THREAD_DEFINE starts the sampling thread during kernel init, i.e.
	 * before main() calls wheel_config_init() - cfg is all zeroes until then.
	 * Bail out rather than divide by a zero ODR.
	 */
	if (cfg->odr_hz == 0U) {
		k_sleep(K_MSEC(10));
		return;
	}

	/* Pick up a `wheel cal set ...` without a reboot, including the
	 * geometry the detector copies at init - circumference and rpm_max
	 * used to be ignored here, leaving it computing with stale values. */
	{
		struct wheel_reconfig_keys wanted = {
			.odr_hz = cfg->odr_hz,
			.range_g = cfg->range_g,
			.circ_mm = cfg->circ_mm,
			.rpm_max = cfg->rpm_max,
		};

		if (wheel_reconfig_needed(&reconfig, &wanted)) {
			if (!wheel_reconfig_may_retry(&reconfig, k_uptime_get())) {
				/*
				 * Fail-closed: the sensor and the detector may
				 * disagree, so nothing is forwarded and no
				 * revolution is published. The CSC counters
				 * live outside this gate and keep their
				 * values, so the host sees no gap beyond the
				 * one that really happened.
				 */
				k_sleep(K_MSEC(100));
				return;
			}

			if (!accel_apply_config()) {
				wheel_reconfig_failed(&reconfig, k_uptime_get(),
						      CONFIG_RETRY_MS);
				k_sleep(K_MSEC(100));
				return;
			}

			wheel_reconfig_applied(&reconfig, &wanted);
			/* Force the cadence below to restart from "now". */
			*period_us = 0;
		}
	}

	want_us = power_standby() ? STANDBY_POLL_US : 1000000U / cfg->odr_hz;
	if (want_us != *period_us) {
		*period_us = want_us;
		*next_us = now_us(); /* restart the cadence on a rate change */
	}

	t = now_us();
	if (t < *next_us) {
		k_sleep(K_USEC((uint32_t)(*next_us - t)));
		t = now_us();
	}

	/*
	 * Never leave the deadline in the past. One missed period is absorbed by
	 * the arithmetic below; without this, a wake-up later than a period would
	 * make every following pass fetch back to back - reading the same sample
	 * over and over - until the deadline caught up.
	 */
	if (*next_us + *period_us < t) {
		*next_us = t + *period_us;
	} else {
		*next_us += *period_us;
	}

	err = sensor_sample_fetch(accel);

	if (err == -ENODATA) {
		/*
		 * "No new measurement yet" - the normal outcome of polling at the
		 * configured rate, not a lost sample. Forwarding nothing is right,
		 * and telling the detector would only drop a phase that is still
		 * perfectly valid.
		 */
		stats.no_data++;
		return;
	}

	if (err != 0 ||
	    sensor_channel_get(accel, SENSOR_CHAN_ACCEL_XYZ, v) != 0) {
		/*
		 * A bus error: the samples in between never arrived, and all the
		 * detector knows is that time passed. Tell it, so it drops the phase
		 * instead of interpolating across a hole it cannot see.
		 */
		stats.lost++;
		wheel_detector_samples_lost(&det);
		return;
	}

	stats.samples++;

	if (!det_ready) {
		return;
	}

	t = now_us();
	res = wheel_detector_update(&det, (double)t / 1000000.0,
				    (float)v[0].val1 + (float)v[0].val2 / 1000000.0f,
				    (float)v[1].val1 + (float)v[1].val2 / 1000000.0f,
				    (float)v[2].val1 + (float)v[2].val2 / 1000000.0f);

	last_result = res;

	if (res.new_revolution) {
		publish_revolution(&res);
	}

	/*
	 * Hands the standstill/wake decision to wheel_power.c: it puts the
	 * sensor (and, when nobody is connected, the SoC) to sleep after
	 * CONFIG_CSC_POWER_IDLE_TIMEOUT_S without a revolution, and wakes
	 * them up again on the first sign of movement. This is what makes
	 * the device start measuring by itself when the bike moves.
	 */
	power_report(res.state != WHEEL_STATE_IDLE);
}

#ifndef CONFIG_ZTEST
static void accel_thread(void *arg1, void *arg2, void *arg3)
{
	uint64_t next = 0;
	uint32_t period_us = 0;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		wheel_source_accel_step(&next, &period_us);
	}
}

K_THREAD_DEFINE(accel_tid, 2048, accel_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, 0);
#endif /* !CONFIG_ZTEST */

int wheel_source_accel_init(void)
{
	/*
	 * Device power-up is the one moment where the cumulative CSCS counter may
	 * start from zero. Deliberately *not* in accel_apply_config(): a detector
	 * re-initialisation is an internal event and must not be visible to the
	 * host as the distance counter jumping backwards.
	 */
	wheel_csc_init(&csc_state);
	wheel_reconfig_init(&reconfig);

	if (!device_is_ready(accel)) {
		LOG_ERR("accelerometer not ready");
		return -ENODEV;
	}

#if IS_ENABLED(CONFIG_CSC_POWER_SAVE)
	/* Arms the sensor for measurement and resets the standstill timer. */
	return wheel_power_init();
#else
	/* Without the power module the thread configures the sensor itself. */
	LOG_WRN("accelerometer source without power.conf: no automatic standby");
	return 0;
#endif
}
