/*
 * Copyright (c) 2026 Mateusz Żerebecki
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cycling Speed and Cadence Service (UUID 0x1816) implementation.
 *
 * Characteristics:
 *   0x2A5B CSC Measurement   - Notify (wheel and/or crank data)
 *   0x2A5C CSC Feature       - Read
 *   0x2A5D Sensor Location   - Read
 *
 * With BT_GATT_CHARACTERISTIC each characteristic spans two attributes
 * (declaration + value), so the attribute layout of csc_svc is:
 *   [0] service declaration
 *   [1] CSC Measurement declaration   [2] value   [3] CCC descriptor
 *   [4] Sensor Location declaration   [5] value
 *   [6] CSC Feature declaration       [7] value
 */

#include "csc.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(csc, LOG_LEVEL_INF);

/* SC Control Point opcodes, responses and error codes (CSCS v1.0). */
#define SC_CP_OP_SET_CWR      0x01
#define SC_CP_OP_CALIBRATION  0x02
#define SC_CP_OP_UPDATE_LOC   0x03
#define SC_CP_OP_REQ_SUPP_LOC 0x04
#define SC_CP_OP_RESPONSE     0x10
#define SC_CP_RSP_SUCCESS     0x01
#define SC_CP_RSP_OP_NOT_SUPP 0x02
#define SC_CP_RSP_INVAL_PARAM 0x03
#define SC_CP_RSP_FAILED      0x04
#define CSC_ERR_IN_PROGRESS   0x80
#define CSC_ERR_CCC_CONFIG    0x81

static bool notifications_enabled;

/* Reported by the Sensor Location characteristic; may also be updated at
 * runtime through the SC Control Point (CONFIG_CSC_SC_CONTROL_POINT).
 */
static uint8_t sensor_location = CONFIG_CSC_SENSOR_LOCATION;

static void csc_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	notifications_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("CSC measurements %s", notifications_enabled ? "enabled" : "disabled");
}

static ssize_t read_sensor_location(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				    void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &sensor_location,
				 sizeof(sensor_location));
}

static ssize_t read_csc_feature(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
				uint16_t len, uint16_t offset)
{
	uint16_t feature = 0U;

	if (IS_ENABLED(CONFIG_CSC_WHEEL_REV_DATA)) {
		feature |= CSC_FEAT_WHEEL_REV_DATA;
	}

	if (IS_ENABLED(CONFIG_CSC_CRANK_REV_DATA)) {
		feature |= CSC_FEAT_CRANK_REV_DATA;
	}

	if (IS_ENABLED(CONFIG_CSC_SC_CONTROL_POINT)) {
		feature |= CSC_FEAT_MULTI_SENSORS;
	}

	feature = sys_cpu_to_le16(feature);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &feature, sizeof(feature));
}

#if IS_ENABLED(CONFIG_CSC_SC_CONTROL_POINT)
/* Sensor locations offered through the SC Control Point (Bluetooth SIG
 * values). Keep in sync with CONFIG_CSC_SENSOR_LOCATION / the Sensor
 * Location characteristic.
 */
static const uint8_t supported_locations[] = {
	0x00, /* Other */
	0x04, /* Front wheel */
	0x09, /* Front hub */
	0x0b, /* Chainstay */
	0x0c, /* Rear wheel */
	0x0d, /* Rear hub */
};

static bool ctrl_point_configured;
static uint32_t last_reported_revs; /* Raw counter from the last publish. */
static uint32_t revs_offset;        /* Set by SET CUMULATIVE VALUE. */

static void ctrl_point_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	ctrl_point_configured = (value == BT_GATT_CCC_INDICATE);
}

struct write_sc_ctrl_point_req {
	uint8_t op;
	union {
		uint32_t cwr;
		uint8_t location;
	};
} __packed;

static void ctrl_point_ind(struct bt_conn *conn, uint8_t req_op, uint8_t status,
			   const void *data, uint16_t data_len);

static ssize_t write_ctrl_point(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				const void *buf, uint16_t len, uint16_t offset,
				uint8_t flags)
{
	const struct write_sc_ctrl_point_req *req = buf;
	uint8_t status;

	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (!ctrl_point_configured) {
		return BT_GATT_ERR(CSC_ERR_CCC_CONFIG);
	}

	if (len == 0U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (req->op) {
	case SC_CP_OP_SET_CWR:
		if (len != sizeof(req->op) + sizeof(req->cwr)) {
			status = SC_CP_RSP_INVAL_PARAM;
			break;
		}

		/* Adopt the requested cumulative value: reported = raw + offset. */
		revs_offset = sys_le32_to_cpu(req->cwr) - last_reported_revs;
		status = SC_CP_RSP_SUCCESS;
		break;
	case SC_CP_OP_UPDATE_LOC:
		if (len != sizeof(req->op) + sizeof(req->location)) {
			status = SC_CP_RSP_INVAL_PARAM;
			break;
		}

		if (req->location == sensor_location) {
			status = SC_CP_RSP_SUCCESS;
			break;
		}

		status = SC_CP_RSP_INVAL_PARAM;
		for (size_t i = 0U; i < ARRAY_SIZE(supported_locations); i++) {
			if (supported_locations[i] == req->location) {
				sensor_location = req->location;
				status = SC_CP_RSP_SUCCESS;
				break;
			}
		}
		break;
	case SC_CP_OP_REQ_SUPP_LOC:
		if (len != sizeof(req->op)) {
			status = SC_CP_RSP_INVAL_PARAM;
			break;
		}

		/* Respond with the full list right away. */
		ctrl_point_ind(conn, req->op, SC_CP_RSP_SUCCESS, supported_locations,
			       sizeof(supported_locations));
		return len;
	default:
		status = SC_CP_RSP_OP_NOT_SUPP;
		break;
	}

	ctrl_point_ind(conn, req->op, status, NULL, 0);

	return len;
}

#define CSC_SC_CONTROL_POINT_ATTRS                                             \
	,                                                                      \
	BT_GATT_CHARACTERISTIC(BT_UUID_SC_CONTROL_POINT,                       \
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_INDICATE,     \
			       BT_GATT_PERM_WRITE, NULL, write_ctrl_point,     \
			       NULL),                                          \
	BT_GATT_CCC(ctrl_point_ccc_cfg_changed,                                \
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)
#else
#define CSC_SC_CONTROL_POINT_ATTRS
#endif /* CONFIG_CSC_SC_CONTROL_POINT */

BT_GATT_SERVICE_DEFINE(csc_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_CSC),
	BT_GATT_CHARACTERISTIC(BT_UUID_CSC_MEASUREMENT, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(csc_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_SENSOR_LOCATION, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_sensor_location, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_CSC_FEATURE, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_csc_feature, NULL, NULL)
	CSC_SC_CONTROL_POINT_ATTRS);

#if IS_ENABLED(CONFIG_CSC_SC_CONTROL_POINT)
struct sc_ctrl_point_ind {
	uint8_t op;
	uint8_t req_op;
	uint8_t status;
	uint8_t data[];
} __packed;

static void ctrl_point_ind(struct bt_conn *conn, uint8_t req_op, uint8_t status,
			   const void *data, uint16_t data_len)
{
	uint8_t buf[sizeof(struct sc_ctrl_point_ind) + data_len];
	struct sc_ctrl_point_ind *ind = (void *)buf;

	ind->op = SC_CP_OP_RESPONSE;
	ind->req_op = req_op;
	ind->status = status;

	if (data != NULL && data_len > 0U) {
		memcpy(ind->data, data, data_len);
	}

	/* Same pattern as the upstream peripheral_csc sample: the SC Control
	 * Point response goes out as a notification on the characteristic
	 * (TODO: check whether an indication works better with strict clients).
	 */
	(void)bt_gatt_notify(conn, &csc_svc.attrs[8], buf, sizeof(buf));
}
#endif /* CONFIG_CSC_SC_CONTROL_POINT */

static void publish(const uint8_t *measurement, uint16_t length)
{
	int err;

	if (!notifications_enabled) {
		return;
	}

	err = bt_gatt_notify(NULL, &csc_svc.attrs[2], measurement, length);
	if (err != 0 && err != -ENOTCONN) {
		LOG_WRN("CSC notification failed: %d", err);
	}
}

void csc_publish_wheel(uint32_t cumulative_revolutions, uint16_t last_event_time_1024)
{
	uint8_t measurement[1U + 6U]; /* flags + (uint32 revs + uint16 time) */

	if (!IS_ENABLED(CONFIG_CSC_WHEEL_REV_DATA)) {
		return;
	}

#if IS_ENABLED(CONFIG_CSC_SC_CONTROL_POINT)
	/* Track the raw counter and apply the offset requested through the
	 * SC Control Point (SET CUMULATIVE VALUE).
	 */
	last_reported_revs = cumulative_revolutions;
	cumulative_revolutions += revs_offset;
#endif

	measurement[0] = CSC_MEAS_FLAG_WHEEL_REV_DATA;
	sys_put_le32(cumulative_revolutions, &measurement[1]);
	sys_put_le16(last_event_time_1024, &measurement[5]);

	publish(measurement, sizeof(measurement));
}

void csc_publish_crank(uint16_t cumulative_revolutions, uint16_t last_event_time_1024)
{
	uint8_t measurement[1U + 4U]; /* flags + (uint16 revs + uint16 time) */

	if (!IS_ENABLED(CONFIG_CSC_CRANK_REV_DATA)) {
		return;
	}

	measurement[0] = CSC_MEAS_FLAG_CRANK_REV_DATA;
	sys_put_le16(cumulative_revolutions, &measurement[1]);
	sys_put_le16(last_event_time_1024, &measurement[3]);

	publish(measurement, sizeof(measurement));
}

uint16_t csc_event_time_now(void)
{
	/* 1/1024 s units; wraps every 64 s, which is expected by receivers. */
	return (uint16_t)((k_uptime_get() * 1024U) / 1000U);
}

bool csc_notifications_enabled(void)
{
	return notifications_enabled;
}
