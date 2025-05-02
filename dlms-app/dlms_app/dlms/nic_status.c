/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#define DEBUG_LOG_MODULE_NAME "NIC_STAT"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#include "wirepas_com.h"
#include "include/cosem.h" // cosem2
#include "meter_connection_management.h"
#include "data_notification.h"
#include "nic_system_title.h"
#include "nic_status.h"
#include "firmware_update.h"
#include "server_attribute_manager.h"

// Default sending delay is 10 seconds to avoid frequent sending
#define SENDING_DELAY_MS                    10000

// NIC status is first sent every minute if NIC is not registered
#define RECURRING_SENDING_ORIGINAL_DELAY_MS (60 * 1000)
// Largest interval for recurring sending is 8 minutes
#define RECURRING_SENDING_MAX_DELAY_MS      (8 * RECURRING_SENDING_ORIGINAL_DELAY_MS)

typedef enum
{
    STATUS_NOT_TESTED = 0,
    STATUS_TESTED_OK = 1,
    STATUS_TESTED_KO = 2
} connection_status_e;

static uint32_t m_recurring_sending_delay_ms = RECURRING_SENDING_ORIGINAL_DELAY_MS;
typedef struct
{
    bool working;
    bool tested;
} status_level_t;

static status_level_t m_connection_status[APPLICATION_ASSOCIATION_FU + 1];
static uint8_t m_status_reason_bitfield = STATUS_REASON_NO_REASON;

static obis_code_t c_push_nic_status_ln = { 0, 105, 25, 9, 0, 255 };

// Generate the Meter connection status & NIC status reason.
static void compute_statuses(uint8_t * const connection_status,
                                uint8_t * const status_reason)
{
    uint8_t connection_test = 0;

    LOGI("Computing statuses.");
    // Compute the connection status
    *connection_status = 0;
    for (aa_type_e t = APPLICATION_ASSOCIATION_FIRST; t <= APPLICATION_ASSOCIATION_LAST; t++)
    {
        connection_test |= (1 << t);
        *connection_status |= (m_connection_status[t].working & 0x1) << t;
    }

    // Compute the NIC status reason
    if (!(*connection_status & 0x1))
    {
        // PC not working
        *status_reason |= STATUS_REASON_COMMUNICATION_PROBLEM;
    }

    if (*connection_status != connection_test)
    {
        // At least one association not working
        *status_reason |= STATUS_REASON_METER_ASSOCIATION_ISSUES;
    }
}

static void on_data_sent_cb(const app_lib_data_sent_status_t * status)
{

    LOGI("Nic status sent, status: %d", status->success);
    if (!status->success)
    {
        // Nic status was discarded, probably no route to sink.
        // Send a new one until successful sending.
        Nic_status_generate_and_send_notification(STATUS_REASON_NO_REASON);
    }
    else
    {
        bool registered;
        Server_Attribute_Manager_readNicRegistrationStatus(&registered);
        // If we are not registered, we need to send a recurring NIC status
        // with the same status reason except for the NIC reboot bit
        if (registered)
        {
            // NIC status has been sent, reset the status reason
            m_status_reason_bitfield = STATUS_REASON_NO_REASON;
        }
        else
        {
            m_status_reason_bitfield &= ~STATUS_REASON_NIC_REBOOT;
        }
    }
}

// Generate the NIC status push message
static bool generate_push(message * push_msg_p)
{
    gxByteBuffer bb;
    const uint8_t * info_p = NULL;
    uint8_t info_len = 0;
    uint32_t ic;
    uint8_t connection_status;
    bool rc;

    bb_init(&bb);

    // Generate the NIC status body
    //~ 0 NIC system Title
    //~ 1 NIC server US invocation counter
    //~ 2 DLMS APP Firmware Version
    //~ 3 NIC Status Reason
    //~ 4 Meter connection Bitfield of 8 bits
    //~ 5 Meter Serial Number

    // First add a structure:
    if (cosem_setStructure(&bb, 6) != DLMS_ERROR_CODE_OK)
    {
        LOGE("Cannot add initial struct");
        // Do not return to still send something
    }
    // Set NIC system Title
    Nic_ST_get(&info_p, &info_len);
    if (cosem_setOctetString2(&bb, info_p, info_len) != DLMS_ERROR_CODE_OK)
    {
        LOGE("Cannot add NIC ST");
        // Do not return to still send something
    }

    // Set US invocation counter
    Server_Attribute_Manager_readDecryptInvocationCounter(&ic);
    if (cosem_setUInt32(&bb, ic) != DLMS_ERROR_CODE_OK)
    {
        LOGE("Cannot add US IC");
    }

    // Set DLMS app FW version
    if (cosem_setOctetString2(&bb, (const uint8_t *)OFFICIAL_TAG, strlen(OFFICIAL_TAG)) != DLMS_ERROR_CODE_OK)
    {
        LOGE("Cannot add DLMS app FW version");
        // Do not return to still send something
    }

    // Compute the connection status
    compute_statuses(&connection_status, &m_status_reason_bitfield);

    // Add NIC status reason bitfield
    if (cosem_setBitString(&bb, m_status_reason_bitfield, 8) != DLMS_ERROR_CODE_OK)
    {
        LOGE("Cannot add NIC status reason");
    }

    // Add connection status bitfield (last 4 bits are RFU)
    if (cosem_setBitString(&bb, connection_status, 8) != DLMS_ERROR_CODE_OK)
    {
        LOGE("Cannot add connection status bitfield");
    }

    // Set meter SN
    Server_Attribute_Manager_readMeterSerialNumber(&info_p, &info_len);
    if (cosem_setOctetString2(&bb, info_p, info_len) != DLMS_ERROR_CODE_OK)
    {
        LOGE("Cannot add meter SN");
        // Do not return to still send something
    }

    // Generate the data notification
    rc =  Data_Notification_generatePushFromPull(MCM_AA_PC, c_push_nic_status_ln,
                                                 &bb, push_msg_p);
    bb_clear(&bb);
    return rc;
}

static uint32_t send_nic_status_task(void)
{
    message push_message;
    gxByteBuffer bb;
    uint32_t delay = APP_SCHEDULER_STOP_TASK;
    bool registered;

    mes_init(&push_message);

    // Generate the notification
    LOGD("Sending nic status");
    if (generate_push(&push_message))
    {
        if (!Wirepas_com_send_message(push_message.data[0]->data,
                                      push_message.data[0]->size,
                                      on_data_sent_cb,
                                      WC_TYPE_NIC_STATUS))
        {
            LOGE("Cannot send NIC status");
            delay = SENDING_DELAY_MS;
        }
    }
    else
    {
        LOGE("Cannot generate NIC status");
        delay = SENDING_DELAY_MS;
    }
    mes_clear(&push_message);

    // Clear our temporary buffer
    bb_clear(&bb);

    Server_Attribute_Manager_readNicRegistrationStatus(&registered);

    if (!registered && delay == APP_SCHEDULER_STOP_TASK)
    {
        // If the NIC is not registered, we keep sending the NIC status
        // with an increased interval up to 8 minutes
        delay = m_recurring_sending_delay_ms;

        if (m_recurring_sending_delay_ms < RECURRING_SENDING_MAX_DELAY_MS)
        {
            m_recurring_sending_delay_ms <<= 1;
        }
    }

    return delay;
}

void Nic_status_generate_and_send_notification(status_reason_e status_reason)
{
    m_status_reason_bitfield |= status_reason;
    App_Scheduler_addTask_execTime(send_nic_status_task, SENDING_DELAY_MS,
                                   SFSM_EXECUTION_TIME_US);
}

void Nic_status_set_aa_state(aa_type_e type, bool can_connect)
{
    if (m_connection_status[type].tested &&
        (m_connection_status[type].working == can_connect))
    {
        // Type was already tested and same status so nothing to do
        return;
    }

    // Update type state
    m_connection_status[type].working = can_connect;
    m_connection_status[type].tested = true;

    // If cannot connect in PC, assume the other types cannot connect too
    if (type == APPLICATION_ASSOCIATION_PC && !can_connect)
    {
        m_connection_status[APPLICATION_ASSOCIATION_MR].working = false;
        m_connection_status[APPLICATION_ASSOCIATION_US].working = false;
        m_connection_status[APPLICATION_ASSOCIATION_FU].working = false;
    }

    // Check if all level are checked to determine if status can be sent
    for (aa_type_e t = APPLICATION_ASSOCIATION_FIRST; t <= APPLICATION_ASSOCIATION_LAST; t++)
    {
        if (!m_connection_status[t].tested)
        {
            // At least one type not tested do not send status
            return;
        }
    }

    LOGD("Send nic status");
    // At this stage no reason, it will be computed when generating the Nic status
    Nic_status_generate_and_send_notification(STATUS_REASON_NO_REASON);
}

void Nic_status_compute_statuses(uint8_t * const connection_status,
                                    uint8_t * const status_reason)
{
    LOGI("Read statuses");
    compute_statuses(connection_status, status_reason);
}
