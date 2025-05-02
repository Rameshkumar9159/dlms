/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#define DEBUG_LOG_MODULE_NAME "DAILY   "
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

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
#include "meter_clock.h" // MeterClock_get

#include "wirepas_com.h"

#include <malloc.h>

#define INVALID_DAILY_LOAD_START_TIME   0xFFFFFFFF

// Number of retries to read an incremented period
// if the period is in the past and the meter does not return any data
// It means we can recover 5 days per scheduling and converge faster
// to the current date if some data are missing w/o impacting the FSM scheduling
#define LATE_AND_NO_DATA_RETRY_NB       5

//Dailyload Profile OBIS
static obis_code_t c_dl_ln = { 1, 0, 99, 2, 0, 255 };
// OBIS of the generated push for the dailyload profile
static obis_code_t c_push_dl_ln = { 0, 6, 25, 9, 0, 255 };


// A daily is once a day. Try the reading every hour
// Could be dynamic with period of profile that we read
static const uint32_t m_period_ms = DAILYLOAD_PROFILE_DFLT_PERIOD_MS;

/* ************************************ */
/* FSM  For daily load profile       */

/* FSM handler */
static uint32_t read_daily_load_profile_fsm(void);

typedef enum
{
    DAILYLOAD_PROF_STATE_TAKE_LOCK,
    DAILYLOAD_PROF_STATE_CONNECT,
    DAILYLOAD_PROF_STATE_READ_PROFILE,
    DAILYLOAD_PROF_STATE_CLOSE,
    DAILYLOAD_PROF_STATE_SCHEDULE
} daily_load_prof_sfsm_state_e;

typedef struct
{
    int32_t result;
    uint32_t start;
    uint32_t next_start;
    daily_load_prof_sfsm_state_e state;
    uint8_t retry_nb;
} daily_load_prof_fsm_param_t;

static daily_load_prof_fsm_param_t m_param;

#define RESCHEDULE_ASAP()   \
            App_Scheduler_addTask_execTime(read_daily_load_profile_fsm, \
                                           APP_SCHEDULER_SCHEDULE_ASAP,\
                                           SFSM_EXECUTION_TIME_US)

static void on_lock_aquired_cb()
{
    m_param.state = DAILYLOAD_PROF_STATE_CONNECT;
    RESCHEDULE_ASAP();
}

static void open_cb(int32_t result)
{
    if (result == DLMS_ERROR_CODE_OK)
    {
        m_param.state = DAILYLOAD_PROF_STATE_READ_PROFILE;
        m_param.retry_nb = LATE_AND_NO_DATA_RETRY_NB;
    }
    else
    {
        m_param.state = DAILYLOAD_PROF_STATE_SCHEDULE;
    }
    RESCHEDULE_ASAP();
}

static void close_cb(int32_t result)
{
    m_param.state = DAILYLOAD_PROF_STATE_SCHEDULE;
    RESCHEDULE_ASAP();
}

static void on_data_sent_cb(const app_lib_data_sent_status_t * status)
{
    LOGI("Daily load profile sent with status: %d", status->success);
    if (status->success)
    {
        LOGI(" We can increment our interval");
        m_param.start = m_param.next_start;
        Client_Attribute_Manager_writeDailyLoadData(m_param.start);
    }
}

static void on_meter_read_cb(profile_generic_result_e result,
                             gxByteBuffer * profile_data,
                             uint32_t endtime,
                             uint32_t period)
{
    message push_message;
    uint32_t now;
    daily_load_prof_sfsm_state_e next_state = DAILYLOAD_PROF_STATE_CLOSE;

    MeterClock_get(&now, NULL);

    if (result == PROFILE_GENERIC_RESULT_OK)
    {
        uint16_t push_cfg;

        Server_Attribute_Manager_readProfilePushConfig(&push_cfg);
        if (DAILYLOAD_PUSH_PROF_IS_ENABLED(push_cfg))
        {
            mes_init(&push_message);
            if (Data_Notification_generatePushFromPull(MCM_AA_US, c_push_dl_ln,
                                                       profile_data,
                                                       &push_message))
            {
                // Remember end of this reading as potential next read
                m_param.next_start = endtime;
                Wirepas_com_send_message(push_message.data[0]->data,
                                         push_message.data[0]->size,
                                         on_data_sent_cb,
                                         WC_TYPE_DAILY_PROF);
            }
            mes_clear(&push_message);
        }
        else
        {
            LOGI("Dailyload push disabled");
            m_param.start = endtime;
            Client_Attribute_Manager_writeDailyLoadData(m_param.start);
        }
    }
    else if (result == PROFILE_GENERIC_RESULT_NO_DATA)
    {
        LOGI("No new data");
        // If the period is totally in the past, let's increment the start time
        if (endtime < now)
        {
            LOGW("Period in the past, incrementing start period from %u to %u", m_param.start, endtime);
            m_param.start = endtime;
            // As we are already connected to the meter, let's try
            // to read next period immediately if we are not out of trials
            if (m_param.retry_nb)
            {
                next_state = DAILYLOAD_PROF_STATE_READ_PROFILE;
                m_param.retry_nb--;
            }
        }
        else if (now < m_param.start)
        {
            LOGW("Period in the future, now %u vs %u", now, m_param.start);
        }
    }
    else
    {
        LOGE("on_meter_read_cb: %d", result);
        // Some meters return an error when trying to read data for a period in the future, log it
        if (now < m_param.start)
        {
            LOGW("Period in the future, now %u vs %u", now, m_param.start);
        }
    }
    m_param.state = next_state;

    RESCHEDULE_ASAP();
}

static uint32_t read_daily_load_profile_fsm(void)
{
    // default delay is "reschedule ASAP"
    uint32_t delay = SFSM_DELAY_PAUSED;
    Dlms_Lock_return_code_e lock_ret;

    SFSM_ENTRY(m_param.state);

    switch (m_param.state)
    {
        case DAILYLOAD_PROF_STATE_TAKE_LOCK:
            LOGI("Reading daily load profile");
            lock_ret = Dlms_lock_take(DLMS_LOCK_ID_DAILYLOAD,
                                      on_lock_aquired_cb,
                                      DLMS_LOCK_TYPE_WITHOUT_TRAFFIC);

            if (lock_ret == DLMS_LOCK_RET_ACQUIRED)
            {
                // Move to next state immediatelly
                m_param.state = DAILYLOAD_PROF_STATE_CONNECT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            else if (lock_ret != DLMS_LOCK_RET_WAITING_FOR_LOCK)
            {
                LOGE("Cannot take or wait for lock");
                delay = SFSM_DELAY_RETRY;
            }
            break;
        case DAILYLOAD_PROF_STATE_CONNECT:
            if (Meter_Connection_Management_open(open_cb, MCM_SECURED_AA) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_param.state = DAILYLOAD_PROF_STATE_SCHEDULE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case DAILYLOAD_PROF_STATE_READ_PROFILE:
            {
                uint32_t now;

                MeterClock_get(&now, NULL);
                // Check that we are not trying to read data for a period in the future
                if (now >= m_param.start)
                {
                    Profile_Generic_read_by_row_async(c_dl_ln,
                                                      m_param.start,
                                                      DEFAULT_DAILYLOAD_PERIOD_S,
                                                      on_meter_read_cb);
                }
                else
                {
                    LOGI("Period is in the future (now: %u < %u), skipping profile reading",
                         now , m_param.start);
                    m_param.state = DAILYLOAD_PROF_STATE_CLOSE;
                    delay = APP_SCHEDULER_SCHEDULE_ASAP;
                }
            }
            break;

        case DAILYLOAD_PROF_STATE_CLOSE: // close com
            if (Meter_Connection_Management_close(close_cb) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_param.state = DAILYLOAD_PROF_STATE_SCHEDULE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case DAILYLOAD_PROF_STATE_SCHEDULE:
            SFSM_CHECK();
            // No need to be very accurate for this scheduling as we are reading
            // many times "for nothing"
            delay = m_period_ms;

            Dlms_lock_release(DLMS_LOCK_ID_DAILYLOAD);
            m_param.state = DAILYLOAD_PROF_STATE_TAKE_LOCK;
            break;
    } /* switch (_state) */

    SFSM_EXIT();

    return delay;
}

bool Dailyload_isStartTimeValid(uint32_t * time_p)
{
    if (*time_p == INVALID_DAILY_LOAD_START_TIME)
    {
        *time_p = 0;
        return false;
    }
    return true;
}

void Dailyload_profile_start(uint32_t delay_ms)
{
    Client_Attribute_Manager_readDailyLoadData(&m_param.start);

    // Start main FSM
    LOGI("Starting read_daily_load_profile_fsm in %u ms", delay_ms);
    App_Scheduler_addTask_execTime(read_daily_load_profile_fsm,
                                   delay_ms,
                                   SFSM_EXECUTION_TIME_US);
}
