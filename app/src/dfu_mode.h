/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-demand firmware update (DFU) mode.
 *
 * The MCUmgr/SMP service is hidden by default (it is only registered on
 * demand, see CONFIG_MCUMGR_TRANSPORT_BT_DYNAMIC_SVC_REGISTRATION).
 * dfu_mode_enter() registers the service, switches advertising to
 * "Wheel DFU" and lets a phone upload a signed image to the second slot.
 */

#ifndef SPEED_METER_DFU_MODE_H_
#define SPEED_METER_DFU_MODE_H_

/**
 * @brief Prepare DFU support after bt_enable().
 *
 * Ensures the SMP service is not visible until dfu_mode_enter() is called.
 */
int dfu_mode_init(void);

/**
 * @brief Enter DFU mode: register SMP, advertise as "Wheel DFU".
 *
 * @retval 0 on success, -EALREADY if DFU mode is already active, negative
 *         error code otherwise.
 */
int dfu_mode_enter(void);

#endif /* SPEED_METER_DFU_MODE_H_ */
