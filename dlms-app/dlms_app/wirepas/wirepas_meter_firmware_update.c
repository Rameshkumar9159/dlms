/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include <string.h>

#include "wirepas_meter_firmware_update.h"
#include "api.h"
#include "common.h"
#include "shared_data.h"
#include "server_attribute_manager.h"
#include "client_attribute_manager.h"

#define DEBUG_LOG_MODULE_NAME "WP_MFU "
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

/** Endpoints for incoming meter firmware update traffic */
#define METER_FIRMWARE_UPDATE_EP    64

typedef enum
{
    MFU_CMD_RESET,
    MFU_CMD_QUERY_FIRMWARE_UPDATE_STATUS,
    MFU_CMD_PROCESS_FIRMWARE_UPDATE,
    MFU_CMD_NB
} mfu_command_e;

#define MIN_IMAGE_IDENTIFIER_LEN    2
#define MAX_IMAGE_IDENTIFIER_LEN    32

typedef struct
{
    uint8_t len;
    uint8_t id[MAX_IMAGE_IDENTIFIER_LEN];
} mfu_command_img_id_t;

typedef enum
{
    MFU_CMD_STATUS_ACCEPTED,
    MFU_CMD_STATUS_EXECUTED,
    MFU_CMD_STATUS_EXECUTED_WITH_ERRORS,
    MFU_CMD_STATUS_INVALID_OR_MISSING_ARGUMENT,
    MFU_CMD_STATUS_NOT_IMPLEMENTED,
    MFU_CMD_STATUS_NOT_SUPPORTED
} mfu_cmd_status_e;

typedef union __attribute__ ((__packed__))
{
    mfu_command_img_id_t img_id;
    uint32_t unused;
} mfu_command_u;

typedef struct __attribute__ ((__packed__))
{
    mfu_command_e command;
    mfu_command_u u;
} mfu_command_t;

typedef struct __attribute__ ((__packed__))
{
    mfu_command_e command;
    mfu_cmd_status_e status;
} mfu_response_header_t;

typedef union __attribute__ ((__packed__))
{
    uint8_t data;
    firmware_update_status_t fu_status;
} mfu_response_u;

typedef struct __attribute__ ((__packed__))
{
    mfu_response_header_t hdr;
    mfu_response_u data;
} mfu_response_t;

static app_lib_data_receive_res_e mfu_data_received_cb(const shared_data_item_t * item,
                                                       const app_lib_data_received_t * data);

static shared_data_item_t m_mfu_packets_filter =
{
    .cb = mfu_data_received_cb,
    .filter = {
        .mode = SHARED_DATA_NET_MODE_UNICAST,
        .src_endpoint = METER_FIRMWARE_UPDATE_EP,
        .dest_endpoint = METER_FIRMWARE_UPDATE_EP,
        .multicast_cb = NULL
    }
};

static void send_response(mfu_command_e cmd, mfu_cmd_status_e status,
                          const uint8_t * data, uint8_t data_len)
{
    mfu_response_t rsp;

    rsp.hdr.command = cmd;
    rsp.hdr.status = status;
    if (data_len && data)
    {
        memcpy(&rsp.data, data, MIN(data_len, sizeof(mfu_response_u)));
    }

    app_lib_data_to_send_t data_to_send = {
        .bytes = (const uint8_t *) &rsp,
        .num_bytes = sizeof(mfu_response_header_t) + data_len,
        .dest_address = APP_ADDR_ANYSINK,
        .src_endpoint = METER_FIRMWARE_UPDATE_EP,
        .dest_endpoint = METER_FIRMWARE_UPDATE_EP,
        .qos = APP_LIB_DATA_QOS_HIGH,
        .flags = APP_LIB_DATA_SEND_FLAG_NONE,
    };

    if (Shared_Data_sendData(&data_to_send, NULL) != APP_LIB_DATA_SEND_RES_SUCCESS)
    {
        LOGE("Failed to send response");
    }
}

static void reboot(mfu_command_e cmd)
{
    send_response(cmd, MFU_CMD_STATUS_EXECUTED, NULL, 0);
    // Reboot after a delay to be sure our response was sent
    Common_reboot(10);
}

static bool is_request_allowed(void)
{
    bool registered;

    Server_Attribute_Manager_readNicRegistrationStatus(&registered);

    if (! registered)
    {
        LOGE("Firmware update requests are disabled if NIC is unregistered");
        return false;
    }
    return true;
}

static void query_firmware_update_status(mfu_command_e cmd)
{
    firmware_update_status_t status;

    if (is_request_allowed())
    {
        Firmware_Update_query_firmware_update_status(&status);
        send_response(cmd, MFU_CMD_STATUS_EXECUTED, (uint8_t *) &status, sizeof(status));
    }
    else
    {
        send_response(cmd, MFU_CMD_STATUS_NOT_SUPPORTED, NULL, 0);
    }
}

static void process_firmware_update_command(mfu_command_e cmd, uint16_t remaining_bytes, const mfu_command_img_id_t * img_id_p)
{

    if (is_request_allowed())
    {
        if ((remaining_bytes < MIN_IMAGE_IDENTIFIER_LEN) ||
            (remaining_bytes > sizeof(mfu_command_img_id_t)))
        {
            send_response(cmd, MFU_CMD_STATUS_INVALID_OR_MISSING_ARGUMENT, NULL, 0);
        }
        else
        {
            remaining_bytes--;
            if (! img_id_p->len || img_id_p->len > MAX_IMAGE_IDENTIFIER_LEN ||
                img_id_p->len != remaining_bytes)
            {
                send_response(cmd, MFU_CMD_STATUS_INVALID_OR_MISSING_ARGUMENT, NULL, 0);
            }
            else if (! Firmware_Update_process_firmware_update(img_id_p->id, img_id_p->len))
            {
                send_response(cmd, MFU_CMD_STATUS_EXECUTED_WITH_ERRORS, NULL, 0);
            }
        }
    }
    else
    {
        send_response(cmd, MFU_CMD_STATUS_NOT_SUPPORTED, NULL, 0);
    }
}

static app_lib_data_receive_res_e mfu_data_received_cb(const shared_data_item_t * item,
                                                       const app_lib_data_received_t * data)
{
    mfu_command_t * cmd_p = (mfu_command_t *) data->bytes;
    mfu_command_e cmd;

    if (! data->num_bytes)
    {
        LOGE("No data");
        send_response(MFU_CMD_NB, MFU_CMD_STATUS_NOT_SUPPORTED, NULL, 0);
        return APP_LIB_DATA_RECEIVE_RES_HANDLED;
    }

    cmd = *(mfu_command_e *) data->bytes;

    if (cmd >= MFU_CMD_NB)
    {
        LOGE("Invalid command");
        send_response(cmd, MFU_CMD_STATUS_NOT_SUPPORTED, NULL, 0);
        return APP_LIB_DATA_RECEIVE_RES_HANDLED;
    }

    LOGI("MFU cmd #%u", cmd);
    LOG_BUFFER(LVL_DEBUG, data->bytes, data->num_bytes);

    switch (cmd)
    {
        case MFU_CMD_RESET:
            reboot(cmd);
            break;

        case MFU_CMD_QUERY_FIRMWARE_UPDATE_STATUS:
            query_firmware_update_status(cmd);
            break;

        case MFU_CMD_PROCESS_FIRMWARE_UPDATE:
            process_firmware_update_command(cmd, data->num_bytes - 1, &cmd_p->u.img_id);
            break;

        default :
            LOGW("Command not handled yet");
            send_response(cmd, MFU_CMD_STATUS_NOT_IMPLEMENTED, NULL, 0);
            break;
    }

    return APP_LIB_DATA_RECEIVE_RES_HANDLED;
}

void Wirepas_Meter_Firmware_Update_notify_status(firmware_update_status_t * status_p)
{
    send_response(MFU_CMD_PROCESS_FIRMWARE_UPDATE, MFU_CMD_STATUS_EXECUTED, (uint8_t *) status_p, sizeof(*status_p));
}

void Wirepas_Meter_Firmware_Update_init(void)
{
    Shared_Data_addDataReceivedCb(&m_mfu_packets_filter);
}
