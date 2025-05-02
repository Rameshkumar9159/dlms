/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */
#include <stdio.h>

#define DEBUG_LOG_MODULE_NAME "NAME_PLT"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"


#include "dlms_com.h"
#include "common.h"
#include "meter_connection_management.h"
#include "profile_generic.h"
#include "dlms_lock.h"
#include "wirepas_com.h"
#include "name_plate_profile.h"
#include "data_notification.h"
#include "nic_status.h"

#include "include/cosem.h" // cosem2
#include "include/notify.h" // notify_generateDataNotificationMessages2
#include "include/client.h" // cl_readLN

#include <malloc.h>

static obis_code_t c_name_plate_ln = { 0, 0, 94, 91, 10, 255 };
static obis_code_t c_push_name_plate_ln = { 0, 104, 25, 9, 0, 255 };

/* FSM handler */
static uint32_t read_name_plate_profile_fsm(void);

/* Delay to retry to read name plate profile */
/* If cannot be read (wrong keys), it is also the period to send NIC status */
/* So if there was no key received in between, result will be same */
#define NAME_PLATE_RETRY_DELAY_MS   (60 * 1000)

typedef enum
{
    NP_PROF_STATE_TAKE_LOCK,
    NP_PROF_STATE_CONNECT,
    NP_PROF_STATE_READ_NAME_PLATE,
    NP_PROF_STATE_CLOSE,
    NP_PROF_STATE_SCHEDULE
} np_prof_sfsm_state_e;

typedef struct
{
    np_prof_sfsm_state_e state;
    dlms_np_read_name_plate_cb cb;
    bool np_read;
} np_prof_fsm_param_t;

static np_prof_fsm_param_t m_param;

#define RESCHEDULE_ASAP()   App_Scheduler_addTask_execTime(read_name_plate_profile_fsm, \
                                                APP_SCHEDULER_SCHEDULE_ASAP,\
                                                SFSM_EXECUTION_TIME_US)

static void on_data_sent_cb(const app_lib_data_sent_status_t * status)
{
    LOGI("Name plate profile sent, status: %d", status->success);
    // TODO add logic here or in wirepas_com_module to handle the retry
}

static void on_lock_aquired_cb()
{
    // lock is for us \o/
    m_param.state = NP_PROF_STATE_CONNECT;
    RESCHEDULE_ASAP();
}

static void open_cb(int32_t result)
{
    if (result == DLMS_ERROR_CODE_OK)
    {
        m_param.state = NP_PROF_STATE_READ_NAME_PLATE;
    }
    else
    {
        m_param.state = NP_PROF_STATE_SCHEDULE;
        // Cannot open association
        Nic_status_generate_and_send_notification(STATUS_REASON_METER_ASSOCIATION_ISSUES);
    }

    RESCHEDULE_ASAP();
}

static void close_cb(int32_t result)
{
    m_param.state = NP_PROF_STATE_SCHEDULE;
    RESCHEDULE_ASAP();
}

static void read_name_plate_cb(profile_generic_result_e result,
                               gxByteBuffer * profile_data,
                               uint32_t endtime,
                               uint32_t period)
{
    message push_message;

    if (result == PROFILE_GENERIC_RESULT_OK)
    {
        mes_init(&push_message);
        if (Data_Notification_generatePushFromPull(MCM_AA_US,
                                                   c_push_name_plate_ln,
                                                   profile_data,
                                                   &push_message))
        {
            // Todo: check return code
            Wirepas_com_send_message(push_message.data[0]->data,
                                     push_message.data[0]->size,
                                     on_data_sent_cb,
                                     WC_TYPE_NAMEPLATE);
            m_param.np_read = true;
        }
        mes_clear(&push_message);
    }
    else
    {
        LOGE("Read ProfileGeneric: %d", result);
    }
    m_param.state = NP_PROF_STATE_CLOSE;

    RESCHEDULE_ASAP();
}

static uint32_t read_name_plate_profile_fsm(void)
{
    uint32_t delay = SFSM_DELAY_PAUSED;
    Dlms_Lock_return_code_e lock_ret;

    SFSM_ENTRY(m_param.state);

    switch (m_param.state)
    {
        case NP_PROF_STATE_TAKE_LOCK:
            LOGI("Reading name plate profile");
            lock_ret = Dlms_lock_take(DLMS_LOCK_ID_NAME_PLATE,
                                      on_lock_aquired_cb,
                                      DLMS_LOCK_TYPE_WITHOUT_TRAFFIC);

            if (lock_ret == DLMS_LOCK_RET_ACQUIRED)
            {
                // Move to next state immediatelly
                m_param.state = NP_PROF_STATE_CONNECT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            else if (lock_ret != DLMS_LOCK_RET_WAITING_FOR_LOCK)
            {
                LOGE("Cannot take or wait for lock");
                delay = SFSM_DELAY_RETRY;
            }
            break;

        case NP_PROF_STATE_CONNECT:
            if (Meter_Connection_Management_open(open_cb, MCM_SECURED_AA) !=
                                                          DLMS_ERROR_CODE_OK)
            {
                m_param.state = NP_PROF_STATE_SCHEDULE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case NP_PROF_STATE_READ_NAME_PLATE:
            Profile_Generic_read_all_async(c_name_plate_ln,
                                           read_name_plate_cb);
            break;

        case NP_PROF_STATE_CLOSE: // close com
            if (Meter_Connection_Management_close(close_cb) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_param.state = NP_PROF_STATE_SCHEDULE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case NP_PROF_STATE_SCHEDULE:
            SFSM_CHECK();
            Dlms_lock_release(DLMS_LOCK_ID_NAME_PLATE);
            m_param.state = NP_PROF_STATE_TAKE_LOCK;
            if (! m_param.np_read)
            {
                // We were not able to read, start again
                delay = NAME_PLATE_RETRY_DELAY_MS;
            }

            if (m_param.cb != NULL)
            {
                m_param.cb(m_param.np_read);
            }
            break;
    } /* switch (_state) */

    SFSM_EXIT();

    return delay;
}
/*****************************/

void Name_Plate_Profile_read(dlms_np_read_name_plate_cb cb)
{
    // Start main FSM
    LOGI("Starting read_name_plate_profile_fsm");

    m_param.cb = cb;
    m_param.np_read = false;
    m_param.state = NP_PROF_STATE_TAKE_LOCK;
    App_Scheduler_addTask_execTime(read_name_plate_profile_fsm,
                                   APP_SCHEDULER_SCHEDULE_ASAP,
                                   SFSM_EXECUTION_TIME_US);
    return;
}
