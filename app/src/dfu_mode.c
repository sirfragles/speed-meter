/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-demand firmware update (DFU) mode over BLE SMP.
 *
 * With CONFIG_MCUMGR_TRANSPORT_BT_DYNAMIC_SVC_REGISTRATION the SMP service
 * (which carries the image-management group) is NOT auto-registered at boot,
 * so DFU is hidden. dfu_mode_enter() registers it and switches advertising to
 * "Wheel DFU"; a phone (nRF Connect Device Manager) then uploads a signed
 * image to slot 1 and MCUboot installs it on the next reboot.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#include <zephyr/logging/log.h>

#include "bt.h"
#include "dfu_mode.h"

LOG_MODULE_REGISTER(dfu_mode, LOG_LEVEL_INF);

static atomic_t dfu_enabled;

static const struct bt_data dfu_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, SMP_BT_SVC_UUID_VAL),
};

static const struct bt_data dfu_sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, "Wheel DFU", sizeof("Wheel DFU") - 1),
};

int dfu_mode_init(void)
{
	atomic_clear(&dfu_enabled);

	/*
	 * The SMP service is already unregistered with dynamic service
	 * registration, so this is a defensive no-op; ignore the result (it
	 * may legitimately report that the service is not registered).
	 */
	(void)smp_bt_unregister();

	return 0;
}

int dfu_mode_enter(void)
{
	int err;

	if (!atomic_cas(&dfu_enabled, 0, 1)) {
		return -EALREADY;
	}

	err = bt_le_adv_stop();
	if (err != 0 && err != -EALREADY) {
		LOG_ERR("Cannot stop normal advertising: %d", err);
		goto fail;
	}

	err = smp_bt_register();
	if (err != 0 && err != -EALREADY) {
		LOG_ERR("Cannot register SMP service: %d", err);
		goto restore_normal;
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, dfu_ad,
			      ARRAY_SIZE(dfu_ad), dfu_sd,
			      ARRAY_SIZE(dfu_sd));
	if (err != 0) {
		LOG_ERR("Cannot start DFU advertising: %d", err);
		(void)smp_bt_unregister();
		goto restore_normal;
	}

	LOG_INF("BLE DFU enabled");
	return 0;

restore_normal:
	bt_start_advertising();
fail:
	atomic_clear(&dfu_enabled);
	return err;
}
