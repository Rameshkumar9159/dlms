/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include <string.h>

#include "wirepas_ping.h"
#include "api.h"
#include "common.h"
#include "shared_data.h"
#include "rng.h"
#include "meter_clock.h"
#include "nic_system_title.h"

#define DEBUG_LOG_MODULE_NAME "WP_PING"
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

/** Endpoints for incoming meter firmware update traffic */
#define PING_EP                 66

#define PING_REQUEST            0x00
#define PING_RESPONSE           0x80

#define PING_PAYLOAD_VERSION    0x01

#define METER_RT_CLOCK_LEN      12

// Retry after 2 seconds if sending failed
#define PING_DELAY_RETRY_MS     (2 * 1000)

typedef struct __attribute__ ((__packed__))
{
    uint8_t command;
    uint8_t id;
    uint8_t rand_interval_s;
} ping_command_t;

typedef struct __attribute__ ((__packed__))
{
    uint8_t response;
    uint8_t id;
    uint32_t dl_travel_time_ms;
    uint32_t rand_interval_time_ms;
} ping_response_header_t;

typedef struct __attribute__ ((__packed__))
{
    ping_response_header_t hdr;
    uint8_t body[102 - sizeof(ping_response_header_t)];
} ping_response_t;

static app_lib_data_receive_res_e ping_data_received_cb(const shared_data_item_t * item,
                                                        const app_lib_data_received_t * data);

static shared_data_item_t m_ping_packets_filter =
{
    .cb = ping_data_received_cb,
    .filter = {
        .mode = SHARED_DATA_NET_MODE_ALL,
        .src_endpoint = PING_EP,
        .dest_endpoint = PING_EP,
        .multicast_cb = NULL
    }
};

static ping_response_header_t m_rsp_hdr;
static bool m_ping_ongoing;

static bool send_ping_response(void)
{
    ping_response_t rsp;
    const uint8_t * info_p = NULL;
    uint8_t * ptr_p = &rsp.body[0];
    uint32_t epoch;
    uint32_t ic;
    int16_t deviation;
    uint8_t info_len = 0;
    bool sent = true;

    // Copy the header
    memcpy(&rsp.hdr, &m_rsp_hdr, sizeof(rsp.hdr));

    // Generate the PING response body
    // 0- Ping payload version
    *ptr_p++ = PING_PAYLOAD_VERSION;

    // 1- Meter device id length
#ifdef METER_RETROFIT
    // Device id does not exist, so serial number is used instead
    Server_Attribute_Manager_readMeterSerialNumber(&dev_id_p, &dev_id_len);
#ifdef METER_RETROFIT_FLAG_ID
    char device_id[METER_DEVICE_ID_MAX_SIZE];
    snprintf(device_id, sizeof(device_id), "%.3s%s", METER_RETROFIT_FLAG_ID, (const char *)dev_id_p);
    dev_id_p = (const uint8_t *)device_id;
    dev_id_len = strlen(device_id);
#endif
#else
    Server_Attribute_Manager_readMeterDeviceId(&info_p, &info_len);
#endif
    // dev_id_len can't exceed METER_DEVICE_ID_MAX_SIZE - 1
    *ptr_p++ = info_len;

    // 2- Meter device id (if not empty)
    if (info_len)
    {
        memcpy(ptr_p, info_p, info_len);
        ptr_p += info_len;
    }

    // 3- Meter real time clock (12 bytes)
    // Data is send in octet string. Remove data type.
    MeterClock_get(&epoch, &deviation);
    memset(ptr_p, 0x00, METER_RT_CLOCK_LEN);
    if (epoch)
    {
        gxByteBuffer bb;
        gxtime t;

        time_initUnix(&t, epoch);
        t.deviation = deviation;

        bb_init(&bb);

        if (var_getDateTime2(&t, &bb) == DLMS_ERROR_CODE_OK)
        {
            memcpy(ptr_p, bb.data, MIN(METER_RT_CLOCK_LEN, bb.size));
        }
        bb_clear(&bb);
    }
    ptr_p += METER_RT_CLOCK_LEN;

    // 4- NIC system Title, size is always 8 bytes
    Nic_ST_get(&info_p, &info_len);
    memcpy(ptr_p, info_p, info_len);
    ptr_p += info_len;

    // 5- NIC Invocation Counter
    Server_Attribute_Manager_readDecryptInvocationCounter(&ic);
    memcpy(ptr_p, &ic, sizeof(ic));
    ptr_p += sizeof(ic);

    // 6- DLMS app version Length
    *ptr_p++ = strlen(OFFICIAL_TAG);

    // 7- DLMS app version
    memcpy(ptr_p, (const uint8_t *)OFFICIAL_TAG, strlen(OFFICIAL_TAG));
    ptr_p += strlen(OFFICIAL_TAG);

    // 8- Meter Serial Number Length
    Server_Attribute_Manager_readMeterSerialNumber(&info_p, &info_len);
    *ptr_p++ = info_len;

    // 9- Meter Serial Number (if not empty)
    if (info_len)
    {
        memcpy(ptr_p, info_p, info_len);
        ptr_p += info_len;
    }

    // Send the reply
    app_lib_data_to_send_t data_to_send = {
        .bytes = (const uint8_t *) &rsp,
        .num_bytes = (size_t)(ptr_p - (const uint8_t *) &rsp),
        .dest_address = APP_ADDR_ANYSINK,
        .src_endpoint = PING_EP,
        .dest_endpoint = PING_EP,
        .qos = APP_LIB_DATA_QOS_HIGH,
        .flags = APP_LIB_DATA_SEND_FLAG_NONE,
    };

    if (Shared_Data_sendData(&data_to_send, NULL) != APP_LIB_DATA_SEND_RES_SUCCESS)
    {
        LOGE("Failed to send response");
        sent = false;
    }

    return sent;
}

static uint32_t send_ping_response_task(void)
{
    if (! send_ping_response())
    {
        return PING_DELAY_RETRY_MS;
    }
    m_ping_ongoing = false;

    return APP_SCHEDULER_STOP_TASK;
}

static app_lib_data_receive_res_e ping_data_received_cb(const shared_data_item_t * item,
                                                        const app_lib_data_received_t * data)
{
    const ping_command_t * cmd_p;
    uint32_t delay_ms;

    if (data->num_bytes < sizeof(ping_command_t))
    {
        LOGE("No data");
        return APP_LIB_DATA_RECEIVE_RES_HANDLED;
    }

    cmd_p = (ping_command_t *) data->bytes;

    if (cmd_p->command != PING_REQUEST)
    {
        LOGE("Invalid command");
        return APP_LIB_DATA_RECEIVE_RES_HANDLED;
    }

    LOGI("Received PING cmd");
    LOG_BUFFER(LVL_DEBUG, data->bytes, data->num_bytes);

    if (m_ping_ongoing)
    {
        // Send the previous ping response
        (void)send_ping_response();
        App_Scheduler_cancelTask(send_ping_response_task);
    }

    m_ping_ongoing = true;
    if (! cmd_p->rand_interval_s)
    {
        delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;
    }
    else
    {
        delay_ms = Rng_number(cmd_p->rand_interval_s * 1000);
    }
    m_rsp_hdr.response = PING_RESPONSE;
    m_rsp_hdr.id = cmd_p->id;
    // End-to-end transmission delay, in 1 / 1024 of seconds
    m_rsp_hdr.dl_travel_time_ms = data->delay_hp;
    m_rsp_hdr.rand_interval_time_ms = delay_ms;

    App_Scheduler_addTask_execTime(send_ping_response_task,
                                   delay_ms,
                                   SFSM_EXECUTION_TIME_US);
    return APP_LIB_DATA_RECEIVE_RES_HANDLED;
}

void Wirepas_Ping_init(void)
{
    Shared_Data_addDataReceivedCb(&m_ping_packets_filter);
}
