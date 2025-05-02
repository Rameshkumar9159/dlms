/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    eol_testing_protocol.h
 * \brief   Protocol for End of Line (EOL) testing of DUT
 */
#ifndef EOL_TESTING_PROTOCOL_H_
#define EOL_TESTING_PROTOCOL_H_

#include <stdint.h>

#include "wms_app.h"
#include "wms_settings.h"
#include "hdlc.h"

//------------------------------------------------------------------------------
// Generic stuff
//------------------------------------------------------------------------------
#define MAX_DUT_PER_JIG 50
// Must be kept in sync with device ID length in DLMS app
#define METER_DEVICE_ID_MAX_LENGTH  16
// Must be kept in sync with DLMS app version length
#define DUT_APP_VERSION_MAX_LENGTH 10

typedef struct
{
    int8_t rssi;
    int8_t tx_pwr;
} rf_measurement_t;

// A DUT test summary
typedef struct __attribute__((packed))
{
    uint8_t device_id[METER_DEVICE_ID_MAX_LENGTH];
    uint8_t app_version[DUT_APP_VERSION_MAX_LENGTH];
    app_addr_t mac_address; // DUT Wirepas mesh address
    uint8_t serial_com_status; // Use test_status_e values
    uint8_t association_lvl_status;
    uint8_t rf_status; // Use test_status_e values
    rf_measurement_t rf_results_dut_rx;
    rf_measurement_t rf_results_dut_tx;
} dut_testing_summary_t;

//------------------------------------------------------------------------------
// RF packets
//------------------------------------------------------------------------------
typedef struct
{
    rf_measurement_t rf_meas;
    app_addr_t mac_address;
} eol_test_router_info_t;

typedef struct
{
    uint8_t device_id[METER_DEVICE_ID_MAX_LENGTH];
    uint8_t dlms_app_version[DUT_APP_VERSION_MAX_LENGTH];
    uint8_t nic_status_reason_bf; // Bitfield from NIC status packet
    uint8_t meter_connection_bf; // Bitfield from NIC status packet
    eol_test_router_info_t router_info;
} dut_packet_t;

typedef struct
{
    app_addr_t target_address;
} router_packet_t;

//------------------------------------------------------------------------------
// HDLC packets
//------------------------------------------------------------------------------
// EOL test Protocol Version used in the message header
#define METERING_TEST_PROTOCOL_VERSION 0

typedef enum
{
    MSG_ID_REQ_TEST_INIT,
    MSG_ID_RSP_TEST_INIT,
    MSG_ID_REQ_TEST_START,
    MSG_ID_RSP_TEST_START,
    MSG_ID_REQ_TEST_RESULT,
    MSG_ID_RSP_TEST_RESULT,
    MSG_ID_REQ_CONFIG_ROUTER = 240,
    MSG_ID_RSP_CONFIG_ROUTER = 241
} msg_id_e;

typedef enum
{
    TEST_REQ_STATUS_OK,
    TEST_REQ_STATUS_CONFIG_ERROR, // test router not configured properly
    TEST_REQ_STATUS_GENERIC_ERROR // any other error for now
} test_req_status_e;

typedef enum
{
    TEST_STATUS_SUCCESS,
    TEST_STATUS_UNTESTED,
    TEST_STATUS_FAILED
} test_status_e;

typedef struct __attribute__((packed))
{
    app_addr_t                     router_address;
    app_lib_settings_net_addr_t    net_address;
    app_lib_settings_net_channel_t net_channel;
    uint8_t                        enc_key[APP_LIB_SETTINGS_AES_KEY_NUM_BYTES];
    uint8_t                        auth_key[APP_LIB_SETTINGS_AES_KEY_NUM_BYTES];
} network_settings_t;

typedef struct __attribute__((packed))
{
    int8_t rx_rssi;
    int8_t tx_rssi;
} rf_testing_parameters_t;

typedef struct __attribute__((packed))
{
    rf_testing_parameters_t rf_test_params;
} pl_req_test_init_t;

// Use test_init_status_e values
typedef struct __attribute__((packed))
{
    uint8_t test_init_status;  // Use test_req_status_e values
    app_addr_t tr_mac_address; // test router mac address
} pl_rsp_test_init_t;

typedef struct __attribute__((packed))
{
    uint8_t reserved;   // no payload data needed for now
} pl_req_test_start_t;

// Use test_start_status_e values
typedef struct __attribute__((packed))
{
    uint8_t test_start_status;  // Use test_req_status_e values
} pl_rsp_test_start_t;

typedef struct __attribute__((packed))
{
    network_settings_t settings;
} pl_req_config_router_t;

typedef bool pl_rsp_config_router_t;

typedef struct __attribute__((packed))
{
    uint8_t index;  // test result record to query
} pl_req_test_results_t;

typedef struct __attribute__((packed))
{
    uint8_t total_index; // test result record total count
    dut_testing_summary_t test_result_record;
} pl_rsp_test_result_t;

#endif  // EOL_TESTING_PROTOCOL_H_
