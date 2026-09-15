// REFERENCE COPY (commented out) - spasoye/nrf52840_zephyr_CSC_sensor (MIT, see LICENSE-MIT.txt)
// Original file: ble_csc.c - https://github.com/spasoye/nrf52840_zephyr_CSC_sensor
// /* ble_csc.c - Bluetooth Cycling Speed and Cadence Service implementation */
// 
// /*
//  * SPDX-License-Identifier: Apache-2.0
//  */
// 
// #include "ble_csc.h"
// 
// #include <stdbool.h>
// #include <zephyr/types.h>
// #include <stddef.h>
// #include <string.h>
// #include <zephyr/random/random.h>
// #include <zephyr/sys/printk.h>
// #include <zephyr/sys/byteorder.h>
// 
// #include <zephyr/bluetooth/bluetooth.h>
// #include <zephyr/bluetooth/conn.h>
// #include <zephyr/bluetooth/uuid.h>
// #include <zephyr/bluetooth/gatt.h>
// 
// /* CSC Definitions */
// #define CSC_SUPPORTED_LOCATIONS		{ CSC_LOC_OTHER, \
//                       CSC_LOC_FRONT_WHEEL, \
//                       CSC_LOC_REAR_WHEEL, \
//                       CSC_LOC_LEFT_CRANK, \
//                       CSC_LOC_RIGHT_CRANK }
// #define CSC_FEATURE			(CSC_FEAT_WHEEL_REV | \
//                      CSC_FEAT_CRANK_REV | \
//                      CSC_FEAT_MULTI_SENSORS)
// 
// /* CSC Sensor Locations */
// #define CSC_LOC_OTHER           0x00
// #define CSC_LOC_TOP_OF_SHOE     0x01
// #define CSC_LOC_IN_SHOE         0x02
// #define CSC_LOC_HIP             0x03
// #define CSC_LOC_FRONT_WHEEL     0x04
// #define CSC_LOC_LEFT_CRANK      0x05
// #define CSC_LOC_RIGHT_CRANK     0x06
// #define CSC_LOC_LEFT_PEDAL      0x07
// #define CSC_LOC_RIGHT_PEDAL     0x08
// #define CSC_LOC_FRONT_HUB       0x09
// #define CSC_LOC_REAR_DROPOUT    0x0a
// #define CSC_LOC_CHAINSTAY       0x0b
// #define CSC_LOC_REAR_WHEEL      0x0c
// #define CSC_LOC_REAR_HUB        0x0d
// #define CSC_LOC_CHEST           0x0e
// 
// /* CSC Application error codes */
// #define CSC_ERR_IN_PROGRESS     0x80
// #define CSC_ERR_CCC_CONFIG      0x81
// 
// /* SC Control Point Opcodes */
// #define SC_CP_OP_SET_CWR        0x01
// #define SC_CP_OP_CALIBRATION    0x02
// #define SC_CP_OP_UPDATE_LOC     0x03
// #define SC_CP_OP_REQ_SUPP_LOC   0x04
// #define SC_CP_OP_RESPONSE       0x10
// 
// /* SC Control Point Response Values */
// #define SC_CP_RSP_SUCCESS       0x01
// #define SC_CP_RSP_OP_NOT_SUPP   0x02
// #define SC_CP_RSP_INVAL_PARAM   0x03
// #define SC_CP_RSP_FAILED        0x04
// 
// /* CSC Feature */
// #define CSC_FEAT_WHEEL_REV      BIT(0)
// #define CSC_FEAT_CRANK_REV      BIT(1)
// #define CSC_FEAT_MULTI_SENSORS      BIT(2)
// 
// /* CSC Measurement Flags */
// #define CSC_WHEEL_REV_DATA_PRESENT  BIT(0)
// #define CSC_CRANK_REV_DATA_PRESENT  BIT(1)
// 
// /* Static variables */
// static uint32_t c_wheel_revs; /* Cumulative Wheel Revolutions */
// static uint8_t supported_locations[] = CSC_SUPPORTED_LOCATIONS;
// static uint8_t sensor_location; /* Current Sensor Location */
// static bool csc_simulate;
// static bool ctrl_point_configured;
// 
// static uint16_t lwet; /* Last Wheel Event Time */
// static uint16_t ccr;  /* Cumulative Crank Revolutions */
// static uint16_t lcet; /* Last Crank Event Time */
// 
// /* Forward declarations */
// static void ctrl_point_ind(struct bt_conn *conn, uint8_t req_op, uint8_t status,
//                const void *data, uint16_t data_len);
// 
// /* CCC Configuration changed callbacks */
// static void csc_meas_ccc_cfg_changed(const struct bt_gatt_attr *attr,
//                      uint16_t value)
// {
//     csc_simulate = value == BT_GATT_CCC_NOTIFY;
// }
// 
// static void ctrl_point_ccc_cfg_changed(const struct bt_gatt_attr *attr,
//                        uint16_t value)
// {
//     ctrl_point_configured = value == BT_GATT_CCC_INDICATE;
// }
// 
// /* GATT read callbacks */
// static ssize_t read_location(struct bt_conn *conn,
//                  const struct bt_gatt_attr *attr, void *buf,
//                  uint16_t len, uint16_t offset)
// {
//     uint8_t *value = attr->user_data;
// 
//     return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
//                  sizeof(*value));
// }
// 
// static ssize_t read_csc_feature(struct bt_conn *conn,
//                 const struct bt_gatt_attr *attr, void *buf,
//                 uint16_t len, uint16_t offset)
// {
//     uint16_t csc_feature = CSC_FEATURE;
// 
//     return bt_gatt_attr_read(conn, attr, buf, len, offset,
//                  &csc_feature, sizeof(csc_feature));
// }
// 
// /* Control Point structures */
// struct write_sc_ctrl_point_req {
//     uint8_t op;
//     union {
//         uint32_t cwr;
//         uint8_t location;
//     };
// } __packed;
// 
// /* GATT write callbacks */
// static ssize_t write_ctrl_point(struct bt_conn *conn,
//                 const struct bt_gatt_attr *attr,
//                 const void *buf, uint16_t len, uint16_t offset,
//                 uint8_t flags)
// {
//     const struct write_sc_ctrl_point_req *req = buf;
//     uint8_t status;
//     int i;
// 
//     if (!ctrl_point_configured) {
//         return BT_GATT_ERR(CSC_ERR_CCC_CONFIG);
//     }
// 
//     if (!len) {
//         return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
//     }
// 
//     switch (req->op) {
//     case SC_CP_OP_SET_CWR:
//         if (len != sizeof(req->op) + sizeof(req->cwr)) {
//             status = SC_CP_RSP_INVAL_PARAM;
//             break;
//         }
// 
//         c_wheel_revs = sys_le32_to_cpu(req->cwr);
//         status = SC_CP_RSP_SUCCESS;
//         break;
//     case SC_CP_OP_UPDATE_LOC:
//         if (len != sizeof(req->op) + sizeof(req->location)) {
//             status = SC_CP_RSP_INVAL_PARAM;
//             break;
//         }
// 
//         /* Break if the requested location is the same as current one */
//         if (req->location == sensor_location) {
//             status = SC_CP_RSP_SUCCESS;
//             break;
//         }
// 
//         /* Pre-set status */
//         status = SC_CP_RSP_INVAL_PARAM;
// 
//         /* Check if requested location is supported */
//         for (i = 0; i < ARRAY_SIZE(supported_locations); i++) {
//             if (supported_locations[i] == req->location) {
//                 sensor_location = req->location;
//                 status = SC_CP_RSP_SUCCESS;
//                 break;
//             }
//         }
// 
//         break;
//     case SC_CP_OP_REQ_SUPP_LOC:
//         if (len != sizeof(req->op)) {
//             status = SC_CP_RSP_INVAL_PARAM;
//             break;
//         }
// 
//         /* Indicate supported locations and return */
//         ctrl_point_ind(conn, req->op, SC_CP_RSP_SUCCESS,
//                    &supported_locations,
//                    sizeof(supported_locations));
// 
//         return len;
//     default:
//         status = SC_CP_RSP_OP_NOT_SUPP;
//     }
// 
//     ctrl_point_ind(conn, req->op, status, NULL, 0);
// 
//     return len;
// }
// 
// /* GATT Service Definition */
// BT_GATT_SERVICE_DEFINE(csc_svc,
//     BT_GATT_PRIMARY_SERVICE(BT_UUID_CSC),
//     BT_GATT_CHARACTERISTIC(BT_UUID_CSC_MEASUREMENT, BT_GATT_CHRC_NOTIFY,
//                    0x00, NULL, NULL, NULL),
//     BT_GATT_CCC(csc_meas_ccc_cfg_changed,
//             BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
//     BT_GATT_CHARACTERISTIC(BT_UUID_SENSOR_LOCATION, BT_GATT_CHRC_READ,
//                    BT_GATT_PERM_READ, read_location, NULL,
//                    &sensor_location),
//     BT_GATT_CHARACTERISTIC(BT_UUID_CSC_FEATURE, BT_GATT_CHRC_READ,
//                    BT_GATT_PERM_READ, read_csc_feature, NULL, NULL),
//     BT_GATT_CHARACTERISTIC(BT_UUID_SC_CONTROL_POINT,
//                    BT_GATT_CHRC_WRITE | BT_GATT_CHRC_INDICATE,
//                    BT_GATT_PERM_WRITE, NULL, write_ctrl_point,
//                    &sensor_location),
//     BT_GATT_CCC(ctrl_point_ccc_cfg_changed,
//             BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
// );
// 
// /* Control Point Indication */
// struct sc_ctrl_point_ind {
//     uint8_t op;
//     uint8_t req_op;
//     uint8_t status;
//     uint8_t data[];
// } __packed;
// 
// static void ctrl_point_ind(struct bt_conn *conn, uint8_t req_op, uint8_t status,
//                const void *data, uint16_t data_len)
// {
//     struct sc_ctrl_point_ind *ind;
//     uint8_t buf[sizeof(*ind) + data_len];
// 
//     ind = (void *) buf;
//     ind->op = SC_CP_OP_RESPONSE;
//     ind->req_op = req_op;
//     ind->status = status;
// 
//     /* Send data (supported locations) if present */
//     if (data && data_len) {
//         memcpy(ind->data, data, data_len);
//     }
// 
//     bt_gatt_notify(conn, &csc_svc.attrs[8], buf, sizeof(buf));
// }
// 
// /* Measurement Notification */
// struct csc_measurement_nfy {
//     uint8_t flags;
//     uint8_t data[];
// } __packed;
// 
// struct wheel_rev_data_nfy {
//     uint32_t cwr;
//     uint16_t lwet;
// } __packed;
// 
// struct crank_rev_data_nfy {
//     uint16_t ccr;
//     uint16_t lcet;
// } __packed;
// 
// static void measurement_nfy(struct bt_conn *conn, uint32_t cwr, uint16_t lwet,
//                 uint16_t ccr, uint16_t lcet)
// {
//     struct csc_measurement_nfy *nfy;
//     uint8_t buf[sizeof(*nfy) +
//             (cwr ? sizeof(struct wheel_rev_data_nfy) : 0) +
//             (ccr ? sizeof(struct crank_rev_data_nfy) : 0)];
//     uint16_t len = 0U;
// 
//     nfy = (void *) buf;
//     nfy->flags = 0U;
// 
//     /* Send Wheel Revolution data is present */
//     if (cwr) {
//         struct wheel_rev_data_nfy data;
// 
//         nfy->flags |= CSC_WHEEL_REV_DATA_PRESENT;
//         data.cwr = sys_cpu_to_le32(cwr);
//         data.lwet = sys_cpu_to_le16(lwet);
// 
//         memcpy(nfy->data, &data, sizeof(data));
//         len += sizeof(data);
//     }
// 
//     /* Send Crank Revolution data is present */
//     if (ccr) {
//         struct crank_rev_data_nfy data;
// 
//         nfy->flags |= CSC_CRANK_REV_DATA_PRESENT;
//         data.ccr = sys_cpu_to_le16(ccr);
//         data.lcet = sys_cpu_to_le16(lcet);
// 
//         memcpy(nfy->data + len, &data, sizeof(data));
//     }
// 
//     bt_gatt_notify(NULL, &csc_svc.attrs[1], buf, sizeof(buf));
// }
// 
// /* Public API Implementation */
// 
// void ble_csc_init(void)
// {
//     /* Initialize default sensor location */
//     sensor_location = CSC_LOC_OTHER;
// 
//     printk("CSC service initialized\n");
// }
// 
// bool ble_csc_is_simulation_enabled(void)
// {
//     return csc_simulate;
// }
// 
// bool ble_csc_is_notify_enabled(void)
// {
//     return csc_simulate;  /* csc_simulate is set when CCC is enabled */
// }
// 
// void ble_csc_simulate(void)
// {
//     static uint8_t i;
//     uint8_t rnd = sys_rand8_get();
//     bool nfy_crank = false, nfy_wheel = false;
// 
//     /* Measurements don't have to be updated every second */
//     if (!(i % 2)) {
//         lwet += 1050 + rnd % 50;
//         c_wheel_revs += 2U;
//         nfy_wheel = true;
//     }
// 
//     if (!(i % 3)) {
//         lcet += 1000 + rnd % 50;
//         ccr += 1U;
//         nfy_crank = true;
//     }
// 
//     /*
//      * In typical applications, the CSC Measurement characteristic is
//      * notified approximately once per second. This interval may vary
//      * and is determined by the Server and not required to be configurable
//      * by the Client.
//      */
//     measurement_nfy(NULL, nfy_wheel ? c_wheel_revs : 0, nfy_wheel ? lwet : 0,
//             nfy_crank ? ccr : 0, nfy_crank ? lcet : 0);
// 
//     /*
//      * The Last Crank Event Time value and Last Wheel Event Time roll over
//      * every 64 seconds.
//      */
//     if (!(i % 64)) {
//         lcet = 0U;
//         lwet = 0U;
//         i = 0U;
//     }
// 
//     i++;
// }
// 
// void ble_csc_update_wheel(uint32_t wheel_revs, uint16_t last_wheel_event_time,
//                           bool has_new_wheel_data)
// {
//     if (!csc_simulate) {
//         /* No client subscribed to notifications */
//         return;
//     }
// 
//     /* Update internal state */
//     c_wheel_revs = wheel_revs;
//     lwet = last_wheel_event_time;
// 
//     /* Send notification only if we have new data */
//     if (has_new_wheel_data) {
//         measurement_nfy(NULL, c_wheel_revs, lwet, 0, 0);
//     }
// }
// 
// void ble_csc_update_crank(uint16_t crank_revs, uint16_t last_crank_event_time,
//                           bool has_new_crank_data)
// {
//     if (!csc_simulate) {
//         /* No client subscribed to notifications */
//         return;
//     }
// 
//     /* Update internal state */
//     ccr = crank_revs;
//     lcet = last_crank_event_time;
// 
//     /* Send notification only if we have new data */
//     if (has_new_crank_data) {
//         measurement_nfy(NULL, 0, 0, ccr, lcet);
//     }
// }
