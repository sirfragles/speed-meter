/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Remote diagnostics for the Speed Meter:
 *   - accelerometer recording (raw X/Y/Z into a RAM buffer),
 *   - calibration parameters used by the wheel detector,
 * driven remotely over the MCUmgr shell (see tools/wheel_cal.py and
 * diag.conf). Enable with CONFIG_CSC_DIAG.
 */

#include "wheel_config.h"
#include "wheel_detector.h"

#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#if IS_ENABLED(CONFIG_CSC_POWER_SAVE)
#include "wheel_power.h"
#endif

#if IS_ENABLED(CONFIG_CSC_WHEEL_SENSOR_ACCEL)
#include "wheel_source_accel.h"
#endif

#if IS_ENABLED(CONFIG_CSC_BATTERY)
#include "battery.h"
#endif

LOG_MODULE_REGISTER(wheel_diag, LOG_LEVEL_INF);

#define ACCEL_NODE DT_ALIAS(accel0)

#if !DT_NODE_EXISTS(ACCEL_NODE)
#error "CONFIG_CSC_DIAG needs an accelerometer (devicetree alias accel0)."
#endif

static const struct device *const accel = DEVICE_DT_GET(ACCEL_NODE);

/*
 * The configuration lives in wheel_config.c (persisted in NVS) so the
 * detector, the diagnostics shell and a reboot all see the same values.
 */
static inline const struct wheel_config *wcfg(void)
{
	return wheel_config_get();
}

/* ------------------------------------------------------------------ */
/* Wheel rotation detector (gravity cycles + centripetal cross-check). */
/* ------------------------------------------------------------------ */

static struct wheel_detector det;
static bool det_inited;
static bool det_enabled;
static struct wheel_detector_result det_last;

static void det_configure(void)
{
	const struct wheel_detector_config det_cfg = {
		.circumference_m = wcfg()->circ_mm / 1000.0f,
		.full_scale_ms2 = 9.80665f * (float)wcfg()->range_g,
		.nominal_radius_m = 0.010f,
		.odr_hz = wcfg()->odr_hz,
		.rpm_max = (float)wcfg()->rpm_max,
		.still_gate_ms2 = 0.5f,
	};

	det_inited = wheel_detector_init(&det, &det_cfg);
	det_last = (struct wheel_detector_result){ 0 };
	LOG_INF("Wheel detector: self-calibrating (circ %u mm, +/-%u g, %u Hz)",
		wcfg()->circ_mm, wcfg()->range_g, wcfg()->odr_hz);

#if IS_ENABLED(CONFIG_CSC_POWER_SAVE)
	(void)wheel_power_init();
#endif
}

static void det_feed_sample(const struct sensor_value *v, uint32_t t_us)
{
	float x = (float)v[0].val1 + (float)v[0].val2 / 1000000.0f;
	float y = (float)v[1].val1 + (float)v[1].val2 / 1000000.0f;
	float z = (float)v[2].val1 + (float)v[2].val2 / 1000000.0f;
	struct wheel_detector_result res =
		wheel_detector_update(&det, (double)t_us / 1000000.0, x, y, z);

	det_last = res;

#if IS_ENABLED(CONFIG_CSC_POWER_SAVE)
	wheel_power_report(res.state != WHEEL_STATE_IDLE);
#endif

	if (res.new_revolution) {
		LOG_INF("wheel rev %u: rpm=%d.%d speed=%d.%d km/h (dir %d, r=%d mm)",
			res.revolutions, (int)res.rpm, (int)(res.rpm * 10.0f) % 10,
			(int)res.speed_kmh, (int)(res.speed_kmh * 10.0f) % 10,
			(int)res.direction, (int)(res.radius_m * 1000.0f));
	}
}

/* ------------------------------------------------------------------ */
/* Accelerometer recording.                                            */
/* ------------------------------------------------------------------ */

struct rec_sample {
	uint32_t t_us;
	int16_t ax_mg;
	int16_t ay_mg;
	int16_t az_mg;
} __packed;

static struct rec_sample rec_buf[CONFIG_CSC_DIAG_REC_SAMPLES];
static uint32_t rec_len;
static volatile bool rec_active;

K_SEM_DEFINE(rec_start_sem, 0, 1);

static uint64_t now_us(void)
{
	return k_ticks_to_us_floor64(k_uptime_ticks());
}

static int16_t ms2_to_mg(int32_t ms2_milli)
{
	/* ms2_milli is m/s^2 * 1000; -> milli-g */
	return (int16_t)((int64_t)ms2_milli * 1000 / 9807);
}

static void rec_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		k_sem_take(&rec_start_sem, K_FOREVER);

		const struct wheel_config *c = wcfg();
		struct sensor_value odr;
		struct sensor_value full_scale;
		const uint32_t period_us = 1000000U / c->odr_hz;
		uint64_t next = now_us() + period_us;
		int oerr;
		int rerr;

		odr.val1 = c->odr_hz;
		odr.val2 = 0;
		sensor_g_to_ms2(c->range_g, &full_scale);
		oerr = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ,
				       SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
		rerr = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ,
				       SENSOR_ATTR_FULL_SCALE, &full_scale);

		LOG_INF("Recording: %u Hz (err %d), +/-%u g (err %d), buffer %u samples",
			c->odr_hz, oerr, c->range_g, rerr,
			(unsigned int)ARRAY_SIZE(rec_buf));

		while (rec_active) {
			uint64_t t = now_us();

			if (t < next) {
				k_sleep(K_USEC((uint32_t)(next - t)));
			}
			next += period_us;

			if (rec_len >= ARRAY_SIZE(rec_buf)) {
				rec_active = false;
				LOG_INF("Recording buffer full (%u samples)", rec_len);
				break;
			}

			struct sensor_value v[3];

			if (sensor_sample_fetch(accel) != 0 ||
			    sensor_channel_get(accel, SENSOR_CHAN_ACCEL_XYZ, v) != 0) {
				continue;
			}

			struct rec_sample *s = &rec_buf[rec_len++];

			s->t_us = (uint32_t)t;
			s->ax_mg = ms2_to_mg(v[0].val1 * 1000 + v[0].val2 / 1000);
			s->ay_mg = ms2_to_mg(v[1].val1 * 1000 + v[1].val2 / 1000);
			s->az_mg = ms2_to_mg(v[2].val1 * 1000 + v[2].val2 / 1000);

			if (det_enabled && det_inited) {
				det_feed_sample(v, (uint32_t)t);
			}
		}

		LOG_INF("Recording stopped: %u samples", rec_len);
	}
}

K_THREAD_DEFINE(rec_tid, 1536, rec_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(7), 0, 0);

/* ------------------------------------------------------------------ */
/* Shell commands: wheel status|cal|record ...                         */
/* ------------------------------------------------------------------ */

static char axis_letter(uint8_t axis)
{
	return (char)('x' + axis);
}

static const char *det_state_name(enum wheel_detector_state state)
{
	switch (state) {
	case WHEEL_STATE_CALIBRATING:
		return "CALIBRATING";
	case WHEEL_STATE_LOCKED:
		return "LOCKED";
	default:
		return "IDLE";
	}
}

static void print_cal(const struct shell *sh)
{
	const struct wheel_config *c = wcfg();
	char axes[5];

	if (c->axis_a >= 3U) {
		strcpy(axes, "auto");
	} else {
		axes[0] = axis_letter(c->axis_a);
		axes[1] = axis_letter(c->axis_b);
		axes[2] = '\0';
	}

	shell_print(sh,
		    "cal: axes=%s invert_a=%d invert_b=%d rpm_min=%u rpm_max=%u "
		    "amp_mg=%u step_mrad=%u alpha_milli=%u stop_ms=%u circ_mm=%u",
		    axes, c->invert_a, c->invert_b, c->rpm_min, c->rpm_max,
		    c->amp_mg, c->step_mrad, c->alpha_milli, c->stop_ms,
		    c->circ_mm);
	shell_print(sh, "cal: odr_hz=%u range_g=%u", c->odr_hz, c->range_g);
}

#if IS_ENABLED(CONFIG_CSC_WHEEL_SENSOR_ACCEL)
/*
 * The production detector, not the one this shell drives for its own recording.
 * Those are two different instances, and only this one counts the wheel - so
 * without this, `wheel status` would answer a question nobody asked.
 */
static void print_source_det(const struct shell *sh)
{
	struct wheel_detector_stats st;
	struct wheel_detector_result last;

	if (!wheel_source_accel_detector_stats(&st, &last)) {
		shell_print(sh, "src: accelerometer source has no detector");
		return;
	}

	shell_print(sh,
		    "src: state=%s revs=%u rpm=%d.%d plane_try=%u ok=%u slides=%u resets=%u",
		    det_state_name(last.state), last.revolutions,
		    (int)last.rpm, (int)(last.rpm * 10.0f) % 10,
		    st.plane_attempts, st.plane_successes, st.window_slides,
		    st.phase_resets);

	/*
	 * The gate in mg, next to the noise that produced it, because that is the
	 * pair that decides whether the detector can move at all: the gate is
	 * max(4 * noise, still_gate) and the noise is measured on the samples the
	 * gate rejected. Once the gate passes the gravity circle - about 2 g, the
	 * most the sensor can show - nothing can be moving any more, and the
	 * detector stays in IDLE permanently. That is the LATCHED flag.
	 */
	shell_print(sh,
		    "src: q_last=%d.%02d q_best=%d.%02d gate moving=%u still=%u "
		    "noise=%dmg gate=%dmg%s",
		    (int)st.last_quality, (int)(st.last_quality * 100.0f) % 100,
		    (int)st.best_quality, (int)(st.best_quality * 100.0f) % 100,
		    st.moving_samples, st.still_windows,
		    (int)(st.noise_last / 9.80665f * 1000.0f),
		    (int)(st.gate_last / 9.80665f * 1000.0f),
		    (st.gate_last > 2.0f * 9.80665f) ? " LATCHED" : "");
}
#endif

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "wheel status: recording=%d len=%u cap=%u odr_hz=%u range_g=%u",
		    rec_active ? 1 : 0, rec_len, (unsigned int)ARRAY_SIZE(rec_buf),
		    wcfg()->odr_hz, wcfg()->range_g);
	if (det_inited) {
		shell_print(sh,
			    "det: enabled=%d state=%s revs=%u dir=%d rpm=%d.%d speed_kmh=%d.%d conf=%d.%02d",
			    det_enabled ? 1 : 0, det_state_name(det_last.state),
			    det_last.revolutions, (int)det_last.direction,
			    (int)det_last.rpm, (int)(det_last.rpm * 10.0f) % 10,
			    (int)det_last.speed_kmh,
			    (int)(det_last.speed_kmh * 10.0f) % 10,
			    (int)det_last.confidence,
			    (int)(det_last.confidence * 100.0f) % 100);
		shell_print(sh,
			    "det: plane_q=%d.%02d noise_mg=%d radius_mm=%d radial_m=(%d,%d,%d)",
			    (int)det_last.plane_quality,
			    (int)(det_last.plane_quality * 100.0f) % 100,
			    (int)(det_last.noise_ms2 / 9.80665f * 1000.0f),
			    (int)(det_last.radius_m * 1000.0f),
			    (int)(det_last.radial_dir[0] * 1000.0f),
			    (int)(det_last.radial_dir[1] * 1000.0f),
			    (int)(det_last.radial_dir[2] * 1000.0f));
	}
#if IS_ENABLED(CONFIG_CSC_WHEEL_SENSOR_ACCEL)
	print_source_det(sh);
#endif
	print_cal(sh);
	return 0;
}

static int cmd_cal_get(const struct shell *sh, size_t argc, char **argv)
{
	const struct wheel_config *c = wcfg();

	if (argc < 2) {
		print_cal(sh);
		return 0;
	}

	const char *key = argv[1];

	if (!strcmp(key, "axes")) {
		if (c->axis_a >= 3U) {
			shell_print(sh, "cal axes=auto");
		} else {
			shell_print(sh, "cal axes=%c%c", axis_letter(c->axis_a),
				    axis_letter(c->axis_b));
		}
	} else if (!strcmp(key, "invert_a")) {
		shell_print(sh, "cal invert_a=%d", c->invert_a);
	} else if (!strcmp(key, "invert_b")) {
		shell_print(sh, "cal invert_b=%d", c->invert_b);
	} else if (!strcmp(key, "rpm_min")) {
		shell_print(sh, "cal rpm_min=%u", c->rpm_min);
	} else if (!strcmp(key, "rpm_max")) {
		shell_print(sh, "cal rpm_max=%u", c->rpm_max);
	} else if (!strcmp(key, "amp_mg")) {
		shell_print(sh, "cal amp_mg=%u", c->amp_mg);
	} else if (!strcmp(key, "step_mrad")) {
		shell_print(sh, "cal step_mrad=%u", c->step_mrad);
	} else if (!strcmp(key, "alpha_milli")) {
		shell_print(sh, "cal alpha_milli=%u", c->alpha_milli);
	} else if (!strcmp(key, "stop_ms")) {
		shell_print(sh, "cal stop_ms=%u", c->stop_ms);
	} else if (!strcmp(key, "circ_mm")) {
		shell_print(sh, "cal circ_mm=%u", c->circ_mm);
	} else if (!strcmp(key, "odr_hz")) {
		shell_print(sh, "cal odr_hz=%u", c->odr_hz);
	} else if (!strcmp(key, "range_g")) {
		shell_print(sh, "cal range_g=%u", c->range_g);
	} else {
		shell_error(sh, "unknown key '%s'", key);
		return -EINVAL;
	}

	return 0;
}

static int cmd_cal_set(const struct shell *sh, size_t argc, char **argv)
{
	const char *key = argv[1];
	const char *value = argv[2];
	/*
	 * All validation and the write back to NVS live in wheel_config.c, so
	 * the shell and the detector configuration can never drift apart.
	 */
	int err = wheel_config_set(key, value);

	if (err != 0) {
		shell_error(sh, "cal %s: unknown key or value '%s' out of range",
			    key, value);
		return err;
	}

	/* Echo in the form tools/wheel_cal.py expects. */
	if (!strcmp(key, "axes")) {
		shell_print(sh, "cal axes=%s", value);
	} else {
		shell_print(sh, "cal %s=%s", key, value);
	}

	return 0;
}

static int cmd_rec_start(const struct shell *sh, size_t argc, char **argv)
{
	if (rec_active) {
		shell_error(sh, "already recording");
		return -EBUSY;
	}

	if (argc > 1) {
		if (wheel_config_set("odr_hz", argv[1]) != 0) {
			shell_error(sh, "odr must be 1/10/25/50/100/200/400");
			return -EINVAL;
		}
	}

	if (argc > 2) {
		if (wheel_config_set("range_g", argv[2]) != 0) {
			shell_error(sh, "range must be 2, 4, 8 or 16");
			return -EINVAL;
		}
	}

	det_enabled = (argc > 3) && (strtol(argv[3], NULL, 0) != 0);
	if (det_enabled) {
		det_configure();
	}

	rec_len = 0;
	rec_active = true;
	k_sem_give(&rec_start_sem);
	shell_print(sh,
		    "wheel record: starting, odr_hz=%u range_g=%u detect=%d cap=%u",
		    wcfg()->odr_hz, wcfg()->range_g, det_enabled ? 1 : 0,
		    (unsigned int)ARRAY_SIZE(rec_buf));
	return 0;
}

static int cmd_rec_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rec_active = false;
	shell_print(sh, "wheel record: stopping, len=%u", rec_len);
	return 0;
}

static int cmd_rec_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "wheel record: recording=%d len=%u cap=%u odr_hz=%u range_g=%u",
		    rec_active ? 1 : 0, rec_len, (unsigned int)ARRAY_SIZE(rec_buf),
		    wcfg()->odr_hz, wcfg()->range_g);
	return 0;
}

static int cmd_rec_dump(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t start = 0;
	uint32_t count = 64;

	if (argc > 1) {
		start = (uint32_t)strtoul(argv[1], NULL, 0);
	}
	if (argc > 2) {
		count = (uint32_t)strtoul(argv[2], NULL, 0);
	}

	count = MIN(count, 128U);

	for (uint32_t i = start; i < rec_len && i < start + count; i++) {
		const struct rec_sample *s = &rec_buf[i];

		shell_print(sh, "%u,%d,%d,%d", s->t_us, s->ax_mg, s->ay_mg,
			    s->az_mg);
	}

	return 0;
}

static int cmd_accel(const struct shell *sh, size_t argc, char **argv)
{
	long count = 3;
	long odr_hz = 0;
	long range_g = 0;
	int oerr = 0;
	int rerr = 0;

	if (argc > 1) {
		count = strtol(argv[1], NULL, 0);
	}
	if (argc > 2) {
		odr_hz = strtol(argv[2], NULL, 0);
	}
	if (argc > 3) {
		range_g = strtol(argv[3], NULL, 0);
	}

	if (odr_hz > 0) {
		struct sensor_value odr = { .val1 = (int32_t)odr_hz, .val2 = 0 };

		oerr = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ,
				       SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
	}
	if (range_g > 0) {
		struct sensor_value full_scale;

		sensor_g_to_ms2((int32_t)range_g, &full_scale);
		rerr = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ,
				       SENSOR_ATTR_FULL_SCALE, &full_scale);
	}
	if (odr_hz > 0 || range_g > 0) {
		shell_print(sh, "cfg: odr=%ld (err %d), range=%ld g (err %d)", odr_hz,
			    oerr, range_g, rerr);
	}

	for (long i = 0; i < count; i++) {
		struct sensor_value v[3];
		int fr = sensor_sample_fetch(accel);
		int cr = -1;

		if (fr == 0) {
			cr = sensor_channel_get(accel, SENSOR_CHAN_ACCEL_XYZ, v);
		}

		if (fr == 0 && cr == 0) {
			shell_print(sh, "accel[%ld]: %d.%06d %d.%06d %d.%06d m/s2",
				    i, v[0].val1, v[0].val2, v[1].val1, v[1].val2,
				    v[2].val1, v[2].val2);
		} else {
			shell_print(sh, "accel[%ld]: fetch=%d get=%d", i, fr, cr);
		}

		k_sleep(K_MSEC(100));
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_cal,
	SHELL_CMD_ARG(get, NULL, "Get all or one key: get [key]", cmd_cal_get, 1, 1),
	SHELL_CMD_ARG(set, NULL, "Set: set <key> <value>", cmd_cal_set, 3, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_record,
	SHELL_CMD_ARG(start, NULL, "Start: start [odr_hz] [range_g] [detect]", cmd_rec_start, 1, 3),
	SHELL_CMD_ARG(stop, NULL, "Stop recording", cmd_rec_stop, 1, 0),
	SHELL_CMD_ARG(info, NULL, "Show recorder state", cmd_rec_info, 1, 0),
	SHELL_CMD_ARG(dump, NULL, "Dump: dump [start] [count<=128]", cmd_rec_dump, 1, 2),
	SHELL_SUBCMD_SET_END);

#if IS_ENABLED(CONFIG_CSC_POWER_SAVE)
static int cmd_power(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "power=%s woke_from_off=%d",
			    wheel_power_is_standby() ? "standby" : "active",
			    wheel_power_woke_from_off() ? 1 : 0);
		return 0;
	}

	if (!strcmp(argv[1], "active")) {
		int err = wheel_power_set_active();

		if (err != 0) {
			shell_error(sh, "power active failed: %d", err);
			return err;
		}
		shell_print(sh, "power=active");
		return 0;
	}

	if (!strcmp(argv[1], "standby")) {
		int err = wheel_power_set_standby();

		if (err != 0) {
			shell_error(sh, "power standby failed: %d", err);
			return err;
		}
		shell_print(sh, "power=standby");
		return 0;
	}

	shell_error(sh, "usage: wheel power <active|standby|info>");
	return -EINVAL;
}
#endif

#if IS_ENABLED(CONFIG_CSC_BATTERY)
static int cmd_battery(const struct shell *sh, size_t argc, char **argv)
{
	int32_t mv = 0;
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	err = battery_read_mv(&mv);
	if (err != 0) {
		shell_error(sh, "battery read failed: %d", err);
		return err;
	}

	/* Both the raw voltage and the percentage: the percentage is only as
	 * good as the CR2032 curve, the voltage is what a multimeter shows. */
	shell_print(sh, "battery: %d mV, %u%% (last published %u%%)", (int)mv,
		    battery_percent_from_mv(mv), battery_percent());
	return 0;
}
#endif

#if IS_ENABLED(CONFIG_CSC_WHEEL_SENSOR_ACCEL)
/*
 * `wheel stats` reads and clears the production source's counters. Two sets,
 * two questions: the source counters say how the bus behaved, the detector
 * counters say whether the calibration could start at all. Cleared together
 * because they are read together, after a run with a known revolution count.
 */
static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	struct wheel_source_stats src;

	if (argc > 1 && !strcmp(argv[1], "reset")) {
		wheel_source_accel_stats_reset();
		wheel_source_accel_detector_stats_reset();
		shell_print(sh, "stats: reset");
		return 0;
	}

	wheel_source_accel_stats(&src);
	shell_print(sh, "src: samples=%u no_data=%u lost=%u",
		    src.samples, src.no_data, src.lost);
	print_source_det(sh);
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(wheel_cmds,
	SHELL_CMD(status, NULL, "Show status and calibration", cmd_status),
	SHELL_CMD_ARG(accel, NULL, "Read accel: accel [count] [odr_hz] [range_g]", cmd_accel, 1, 3),
	SHELL_CMD(cal, &sub_cal, "Calibration parameters", NULL),
	SHELL_CMD(record, &sub_record, "Accelerometer recording", NULL),
#if IS_ENABLED(CONFIG_CSC_WHEEL_SENSOR_ACCEL)
	SHELL_CMD_ARG(stats, NULL, "Source and calibration counters: stats [reset]", cmd_stats, 1, 1),
#endif
#if IS_ENABLED(CONFIG_CSC_POWER_SAVE)
	SHELL_CMD_ARG(power, NULL, "power <active|standby> (low-power control)", cmd_power, 1, 1),
#endif
#if IS_ENABLED(CONFIG_CSC_BATTERY)
	SHELL_CMD(battery, NULL, "CR2032 voltage (mV) and state of charge", cmd_battery),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(wheel, &wheel_cmds, "Wheel sensor diagnostics", NULL);
