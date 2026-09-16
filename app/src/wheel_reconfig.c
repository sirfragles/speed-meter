/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fail-closed gate for sensor reconfiguration - see wheel_reconfig.h.
 */

#include "wheel_reconfig.h"

static bool keys_equal(const struct wheel_reconfig_keys *a,
		       const struct wheel_reconfig_keys *b)
{
	return a->odr_hz == b->odr_hz && a->range_g == b->range_g &&
	       a->circ_mm == b->circ_mm && a->rpm_max == b->rpm_max;
}

void wheel_reconfig_init(struct wheel_reconfig *rc)
{
	*rc = (struct wheel_reconfig){ 0 };
}

bool wheel_reconfig_needed(const struct wheel_reconfig *rc,
			   const struct wheel_reconfig_keys *wanted)
{
	return !rc->valid || !keys_equal(&rc->applied, wanted);
}

bool wheel_reconfig_may_retry(const struct wheel_reconfig *rc, int64_t now_ms)
{
	return !rc->valid ? (now_ms >= rc->retry_at_ms) : true;
}

void wheel_reconfig_applied(struct wheel_reconfig *rc,
			    const struct wheel_reconfig_keys *wanted)
{
	rc->applied = *wanted;
	rc->valid = true;
	rc->retry_at_ms = 0;
}

void wheel_reconfig_failed(struct wheel_reconfig *rc, int64_t now_ms,
			   uint32_t retry_ms)
{
	rc->valid = false;
	rc->retry_at_ms = now_ms + (int64_t)retry_ms;
}

bool wheel_reconfig_ready(const struct wheel_reconfig *rc)
{
	return rc->valid;
}
