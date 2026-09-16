/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A controllable accelerometer for the wheel source integration test.
 *
 * The point is not to emulate the LIS2DH12 - the driver already has its own
 * emulator tests. It is to be able to say "the ODR took effect but the full
 * scale was refused", or "there is no new sample yet", and then watch what the
 * wheel source does with that.
 */

#define DT_DRV_COMPAT vnd_fake_accel

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include "fake_accel.h"

static struct {
	bool fail_frequency;
	bool fail_full_scale;
	bool no_data;
	int fetch_err;
	float x;
	float y;
	float z;
	unsigned int attr_set_calls;
	unsigned int frequency_sets;
	unsigned int full_scale_sets;
} fake;

void fake_accel_fail_frequency(bool fail)
{
	fake.fail_frequency = fail;
}

void fake_accel_fail_full_scale(bool fail)
{
	fake.fail_full_scale = fail;
}

void fake_accel_set_sample(float x, float y, float z)
{
	fake.x = x;
	fake.y = y;
	fake.z = z;
}

void fake_accel_report_no_data(bool no_data)
{
	fake.no_data = no_data;
}

void fake_accel_fail_fetch(int err)
{
	fake.fetch_err = err;
}

unsigned int fake_accel_attr_set_calls(void)
{
	return fake.attr_set_calls;
}

unsigned int fake_accel_frequency_sets(void)
{
	return fake.frequency_sets;
}

unsigned int fake_accel_full_scale_sets(void)
{
	return fake.full_scale_sets;
}

void fake_accel_reset_counters(void)
{
	fake.attr_set_calls = 0;
	fake.frequency_sets = 0;
	fake.full_scale_sets = 0;
}

static int fake_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static int fake_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	ARG_UNUSED(dev);

	if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_ACCEL_XYZ) {
		return -ENOTSUP;
	}
	if (fake.fetch_err != 0) {
		return fake.fetch_err;
	}
	if (fake.no_data) {
		return -ENODATA;
	}

	return 0;
}

static void set_value(struct sensor_value *val, float value)
{
	val->val1 = (int32_t)value;
	val->val2 = (int32_t)((value - (float)val->val1) * 1000000.0f);
}

static int fake_channel_get(const struct device *dev, enum sensor_channel chan,
			    struct sensor_value *val)
{
	ARG_UNUSED(dev);

	if (chan != SENSOR_CHAN_ACCEL_XYZ) {
		return -ENOTSUP;
	}

	set_value(&val[0], fake.x);
	set_value(&val[1], fake.y);
	set_value(&val[2], fake.z);

	return 0;
}

static int fake_attr_set(const struct device *dev, enum sensor_channel chan,
			 enum sensor_attribute attr,
			 const struct sensor_value *val)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);
	ARG_UNUSED(val);

	fake.attr_set_calls++;

	switch (attr) {
	case SENSOR_ATTR_SAMPLING_FREQUENCY:
		fake.frequency_sets++;
		return fake.fail_frequency ? -EIO : 0;
	case SENSOR_ATTR_FULL_SCALE:
		fake.full_scale_sets++;
		return fake.fail_full_scale ? -EIO : 0;
	default:
		return -ENOTSUP;
	}
}

/* DEVICE_API() is what puts the struct in the sensor API section the sensor
 * subsystem asserts against; a plain const struct would be rejected at
 * runtime. */
static DEVICE_API(sensor, fake_api) = {
	.sample_fetch = fake_sample_fetch,
	.channel_get = fake_channel_get,
	.attr_set = fake_attr_set,
};

#define FAKE_ACCEL_INST(inst)						\
	SENSOR_DEVICE_DT_INST_DEFINE(inst, fake_init, NULL, NULL, NULL,	\
				     POST_KERNEL,			\
				     CONFIG_SENSOR_INIT_PRIORITY,	\
				     &fake_api);

DT_INST_FOREACH_STATUS_OKAY(FAKE_ACCEL_INST)
