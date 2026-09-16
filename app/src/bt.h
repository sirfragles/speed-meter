/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bluetooth bring-up: advertising and connection/pairing callbacks.
 * Signals the connection state on the blue LED (led2) via the Zephyr LED
 * driver; every signal ends with the LED off to save battery.
 */

#ifndef SPEED_METER_BT_H_
#define SPEED_METER_BT_H_

/** @brief Prepare advertising and register pairing callbacks (after bt_enable). */
int bt_init(void);

/** @brief Start normal (CSCS) advertising. */
void bt_start_advertising(void);

/**
 * @brief Stop advertising and drop every link, ahead of System OFF.
 *
 * A wake from System OFF is a reboot, so there is nothing to preserve: the
 * client reconnects by itself once the device is back and advertising again.
 * Advertising stays suppressed until then, so nothing re-attaches during the
 * teardown window.
 */
void bt_prepare_sleep(void);

/**
 * @brief Undo bt_prepare_sleep(): advertise again.
 *
 * Needed by boards where System OFF is not used and the radio is what sleeps
 * instead - there the device comes back without a reboot, so something has to
 * lift the suppression. Until this existed the flag was a one-way door, which
 * was fine only because its single caller was followed by sys_poweroff().
 *
 * Safe to call when already advertising: bt_le_adv_start() returns -EALREADY
 * and that is handled.
 */
void bt_resume(void);

#endif /* SPEED_METER_BT_H_ */
