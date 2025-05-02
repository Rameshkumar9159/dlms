/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#define DEBUG_LOG_MODULE_NAME "TRANS_MO"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#include "transparent_mode.h"
#include "meter_uart.h"
#include "shared_data.h"
#include "common.h"
#include "server_attribute_manager.h"
#include "dlms_lock.h"

#define FRAME_SIZE  1500

/** Endpoints for incoming DLMS traffic*/
#define DLMS_TRANSPARENT_EP   65

static uint8_t * m_rx_buffer_p;

static bool m_enabled = false;

static app_lib_data_receive_res_e transparentDLMSDataReceivedCb(const shared_data_item_t * item,
                                                    const app_lib_data_received_t * data)
{
    bool registered;

    Server_Attribute_Manager_readNicRegistrationStatus(&registered);

    LOGD("Transparent traffic received from network");
    if (! registered)
    {
        LOGE("Data in transparent mode but NIC is unregistered");
    }
    else
    {
        if (m_enabled)
        {
            uint32_t written_bytes;
            written_bytes = Meter_uart_send_frame(data->bytes, data->num_bytes);
            if (written_bytes != data->num_bytes)
            {
                LOGE("Cannot write message from Transparent");
            }
            else
            {
                LOGI("To meter: %d bytes", data->num_bytes);
            }
        }
        else
        {
            LOGE("Data in transparent mode but not enabled");
        }
    }
    return APP_LIB_DATA_RECEIVE_RES_HANDLED;
}

static shared_data_item_t m_dlms_transparent_packets_filter =
{
        .cb = transparentDLMSDataReceivedCb,
        .filter = {
                .mode = SHARED_DATA_NET_MODE_UNICAST,
                .src_endpoint = DLMS_TRANSPARENT_EP,
                .dest_endpoint = DLMS_TRANSPARENT_EP,
                .multicast_cb = NULL
        }
};

static Meter_uart_rx_code_e rx_data_cb(uint8_t * bytes, size_t size)
{
    // Data received from meter, forward it to the network
    app_lib_data_send_res_e res;

    // No tracking for now
    app_lib_data_to_send_t data_to_send = {
            .bytes = (const uint8_t*)bytes,
            .num_bytes = size,
            .dest_address = APP_ADDR_ANYSINK,
            .src_endpoint = DLMS_TRANSPARENT_EP,
            .dest_endpoint = DLMS_TRANSPARENT_EP,
            .qos = APP_LIB_DATA_QOS_HIGH,
            .flags = APP_LIB_DATA_SEND_FLAG_NONE
    };

    res = Shared_Data_sendData(&data_to_send,
                               NULL);

    if (res != APP_LIB_DATA_SEND_RES_SUCCESS)
    {
        LOGE("Cannot send transparent traffic");
        // Could be stored in a temporary buffer to retry later
    }
    return METER_UART_RX_OK;
}

bool Transparent_mode_init(void)
{
    bool stored_state = false;

    Shared_Data_addDataReceivedCb(&m_dlms_transparent_packets_filter);
    Server_Attribute_Manager_readTransparentConfig(&stored_state);
    LOGI("Initialize transparent mode: %d", stored_state);
    if (stored_state)
    {
        Dlms_Lock_return_code_e lock_ret;

        LOGI("Transparent was enabled before reboot");
        // Take the lock from NIC server as it is required by transparent
        lock_ret = Dlms_lock_take(DLMS_LOCK_ID_NIC_SERVER,
                           NULL,
                           DLMS_LOCK_TYPE_WITH_TRAFFIC);
        if (lock_ret != DLMS_LOCK_RET_ACQUIRED)
        {
            LOGE("Cannot aquire lock!");
            return false;
        }
        // Enable it again once we have the lock
        Transparent_mode_enable();

    }
    return true;
}

bool Transparent_mode_enable(void)
{
    if (m_enabled)
    {
        LOGE("Already enabled");
        return true;
    }

    m_rx_buffer_p = malloc(FRAME_SIZE);
    if (m_rx_buffer_p == NULL)
    {
        LOGE("Cannot allocate rx buffer for transparent");
        return false;
    }

    if (!Meter_uart_register_rx_cb(rx_data_cb, m_rx_buffer_p, FRAME_SIZE))
    {
        LOGE("Cannot register rx_cb for transparent");
        free(m_rx_buffer_p);
        return false;
    }
    Server_Attribute_Manager_writeTransparentConfig(true);
    m_enabled = true;
    return true;
}

bool Transparent_mode_disable(void)
{
    if (!m_enabled)
    {
        LOGE("Cannot disable, not enabled");
        return false;
    }

    Meter_uart_unregister_rx_cb(rx_data_cb);
    free(m_rx_buffer_p);
    m_enabled = false;
    Server_Attribute_Manager_writeTransparentConfig(false);
    return true;
}

bool Transparent_mode_is_enabled(void)
{
    return m_enabled;
}
