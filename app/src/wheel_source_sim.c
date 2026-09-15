/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Simulated wheel (and crank) revolutions - test source for the CSCS service,
 * enabled with CONFIG_CSC_SIMULATE. Publishes fake revolutions so the sensor
 * can be paired and tested before the real wheel sensor hardware is attached.
 */

#include "csc.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wheel_source_sim, LOG_LEVEL_INF);

static void simulation_thread(void *arg1, void *arg2, void *arg3)
{
	uint64_t elapsed_us = 0U;
	uint32_t wheel_revolutions = 0U;
	uint16_t wheel_time = 0U;
	const uint32_t wheel_period_us = 1000000U / CONFIG_CSC_SIM_WHEEL_RPS;
	uint32_t next_wheel_us = wheel_period_us;
#if IS_ENABLED(CONFIG_CSC_SIMULATE_CRANK)
	uint32_t crank_revolutions = 0U;
	uint16_t crank_time = 0U;
	const uint32_t crank_period_us = 60000000U / CONFIG_CSC_SIM_CRANK_RPM;
	uint32_t next_crank_us = crank_period_us;
#endif

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	LOG_INF("Wheel simulation: %d rev/s", CONFIG_CSC_SIM_WHEEL_RPS);
#if IS_ENABLED(CONFIG_CSC_SIMULATE_CRANK)
	LOG_INF("Crank simulation: %d rpm", CONFIG_CSC_SIM_CRANK_RPM);
#endif

	for (;;) {
		bool wheel_event = false;
#if IS_ENABLED(CONFIG_CSC_SIMULATE_CRANK)
		bool crank_event = false;
#endif

		k_sleep(K_MSEC(CONFIG_CSC_SIM_TICK_MS));
		elapsed_us += 1000U * (uint64_t)CONFIG_CSC_SIM_TICK_MS;

		while (elapsed_us >= next_wheel_us) {
			wheel_revolutions++;
			/* 1024 units per revolution keeps the reported event
			 * time in sync with the simulated speed. The uint16
			 * wraps every 64 s, exactly as the CSCS spec expects.
			 */
			wheel_time += 1024U / CONFIG_CSC_SIM_WHEEL_RPS;
			next_wheel_us += wheel_period_us;
			wheel_event = true;
		}

		if (wheel_event) {
			csc_publish_wheel(wheel_revolutions, wheel_time);
		}

#if IS_ENABLED(CONFIG_CSC_SIMULATE_CRANK)
		while (elapsed_us >= next_crank_us) {
			crank_revolutions++;
			crank_time += 61440U / CONFIG_CSC_SIM_CRANK_RPM;
			next_crank_us += crank_period_us;
			crank_event = true;
		}

		if (crank_event) {
			csc_publish_crank((uint16_t)crank_revolutions, crank_time);
		}
#endif
	}
}

K_THREAD_DEFINE(simulation_thread_id, 1536, simulation_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(7), 0, 0);
