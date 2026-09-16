/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bluetooth bring-up - see bt.h.
 */

#include "bt.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "led_sw_blink.h"

#if IS_ENABLED(CONFIG_CSC_BATTERY)
#include "battery.h"
#endif

#if IS_ENABLED(CONFIG_MCUMGR_TRANSPORT_BT)
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#endif

LOG_MODULE_REGISTER(bt, LOG_LEVEL_INF);

/*
 * Advertising profile. The GAP "fast" profile (BT_LE_ADV_CONN_FAST_1) is
 * 30/60 ms, which is wasteful for a sensor that may advertise for hours when
 * nobody is around. 100/150 ms keeps discovery snappy while cutting the
 * average advertising current; the real saving is System OFF, see
 * wheel_power.c.
 */
#define BT_LE_ADV_WHEEL                                                                           \
	BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,                            \
			BT_GAP_ADV_FAST_INT_MAX_2, NULL)

/* Number of live links, so advertising can stop exactly at BT_MAX_CONN. */
static atomic_t conn_count;

/* Set while the device is on its way into System OFF: every advertising
 * request (including the one a disconnect would normally trigger) is ignored,
 * so a client cannot attach during the teardown window. */
static atomic_t adv_suppressed;

/* Blue LED signalling: 3 blinks on connect/pairing, fast blink while pairing. */
#define BT_LED_ON_MS  100
#define BT_LED_OFF_MS 100
#define BT_LED_BLINKS 3

static const struct led_dt_spec led_blue = LED_DT_SPEC_GET(DT_ALIAS(led2));

static struct led_sw_blink led_blue_blink;

static const struct bt_data advertising_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	/*
	 * The CSCS service UUID (0x1816) MUST be advertised - Apple Watch
	 * lists sensors found in Settings -> Bluetooth -> cycling sensors
	 * based on this UUID.
	 */
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_CSC_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
#if IS_ENABLED(CONFIG_MCUMGR_TRANSPORT_BT) && \
	!IS_ENABLED(CONFIG_MCUMGR_TRANSPORT_BT_DYNAMIC_SVC_REGISTRATION)
	/*
	 * MCUmgr/SMP service - remote diagnostics (tools/wheel_cal.py).
	 * With dynamic service registration the SMP service is hidden until
	 * DFU mode is entered, so it is not advertised here.
	 */
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, SMP_BT_SVC_UUID_VAL),
#endif
};

static const struct bt_data scan_response_data[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static struct k_work advertise_work;

static void advertise(struct k_work *work)
{
	ARG_UNUSED(work);

	int err;

	if (atomic_get(&adv_suppressed) != 0) {
		return;
	}

	/*
	 * Advertising is connectable, so the controller stops it by itself when
	 * a link comes up. Re-evaluate on every event: advertise while there is
	 * a free slot (so a second central can connect), stop once full.
	 */
	if (atomic_get(&conn_count) >= CONFIG_BT_MAX_CONN) {
		err = bt_le_adv_stop();
		if (err != 0 && err != -EALREADY) {
			LOG_WRN("Advertising stop failed: %d", err);
		}
		return;
	}

	err = bt_le_adv_start(BT_LE_ADV_WHEEL, advertising_data,
			      ARRAY_SIZE(advertising_data), scan_response_data,
			      ARRAY_SIZE(scan_response_data));
	if (err != 0 && err != -EALREADY) {
		LOG_ERR("Advertising failed: %d", err);
	}
}

void bt_start_advertising(void)
{
	k_work_submit(&advertise_work);
}

static void disconnect_one(struct bt_conn *conn, void *data)
{
	ARG_UNUSED(data);

	/* BT_HCI_ERR_REMOTE_USER_TERM_CONN: a clean "user asked to stop", which
	 * clients handle better than a supervision timeout. Even if it does not
	 * get out before the radio dies, the peer simply times out and
	 * reconnects on the next wake.
	 */
	(void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

void bt_prepare_sleep(void)
{
	/* Suppress first: disconnecting would otherwise re-arm advertising. */
	(void)atomic_set(&adv_suppressed, 1);
	(void)bt_le_adv_stop();
	bt_conn_foreach(BT_CONN_TYPE_LE, disconnect_one, NULL);
}

void bt_resume(void)
{
	/*
	 * The mirror image, and the reason the flag is not simply set and
	 * forgotten: on a board without a wake line the radio is what goes to
	 * sleep, and this runs again on the next revolution with no reboot in
	 * between.
	 */
	(void)atomic_set(&adv_suppressed, 0);
	bt_start_advertising();
}

/* Finite blink: N cycles via the software blinker, then off. */
static inline void bt_led_blink_n(uint32_t cycles, uint32_t on_ms, uint32_t off_ms)
{
	(void)led_sw_blink_start(&led_blue_blink, on_ms, off_ms, cycles);
}

/* Continuous blink (0 cycles = until stopped), e.g. while pairing. */
static inline void bt_led_blink_forever(uint32_t on_ms, uint32_t off_ms)
{
	(void)led_sw_blink_start(&led_blue_blink, on_ms, off_ms, 0U);
}

static inline void bt_led_off(void)
{
	led_sw_blink_stop(&led_blue_blink);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);

	if (err != 0U) {
		LOG_WRN("Connection failed: 0x%02x %s", err, bt_hci_err_to_str(err));
		/* The slot never became usable - make sure we still advertise. */
		bt_start_advertising();
		return;
	}

	(void)atomic_inc(&conn_count);

	LOG_INF("Connected (%ld/%d)", (long)atomic_get(&conn_count),
		CONFIG_BT_MAX_CONN);
	bt_led_blink_n(BT_LED_BLINKS, BT_LED_ON_MS, BT_LED_OFF_MS);

	/*
	 * Pairing and service discovery are the heaviest radio burst the device
	 * ever runs, and a coin cell sags under that load. Re-measure shortly
	 * afterwards so the Battery Service reflects the real, loaded voltage
	 * rather than the relaxed reading taken while idle.
	 */
#if IS_ENABLED(CONFIG_CSC_BATTERY)
	battery_request_update();
#endif

	/*
	 * Keep advertising while a slot is free: this is what lets the Apple
	 * Watch and a bike computer / phone display stay connected at the same
	 * time. bt_gatt_notify(NULL, ...) feeds them all (see csc.c).
	 */
	bt_start_advertising();
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);

	LOG_INF("Disconnected: 0x%02x %s", reason, bt_hci_err_to_str(reason));
	(void)atomic_dec(&conn_count);
	bt_led_off();

	/* Auto-reconnect: advertise again so the watch/display can come back. */
	bt_start_advertising();
}

#if IS_ENABLED(CONFIG_BT_SMP)
static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(level);

	if (err != BT_SECURITY_ERR_SUCCESS) {
		LOG_WRN("Pairing failed: %d", err);
		bt_led_off();
		return;
	}

	/* Paired: quick 3-blink confirmation, then off. */
	bt_led_blink_n(BT_LED_BLINKS, BT_LED_ON_MS, BT_LED_OFF_MS);
}
#endif /* CONFIG_BT_SMP */

static void recycled(void)
{
	k_work_submit(&advertise_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
#if IS_ENABLED(CONFIG_BT_SMP)
	.security_changed = security_changed,
#endif
	.recycled = recycled,
};

#if IS_ENABLED(CONFIG_BT_SMP)
static void pairing_confirm(struct bt_conn *conn)
{
	ARG_UNUSED(conn);

	LOG_INF("Pairing requested");
	bt_led_blink_forever(BT_LED_ON_MS, BT_LED_OFF_MS);
}

static void pairing_cancel(struct bt_conn *conn)
{
	ARG_UNUSED(conn);

	LOG_INF("Pairing cancelled");
	bt_led_off();
}

static struct bt_conn_auth_cb auth_callbacks = {
	.pairing_confirm = pairing_confirm,
	.cancel = pairing_cancel,
};
#endif /* CONFIG_BT_SMP */

int bt_init(void)
{
	k_work_init(&advertise_work, advertise);
	(void)led_sw_blink_init(&led_blue_blink, &led_blue);

#if IS_ENABLED(CONFIG_BT_SMP)
	(void)bt_conn_auth_cb_register(&auth_callbacks);
#endif
	return 0;
}
