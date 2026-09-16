/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The wheel source's collaborators, reduced to what the test observes: the
 * configuration it reads, and the measurements it publishes.
 *
 * The real wheel_config.c persists through NVS and the real csc.c needs
 * Bluetooth; neither is what this test is about.
 */

#include "stubs.h"

#include "csc.h"
#include "wheel_config.h"

static struct wheel_config cfg = {
	.axis_a = 3, /* plane detected automatically */
	.circ_mm = 2100,
	.odr_hz = 100,
	.range_g = 16,
	.rpm_max = 1200,
	.rpm_min = 12,
};

const struct wheel_config *wheel_config_get(void)
{
	return &cfg;
}

void stub_config_set_odr(uint16_t hz)
{
	cfg.odr_hz = hz;
}

void stub_config_set_range(uint8_t g)
{
	cfg.range_g = g;
}

void stub_config_set_circumference(uint16_t mm)
{
	cfg.circ_mm = mm;
}

void stub_config_set_rpm_max(uint16_t rpm)
{
	cfg.rpm_max = rpm;
}

/* Published measurements, in order. */
#define PUBLISHED_MAX 256U

static struct {
	uint32_t revolutions;
	uint16_t event_time;
} published[PUBLISHED_MAX];
static unsigned int published_count;

void csc_publish_wheel(uint32_t cumulative_revolutions,
		       uint16_t last_event_time_1024)
{
	if (published_count < PUBLISHED_MAX) {
		published[published_count].revolutions = cumulative_revolutions;
		published[published_count].event_time = last_event_time_1024;
	}
	published_count++;
}

unsigned int stub_published_count(void)
{
	return published_count;
}

uint32_t stub_published_revolutions(unsigned int index)
{
	return index < published_count ? published[index].revolutions : 0U;
}

uint16_t stub_published_event_time(unsigned int index)
{
	return index < published_count ? published[index].event_time : 0U;
}

void stub_publish_reset(void)
{
	published_count = 0;
}
