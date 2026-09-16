/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPEED_METER_TEST_STUBS_H_
#define SPEED_METER_TEST_STUBS_H_

#include <stdint.h>

/* The configuration the source under test reads. */
void stub_config_set_odr(uint16_t hz);
void stub_config_set_range(uint8_t g);
void stub_config_set_circumference(uint16_t mm);
void stub_config_set_rpm_max(uint16_t rpm);

/* What the source published over CSCS. */
unsigned int stub_published_count(void);
uint32_t stub_published_revolutions(unsigned int index);
uint16_t stub_published_event_time(unsigned int index);
void stub_publish_reset(void);

#endif /* SPEED_METER_TEST_STUBS_H_ */
