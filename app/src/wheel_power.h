/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Low-power management: when the wheel is still, the LIS2DH12 drops to a
 * 1 Hz low-power mode with a movement interrupt armed on the selected INT
 * line (INT1 or INT2, see CONFIG_CSC_POWER_WAKE_INT*), so the SoC can sleep
 * and wake on rotation. Enable with CONFIG_CSC_POWER_SAVE.
 *
 * NOTE (HOLYIOT-25008): the stock wiring puts INT1 on P2.00 and INT2 on
 * P2.03. nRF54L15 GPIO port 2 has neither GPIOTE nor SENSE/DETECT, so those
 * pins are polling-only and CANNOT wake the SoC. For event-driven wake, INT1
 * is additionally wired to P1.05 (a P1 pin with SENSE/DETECT); the wake pin is
 * read from the `irq-gpios` entry selected by CONFIG_CSC_POWER_WAKE_INT*.
 * P2.00 is left as an unused high-Z input. P1.05 is UART RX, so even a
 * bootloader that enables UART keeps it as an input and cannot fight the INT1
 * push-pull output.
 */

#ifndef SPEED_METER_WHEEL_POWER_H_
#define SPEED_METER_WHEEL_POWER_H_

#include <stdbool.h>
#include <stdint.h>

/* IS_ENABLED() below, for the System OFF guard. */
#include <zephyr/sys/util_macro.h>

/* Three-tier standstill policy, mirroring what the commercial speed sensors do:
 *
 *   tier 1  0 .. WHEEL_POWER_IDLE_TIMEOUT_S
 *           measuring normally, CSC published about once per second
 *   tier 2  WHEEL_POWER_IDLE_TIMEOUT_S .. WHEEL_POWER_DEEP_SLEEP_TIMEOUT_S
 *           sensor only: 1 Hz + movement interrupt. The BLE link is kept, so a
 *           stop at a traffic light does not drop the watch connection, and the
 *           CPU stays idle between connection events.
 *   tier 3  beyond WHEEL_POWER_DEEP_SLEEP_TIMEOUT_S
 *           the ride is over: links dropped, SoC into System OFF. The next
 *           wheel movement wakes the chip and the client reconnects by itself.
 *
 * Tier 3 is what makes a CR2032 last a season: a connected client would
 * otherwise keep the device awake around the clock.
 */
#define WHEEL_POWER_IDLE_TIMEOUT_S CONFIG_CSC_POWER_IDLE_TIMEOUT_S

#if IS_ENABLED(CONFIG_CSC_POWER_SYSTEM_OFF)
#define WHEEL_POWER_DEEP_SLEEP_TIMEOUT_S CONFIG_CSC_POWER_DEEP_SLEEP_TIMEOUT_S
#else
/* Without System OFF there is no tier 3. Keep it finite so that
 * (int64_t)WHEEL_POWER_DEEP_SLEEP_TIMEOUT_S * 1000 cannot overflow.
 */
#define WHEEL_POWER_DEEP_SLEEP_TIMEOUT_S INT32_MAX
#endif

/* Initialise the module and put the sensor into the active (measuring) mode. */
int wheel_power_init(void);

/* Feed the detector's rotating flag once per sample; manages active/standby. */
void wheel_power_report(bool rotating);

/* Force a state (used by the shell and tests). */
int wheel_power_set_active(void);
int wheel_power_set_standby(void);
bool wheel_power_is_standby(void);

/* True when the chip rebooted because of a System OFF wake via GPIO DETECT
 * (i.e. the wheel started moving while the device was off). */
bool wheel_power_woke_from_off(void);

/* True when the LIS2DH12 answers WHO_AM_I over SPI (minimal self-test). */
bool wheel_power_selftest(void);

/* Platform hooks: put the SoC into its low-power state and wake it back up.
 * Weak defaults do nothing; override them in the board support layer. On
 * HOLYIOT-25008 the INT1 line (P2.00) cannot wake the chip, so a real
 * implementation must either rework the board (INT1 on P0/P1) or use a
 * periodic timer wake that polls P2.00. */
void wheel_power_soc_suspend(void);
void wheel_power_soc_resume(void);

#endif /* SPEED_METER_WHEEL_POWER_H_ */