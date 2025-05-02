/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    app.c
 * \brief   This file implements the DUT application for radio tests in factory
 */

#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "node_configuration.h"
#include "shared_data.h"
#include "app_scheduler.h"
#include "usart.h"
#include "random.h"

#include "dut_protocol.h"

#define DEBUG_LOG_MODULE_NAME "RF TEST "
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#define SND_EP 4
#define RCV_EP 5

#define MIN(a, b) (a) < (b) ? (a) : (b)

#define RANDOM_RAND_MAX INT_MAX

#define RF_TASK_EXEC_TIME_US 1000
#define RETRY_DELAY_MS 500

#define MAX_DELAY_BEFORE_CSMA_CA_RETRY_MS 100

#define TIMEOUT_30_SECONDS_TASK_DELAY_MS 30000
#define TIMEOUT_30_SECONDS_EXEC_TIME_US 100

#define DO_SCAN_PERIOD_MS (1000)
#define DO_SCAN_EXEC_TIME_US (100)
// Wait for stack to be up and running
#define DO_SCAN_INITIAL_DELAY_MS (5 * 1000)

#define SCAN_DURATION_MS (1.5 * 1000)
#define STOP_SCAN_EXEC_TIME_US (100)

typedef struct
{
    int8_t rx_rssi;
    int8_t tx_rssi;
    bool rx_received;
    bool tx_received;
} router_message_t;

// Stores RF Thresholds
static rf_testing_parameters_t m_rf_test_thresholds = { 0 };

// Callback used when the flash testing is ended
static void (*m_end_cb)(test_status_e, int8_t, int8_t) = NULL;

static router_message_t m_router_msg;
static app_addr_t m_dut_address;
static app_addr_t m_router_address;

static uint32_t msg_rx_cnt;

// Prototypes
static app_lib_data_receive_res_e data_receiver_cb(
    const shared_data_item_t * item, const app_lib_data_received_t * data);
static uint32_t rf_send_unack_csma_ca_task(void);
static uint32_t timeout_30_seconds_task(void);
static uint32_t do_scan_task(void);
static uint32_t stop_scan_task(void);

// Filters
static const shared_data_filter_t m_data_receiver_filter
    = { .dest_endpoint = RCV_EP,
        .mode = SHARED_DATA_NET_MODE_BROADCAST,
        .src_endpoint = RCV_EP,
        .multicast_cb = NULL };

static shared_data_item_t m_item
    = { .cb = data_receiver_cb, .filter = m_data_receiver_filter };

static uint32_t rng_number(uint32_t limit)
{
    uint32_t nb;

    // Remove the modulo bias, refer to
    // https://stackoverflow.com/questions/10984974/why-do-people-say-there-is-modulo-bias-when-using-a-random-number-generator
    do
    {
        nb = Random_get32();
    } while (nb >= (RANDOM_RAND_MAX - (RANDOM_RAND_MAX % limit)));

    nb %= limit;

    return nb;
}

static void rf_test_ending(void)
{
    test_status_e status = TEST_STATUS_SUCCESS;

    App_Scheduler_cancelTask(rf_send_unack_csma_ca_task);
    App_Scheduler_cancelTask(do_scan_task);
    App_Scheduler_cancelTask(stop_scan_task);

    if (!m_router_msg.rx_received || !m_router_msg.tx_received)
    {
        status = TEST_STATUS_FAILED;
    }

    if (m_router_msg.rx_rssi < m_rf_test_thresholds.rx_rssi
        || m_router_msg.tx_rssi < m_rf_test_thresholds.tx_rssi)
    {
        status = TEST_STATUS_FAILED;
    }

    m_end_cb(status, m_router_msg.rx_rssi, m_router_msg.tx_rssi);
}

static app_lib_data_receive_res_e data_receiver_cb(
    const shared_data_item_t * item, const app_lib_data_received_t * data)
{
    LOGD("data_receiver_cb, message received #%u", ++msg_rx_cnt);
    LOG_BUFFER(LVL_DEBUG, data->bytes, data->num_bytes);

    // With the filter, we receive broadcast message from EP 5 to EP 5 only
    // Let's check this message is coming from our router
    if (data->src_address == m_router_address
        && sizeof(router_packet_t) == data->num_bytes)
    {
        dut_element_t * dut = (dut_element_t *) data->bytes;
        LOG(LVL_INFO,
            "Received message from 0x%08X, RSSI: %i dBm",
            data->src_address,
            data->rssi);
        m_router_msg.rx_received = true;
        m_router_msg.rx_rssi = data->rssi;

        LOGD("Listing dut addresses in the received packet");
        for (uint8_t i = 0; i < MAX_DUT_PER_JIG; i++)
        {
            LOGD("dut[%u]: 0x%08X", i, dut[i].address);
            if (dut[i].address == m_dut_address)
            {
                m_router_msg.tx_received = true;
                m_router_msg.tx_rssi = dut[i].rssi;
                // Stop listening
                Shared_Data_removeDataReceivedCb(&m_item);
                App_Scheduler_cancelTask(timeout_30_seconds_task);
                rf_test_ending();
                break;
            }
        }
        // We are not in the list, let's resend the unack CSMA-CA
        // in a random interval
        App_Scheduler_addTask_execTime(
            rf_send_unack_csma_ca_task,
            rng_number(MAX_DELAY_BEFORE_CSMA_CA_RETRY_MS),
            RF_TASK_EXEC_TIME_US);
    }
    else if (m_router_address)
    {
        LOG(LVL_INFO,
            "Received message from 0x%08X but router is 0x%08X, keep "
            "listening",
            data->src_address,
            m_router_address);
    }
    else
    {
        LOG(LVL_INFO,
            "Received message from 0x%08X but router address has not "
            "been received yet",
            data->src_address,
            m_router_address);
    }
    return APP_LIB_DATA_RECEIVE_RES_HANDLED;
}

static uint32_t rf_send_unack_csma_ca_task(void)
{
    dut_packet_t packet = { .dut_serial_number = SYSTEM_GetUnique(),
                            .dut_address = m_dut_address,
                            .router_address = m_router_address };

    app_lib_data_to_send_t data
        = { .bytes = (const uint8_t *) &packet,
            .num_bytes = sizeof packet,
            .tracking_id = APP_LIB_DATA_NO_TRACKING_ID,
            .qos = APP_LIB_DATA_QOS_HIGH,
            /* This packet will only be received by CSMA nodes. */
            .flags = (APP_LIB_DATA_SEND_FLAG_UNACK_CSMA_CA
                      | APP_LIB_DATA_SEND_SET_HOP_LIMITING),
            .src_endpoint = SND_EP,
            .dest_endpoint = SND_EP,
            .dest_address = APP_ADDR_BROADCAST,
            // We don't want retransmission of the packet
            .hop_limit = 1 };

    // Send the packet
    if (Shared_Data_sendData(&data, NULL) == APP_LIB_DATA_SEND_RES_SUCCESS)
    {
        LOG(LVL_INFO, "unack CSMA-CA successfully sent");

        // Message has been sent, stop the task
        return APP_SCHEDULER_STOP_TASK;
    }

    LOG(LVL_ERROR, "Failed to send unack CSMA-CA");
    // Retry later
    return RETRY_DELAY_MS;
}

static uint32_t timeout_30_seconds_task(void)
{
    rf_test_ending();

    return APP_SCHEDULER_STOP_TASK;
}

static uint32_t stop_scan_task(void)
{
    LOGD("Stop scan");
    lib_state->stopScanNbors();

    App_Scheduler_addTask_execTime(do_scan_task,
                                   DO_SCAN_PERIOD_MS,
                                   DO_SCAN_EXEC_TIME_US);

    return APP_SCHEDULER_STOP_TASK;
}

static uint32_t do_scan_task(void)
{
    LOGD("Do scan");

    App_Scheduler_addTask_execTime(stop_scan_task,
                                   SCAN_DURATION_MS,
                                   STOP_SCAN_EXEC_TIME_US);
    lib_state->startScanNbors();

    return APP_SCHEDULER_STOP_TASK;
}

void factory_rf_tests_dut_init(rf_testing_parameters_t rf_test_params,
                               app_addr_t router_addr,
                               void (*end_cb)(test_status_e, int8_t, int8_t))
{
    lib_settings->getNodeAddress(&m_dut_address);
    LOG(LVL_INFO, "DUT address: 0x%08X", m_dut_address);

    m_router_address = router_addr;
    LOG(LVL_INFO, "Router Address: 0x%08X", m_router_address);

    m_end_cb = end_cb;

    m_rf_test_thresholds = rf_test_params;

    /*
     * Start the stack.
     * This is really important step, otherwise the stack will stay stopped and
     * will not be part of any network. So the device will not be reachable
     * without reflashing it
     */
    LOG(LVL_INFO, "Starting stack");
    lib_state->startStack();

    // Add data receiver immediately
    if (APP_RES_OK != Shared_Data_addDataReceivedCb(&m_item))
    {
        LOG(LVL_ERROR, "Failed to register data receiver");
    }

    LOG_FLUSH(LVL_INFO);

    // Trigger 1.5 second stack scan periodically
    App_Scheduler_addTask_execTime(do_scan_task,
                                   DO_SCAN_INITIAL_DELAY_MS,
                                   DO_SCAN_EXEC_TIME_US);

    // Send unack CSMA-CA
    App_Scheduler_addTask_execTime(rf_send_unack_csma_ca_task,
                                   APP_SCHEDULER_SCHEDULE_ASAP,
                                   RF_TASK_EXEC_TIME_US);

    App_Scheduler_addTask_execTime(timeout_30_seconds_task,
                                   TIMEOUT_30_SECONDS_TASK_DELAY_MS,
                                   TIMEOUT_30_SECONDS_EXEC_TIME_US);
}
