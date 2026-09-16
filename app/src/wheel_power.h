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

/* Three-tier standstill policy, mirroring what the commercial speed sensors do:
 *
 *   tier 1  0 .. WHEEL_POWER_IDLE_TIMEOUT_S
 *           measuring normally, CSC published about once per second
 *   tier 2  WHEEL_POWER_IDLE_TIMEOUT_S .. WHEEL_POWER_DEEP_SLEEP_TIMEOUT_S
 *           sensor only: 1 Hz + movement interrupt. The BLE link is kept, so a
 *           stop at a traffic light does not drop the watch connection, and the
 *           CPU stays idle between connection events.
 *   tier 3  beyond WHEEL_POWER_DEEP_SLEEP_TIMEOUT_S
 *           the ride is over: links dropped, advertising stopped, and the
 *           device stays in System ON Idle - the CPU stopping in WFI between
 *           1 Hz polls while everything else keeps running. That poll is the
 *           wake source: the sampling thread notices the wheel turning and
 *           calls bt_resume(), so the wheel still starts the device by itself.
 *
 * There is deliberately no System OFF tier. Waking from it is GPIO DETECT on
 * P0/P1 or RESET, and this board's only accelerometer interrupts sit on port
 * P2, which carries no gpiote-instance in the SoC devicetree (gpiote30 belongs
 * to gpio0, gpiote20 to gpio1). Arming a pin nothing can pull would power the
 * device down for good. See wheel_power_soc_suspend() for what a wake source
 * would take.
 *
 * "System ON Idle" is a description, not a switch. On nRF54L15 it is what
 * arch_cpu_idle() does; the SoC selects no HAS_PM and implements no
 * pm_state_set(), so CONFIG_PM=y does not take - Kconfig says so, the build
 * succeeds anyway, and the only related knob is CONFIG_SOC_NRF_FORCE_CONSTLAT
 * (off, because it trades power for wake latency).
 *
 * Tier 3 is what makes a CR2032 last a season: a connected client would
 * otherwise keep the device awake around the clock.
 */
#define WHEEL_POWER_IDLE_TIMEOUT_S CONFIG_CSC_POWER_IDLE_TIMEOUT_S

/*
 * Not conditional on anything. It used to be guarded on the System OFF option,
 * substituting INT32_MAX under a comment saying "without System OFF there is no
 * tier 3" - which stopped being true once the radio-off path existed.
 */
#define WHEEL_POWER_DEEP_SLEEP_TIMEOUT_S CONFIG_CSC_POWER_DEEP_SLEEP_TIMEOUT_S

/* Initialise the module and put the sensor into the active (measuring) mode. */
int wheel_power_init(void);

/* Feed the detector's rotating flag once per sample; manages active/standby. */
void wheel_power_report(bool rotating);

/* Force a state (used by the shell and tests). */
int wheel_power_set_active(void);
int wheel_power_set_standby(void);
bool wheel_power_is_standby(void);

/* True when the last reset came from a System OFF wake via GPIO DETECT. Always
 * false on HOLYIOT-25008, which has no System OFF tier - kept because it reads
 * a real hardware register and is correct either way. */
bool wheel_power_woke_from_off(void);

/* True when the LIS2DH12 answers WHO_AM_I over SPI (minimal self-test). */
bool wheel_power_selftest(void);

/* Platform hooks: put the device into its low-power state and bring it back.
 * Weak defaults do nothing; override them in the board support layer.
 *
 * On HOLYIOT-25008 the default is the real implementation: the radio is what
 * sleeps, and the 1 Hz wheel poll is the wake source. There is no System OFF
 * because nothing on P0/P1 can report the wheel (see the tier-3 note above). */
void wheel_power_soc_suspend(void);
void wheel_power_soc_resume(void);

#endif /* SPEED_METER_WHEEL_POWER_H_ */