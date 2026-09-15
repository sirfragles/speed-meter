/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Boot-time self-test and firmware confirmation (MCUboot).
 *
 * After a DFU reboot the new image is confirmed only if the self-test passes;
 * otherwise MCUboot rolls it back on the next reboot. The result is signalled
 * on the LEDs: green = confirmed, blue/red alternating = failed.
 */

#ifndef SPEED_METER_BOOT_H_
#define SPEED_METER_BOOT_H_

/** @brief Run the self-test and confirm the running image. */
void boot_init(void);

#endif /* SPEED_METER_BOOT_H_ */
