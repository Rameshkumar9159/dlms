/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    dut_protocol.h
 * \brief   example of protocol for DUT tests
 */
#ifndef DUT_PROTOCOL_H_
#define DUT_PROTOCOL_H_

#include <stdint.h>

#include "wms_app.h"
#include "wms_settings.h"
#include "hdlc.h"

//------------------------------------------------------------------------------
// RF packets
//------------------------------------------------------------------------------
#define MAX_DUT_PER_JIG 10

typedef struct
{
    uint64_t   dut_serial_number;
    app_addr_t dut_address;
    app_addr_t router_address;
} dut_packet_t;

typedef struct
{
    app_addr_t address;
    int8_t     rssi;
} dut_element_t;

typedef dut_element_t dut_list_t[MAX_DUT_PER_JIG];
typedef dut_list_t    router_packet_t;

//------------------------------------------------------------------------------
// HDLC packets
//------------------------------------------------------------------------------
// Provisioning Protocol Version used in the message header
#define METERING_TEST_PROTOCOL_VERSION 0

#define NIC_SERIAL_NUMBER_MAX_LENGTH 32

typedef enum
{
    MSG_ID_REQ_TEST_INIT,
    MSG_ID_RSP_TEST_INIT,
    MSG_ID_REQ_TEST_EXT_FLASH,
    MSG_ID_RSP_TEST_EXT_FLASH,
    MSG_ID_REQ_TEST_RF,
    MSG_ID_RSP_TEST_RF,
    MSG_ID_REQ_CONFIG_ROUTER = 240,
    MSG_ID_RSP_CONFIG_ROUTER = 241
} msg_id_e;

typedef enum
{
    TEST_INIT_STATUS_OK,
    TEST_INIT_STATUS_CONFIG_ERROR,
    TEST_INIT_STATUS_TESTING_ALREADY_DONE
} test_init_status_e;

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
    network_settings_t      net_settings;
    rf_testing_parameters_t rf_test_params;
    uint8_t                 serial_number[NIC_SERIAL_NUMBER_MAX_LENGTH];
} pl_req_test_init_t;

// Use test_init_status_e values
typedef struct __attribute__((packed))
{
    uint8_t test_init_status;  // Use test_init_status_e values
    uint8_t extflash_status;   // Use test_status_e values
    uint8_t rf_status;         // Use test_status_e values
} pl_rsp_test_init_t;

typedef uint8_t pl_rsp_test_ext_flash_t;  // Use test_status_e values

typedef struct __attribute__((packed))
{
    rf_testing_parameters_t meas_rf;
    uint8_t                 status;  // Use test_status_e values
} pl_rsp_test_rf_t;

typedef struct __attribute__((packed))
{
    network_settings_t settings;
} pl_req_config_router_t;

typedef bool pl_rsp_config_router_t;

//------------------------------------------------------------------------------
// Testing Summary
//------------------------------------------------------------------------------
typedef struct __attribute__((packed))
{
    uint8_t serial_number[NIC_SERIAL_NUMBER_MAX_LENGTH];
    uint8_t extflash_status;  // Use test_status_e values
    uint8_t rf_status;        // Use test_status_e values
    rf_testing_parameters_t rf_results;
} testing_summary_t;

#endif  // DUT_PROTOCOL_H_
