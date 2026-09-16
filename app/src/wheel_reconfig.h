/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fail-closed gate for sensor reconfiguration.
 *
 * Pushing a configuration into the sensor is not atomic: the ODR may be
 * accepted while the full scale is rejected, and the sensor then holds neither
 * the old values nor the new ones. The detector, meanwhile, is configured from
 * the *requested* values - so it would classify saturated samples as good,
 * because its idea of full scale disagrees with the hardware's.
 *
 * This gate makes that state explicit and inert: while the applied keys are
 * not known to match what was asked for, samples must not be forwarded to the
 * detector, revolutions must not be published, and a retry may only happen
 * after a delay. The CSC counters live outside this module and keep their
 * values, so the host sees a gap no longer than the one that really happened.
 *
 * No Zephyr dependency, so the whole decision is unit-testable.
 */

#ifndef SPEED_METER_WHEEL_RECONFIG_H_
#define SPEED_METER_WHEEL_RECONFIG_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * The configuration fields the wheel source pushes into the sensor and the
 * detector. Plain integers on purpose: this module knows nothing about
 * wheel_config, the sensor API or the kernel.
 */
struct wheel_reconfig_keys {
	uint32_t odr_hz;
	uint32_t range_g;
	uint32_t circ_mm;
	uint32_t rpm_max;
};

struct wheel_reconfig {
	struct wheel_reconfig_keys applied;
	int64_t retry_at_ms;
	bool valid;
};

void wheel_reconfig_init(struct wheel_reconfig *rc);

/** True when the wanted keys differ from the applied ones. */
bool wheel_reconfig_needed(const struct wheel_reconfig *rc,
			   const struct wheel_reconfig_keys *wanted);

/** True when an attempt may be made now. */
bool wheel_reconfig_may_retry(const struct wheel_reconfig *rc, int64_t now_ms);

/** The attempt succeeded: the wanted keys are the applied ones. */
void wheel_reconfig_applied(struct wheel_reconfig *rc,
			    const struct wheel_reconfig_keys *wanted);

/** The attempt failed: hold everything back until the retry delay expires. */
void wheel_reconfig_failed(struct wheel_reconfig *rc, int64_t now_ms,
			   uint32_t retry_ms);

/**
 * @brief True only while the sensor and the detector are known to agree.
 *
 * Samples may be forwarded and revolutions published only when this is true.
 */
bool wheel_reconfig_ready(const struct wheel_reconfig *rc);

#endif /* SPEED_METER_WHEEL_RECONFIG_H_ */
