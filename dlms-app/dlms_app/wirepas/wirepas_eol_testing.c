/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include "api.h"
#include "shared_data.h"
#include "stack_state.h"
#include "app_scheduler.h"
#include "common.h"
#include "rng.h"
#include "server_attribute_manager.h"
#include "nic_status.h"

#define DEBUG_LOG_MODULE_NAME "WP_EOL_T"
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

/** Endpoint for incoming test mode signaling (mini-beacons) */
#define EOL_TEST_SIGN_EP    70
/** Endpoint for outgoing test result (unack CSMA-CA broadcast) */
#define EOL_TEST_RES_EP     70

/** Number of time test results are retransmitted to the test router. */
#define EOL_TEST_RES_MAX_RETRANSMIT     3
/** Test result sending initial delay: 20 seconds. */
#define EOL_TEST_SEND_INITIAL_DELAY_S           20
/** Test result retransmit delay. (leave enough time to escape a network scan ~3s) */
#define EOL_TEST_RES_RETRANSMIT_DELAY_MS        4000

/** Total allowed time to get meter test result.
 * Should leave enough time for DLMS app to try twice to communicate with the meter.
*/
#define EOL_TEST_MAX_DURATION_S     180
/** Time allowed to known if EOL test mode should be enabled.
 * Should leave enough time for two scans when no route to sink
*/
#define EOL_TEST_SIGN_RX_WAIT_S     60
/** Time delay to get meter association level reached information.
 * Should leave enough time for all meter association lvl to be tested
*/
#define EOL_TEST_ASSO_LVL_WAIT_S    25
/** Various tasks execution time. */
#define EOL_TEST_TIMEOUT_TASK_EXEC_TIME_US              100
#define EOL_TEST_METER_COM_NOTIFY_TASK_EXEC_TIME_US     100
#define EOL_TEST_SEND_RES_TASK_EXEC_TIME_US             100

#define EOL_TEST_METER_SERIAL_AVAILABLE_MASK            0x01
#define EOL_TEST_DEVICE_ID_AVAILABLE_MASK               0x04

/** Max count of test router info to store. */
#define EOL_TEST_MAX_ROUTER_INFO    3
/** Maximum sample history in RSSI filter for router selection.
 * RSSI sample every 200ms expected for a single 1.5s scan.
 * 8 entries should be enough.
*/
#define MAX_FILTER_SAMPLES 8

#define DLMS_APP_VERSION_LENGTH     10

typedef struct __attribute__ ((__packed__))
{
    app_addr_t target_address;
} router_packet_t;

typedef struct
{
    int8_t rssi;
    int8_t tx_pwr;
} rf_measurement_t;

typedef struct
{
    rf_measurement_t rf_meas;
    app_addr_t mac_address;
} eol_test_router_info_t;

typedef struct
{
    uint8_t device_id[METER_DEVICE_ID_MAX_SIZE];
    uint8_t dlms_app_version[DLMS_APP_VERSION_LENGTH];
    uint8_t nic_status_reason_bf; // Bitfield from NIC status packet
    uint8_t meter_connection_bf; // Bitfield from NIC status packet
    eol_test_router_info_t router_info;
} dut_packet_t;

typedef struct
{
    eol_test_router_info_t router_info;
    uint8_t beacon_samples;
} eol_test_router_select_info_t;

typedef struct
{
    eol_test_router_select_info_t routers[EOL_TEST_MAX_ROUTER_INFO];
    uint8_t num_routers;
    uint8_t best_router_index;
    int16_t best_router_rss;
} eol_test_router_table_t;

typedef enum
{
    EVENT_METER_COM_NOTIFY_TIMEOUT = 0,
    EVENT_EOL_SIGN_MODE_RX_TIMEOUT,
    EVENT_SCAN_END,
    EVENT_EOL_SIGN_MODE_RX,
    EVENT_METER_COM_NOTIFY,
    EVENT_METER_TEST_DONE,
    EVENT_TEST_RESULT_SENT
} event_type_t;

static uint32_t meter_com_ok_notification_task(void);
static uint32_t eol_test_timeout_task(void);
static uint32_t eol_test_send_result_task(void);
static void Wirepas_eol_testing_deinit(void);
static void eol_testing_fill_result(dut_packet_t * test_result);
static void handle_event(event_type_t event);

static void stack_scan_end_cb(app_lib_stack_event_e event, void * param);
static app_lib_data_receive_res_e eol_test_sign_received_cb(const shared_data_item_t * item,
                                                                const app_lib_data_received_t * data);

static void insert_router(const eol_test_router_select_info_t * router_select_info);
static void update_best_router(void);
static eol_test_router_info_t * get_test_router_info(void);

static shared_data_item_t m_eol_test_sign_filter =
{
    .cb = eol_test_sign_received_cb,
    .filter = {
        .mode = SHARED_DATA_NET_MODE_BROADCAST,
        .src_endpoint = EOL_TEST_SIGN_EP,
        .dest_endpoint = EOL_TEST_SIGN_EP,
        .multicast_cb = NULL
    }
};

static bool m_initialised;
static bool m_eol_test_enabled;
static bool m_meter_test_complete;
static bool m_eol_test_route_ok;
static uint32_t m_test_result_send_delay_ms;
static eol_test_router_table_t m_router_table;
static dut_packet_t m_dut_packet;

static void update_best_router(void)
{
    if (m_router_table.num_routers > 1)
    {
        // Force refresh.
        m_router_table.best_router_index = 0;
        m_router_table.best_router_rss = m_router_table.routers[0].router_info.rf_meas.rssi;
        for (uint8_t i = 1; i < m_router_table.num_routers; i++)
        {
            // updates best router location
            if (m_router_table.routers[i].router_info.rf_meas.rssi > m_router_table.best_router_rss)
            {
                m_router_table.best_router_index = i;
                m_router_table.best_router_rss = m_router_table.routers[i].router_info.rf_meas.rssi;
            }
        }
    }
}

static void insert_router(const eol_test_router_select_info_t * router_select_info)
{
    uint8_t i = 0;
    uint8_t insert_idx = EOL_TEST_MAX_ROUTER_INFO;
    bool match = false;
    eol_test_router_select_info_t * router = NULL;

    // searches for itself
    for (i = 0; i < m_router_table.num_routers; i++)
    {
        // if there is an entry present, use that index
        if (m_router_table.routers[i].router_info.mac_address == router_select_info->router_info.mac_address)
        {
            insert_idx = i;
            match = true;
            break;
        }
    }

    // if there is no entry in the table for the given address, then simply
    // append the beacon, otherwise replace the entry with the best info (RSSI wise)
    if (!match)
    {
        if (m_router_table.num_routers == EOL_TEST_MAX_ROUTER_INFO) // no space
        {
            // Find current worst entry and replace it.
            int8_t min_rss = 0;
            for (i = 0; i < m_router_table.num_routers; i++)
            {
                if (m_router_table.routers[i].router_info.rf_meas.rssi < min_rss)
                {
                    min_rss = m_router_table.routers[i].router_info.rf_meas.rssi;
                    insert_idx = i;
                }
            }

            if (min_rss < router_select_info->router_info.rf_meas.rssi)
            {
                // If new router is better, reset table entry before reusing it
                memset(&m_router_table.routers[insert_idx], 0, sizeof(eol_test_router_select_info_t));
            }
            else
            {
                // Set insert_idx to discard received router.
                insert_idx = EOL_TEST_MAX_ROUTER_INFO;
            }
        }
        else
        {
            insert_idx = m_router_table.num_routers;
            m_router_table.num_routers++;
        }
    }

    // update the table
    if (insert_idx < EOL_TEST_MAX_ROUTER_INFO)
    {
        router = &m_router_table.routers[insert_idx];
        if (router->beacon_samples < MAX_FILTER_SAMPLES)
        {
           router->beacon_samples++;
        }
        router->router_info.mac_address = router_select_info->router_info.mac_address;
        router->router_info.rf_meas.tx_pwr = router_select_info->router_info.rf_meas.tx_pwr;

        if (router->beacon_samples > 1)
        {
            router->router_info.rf_meas.rssi += (router_select_info->router_info.rf_meas.rssi - router->router_info.rf_meas.rssi) / router->beacon_samples;
        }
        else
        {
            router->router_info.rf_meas.rssi = router_select_info->router_info.rf_meas.rssi;
        }

        // Update best router info
        update_best_router();
        LOGD("Inserted router > idx:%u,address:%lu,rss:%d,txpower:%d",
            insert_idx,
            router_select_info->router_info.mac_address,
            router_select_info->router_info.rf_meas.rssi,
            router_select_info->router_info.rf_meas.tx_pwr);
    }
}

static app_lib_data_receive_res_e eol_test_sign_received_cb(const shared_data_item_t * item,
                                                                const app_lib_data_received_t * data)
{
    const router_packet_t * router_pkt_p;
    eol_test_router_select_info_t router_select_info_new;

    if (data->num_bytes < sizeof(router_packet_t))
    {
        LOGE("No data");
        return APP_LIB_DATA_RECEIVE_RES_HANDLED;
    }

    router_pkt_p = (router_packet_t *) data->bytes;

    /* Check if message is for us. */
    if (router_pkt_p->target_address != APP_ADDR_BROADCAST)
    {
        LOGI("Not for us");
        return APP_LIB_DATA_RECEIVE_RES_HANDLED;
    }

    // Valid eol testing signaling received so enter 'test mode'
    m_eol_test_enabled = true;
    // Store best router, RSSI wise, info
    LOGD("Received bcn from 0x%08X @ %d / %d ", data->src_address, data->rssi, data->tx_power);
    router_select_info_new.router_info.mac_address = data->src_address;
    router_select_info_new.router_info.rf_meas.tx_pwr = data->tx_power;
    router_select_info_new.router_info.rf_meas.rssi = data->rssi;
    router_select_info_new.beacon_samples = 1;
    // Insert beacon
    insert_router(&router_select_info_new);

    return APP_LIB_DATA_RECEIVE_RES_HANDLED;
}

static eol_test_router_info_t * get_test_router_info(void)
{
    return &(m_router_table.routers[m_router_table.best_router_index].router_info);
}

static void stack_scan_end_cb(app_lib_stack_event_e event, void * param)
{
    if (event == APP_LIB_STATE_STACK_EVENT_SCAN_STOPPED)
    {
        LOGI("Scan ended");
        handle_event(EVENT_SCAN_END);
    }

    if (event == APP_LIB_STATE_STACK_EVENT_SCAN_STARTED)
    {
        LOGI("Scan started");
    }
}

static void eol_testing_fill_result(dut_packet_t * test_result)
{
    const uint8_t * str_p;
    uint8_t str_len;
    eol_test_router_info_t * route_info_p;

    // Get statuses.
    Nic_status_compute_statuses(&(test_result->meter_connection_bf), &(test_result->nic_status_reason_bf));

    // Get DLMS app version
    memcpy(test_result->dlms_app_version, (const uint8_t *)OFFICIAL_TAG, MIN((size_t)DLMS_APP_VERSION_LENGTH, sizeof(OFFICIAL_TAG)));

    /** Fill meter identification info.
     * Note:
     * The full device ID length is reserved and "used" even if not all information is available.
     * If "meter flag ID" or "device ID" or both could not be read, they will be be left initialized to 0
     * aka NUL character when processed at a later stage.
     */
    memset(test_result->device_id, 0, (size_t)METER_DEVICE_ID_MAX_SIZE);

    if (!(test_result->nic_status_reason_bf & EOL_TEST_METER_SERIAL_AVAILABLE_MASK))
    {
        // Meter serial available so fill device ID associated part.
        Server_Attribute_Manager_readMeterSerialNumber(&str_p, &str_len);
        memcpy(&(test_result->device_id[METER_FLAG_ID_LEN]),
                str_p, (size_t)str_len);
    }

    // Fill device ID part.
    if (test_result->meter_connection_bf & EOL_TEST_DEVICE_ID_AVAILABLE_MASK)
    {
        // Info available.
        Server_Attribute_Manager_readMeterDeviceId(&str_p, &str_len);
        memcpy(test_result->device_id, str_p, (size_t)str_len);
    }

    // Fill RF test data.
    route_info_p = get_test_router_info();
    m_dut_packet.router_info.mac_address = route_info_p->mac_address;
    m_dut_packet.router_info.rf_meas.tx_pwr = route_info_p->rf_meas.tx_pwr;
    m_dut_packet.router_info.rf_meas.rssi = route_info_p->rf_meas.rssi;
}

static uint32_t eol_test_timeout_task(void)
{
    static bool rescheduled = false;
    uint32_t ret_val = APP_SCHEDULER_STOP_TASK;

    if (!m_eol_test_enabled)
    {
        LOGI("Test signal rx timeout reached");
        // Test mode signaling reception timeout reached. Stop module.
        handle_event(EVENT_EOL_SIGN_MODE_RX_TIMEOUT);
    }
    else
    {
        if (rescheduled)
        {
            if (!m_meter_test_complete)
            {
                // Meter test com timed out.
                LOGI("Test timeout reached");
                handle_event(EVENT_METER_COM_NOTIFY_TIMEOUT);
            }
            // else timeout reached while we are in final stage of the test procedure: ignore timeout
        }
        else
        {
            // reschedule for meter com
            ret_val = (uint32_t)(EOL_TEST_MAX_DURATION_S - EOL_TEST_SIGN_RX_WAIT_S) * 1000;
            rescheduled = true;
        }
    }

    LOGI("Timeout re-sched in %ld", ret_val);
    return ret_val;
}

static uint32_t eol_test_send_result_task(void)
{
    static uint8_t sending_seq = 0;
    /** Packet is sent as Unack'ed CSMA-CA with hop limit = 1. */
    app_lib_data_to_send_t data = {
        .bytes = (const uint8_t *)&m_dut_packet,
        .num_bytes = sizeof(dut_packet_t),
        .tracking_id = APP_LIB_DATA_NO_TRACKING_ID,
        .qos         = APP_LIB_DATA_QOS_HIGH,
        .flags       = (APP_LIB_DATA_SEND_FLAG_UNACK_CSMA_CA
                        | APP_LIB_DATA_SEND_SET_HOP_LIMITING),
        .src_endpoint  = EOL_TEST_RES_EP,
        .dest_endpoint = EOL_TEST_RES_EP,
        .dest_address  = APP_ADDR_BROADCAST,
        .hop_limit = 1
    };
    uint32_t ret_val = APP_SCHEDULER_STOP_TASK;

    if (sending_seq < EOL_TEST_RES_MAX_RETRANSMIT)
    {
        // Send packet
        if (Shared_Data_sendData(&data, NULL) == APP_LIB_DATA_SEND_RES_SUCCESS)
        {
            LOGI("Unack CSMA-CA #%u sent", sending_seq);
        }
        else
        {
            LOGE("Failed to send unack CSMA-CA #%u", sending_seq);
        }
        sending_seq++;
        ret_val = EOL_TEST_RES_RETRANSMIT_DELAY_MS;
    }
    else
    {
        handle_event(EVENT_TEST_RESULT_SENT);
    }
    return ret_val;
}

static uint32_t meter_com_ok_notification_task(void)
{
    LOGI("Test meter done");
    handle_event(EVENT_METER_TEST_DONE);

    return APP_SCHEDULER_STOP_TASK;
}

static void handle_event(event_type_t event)
{
    LOGI("Event %u handling", event);

    switch (event)
    {
        case EVENT_METER_TEST_DONE:
        case EVENT_METER_COM_NOTIFY_TIMEOUT:
            m_meter_test_complete = true;

            eol_testing_fill_result(&m_dut_packet);

            // Timeout can only be reached if got a route to send test result.
            // In case test is finished before valid route wait for it or valid route timeout event
            // to exit test mode.
            if (m_eol_test_route_ok)
            {
                App_Scheduler_addTask_execTime(eol_test_send_result_task,
                                                m_test_result_send_delay_ms,
                                                EOL_TEST_SEND_RES_TASK_EXEC_TIME_US);
            }
            break;

        case EVENT_EOL_SIGN_MODE_RX_TIMEOUT:
            m_eol_test_route_ok = false;
            // Exit test mode.
            Wirepas_eol_testing_deinit();
            break;

        case EVENT_SCAN_END:
            if (m_eol_test_enabled)
            {
                LOGI("EOL mode enabled");
                m_eol_test_route_ok = true;

                /* Stop test signaling reception. */
                Shared_Data_removeDataReceivedCb(&m_eol_test_sign_filter);
                /* Remove scan end event. */
                Stack_State_removeEventCb(stack_scan_end_cb);
                
                if (m_meter_test_complete)
                {
                    // Get meter test results
                    eol_testing_fill_result(&m_dut_packet);

                    // Schedule data sending.
                    App_Scheduler_addTask_execTime(eol_test_send_result_task,
                                                    m_test_result_send_delay_ms,
                                                    EOL_TEST_SEND_RES_TASK_EXEC_TIME_US);
                }
                // else wait for meter com OK notification or METER_COM test timeout
            }
            // else wait for another scan or EVENT_EOL_SIGN_MODE_RX_TIMEOUT event
            break;

        case EVENT_METER_COM_NOTIFY:
            /* Leave enough time
            * for all association level to be tested.
            */
            App_Scheduler_addTask_execTime(meter_com_ok_notification_task,
                                            1000*EOL_TEST_ASSO_LVL_WAIT_S,
                                            EOL_TEST_TIMEOUT_TASK_EXEC_TIME_US);
            break;

        case EVENT_TEST_RESULT_SENT:
            // Exit test mode.
            Wirepas_eol_testing_deinit();
            break;

        default:
            // Unknow event_type_t. Should not happen. Exit test mode.
            LOGE("Unknown event %u", event);
            Wirepas_eol_testing_deinit();
    }
}

static void Wirepas_eol_testing_deinit(void)
{
    LOGI("Module deinit");

    /* Stop timeouts. */
    App_Scheduler_cancelTask(eol_test_timeout_task);

    /* Stop test signaling reception. */
    Shared_Data_removeDataReceivedCb(&m_eol_test_sign_filter);

    /* Unregister stack scan end notification. */
    Stack_State_removeEventCb(stack_scan_end_cb);

    /* Stop test result sending. */
    App_Scheduler_cancelTask(eol_test_send_result_task);

    /* Stop meter com notification task. */
    App_Scheduler_cancelTask(meter_com_ok_notification_task);

    m_initialised = false;
}

void Wirepas_eol_testing_init(void)
{
    Shared_Data_addDataReceivedCb(&m_eol_test_sign_filter);
    Stack_State_addEventCb(stack_scan_end_cb, 1 << APP_LIB_STATE_STACK_EVENT_SCAN_STARTED | 1 << APP_LIB_STATE_STACK_EVENT_SCAN_STOPPED);

    App_Scheduler_addTask_execTime(eol_test_timeout_task,
                                    1000*EOL_TEST_SIGN_RX_WAIT_S,
                                    EOL_TEST_TIMEOUT_TASK_EXEC_TIME_US);

    /* Test report 1st sending ramdomized over EOL_TEST_SEND_INITIAL_DELAY_S period. */
    m_test_result_send_delay_ms = 1000 * Rng_number(EOL_TEST_SEND_INITIAL_DELAY_S);

    m_initialised = true;
    LOGI("Module init");
    LOGD("Test res delay: %u", m_test_result_send_delay_ms);
}

void Wirepas_eol_testing_notify_meter_com_ok(void)
{
    if (m_initialised)
    {
        handle_event(EVENT_METER_COM_NOTIFY);
    }
    else
    {
        LOGE("Cannot process notification");
    }
}
