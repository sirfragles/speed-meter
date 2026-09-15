/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wheel configuration with persistence - see wheel_config.h.
 *
 * Storage format: one versioned, packed blob under the settings key
 * "wheel/cfg". The blob is written explicitly instead of dumping the struct,
 * so a future change to struct wheel_config cannot make an old blob load as
 * garbage.
 */

#include "wheel_config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(wheel_config, LOG_LEVEL_INF);

/* Settings subtree/key: "wheel/cfg". */
#define WHEEL_SETTINGS_SUBTREE "wheel"
#define WHEEL_SETTINGS_KEY     "cfg"

#define WHEEL_BLOB_MAGIC   0xA7U
#define WHEEL_BLOB_VERSION 1U

/* Debounce: a `cal-set axes xy amp_mg 200 step_mrad 1600` run arrives as three
 * separate shell commands; coalesce them into one NVS write.
 */
#define WHEEL_SAVE_DELAY_MS 500

struct wheel_settings_blob {
	uint8_t magic;
	uint8_t version;
	uint8_t axis_a;
	uint8_t axis_b;
	uint8_t invert_a;
	uint8_t invert_b;
	uint8_t range_g;
	uint16_t rpm_min;
	uint16_t rpm_max;
	uint16_t amp_mg;
	uint16_t step_mrad;
	uint16_t alpha_milli;
	uint16_t stop_ms;
	uint16_t circ_mm;
	uint16_t odr_hz;
} __packed;

/* Built-in defaults, as tuned during bring-up. */
static const struct wheel_config cfg_defaults = {
	.axis_a = 3,  /* >= 3: axis plane detected automatically */
	.axis_b = 0,
	.invert_a = false,
	.invert_b = false,
	.rpm_min = 12,   /* 0.2 rev/s */
	.rpm_max = 1200, /* 20 rev/s */
	.amp_mg = 150,
	.step_mrad = 1600,
	.alpha_milli = 5,
	.stop_ms = 3000,
	.circ_mm = 2100, /* 28" wheel */
	.odr_hz = 100,
	/* 16 g gives headroom for the centripetal force at speed. */
	.range_g = 16,
};

/* Live configuration; wheel_config_init() seeds it from cfg_defaults. */
static struct wheel_config cfg;

static struct k_work_delayable save_work;
static bool settings_ready;

const struct wheel_config *wheel_config_get(void)
{
	return &cfg;
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

static bool valid_odr(uint16_t odr)
{
	switch (odr) {
	case 1:
	case 10:
	case 25:
	case 50:
	case 100:
	case 200:
	case 400:
		return true;
	default:
		return false;
	}
}

static bool valid_range(uint8_t range)
{
	return range == 2 || range == 4 || range == 8 || range == 16;
}

static void blob_from_cfg(struct wheel_settings_blob *b)
{
	b->magic = WHEEL_BLOB_MAGIC;
	b->version = WHEEL_BLOB_VERSION;
	b->axis_a = cfg.axis_a;
	b->axis_b = cfg.axis_b;
	b->invert_a = cfg.invert_a ? 1U : 0U;
	b->invert_b = cfg.invert_b ? 1U : 0U;
	b->range_g = cfg.range_g;
	b->rpm_min = cfg.rpm_min;
	b->rpm_max = cfg.rpm_max;
	b->amp_mg = cfg.amp_mg;
	b->step_mrad = cfg.step_mrad;
	b->alpha_milli = cfg.alpha_milli;
	b->stop_ms = cfg.stop_ms;
	b->circ_mm = cfg.circ_mm;
	b->odr_hz = cfg.odr_hz;
}

static void cfg_from_blob(const struct wheel_settings_blob *b)
{
	cfg.axis_a = b->axis_a;
	cfg.axis_b = b->axis_b;
	cfg.invert_a = b->invert_a != 0U;
	cfg.invert_b = b->invert_b != 0U;
	cfg.range_g = b->range_g;
	cfg.rpm_min = b->rpm_min;
	cfg.rpm_max = b->rpm_max;
	cfg.amp_mg = b->amp_mg;
	cfg.step_mrad = b->step_mrad;
	cfg.alpha_milli = b->alpha_milli;
	cfg.stop_ms = b->stop_ms;
	cfg.circ_mm = b->circ_mm;
	cfg.odr_hz = b->odr_hz;
}

/*
 * A stored blob is untrusted input: it may come from an older firmware or from
 * a corrupted sector. Reject anything that would leave the detector with an
 * unusable configuration rather than silently misbehaving.
 */
static bool blob_is_sane(const struct wheel_settings_blob *b)
{
	if (b->magic != WHEEL_BLOB_MAGIC || b->version != WHEEL_BLOB_VERSION) {
		return false;
	}
	if (b->axis_a > 3U || b->axis_b > 2U) {
		return false;
	}
	if (!valid_odr(b->odr_hz) || !valid_range(b->range_g)) {
		return false;
	}
	if (b->rpm_min == 0U || b->rpm_max <= b->rpm_min) {
		return false;
	}
	if (b->circ_mm < 500U || b->circ_mm > 3000U) {
		return false;
	}
	if (b->amp_mg == 0U || b->step_mrad < 100U || b->stop_ms < 100U) {
		return false;
	}
	if (b->alpha_milli == 0U || b->alpha_milli > 1000U) {
		return false;
	}
	return true;
}

static int wheel_settings_set(const char *name, size_t len,
			      settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	struct wheel_settings_blob blob;
	int rc;

	if (!settings_name_steq(name, WHEEL_SETTINGS_KEY, &next) || next != NULL) {
		return -ENOENT;
	}

	if (len != sizeof(blob)) {
		LOG_WRN("wheel settings: unexpected size %zu (want %zu)", len,
			sizeof(blob));
		return -EINVAL;
	}

	rc = read_cb(cb_arg, &blob, sizeof(blob));
	if (rc < 0) {
		return rc;
	}

	if (!blob_is_sane(&blob)) {
		LOG_WRN("wheel settings: stored blob rejected, keeping defaults");
		return -EINVAL;
	}

	cfg_from_blob(&blob);
	LOG_INF("wheel config restored (circ %u mm, odr %u Hz, +/-%u g)", cfg.circ_mm,
		cfg.odr_hz, cfg.range_g);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(wheel_cfg, WHEEL_SETTINGS_SUBTREE, NULL,
			       wheel_settings_set, NULL, NULL);

static void save_work_handler(struct k_work *work)
{
	struct wheel_settings_blob blob;
	int err;

	ARG_UNUSED(work);

	if (!settings_ready) {
		return;
	}

	blob_from_cfg(&blob);
	err = settings_save_one(WHEEL_SETTINGS_SUBTREE "/" WHEEL_SETTINGS_KEY, &blob,
				sizeof(blob));
	if (err != 0) {
		LOG_ERR("wheel config save failed: %d", err);
	} else {
		LOG_DBG("wheel config saved");
	}
}

static void schedule_save(void)
{
	if (IS_ENABLED(CONFIG_SETTINGS)) {
		(void)k_work_reschedule(&save_work, K_MSEC(WHEEL_SAVE_DELAY_MS));
	}
}

int wheel_config_init(void)
{
	int err;

	/* Seed with the built-in defaults; settings_load() may override them. */
	cfg = cfg_defaults;

	k_work_init_delayable(&save_work, save_work_handler);

	if (!IS_ENABLED(CONFIG_SETTINGS)) {
		LOG_WRN("settings disabled - wheel config is RAM only");
		return 0;
	}

	err = settings_subsys_init();
	if (err != 0) {
		LOG_ERR("settings_subsys_init failed: %d", err);
		return err;
	}

	settings_ready = true;
	return 0;
}

int wheel_config_reset(void)
{
	int err;

	/* Forget the stored blob, then persist the defaults so the reset sticks. */
	if (settings_ready) {
		err = settings_delete(WHEEL_SETTINGS_SUBTREE "/" WHEEL_SETTINGS_KEY);
		if (err != 0 && err != -ENOENT) {
			LOG_WRN("wheel config delete failed: %d", err);
		}
	}

	cfg = cfg_defaults;
	schedule_save();
	LOG_INF("wheel config reset to defaults");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Setters                                                            */
/* ------------------------------------------------------------------ */

static int parse_axes(const char *value, uint8_t *a, uint8_t *b)
{
	if (!strcmp(value, "auto")) {
		/* Let the detector find the rotation plane by itself. */
		*a = 3U;
		*b = 0U;
	} else if (!strcmp(value, "xy")) {
		*a = 0;
		*b = 1;
	} else if (!strcmp(value, "xz")) {
		*a = 0;
		*b = 2;
	} else if (!strcmp(value, "yz")) {
		*a = 1;
		*b = 2;
	} else {
		return -EINVAL;
	}
	return 0;
}

int wheel_config_set(const char *key, const char *value)
{
	long number;

	if (!strcmp(key, "axes")) {
		uint8_t a;
		uint8_t b;

		if (parse_axes(value, &a, &b) != 0) {
			return -EINVAL;
		}
		cfg.axis_a = a;
		cfg.axis_b = b;
		schedule_save();
		return 0;
	}

	number = strtol(value, NULL, 0);

	if (!strcmp(key, "invert_a")) {
		cfg.invert_a = number != 0;
	} else if (!strcmp(key, "invert_b")) {
		cfg.invert_b = number != 0;
	} else if (!strcmp(key, "rpm_min")) {
		if (number < 1 || number > 3000) {
			return -EINVAL;
		}
		cfg.rpm_min = (uint16_t)number;
	} else if (!strcmp(key, "rpm_max")) {
		if (number < 1 || number > 3000) {
			return -EINVAL;
		}
		cfg.rpm_max = (uint16_t)number;
	} else if (!strcmp(key, "amp_mg")) {
		if (number < 1 || number > 10000) {
			return -EINVAL;
		}
		cfg.amp_mg = (uint16_t)number;
	} else if (!strcmp(key, "step_mrad")) {
		if (number < 100 || number > 3141) {
			return -EINVAL;
		}
		cfg.step_mrad = (uint16_t)number;
	} else if (!strcmp(key, "alpha_milli")) {
		if (number < 1 || number > 1000) {
			return -EINVAL;
		}
		cfg.alpha_milli = (uint16_t)number;
	} else if (!strcmp(key, "stop_ms")) {
		if (number < 100 || number > 60000) {
			return -EINVAL;
		}
		cfg.stop_ms = (uint16_t)number;
	} else if (!strcmp(key, "circ_mm")) {
		if (number < 500 || number > 3000) {
			return -EINVAL;
		}
		cfg.circ_mm = (uint16_t)number;
	} else if (!strcmp(key, "odr_hz")) {
		if (number < 1 || number > 400 || !valid_odr((uint16_t)number)) {
			return -EINVAL;
		}
		cfg.odr_hz = (uint16_t)number;
	} else if (!strcmp(key, "range_g")) {
		if (!valid_range((uint8_t)number)) {
			return -EINVAL;
		}
		cfg.range_g = (uint8_t)number;
	} else {
		return -EINVAL;
	}

	schedule_save();
	return 0;
}
