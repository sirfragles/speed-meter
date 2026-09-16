/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Low-power management - see wheel_power.h for the state machine.
 *
 * The accelerometer is driven register-by-register over SPI (same bus and mode
 * as the rest of the project), so the Zephyr sensor driver is not involved in
 * the sleep/wake transitions.
 */

#include "wheel_power.h"
#include "bt.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>

#include <hal/nrf_gpio.h>
#include <helpers/nrfx_reset_reason.h>

#include <stdint.h>

LOG_MODULE_REGISTER(wheel_power, LOG_LEVEL_INF);

#define SENSOR_NODE DT_NODELABEL(lis2dh)

static const struct spi_dt_spec lis_spi =
	SPI_DT_SPEC_GET(SENSOR_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB);

/* LIS2DH12 registers used here. */
#define REG_CTRL1     0x20
#define REG_CTRL3     0x22
#define REG_CTRL4     0x23
#define REG_WHO_AM_I  0x0F
#define REG_INT1_CFG  0x30
#define REG_INT1_THS  0x32
#define REG_INT1_DUR  0x33
#define REG_INT2_CFG  0x34
#define REG_INT2_THS  0x36
#define REG_INT2_DUR  0x37

#define WHO_AM_I_LIS2DH12 0x33

/* Active measurement: 100 Hz, high resolution, all axes, +/-16 g, BDU. */
#define ACT_CTRL1 0x57
#define ACT_CTRL4 0xB8

/* Standby: 1 Hz, low-power, +/-2 g, the selected INT line armed on any-axis
 * movement. The wake threshold is in +/-2 g digits (16 mg each). */
#define STBY_CTRL1   0x1F
#define STBY_CTRL4   0x80
#define STBY_INT_CFG 0x3F /* XLIE..ZHIE: every axis, both polarities */
#define STBY_INT_DUR 0x00

/* Which LIS2DH12 interrupt line wakes the SoC (Kconfig choice). */
#if IS_ENABLED(CONFIG_CSC_POWER_WAKE_INT2)
#define WAKE_INT_IDX   1
#define WAKE_INT_CFG   REG_INT2_CFG
#define WAKE_INT_THS   REG_INT2_THS
#define WAKE_INT_DUR   REG_INT2_DUR
#define WAKE_INT_ROUTE 0x20 /* I2_IA2 -> INT2 */
#else
#define WAKE_INT_IDX   0
#define WAKE_INT_CFG   REG_INT1_CFG
#define WAKE_INT_THS   REG_INT1_THS
#define WAKE_INT_DUR   REG_INT1_DUR
#define WAKE_INT_ROUTE 0x40 /* I1_IA1 -> INT1 */
#endif

/* SoC-side wake pin: the selected INT line from the devicetree `irq-gpios`. */
#define WAKE_GPIO_PORT \
	DT_PROP(DT_GPIO_CTLR_BY_IDX(SENSOR_NODE, irq_gpios, WAKE_INT_IDX), port)
#define WAKE_GPIO_PIN \
	DT_GPIO_PIN_BY_IDX(SENSOR_NODE, irq_gpios, WAKE_INT_IDX)

static bool standby;
static bool inited;
static bool radio_off;
static int64_t last_activity_ms;

static int reg_write(uint8_t reg, uint8_t value)
{
	uint8_t tx[2] = { (uint8_t)(reg & 0x7F), value };
	const struct spi_buf buf = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf_set set = { .buffers = &buf, .count = 1 };

	return spi_write_dt(&lis_spi, &set);
}

static int reg_read(uint8_t reg, uint8_t *value)
{
	uint8_t tx[2] = { (uint8_t)(reg | 0x80), 0 };
	uint8_t rx = 0;
	const struct spi_buf tx_buf = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf rx_buf[2] = {
		{ .buf = NULL, .len = 1 },
		{ .buf = &rx, .len = 1 },
	};
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	const struct spi_buf_set rx_set = { .buffers = rx_buf, .count = 2 };
	int err = spi_transceive_dt(&lis_spi, &tx_set, &rx_set);

	if (err == 0) {
		*value = rx;
	}
	return err;
}

static int sensor_active(void)
{
	uint8_t v;
	int err;

	err = reg_write(REG_CTRL1, 0x00); /* power-down before reconfiguring */
	if (err != 0) {
		return err;
	}
	err = reg_write(REG_CTRL3, 0x00); /* no INT routing while measuring */
	if (err != 0) {
		return err;
	}
	err = reg_write(REG_CTRL4, ACT_CTRL4);
	if (err != 0) {
		return err;
	}
	err = reg_write(REG_CTRL1, ACT_CTRL1);
	if (err != 0) {
		return err;
	}
	err = reg_read(REG_CTRL4, &v);
	if (err != 0) {
		return err;
	}
	if (v != ACT_CTRL4) {
		LOG_WRN("power: CTRL4 readback 0x%02x != 0x%02x", v, ACT_CTRL4);
	}
	return 0;
}

static int sensor_standby(void)
{
	uint8_t v;
	int err;

	err = reg_write(REG_CTRL1, 0x00); /* power-down before reconfiguring */
	if (err != 0) {
		return err;
	}
	err = reg_write(REG_CTRL4, STBY_CTRL4);
	if (err != 0) {
		return err;
	}
	err = reg_write(WAKE_INT_THS, (uint8_t)CONFIG_CSC_POWER_WAKE_THS);
	if (err != 0) {
		return err;
	}
	err = reg_write(WAKE_INT_DUR, STBY_INT_DUR);
	if (err != 0) {
		return err;
	}
	err = reg_write(WAKE_INT_CFG, STBY_INT_CFG);
	if (err != 0) {
		return err;
	}
	err = reg_write(REG_CTRL3, WAKE_INT_ROUTE);
	if (err != 0) {
		return err;
	}
	err = reg_write(REG_CTRL1, STBY_CTRL1);
	if (err != 0) {
		return err;
	}
	err = reg_read(REG_CTRL4, &v);
	if (err != 0) {
		return err;
	}
	if (v != STBY_CTRL4) {
		LOG_WRN("power: CTRL4 readback 0x%02x != 0x%02x", v, STBY_CTRL4);
	}
	return 0;
}

int wheel_power_init(void)
{
	if (inited) {
		return 0;
	}

	if (!spi_is_ready_dt(&lis_spi)) {
		LOG_ERR("power: SPI not ready");
		return -ENODEV;
	}

	inited = true;
	last_activity_ms = k_uptime_get();
	return wheel_power_set_active();
}

void wheel_power_report(bool rotating)
{
	int64_t idle_ms;

	if (rotating) {
		last_activity_ms = k_uptime_get();

		if (standby) {
			(void)wheel_power_set_active();
		}

		/*
		 * Only when tier 3 actually ran. On a board without a wake line the
		 * chip survived it - there was no reboot to reset anything - so the
		 * radio has to be brought back by hand. That is the whole "wake" on
		 * such a board, and the 1 Hz poll is what triggers it.
		 */
		if (radio_off) {
			radio_off = false;
			wheel_power_soc_resume();
		}

		return;
	}

	idle_ms = k_uptime_get() - last_activity_ms;

	/*
	 * Tier 2 - standstill. Only the sensor goes to sleep: 1 Hz with a
	 * movement interrupt. The BLE link stays up, because stopping at a
	 * traffic light must not cost the rider the watch connection, and the
	 * CPU stays idle between connection events - which is where the real
	 * saving is anyway.
	 */
	if (!standby && idle_ms >= (int64_t)WHEEL_POWER_IDLE_TIMEOUT_S * 1000) {
		if (wheel_power_set_standby() != 0) {
			return;
		}
		LOG_INF("Wheel still for %d s - sensor standby, link kept",
			WHEEL_POWER_IDLE_TIMEOUT_S);
	}

	/*
	 * Tier 3 - the ride is over. Waiting this long means the bike is parked,
	 * not waiting at a light: advertising stops and every link is dropped.
	 *
	 * This is what makes a CR2032 last. A client that stays connected keeps
	 * the radio busy around the clock, and the radio is by far the largest
	 * load on the cell - far more than the core.
	 *
	 * What "sleeping" then means depends on whether this board has a wake
	 * line, and wheel_power_soc_suspend() is where that difference lives:
	 * System OFF where INT1 is wired, radio off where it is not. The latch is
	 * for the second case - the chip keeps running there, so without it this
	 * would fire again on every pass through wheel_power_report().
	 */
	if (standby && !radio_off &&
	    idle_ms >= (int64_t)WHEEL_POWER_DEEP_SLEEP_TIMEOUT_S * 1000) {
		radio_off = true;
		LOG_INF("Wheel still for %d s - ride over",
			WHEEL_POWER_DEEP_SLEEP_TIMEOUT_S);
		wheel_power_soc_suspend();
	}
}

int wheel_power_set_active(void)
{
	int err = sensor_active();

	if (err == 0) {
		standby = false;
	}
	last_activity_ms = k_uptime_get();
	return err;
}

int wheel_power_set_standby(void)
{
	int err = sensor_standby();

	if (err == 0) {
		standby = true;
	}
	return err;
}

bool wheel_power_is_standby(void)
{
	return standby;
}

bool wheel_power_woke_from_off(void)
{
	/* NRFX_RESET_REASON_OFF_MASK: wake from System OFF via GPIO DETECT. */
	return (nrfx_reset_reason_get() & NRFX_RESET_REASON_OFF_MASK) != 0U;
}

bool wheel_power_selftest(void)
{
	uint8_t who = 0U;

	/* WHO_AM_I (0x0F) is readable even in power-down mode, so this only
	 * needs the SPI bus to be ready - it proves the LIS2DH12 is present
	 * and reachable over SPI. */
	if (!spi_is_ready_dt(&lis_spi)) {
		return false;
	}
	if (reg_read(REG_WHO_AM_I, &who) != 0) {
		return false;
	}
	return who == WHO_AM_I_LIS2DH12;
}

__weak void wheel_power_soc_suspend(void)
{
#if IS_ENABLED(CONFIG_CSC_POWER_SYSTEM_OFF)
	uint32_t pin = NRF_GPIO_PIN_MAP(WAKE_GPIO_PORT, WAKE_GPIO_PIN);

	/*
	 * Take the radio down first: the wake is a reboot, so there is no state
	 * worth preserving, and a clean disconnect tells the watch immediately
	 * instead of leaving it to a supervision timeout.
	 */
	bt_prepare_sleep();

	/* Arm the wake pin, then power off; the INT event reboots the chip. */
	nrf_gpio_cfg_sense_input(pin, NRF_GPIO_PIN_PULLDOWN,
				 NRF_GPIO_PIN_SENSE_HIGH);
	nrfx_reset_reason_clear(UINT32_MAX);
	LOG_INF("power: System OFF, wake on %s at P%d.%02d",
		IS_ENABLED(CONFIG_CSC_POWER_WAKE_INT2) ? "INT2" : "INT1",
		(int)WAKE_GPIO_PORT, (int)WAKE_GPIO_PIN);
	sys_poweroff();
#else
	/*
	 * No wake line on this board, so System OFF would be a one-way door:
	 * this part has no timer wake, and the accelerometer's INT1/INT2 sit on
	 * port P2, which carries no GPIOTE instance on nRF54L15 (the SoC
	 * devicetree gives gpiote30 to gpio0 and gpiote20 to gpio1; gpio2 has
	 * no gpiote-instance property at all). Arming that pin and calling
	 * sys_poweroff() would leave a board that never answers again.
	 *
	 * So the device stays in System ON Idle and the radio is what sleeps - and
	 * the radio is also the part that costs the most. Advertising every 100 ms
	 * plus a maintained link runs to tens of microamps, while the core stopping
	 * in WFI between two 1 Hz polls costs a few. The sampling thread keeps
	 * running at STANDBY_POLL_US through all of this, and noticing the wheel
	 * turn there is what brings the radio back. That poll, not a hardware
	 * event, is the wake source.
	 */
	bt_prepare_sleep();
	LOG_INF("power: radio off, System ON idle, wheel polled at 1 Hz");
#endif
}

__weak void wheel_power_soc_resume(void)
{
#if IS_ENABLED(CONFIG_CSC_POWER_SYSTEM_OFF)
	/* Wake from System OFF is a reboot - nothing to resume here. */
	LOG_DBG("SoC resume (no-op: wake is a reboot)");
#else
	/*
	 * The counterpart to the radio-off path above, and the reason it is not
	 * a one-way door: the chip never stopped, so the radio has to be
	 * started again explicitly.
	 */
	LOG_INF("power: radio on - wheel turning again");
	bt_resume();
#endif
}