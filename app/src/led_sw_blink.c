/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software LED blink - see led_sw_blink.h.
 *
 * Each work item turns the LED on (or off) and reschedules itself for the next
 * half period, so nothing ever sleeps and the workqueue stays responsive.
 */

#include "led_sw_blink.h"

#include <errno.h>

static void half_period(struct k_work *work);

static void schedule(struct led_sw_blink *ctx, uint32_t delay_ms)
{
	(void)k_work_reschedule(&ctx->work, K_MSEC(delay_ms));
}

static void half_period(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct led_sw_blink *ctx = CONTAINER_OF(dwork, struct led_sw_blink, work);

	if (ctx->remaining != 0U) {
		ctx->remaining--;
		if (ctx->remaining == 0U) {
			/* Last half period done: always end with the LED off. */
			(void)led_off_dt(ctx->spec);
			ctx->lit = false;
			return;
		}
	}

	ctx->lit = !ctx->lit;
	if (ctx->lit) {
		(void)led_on_dt(ctx->spec);
		schedule(ctx, ctx->on_ms);
	} else {
		(void)led_off_dt(ctx->spec);
		schedule(ctx, ctx->off_ms);
	}
}

int led_sw_blink_init(struct led_sw_blink *ctx, const struct led_dt_spec *spec)
{
	if (ctx == NULL || spec == NULL) {
		return -EINVAL;
	}

	ctx->spec = spec;
	ctx->on_ms = 0U;
	ctx->off_ms = 0U;
	ctx->remaining = 0U;
	ctx->lit = false;
	k_work_init_delayable(&ctx->work, half_period);

	return 0;
}

int led_sw_blink_start(struct led_sw_blink *ctx, uint32_t on_ms, uint32_t off_ms,
		       uint32_t cycles)
{
	if (ctx == NULL || ctx->spec == NULL) {
		return -EINVAL;
	}
	if (on_ms == 0U || off_ms == 0U) {
		return -EINVAL;
	}
	if (!led_is_ready_dt(ctx->spec)) {
		return -ENODEV;
	}

	/*
	 * Cancel any pending half period first: without this the old cadence
	 * would fire once more and could leave the LED in the wrong state.
	 */
	(void)k_work_cancel_delayable(&ctx->work);

	ctx->on_ms = on_ms;
	ctx->off_ms = off_ms;
	/* Two half periods per cycle; 0 stays 0, meaning "until stopped". */
	ctx->remaining = (cycles == 0U) ? 0U : cycles * 2U;
	ctx->lit = false;

	(void)led_off_dt(ctx->spec);
	schedule(ctx, on_ms);

	return 0;
}

void led_sw_blink_stop(struct led_sw_blink *ctx)
{
	if (ctx == NULL || ctx->spec == NULL) {
		return;
	}

	(void)k_work_cancel_delayable(&ctx->work);
	ctx->remaining = 0U;
	ctx->lit = false;
	(void)led_off_dt(ctx->spec);
}

bool led_sw_blink_is_running(const struct led_sw_blink *ctx)
{
	if (ctx == NULL) {
		return false;
	}

	/* k_work_delayable_is_pending() also covers the "queued" state. */
	return k_work_delayable_is_pending((struct k_work_delayable *)&ctx->work);
}
