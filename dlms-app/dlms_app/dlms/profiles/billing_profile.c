/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#define DEBUG_LOG_MODULE_NAME "BILLING "
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#include "crc.h"

#include "dlms_com.h"
#include "common.h"
#include "meter_connection_management.h"
#include "profile_generic.h"
#include "dlms_lock.h"
#include "profiles_config.h"
#include "client_attribute_manager.h"
#include "data_notification.h"
#include "rng.h"
#include "server_attribute_manager.h"

#include "wirepas_com.h"

#include <malloc.h>

#include "include/cosem.h" // cosem2
#include "include/gxmem.h" // gxcalloc
#include "include/client.h" // cl_readLN

// A billing is once a month. Try the reading every 6 hours
// Can't be dynamic with period of profile that we read
static const uint32_t m_period_ms = BILLING_PROFILE_INITIAL_AND_DEFAULT_PERIOD_MS;

/* FSM handler */
static uint32_t read_billing_profile_fsm(void);

typedef enum
{
    BILLING_PROF_STATE_TAKE_LOCK,
    BILLING_PROF_STATE_CONNECT,
    BILLING_PROF_STATE_READ_PROFILE_ENTRIES,
    BILLING_PROF_STATE_READ_ENTRIES_IN_USE,
    BILLING_PROF_STATE_READ_BILLING_ENTRY,
    BILLING_PROF_STATE_CLOSE,
    BILLING_PROF_STATE_SCHEDULE
} billing_prof_sfsm_state_e;

typedef struct
{
    // Variable required to read object
    message  msg;
    gxReplyData  reply;
} billing_prof_data_t;

typedef struct
{
    int32_t result;
    billing_status_t status;
    uint16_t new_crc;
    billing_prof_sfsm_state_e state;
} billing_prof_fsm_param_t;

static billing_prof_fsm_param_t m_params;
static billing_prof_data_t * m_data_p;

#define RESCHEDULE_ASAP()   \
                App_Scheduler_addTask_execTime(read_billing_profile_fsm, \
                                               APP_SCHEDULER_SCHEDULE_ASAP,\
                                               SFSM_EXECUTION_TIME_US)

// Billing Profile OBIS
static obis_code_t c_bp_ln = { 1, 0, 98, 1, 0, 255 };
// OBIS of the generated push for the billing profile
static obis_code_t c_push_bp_ln = { 0, 103, 25, 9, 0, 255 };

static void on_lock_aquired_cb()
{
    m_params.state = BILLING_PROF_STATE_CONNECT;
    RESCHEDULE_ASAP();
}

static void open_cb(int32_t result)
{
    m_params.result = result;
    if (DLMS_ERROR_CODE_OK == m_params.result)
    {
        m_params.state = BILLING_PROF_STATE_READ_PROFILE_ENTRIES;
    }
    else
    {
        m_params.state = BILLING_PROF_STATE_SCHEDULE;
    }
    RESCHEDULE_ASAP();
}

static void close_cb(int32_t result)
{
    m_params.result = result;
    m_params.state = BILLING_PROF_STATE_SCHEDULE;
    RESCHEDULE_ASAP();
}

static void read_profile_entries_cb(int32_t result)
{
    billing_prof_sfsm_state_e next_state = BILLING_PROF_STATE_CLOSE;

    if (DLMS_ERROR_CODE_OK == result && m_data_p->reply.dataValue.vt == DLMS_DATA_TYPE_UINT32)
    {
        uint32_t profiles_entries = m_data_p->reply.dataValue.ulVal;
        // We succesfuly read the number of profile entries in the buffer.
        LOGI("Number of profile entries: %u", profiles_entries);
        // Sanity check
        if (profiles_entries > UINT16_MAX)
        {
            LOGE("uint16_t type too small to store profile entries value");
        }
        else
        {
            // If the entriesInUse is zero, no need to test
            m_params.status.profile_entries = profiles_entries;
            next_state = BILLING_PROF_STATE_READ_ENTRIES_IN_USE;
        }
    }
    else
    {
        LOGE("Read profile entries attr: %d", result);
    }

    // Clear memory for request/reply
    mes_clear(&m_data_p->msg);
    reply_clear(&m_data_p->reply);

    m_params.state = next_state;
    RESCHEDULE_ASAP();
}

static void read_entries_in_use_cb(int32_t result)
{
    billing_prof_sfsm_state_e next_state = BILLING_PROF_STATE_CLOSE;

    if (DLMS_ERROR_CODE_OK == result && m_data_p->reply.dataValue.vt == DLMS_DATA_TYPE_UINT32)
    {
        uint32_t entries_in_use = m_data_p->reply.dataValue.ulVal;
        // We succesfuly read the number of entries in use in the buffer.
        LOGI("Entries in use: %u", entries_in_use);

        // If the entries in use is zero, there is no entry
        if (! entries_in_use)
        {
            LOGI("No entry");
        }
        // Sanity check
        else if (entries_in_use > m_params.status.profile_entries)
        {
            LOGW("EIU > PE");
        }
        else if (entries_in_use == 1)
        {
            LOGI("One entry only (current entry)");
        }
        else
        {
            m_params.status.current_entry = entries_in_use - 1;
            // We are ready to read the entry given by m_params.status.current_entry
            next_state = BILLING_PROF_STATE_READ_BILLING_ENTRY;
        }
    }
    else
    {
        LOGE("Read entries in use attr: %d", result);
    }

    // Clear memory for request/reply
    mes_clear(&m_data_p->msg);
    reply_clear(&m_data_p->reply);

    m_params.state = next_state;
    RESCHEDULE_ASAP();
}

static void on_last_period_sent_cb(const app_lib_data_sent_status_t * status)
{
    LOGI("Billing profile sent with status: %d",
        status->success);
    // If message has been sent successfully, save the status
    // in persistent memory
    if (status->success)
    {
        m_params.status.crc = m_params.new_crc;
        Client_Attribute_Manager_writeBillingData(&m_params.status);
    }
}

static void generate_and_send_push(gxByteBuffer * profile_data,
                                   app_lib_data_data_sent_cb_f sent_cb)
{
    message push_message;

    mes_init(&push_message);
    if (! Data_Notification_generatePushFromPull(MCM_AA_US, c_push_bp_ln,
                                                profile_data,
                                                &push_message))
    {
        LOGE("Failed to generate push");
    }
    else if (! Wirepas_com_send_message(push_message.data[0]->data,
                                        push_message.data[0]->size,
                                        sent_cb,
                                        WC_TYPE_BILLING_PROF))
    {
        LOGE("Failed to send push");
    }
    mes_clear(&push_message);
}

static void on_meter_read_cb(profile_generic_result_e result,
                            gxByteBuffer * profile_data,
                            uint32_t endtime,
                            uint32_t period)
{
    uint16_t push_cfg;

    m_params.result = result;

    if (result == PROFILE_GENERIC_RESULT_OK)
    {
        // Compute crc
        m_params.new_crc = Crc_fromBuffer(profile_data->data, profile_data->size);

        LOGI("old CRC: 0x%04X, new CRC: 0x%04X",
            m_params.status.crc, m_params.new_crc);
        if (m_params.new_crc != m_params.status.crc)
        {
            Server_Attribute_Manager_readProfilePushConfig(&push_cfg);
            if (BILLING_PUSH_PROF_IS_ENABLED(push_cfg))
            {
                LOGI("Sending billing period data");

                generate_and_send_push(profile_data, on_last_period_sent_cb);
            }
            else
            {
                LOGI("Billing push disabled");
                m_params.status.crc = m_params.new_crc;
                Client_Attribute_Manager_writeBillingData(&m_params.status);
            }
        }
        else
        {
            LOGI("Billing period data unchanged, nothing to send");
        }
    }
    else
    {
        LOGI("Read entry from ProfileGeneric: %d", result);
    }

    m_params.state = BILLING_PROF_STATE_CLOSE;

    RESCHEDULE_ASAP();
}

static uint32_t read_billing_profile_fsm(void)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    int res = -1;
    // default delay is paused
    uint32_t delay = SFSM_DELAY_PAUSED;
    Dlms_Lock_return_code_e lock_ret;

    SFSM_ENTRY(m_params.state);

    switch (m_params.state)
    {
        case BILLING_PROF_STATE_TAKE_LOCK:
            LOGI("Reading billing profile");
            lock_ret = Dlms_lock_take(DLMS_LOCK_ID_BILLING,
                                      on_lock_aquired_cb,
                                      DLMS_LOCK_TYPE_WITHOUT_TRAFFIC);

            if (lock_ret == DLMS_LOCK_RET_ACQUIRED)
            {
                // Move to next state immediatelly
                m_params.state = BILLING_PROF_STATE_CONNECT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            else if (lock_ret != DLMS_LOCK_RET_WAITING_FOR_LOCK)
            {
                LOGE("Cannot take or wait for lock");
                delay = SFSM_DELAY_RETRY;
            }
            break;

        case BILLING_PROF_STATE_CONNECT:
            if (Meter_Connection_Management_open(open_cb, MCM_SECURED_AA) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_params.state = BILLING_PROF_STATE_SCHEDULE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case BILLING_PROF_STATE_READ_PROFILE_ENTRIES:
            if (! (m_data_p = gxcalloc(1, sizeof *m_data_p)))
            {
                LOGE("Malloc failed");
                m_params.state = BILLING_PROF_STATE_CLOSE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            else
            {
                // Read profile entries attribute of the profile generic
                LOGI("Reading attr profile entries");

                mes_init(&m_data_p->msg);
                reply_init(&m_data_p->reply);

                res = cl_readLN(ms_p, c_bp_ln,
                    DLMS_OBJECT_TYPE_PROFILE_GENERIC,
                    8, NULL, &m_data_p->msg);

                if (DLMS_ERROR_CODE_OK == res)
                {
                    Dlms_Com_call_async(&m_data_p->msg, &m_data_p->reply, read_profile_entries_cb);
                    delay = SFSM_DELAY_PAUSED;
                }
                else
                {
                    LOGE("cl_readLN profiles entries: %d", res);
                    m_params.state = BILLING_PROF_STATE_CLOSE;
                    delay = APP_SCHEDULER_SCHEDULE_ASAP;
                }
            }
            break;

        case BILLING_PROF_STATE_READ_ENTRIES_IN_USE:
            // Read entries in use attribute of the profile generic
            LOGI("Reading attr entries in use");

            mes_init(&m_data_p->msg);
            reply_init(&m_data_p->reply);

            res = cl_readLN(ms_p, c_bp_ln,
                DLMS_OBJECT_TYPE_PROFILE_GENERIC,
                7, NULL, &m_data_p->msg);

            if (DLMS_ERROR_CODE_OK == res)
            {
                Dlms_Com_call_async(&m_data_p->msg, &m_data_p->reply, read_entries_in_use_cb);
                delay = SFSM_DELAY_PAUSED;
            }
            else
            {
                LOGE("cl_readLN entries in use: %d", res);
                m_params.state = BILLING_PROF_STATE_CLOSE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }

            break;

        case BILLING_PROF_STATE_READ_BILLING_ENTRY:
            Profile_Generic_read_by_entry_async(c_bp_ln,
                                                m_params.status.current_entry,
                                                1,
                                                on_meter_read_cb);
            break;

        case BILLING_PROF_STATE_CLOSE: // close com
            if (m_data_p)
            {
                gxfree(m_data_p);
                m_data_p = NULL;
            }
            if (Meter_Connection_Management_close(close_cb) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_params.state = BILLING_PROF_STATE_SCHEDULE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case BILLING_PROF_STATE_SCHEDULE:
            SFSM_CHECK();

            // No need to be very accurate for this scheduling as we are reading
            // many times "for nothing"
            delay = m_period_ms;

            Dlms_lock_release(DLMS_LOCK_ID_BILLING);
            m_params.state = BILLING_PROF_STATE_TAKE_LOCK;
            break;
    } /* switch (_state) */

    SFSM_EXIT();

    return delay;
}

void Billing_profile_start(uint32_t delay_ms)
{
    Client_Attribute_Manager_readBillingData(&m_params.status);

    // Start main FSM
    LOGI("Starting read_billing_profile_fsm in %u ms", delay_ms);
    App_Scheduler_addTask_execTime(read_billing_profile_fsm,
                                   delay_ms,
                                   SFSM_EXECUTION_TIME_US);
}
