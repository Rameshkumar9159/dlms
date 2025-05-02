/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    app.c
 * \brief   This file implements the router application for radio loopback tests
 * in factory
 */

#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "node_configuration.h"
#include "shared_data.h"
#include "shared_data.h"
#include "scheduler/app_scheduler.h"
#include "led.h"
#include "usart.h"

#include "protocol.h"

#define DEBUG_LOG_MODULE_NAME "TEST_ROUTER"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#ifdef EOL_MODE
#define SND_EP 70
#define RCV_EP 70
#define TEST_ROUTER_MODE_STR "EOL testing"
#define SERIAL_COM_STATUS_BIT_MASK  0x01
#define MAX_OFFLINE_SCAN_VAL_S      3600
#else
#define SND_EP 5
#define RCV_EP 4
#define TEST_ROUTER_MODE_STR "Standalone NIC testing"
#endif

#define BLINK_TASK_EXEC_TIME_US 100
#define SEND_DATA_INTERVAL_MS 200
#define ROUTER_MSG_TASK_EXEC_TIME_US 10000
#define MINI_BEACON_TASK_EXEC_DELAY_MS 10000
#define MINI_BEACON_TASK_EXEC_TIME_US 10000

#define MANAGE_REQ_CONFIG_ROUTER_TASK_EXEC_TIME_US 100
#define STOP_STACK_TASK_EXEC_TIME_US 100
#ifdef EOL_MODE
#define MANAGE_REQ_TEST_INIT_TASK_EXEC_TIME_US     100
#define MANAGE_REQ_TEST_START_TASK_EXEC_TIME_US    100
#define MANAGE_REQ_TEST_RESULT_TASK_EXEC_TIME_US   100
#define TEST_STOP_TASK_EXEC_DELAY_MS               2000
#define TEST_STOP_TASK_EXEC_TIME_US                100
// Waiting for HDLC's ACK/NACK Timeout ending
#define WAIT_FOR_HDLC_ACK_NACK_TIMEOUT_DELAY_MS    30
#else
// Waiting for HDLC's ACK/NACK Timeout ending
#define WAIT_FOR_HDLC_ACK_NACK_TIMEOUT_DELAY_MS    550
#endif

#define FAST_BLINKING_MS 200
#define SLOW_BLINKING_MS 1000


#define MIN(a, b) (a) < (b) ? (a) : (b)
#define MAX(a, b) (a) > (b) ? (a) : (b)

// Prototypes
static app_lib_data_receive_res_e data_receiver_cb(
    const shared_data_item_t * item, const app_lib_data_received_t * data);
static uint32_t send_mini_beacon_task(void);

#ifdef EOL_MODE
uint32_t end_testing_task(void);
#endif

// Filters
static const shared_data_filter_t m_data_receiver_filter
    = { .dest_endpoint = RCV_EP,
        .mode = SHARED_DATA_NET_MODE_BROADCAST,
        .src_endpoint = RCV_EP,
        .multicast_cb = NULL };

static shared_data_item_t m_item
    = { .cb = data_receiver_cb, .filter = m_data_receiver_filter };

static uint32_t m_msg_nb;
static network_settings_t m_nwk_settings;

#ifdef EOL_MODE
/* Storage of all DUTs test results
* For now no filtering method implemented so DUT from other JIG could
* be received by test router. Add some storage margin */
typedef dut_testing_summary_t dut_test_record_list_t[MAX_DUT_PER_JIG*3/2];
static dut_test_record_list_t m_dut_test_result_list;
// For now no filtering implemented so indicate all devices should answer
static router_packet_t m_router_packet = {.target_address = (app_addr_t)APP_ADDR_BROADCAST};
static uint8_t         m_dut_test_list_entry_cnt;
static uint8_t         m_req_dut_record_index;
static bool            m_rf_testing_enabled = false;
static rf_testing_parameters_t m_rf_test_thresholds;

#else
static router_packet_t m_dut_list;
static uint8_t m_dut_list_pos;
#endif

static uint32_t blinking_task_exec_period_ms = SLOW_BLINKING_MS;

/**
 * @brief   periodic callback that toggles the LED to indicate that the tests
 *          are on-going
 * @return  the delay before the next execution of this callback
 */
static uint32_t blink_task(void)
{
    // Toggle all LEDs
    uint8_t leds_nb = Led_getNumber();

    for (uint8_t id = 0; id < leds_nb; id++)
    {
        Led_toggle(id);
    }
    return blinking_task_exec_period_ms;
}

/**
 * \brief Setup to Blink LEDs alternatively
 *
 */
static void start_led_blinking_for_router_ready(void)
{
    App_Scheduler_cancelTask(blink_task);

    uint8_t leds_nb = Led_getNumber();

    for (uint8_t id = 0; id < leds_nb; id++)
    {
        Led_set(id, false);
    }

    Led_set(0, true);
    App_Scheduler_addTask_execTime(blink_task,
                                   APP_SCHEDULER_SCHEDULE_ASAP,
                                   BLINK_TASK_EXEC_TIME_US);
}

#ifdef EOL_MODE
/**
 * \brief Stop to Blink LEDs alternatively.
 * Leave them all ON.
 *
 */
static void stop_led_blinking_for_router_ready(void)
{
    App_Scheduler_cancelTask(blink_task);

    uint8_t leds_nb = Led_getNumber();

    for (uint8_t id = 0; id < leds_nb; id++)
    {
        Led_set(id, true);
    }
}
#endif

/**
 * \brief Set the led to indicate router is waiting for network configuration
 *
 */
static void set_led_to_indicate_network_configuration(void)
{
    App_Scheduler_cancelTask(blink_task);

    uint8_t leds_nb = Led_getNumber();

    for (uint8_t id = 0; id < leds_nb; id++)
    {
        Led_set(id, false);
    }

    App_Scheduler_addTask_execTime(blink_task,
                                   APP_SCHEDULER_SCHEDULE_ASAP,
                                   BLINK_TASK_EXEC_TIME_US);
}

/**
 * \brief Sets the router network parameters
 *
 */
static void set_network_parameters(void)
{
    // Apply network settings
    if (lib_settings->setNetworkAddress(m_nwk_settings.net_address)
            != APP_RES_OK
        || lib_settings->setNetworkChannel(m_nwk_settings.net_channel)
               != APP_RES_OK
        || lib_settings->setAuthenticationKey(m_nwk_settings.auth_key)
               != APP_RES_OK
        || lib_settings->setEncryptionKey(m_nwk_settings.enc_key) != APP_RES_OK)
    {
        LOGE("Failed to apply network settings");
        return;
    }
    LOGI("Network settings ok");

    // Apply router settings
    if (lib_settings->setNodeAddress(m_nwk_settings.router_address)
            != APP_RES_OK
        || lib_settings->setNodeRole(APP_LIB_SETTINGS_ROLE_HEADNODE_LL)
               != APP_RES_OK)
    {
        LOGE("Failed to apply router settings");
        return;
    }
    LOGI("Router settings ok");
}
/**
 * \brief Init test signaling and data reception
 *
 */

static app_res_e init_wirepas_com(void)
{
    app_res_e res = APP_RES_OK;
    res = Shared_Data_addDataReceivedCb(&m_item);
    // Add data receiver
    if (res != APP_RES_OK)
    {
        LOGE("Failed to register data receiver");
        return res;
    }
    LOGI("Data receiver added");

    m_msg_nb = 0;

    App_Scheduler_addTask_execTime(send_mini_beacon_task,
                                   MINI_BEACON_TASK_EXEC_DELAY_MS,
                                   MINI_BEACON_TASK_EXEC_TIME_US);
    
    return res;
}

#ifdef EOL_MODE
/**
 * \brief De-init test signaling and data reception
 *
 */

static void deinit_wirepas_com(void)
{
    Shared_Data_removeDataReceivedCb(&m_item);

    App_Scheduler_cancelTask(send_mini_beacon_task);
}
#endif

/**
 * \brief Starts the stack and initializes the reception callback and mini
 * beacon sending task
 *
 * \return app_res_e Result code, @ref APP_RES_OK if successful
 */
static app_res_e start_test_router_stack(void)
{
    app_res_e res = APP_RES_OK;

#ifdef EOL_MODE
    // Reduce likelihood of scan during test procedure.
    if (APP_RES_OK != lib_settings->setOfflineScan(MAX_OFFLINE_SCAN_VAL_S) )
    {
        LOGW("Failed to set offline scan");
    }
#endif

    // Start the stack.
    res = lib_state->startStack();
    if (res != APP_RES_OK)
    {
        LOGE("Failed to start the stack");
        return res;
    }
    LOGI("Stack started");

#ifndef EOL_MODE
    res = init_wirepas_com();
    LOG_FLUSH(LVL_INFO);
    start_led_blinking_for_router_ready();
#endif

    return res;
}

/**
 * \brief Performs periodic sending of mini-beacons
 *
 * \return uint32_t Next Scheduling Time
 */
static uint32_t send_mini_beacon_task(void)
{
    app_lib_data_send_res_e res;
    app_lib_data_to_send_t  data = {
#ifdef EOL_MODE
                                    .bytes = (const uint8_t *) &m_router_packet,
                                    .num_bytes = sizeof m_router_packet,
#else
                                    .bytes = (const uint8_t *) &m_dut_list,
                                    .num_bytes = sizeof m_dut_list,
#endif
                                     .tracking_id = APP_LIB_DATA_NO_TRACKING_ID,
                                     .qos = APP_LIB_DATA_QOS_HIGH,
                                     /* Sending mini beacon */
                                     .flags = APP_LIB_DATA_SEND_NW_CH_ONLY,
                                     .src_endpoint = SND_EP,
                                     .dest_endpoint = SND_EP,
                                     .dest_address = APP_ADDR_BROADCAST };
    // Increment m_msg_nb
    m_msg_nb++;

    blinking_task_exec_period_ms = FAST_BLINKING_MS;
#ifndef EOL_MODE
    LOGD("payload in send_mini_beacon_task");
    LOG_BUFFER(LVL_DEBUG, (uint8_t *) m_dut_list, sizeof m_dut_list);
#endif
    /* Send packet */
    LOGD("Sending mini beacon #%u", m_msg_nb);
    res = Shared_Data_sendData(&data, NULL);
    if (res == APP_LIB_DATA_SEND_RES_SUCCESS)
    {
        LOGD("mini beacon #%u sent successfully", m_msg_nb);
    }
    else
    {
        LOGE("Failed to send mini beacon #%u", m_msg_nb);
    }

    return SEND_DATA_INTERVAL_MS;
}

#ifdef EOL_MODE
static void normalize_rfmeasurement(dut_packet_t * dut_packet,
                                        rf_measurement_t * router_rf_meas)
{
    int16_t dut_tx_norm_rssi;
    int16_t dut_rx_norm_rssi;

    LOGD("Raw RF meas content:\n"
        "\tdut rf meas -> %d dBm @ %d dBm Tx pwr\n"
        "\ttest router rf meas -> %d dBm @ %d dBm Tx pwr\n",
        dut_packet->router_info.rf_meas.rssi,
        dut_packet->router_info.rf_meas.tx_pwr,
        router_rf_meas->rssi,
        router_rf_meas->tx_pwr);

    dut_tx_norm_rssi = router_rf_meas->rssi - router_rf_meas->tx_pwr;
    dut_rx_norm_rssi = dut_packet->router_info.rf_meas.rssi - dut_packet->router_info.rf_meas.tx_pwr;

    // RSSI value in measurement are int8_t type so saturate if needed.
    if (dut_tx_norm_rssi < INT8_MIN)
    {
        dut_tx_norm_rssi = INT8_MIN;
    }

    if (dut_rx_norm_rssi < INT8_MIN)
    {
        dut_rx_norm_rssi = INT8_MIN;
    }

    // Update rf measurements
    router_rf_meas->rssi = (int8_t)dut_tx_norm_rssi;
    router_rf_meas->tx_pwr = 0;
    dut_packet->router_info.rf_meas.rssi = (int8_t)dut_rx_norm_rssi;
    dut_packet->router_info.rf_meas.tx_pwr = 0;
}

static void compute_dut_test_statuses(const dut_packet_t * dut_packet,
                                        const rf_measurement_t * router_rf_meas,
                                        test_status_e * serial_com_status,
                                        test_status_e * rf_status)
{
    *serial_com_status = TEST_STATUS_SUCCESS;
    *rf_status = TEST_STATUS_SUCCESS;

    LOGI("Computing test record\n");
    LOGD("Test record content:\n"
    "\tcom_status -> %u\n"
    "\tasso_lvl -> %u\n"
    "\tdut rf meas -> %d dBm @ %d dBm Tx pwr\n"
    "\ttest router rf meas -> %d dBm @ %d dBm Tx pwr\n",
    dut_packet->nic_status_reason_bf,
    dut_packet->meter_connection_bf,
    dut_packet->router_info.rf_meas.rssi,
    dut_packet->router_info.rf_meas.tx_pwr,
    router_rf_meas->rssi,
    router_rf_meas->tx_pwr);

    // Get & check meter communication bit
    if( dut_packet->nic_status_reason_bf & SERIAL_COM_STATUS_BIT_MASK )
    {
        *serial_com_status = TEST_STATUS_FAILED;
    }

    // RF test if enabled
    if (m_rf_testing_enabled)
    {
        if (dut_packet->router_info.rf_meas.rssi < m_rf_test_thresholds.rx_rssi
            || router_rf_meas->rssi < m_rf_test_thresholds.tx_rssi)
        {
            *rf_status = TEST_STATUS_FAILED;
        } 
    }
    else
    {
        *rf_status = TEST_STATUS_UNTESTED;
    }

    LOGD("Statuses\n"
    "\t serial > %u\n"
    "\t rf > %u",
    *serial_com_status,
    *rf_status);
}
/**
 * \brief Store a single DUT test record to the test record list
 *
 * \param dut_packet DUT reported data
 * \param dut_mac_address DUT Wirepas node ID
 * \param router_rf_meas DUT RF perfromance measured by router
 * \param index
 * \boot
 */
static void store_dut_test_record(const dut_packet_t * dut_packet,
                                    app_addr_t dut_mac_address,
                                    uint8_t index,
                                    const rf_measurement_t * router_rf_meas,
                                    test_status_e serial_com_status,
                                    test_status_e rf_test_status)
{
    LOGI("Storing test record at index %u\n", index);

    m_dut_test_result_list[index].mac_address = dut_mac_address;
    memcpy(m_dut_test_result_list[index].device_id, dut_packet->device_id, METER_DEVICE_ID_MAX_LENGTH);
    memcpy(m_dut_test_result_list[index].app_version, dut_packet->dlms_app_version, DUT_APP_VERSION_MAX_LENGTH);
    m_dut_test_result_list[index].serial_com_status = (uint8_t)serial_com_status;
    m_dut_test_result_list[index].rf_results_dut_rx.rssi = dut_packet->router_info.rf_meas.rssi;
    m_dut_test_result_list[index].rf_results_dut_rx.tx_pwr = dut_packet->router_info.rf_meas.tx_pwr;
    m_dut_test_result_list[index].rf_results_dut_tx.rssi = router_rf_meas->rssi;
    m_dut_test_result_list[index].rf_results_dut_tx.tx_pwr = router_rf_meas->tx_pwr;
    m_dut_test_result_list[index].rf_status = (uint8_t)rf_test_status;
    m_dut_test_result_list[index].association_lvl_status = dut_packet->meter_connection_bf;

    LOGD("Stored record content:\n"
    "\tDUT mac addr > 0x%0X\n"
    "\tDUT device ID > %s\n"
    "\tDUT app_version > %s\n"
    "\tDUT serial com status > %u\n"
    "\tDUT asso status > %u\n"
    "\tDUT RF status > %u\n"
    "\tDUT RF details:\n"
    "\t\t(rx RSSI,pwr) > (%d,%d)\n"
    "\t\t(tx RSSI,pwr) > (%d,%d)\n",
    m_dut_test_result_list[index].mac_address,
    m_dut_test_result_list[index].device_id,
    m_dut_test_result_list[index].app_version,
    m_dut_test_result_list[index].serial_com_status,
    m_dut_test_result_list[index].association_lvl_status,
    m_dut_test_result_list[index].rf_status,
    m_dut_test_result_list[index].rf_results_dut_rx.rssi,
    m_dut_test_result_list[index].rf_results_dut_rx.tx_pwr,
    m_dut_test_result_list[index].rf_results_dut_tx.rssi,
    m_dut_test_result_list[index].rf_results_dut_tx.tx_pwr
    );

}

/**
 * \brief Adds DUT test result to test record list
 *
 * \param dut_test_result
 */
static void add_dut_test_result_to_list(dut_packet_t * dut_test_result, app_addr_t dut_mac_address, rf_measurement_t * router_rf_meas)
{
    test_status_e rf_status;
    test_status_e serial_status;

    // Normalize RSSI measurement to 0dBm
    normalize_rfmeasurement(dut_test_result, router_rf_meas);

    compute_dut_test_statuses(dut_test_result, router_rf_meas, &serial_status, &rf_status);

    for (uint8_t i = 0; i < MAX_DUT_PER_JIG; i++)
    {
        // If the device is already in the list, no need to add it again
        if (m_dut_test_result_list[i].mac_address == dut_mac_address)
        {
            store_dut_test_record(dut_test_result,
                                    dut_mac_address,
                                    i,
                                    router_rf_meas,
                                    serial_status,
                                    rf_status);
            return;
        }
    }
    // We just keep the last MAX_DUT_PER_JIG DUTs
    // We hope the storage margin is enough to prevent entry override
    store_dut_test_record(dut_test_result,
                            dut_mac_address,
                            m_dut_test_list_entry_cnt,
                            router_rf_meas,
                            serial_status,
                            rf_status);
    
    if(m_dut_test_list_entry_cnt < MAX_DUT_PER_JIG)
    {
        m_dut_test_list_entry_cnt++;
    }
}
#else
/**
 * \brief Adds DUT to list sent using the mini-beacons
 *
 * \param dut_address DUT Address Received
 * \param tx_rssi Measured TX RSSI
 */
static void add_dut_to_list(app_addr_t dut_address, int8_t tx_rssi)
{
    for (uint8_t i = 0; i < MAX_DUT_PER_JIG; i++)
    {
        // If the device is already in the list, no need to add it again
        if (m_dut_list[i].address == dut_address)
        {
            LOGD("Updating RSSI for dut #%u: rssi=%d", i, tx_rssi);
            m_dut_list[i].rssi = tx_rssi;
            return;
        }
    }
    // We just keep the last MAX_DUT_PER_JIG DUTs
    m_dut_list[m_dut_list_pos].address = dut_address;
    m_dut_list[m_dut_list_pos].rssi = tx_rssi;
    m_dut_list_pos = (m_dut_list_pos + 1) % MAX_DUT_PER_JIG;
    LOGD("Adding dut 0x%08X at pos #%u: rssi=%d",
         dut_address,
         m_dut_list_pos,
         tx_rssi);

    LOGD("payload in add_dut_to_list");
    LOG_BUFFER(LVL_DEBUG, (uint8_t *) m_dut_list, sizeof m_dut_list);
}
#endif

/**
 * @brief   The data reception callback.
 *
 * This is the callback called when a packet is received (and allowed).
 * The received packet is represented as a pointer to @ref
 * app_lib_data_received_t struct.
 *
 * @param   item
 *          Pointer to the filter item that initiated the callback.
 * @param   data
 *          Pointer to the received data.
 * @return  Result code, @ref app_lib_data_receive_res_e.
 * @note    If APP_LIB_DATA_RECEIVE_RES_NO_SPACE is returned, the whole data
 *          is blocked for all the app. App will start receiving data once
 *          this filter has called @ref Shared_Data_readyToReceive
 */
static app_lib_data_receive_res_e data_receiver_cb(
    const shared_data_item_t * item, const app_lib_data_received_t * data)
{
    // With the filter, we receive broadcast message from EP 'SND_EP' to EP 'RCV_EP' only
    // Let's check this message is for us
    if (data->num_bytes == sizeof(dut_packet_t))
    {
        dut_packet_t * dp_p = (dut_packet_t *) data->bytes;
#ifdef EOL_MODE
        app_addr_t router_address = dp_p->router_info.mac_address;
#else
        app_addr_t router_address = dp_p->router_address;
#endif

        if (router_address == m_nwk_settings.router_address)
        {
#ifdef EOL_MODE
            rf_measurement_t router_rf_meas = {
                .rssi = data->rssi,
                .tx_pwr = data->tx_power
            };
            LOGI("Received broadcast message from 0x%0X, RSSI: %i dBm",
                    data->src_address,
                    data->rssi);
            add_dut_test_result_to_list(dp_p, data->src_address, &router_rf_meas);
#else
            LOGI("Received broadcast message from 0x%0X, RSSI: %i dBm",
                    dp_p->dut_address,
                    data->rssi);
            add_dut_to_list(dp_p->dut_address, data->rssi);
#endif
        }
        else
        {
            LOGD("Discarding broadcast message for routeur 0x%08X",
                    router_address);
        }
    }
    else
    {
        LOGI("Received message, size: %u bytes", data->num_bytes);
    }
    return APP_LIB_DATA_RECEIVE_RES_HANDLED;
}
/**
 * @brief   Stack events callback
 * @param   event
 *          Which event generated this call
 * @param   param_p
 *          Parameter pointer associated to the event. This pointer
 *          must be casted depending on the event. Its type is
 *          listed in @ref app_lib_stack_event_e
 * @note    Most of the time this callback is generated in critical
 *          section of the stack code so execution time must be short
 * @note    List of event may evolve in future. To write forward compatible code
 *          callback must discard unknown event
 */
static void on_stack_event_cb(app_lib_stack_event_e event, void * param)
{
    switch (event)
    {
        case APP_LIB_STATE_STACK_EVENT_STACK_STOPPED:
            set_network_parameters();
            break;
        default:
            // Nothing to do. New event may be generated in later release
            (void) event;
    }
}

/**
 * \brief Task that will stop the stack
 *
 * \return uint32_t Time before next scheduling
 */
uint32_t stop_stack_task(void)
{
    lib_state->stopStack();
    return APP_SCHEDULER_STOP_TASK;
}

uint32_t manage_req_config_router_task(void)
{
    message_t rsp_config_router_msg = { 0 };
    // Successfully set parameters
    pl_rsp_config_router_t pl = true;

    if (APP_LIB_STATE_STARTED == lib_state->getStackState())
    {
        lib_state->setOnStackEventCb(on_stack_event_cb);
        App_Scheduler_addTask_execTime(stop_stack_task,
                                       WAIT_FOR_HDLC_ACK_NACK_TIMEOUT_DELAY_MS,
                                       STOP_STACK_TASK_EXEC_TIME_US);
    }
    else
    {
        set_network_parameters();
        if (APP_RES_OK != start_test_router_stack())
        {
            pl = false;
        }
    }

    protocol_set_message_header(&rsp_config_router_msg,
                                MSG_ID_RSP_CONFIG_ROUTER);

    protocol_set_message_payload(&rsp_config_router_msg,
                                     (uint8_t *) &pl,
                                     sizeof(pl));

    LOGI("Sending MSG_ID_RSP_CONFIG_ROUTER");
    
    HDLC_send_message((uint8_t *) &rsp_config_router_msg.payload,
                      rsp_config_router_msg.size);

    return APP_SCHEDULER_STOP_TASK;
}

#ifdef EOL_MODE
uint32_t manage_req_test_init_task(void)
{
    message_t rsp_test_init_msg = { 0 };
    // Successfully handled the init request
    pl_rsp_test_init_t pl = {
        .test_init_status = TEST_REQ_STATUS_OK,
        .tr_mac_address = m_nwk_settings.router_address
    };
    m_rf_testing_enabled = true;

    // Check if all thresholds defined
    if ( (m_rf_test_thresholds.rx_rssi == 0) || (m_rf_test_thresholds.tx_rssi == 0) )
    {
        // Disable testing
        m_rf_testing_enabled = false;
    }

    LOGI("RF testing state: %s", m_rf_testing_enabled ? "True":"False");

    // Start from a known state in case last session ended unexpectedly on test station side.
    App_Scheduler_addTask_execTime(end_testing_task, APP_SCHEDULER_SCHEDULE_ASAP, TEST_STOP_TASK_EXEC_TIME_US);

    protocol_set_message_header(&rsp_test_init_msg,
                                MSG_ID_RSP_TEST_INIT);

    protocol_set_message_payload(&rsp_test_init_msg,
                                     (uint8_t *) &pl,
                                     sizeof(pl));

    LOGI("Sending MSG_ID_RSP_TEST_INIT");
    
    HDLC_send_message((uint8_t *) &rsp_test_init_msg.payload,
                        rsp_test_init_msg.size);

    return APP_SCHEDULER_STOP_TASK;
}

uint32_t manage_req_test_start_task(void)
{
    message_t rsp_test_start_msg = { 0 };
    pl_rsp_test_start_t pl = {.test_start_status = TEST_REQ_STATUS_OK};
    
    // Reset storage and linked data
    m_dut_test_list_entry_cnt = 0;
    memset(m_dut_test_result_list, (uint8_t)0, sizeof(m_dut_test_result_list));

    if (APP_LIB_STATE_STARTED != lib_state->getStackState())
    {
        pl.test_start_status = TEST_REQ_STATUS_CONFIG_ERROR;
    }
    // Enable test signaling & result reception
    else if (APP_RES_OK != init_wirepas_com() )
    {
        pl.test_start_status = TEST_REQ_STATUS_GENERIC_ERROR;
    }
    else
    {
        // All good
        start_led_blinking_for_router_ready();
    }

    protocol_set_message_header(&rsp_test_start_msg,
                                MSG_ID_RSP_TEST_START);

    protocol_set_message_payload(&rsp_test_start_msg,
                                    (uint8_t *) &pl,
                                    sizeof(pl));

    LOGI("Sending MSG_ID_RSP_TEST_START");
    
    HDLC_send_message((uint8_t *) &rsp_test_start_msg.payload,
                        rsp_test_start_msg.size);
    
    return APP_SCHEDULER_STOP_TASK;
}

uint32_t manage_req_test_result_task(void)
{
    message_t rsp_test_result_msg = { 0 };
    pl_rsp_test_result_t pl;

    pl.total_index = m_dut_test_list_entry_cnt;
    memcpy(&pl.test_result_record, &m_dut_test_result_list[m_req_dut_record_index], sizeof(dut_testing_summary_t));

    protocol_set_message_header(&rsp_test_result_msg,
                                MSG_ID_RSP_TEST_RESULT);

    protocol_set_message_payload(&rsp_test_result_msg,
                                    (uint8_t *) &pl,
                                    sizeof(pl));

    LOGI("Sending MSG_ID_RSP_TEST_RESULT #%u", m_req_dut_record_index);
    
    HDLC_send_message((uint8_t *) &rsp_test_result_msg.payload,
                        rsp_test_result_msg.size);
    

    // (re)Schedule test session end if no new REQ_TEST_RESULT received within few (2) seconds 
    App_Scheduler_addTask_execTime(end_testing_task, TEST_STOP_TASK_EXEC_DELAY_MS, TEST_STOP_TASK_EXEC_TIME_US);

    return APP_SCHEDULER_STOP_TASK;
}

uint32_t end_testing_task(void)
{
    deinit_wirepas_com();
    stop_led_blinking_for_router_ready();

    return APP_SCHEDULER_STOP_TASK;
}

#endif

/**
 * \brief   Callback called when an HDLC DATA frame has been received
 *
 * \param message       Pointer to received frame
 * \param message_size  Frame size
 * \return  True: Message is valid to be processed; False: Message isn't
 * valid
 */
static bool message_rx_from_script_cb(const uint8_t * message,
                                      size_t message_size)
{
    message_t rx_msg = { 0 };

    if (message_size < sizeof(message_header_t))
    {
        // Message must be bigger than a header
        LOGE("Message too short %d < %d",
             message_size,
             sizeof(message_header_t));
        return false;
    }

    rx_msg.size = message_size;
    memcpy(&rx_msg.payload, message, rx_msg.size);

    if (rx_msg.payload.header.version != METERING_TEST_PROTOCOL_VERSION)
    {
        // Discard message and return True for forward compatibility
        LOGE("Invalid protocol version %d", rx_msg.payload.header.version);
        return true;
    }

    switch (rx_msg.payload.header.type)
    {
        case MSG_ID_REQ_CONFIG_ROUTER:
            LOGI("Received MSG_ID_REQ_CONFIG_ROUTER");
            memcpy((uint8_t *) &m_nwk_settings,
                   rx_msg.payload.data,
                   sizeof(pl_req_config_router_t));

            App_Scheduler_addTask_execTime(
                manage_req_config_router_task,
                WAIT_FOR_HDLC_ACK_NACK_TIMEOUT_DELAY_MS,
                MANAGE_REQ_CONFIG_ROUTER_TASK_EXEC_TIME_US);
            break;
#ifdef EOL_MODE
        case MSG_ID_REQ_TEST_INIT:
            LOGI("Received MSG_ID_REQ_TEST_INIT");
            memcpy((uint8_t *) &m_rf_test_thresholds,
                    rx_msg.payload.data,
                    sizeof(pl_req_test_init_t));
            App_Scheduler_addTask_execTime(
                manage_req_test_init_task,
                WAIT_FOR_HDLC_ACK_NACK_TIMEOUT_DELAY_MS,
                MANAGE_REQ_TEST_INIT_TASK_EXEC_TIME_US);
            break;
        case MSG_ID_REQ_TEST_START:
            LOGI("Received MSG_ID_REQ_TEST_START");
            App_Scheduler_addTask_execTime(
                manage_req_test_start_task,
                WAIT_FOR_HDLC_ACK_NACK_TIMEOUT_DELAY_MS,
                MANAGE_REQ_TEST_INIT_TASK_EXEC_TIME_US);
            break;
        case MSG_ID_REQ_TEST_RESULT:
            LOGI("Received MSG_ID_REQ_TEST_RESULT");
            
            // Store requested test record index
            m_req_dut_record_index = ((pl_req_test_results_t *)&rx_msg.payload.data[0])->index;
            
            App_Scheduler_addTask_execTime(
                manage_req_test_result_task,
                WAIT_FOR_HDLC_ACK_NACK_TIMEOUT_DELAY_MS,
                MANAGE_REQ_TEST_INIT_TASK_EXEC_TIME_US);
            break;
#endif
        default:
            LOGW("Unsupported cmd %d", rx_msg.payload.header.type);
            // Still return true for forward compatibility
            break;
    }

    return true;
}

/**
 * \brief Populate Network Settings with the values stored in the device
 *
 */
void populate_nwk_settings(void)
{
    app_addr_t router_address = 0;
    app_lib_settings_net_addr_t net_address = 0;
    app_lib_settings_net_channel_t net_channel = 0;
    uint8_t enc_key[APP_LIB_SETTINGS_AES_KEY_NUM_BYTES] = { 0 };
    uint8_t auth_key[APP_LIB_SETTINGS_AES_KEY_NUM_BYTES] = { 0 };

    lib_settings->getNodeAddress(&router_address);
    lib_settings->getNetworkAddress(&net_address);
    lib_settings->getNetworkChannel(&net_channel);
    lib_settings->getAuthenticationKey((uint8_t *) &auth_key);
    lib_settings->getEncryptionKey((uint8_t *) &enc_key);

    m_nwk_settings.router_address = router_address;
    m_nwk_settings.net_address = net_address;
    m_nwk_settings.net_channel = net_channel;
    memcpy(m_nwk_settings.auth_key,
           auth_key,
           APP_LIB_SETTINGS_AES_KEY_NUM_BYTES);
    memcpy(m_nwk_settings.enc_key, enc_key, APP_LIB_SETTINGS_AES_KEY_NUM_BYTES);
}

/**
 * \brief   Initialization callback for application
 *
 * This function is called after hardware has been initialized but the
 * stack is not yet running.
 *
 */
void App_init(const app_global_functions_t * functions)
{
    LOG_INIT();

    set_led_to_indicate_network_configuration();
    populate_nwk_settings();

    LOGI("Router mode: %s", (char *)TEST_ROUTER_MODE_STR);
    LOGI("Router address: 0x%08X", m_nwk_settings.router_address);

    HDLC_init(message_rx_from_script_cb);

    start_test_router_stack();
}
