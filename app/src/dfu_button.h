/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DFU entry trigger: a long press of the board button (sw0, P1.13) enters
 * DFU mode via dfu_mode_enter().
 */

#ifndef SPEED_METER_DFU_BUTTON_H_
#define SPEED_METER_DFU_BUTTON_H_

/** @brief Start the long-press detection thread. */
int dfu_button_init(void);

#endif /* SPEED_METER_DFU_BUTTON_H_ */
