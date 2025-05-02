/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#define DEBUG_LOG_MODULE_NAME "BLK_LOAD"
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
#include "meter_clock.h"
#include "server_attribute_manager.h"

#include "wirepas_com.h"

#include <malloc.h>

// This constant determine how many Block load can
// be sent while we are "out of sync" with meter
// We limit it to 3 to avoid sending too much traffic
#define MAX_NUMBER_OF_PUBLISH_PER_PERIOD    3

#define INVALID_BLOCK_LOAD_START_TIME       0xFFFFFFFF

// Blockload Profile OBIS
static obis_code_t c_bl_ln = { 1, 0, 99, 1, 0, 255 };
// OBIS of the generated push for the blockload profile
static obis_code_t c_push_bl_ln = { 0, 5, 25, 9, 0, 255 };

/* ************************************ */
/* FSM  For block load profile       */

/* FSM handler */
static uint32_t read_block_load_profile_fsm(void);

typedef enum
{
    BLOCKLOAD_PROF_STATE_TAKE_LOCK,
    BLOCKLOAD_PROF_STATE_CONNECT,
    BLOCKLOAD_PROF_STATE_READ_PROFILE,
    BLOCKLOAD_PROF_STATE_CLOSE,
    BLOCKLOAD_PROF_STATE_SCHEDULE
} block_load_prof_sfsm_state_e;

typedef enum
{
    BLOCKLOAD_ONGOING_PERIOD_SUPPORT_UNKNOWN,
    BLOCKLOAD_ONGOING_PERIOD_NOT_SUPPORTED,
    BLOCKLOAD_ONGOING_PERIOD_SUPPORTED
} block_load_ongoing_period_support_e;

typedef struct
{
    uint32_t start;
    uint32_t next_start;
    uint32_t next_delay_s;
    uint32_t period_s;
    block_load_prof_sfsm_state_e state;
    block_load_ongoing_period_support_e og_period_support;
    bool new_randomization_needed;
} block_load_prof_fsm_param_t;

static block_load_prof_fsm_param_t m_param;

#define RESCHEDULE_ASAP()   \
                App_Scheduler_addTask_execTime(read_block_load_profile_fsm, \
                                               APP_SCHEDULER_SCHEDULE_ASAP,\
                                               SFSM_EXECUTION_TIME_US)

static void on_lock_aquired_cb()
{
    m_param.state = BLOCKLOAD_PROF_STATE_CONNECT;
    RESCHEDULE_ASAP();
}

static void open_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result)
    {
        m_param.state = BLOCKLOAD_PROF_STATE_READ_PROFILE;
    }
    else
    {
        m_param.state = BLOCKLOAD_PROF_STATE_SCHEDULE;
    }
    RESCHEDULE_ASAP();
}

static void close_cb(int32_t result)
{
    m_param.state = BLOCKLOAD_PROF_STATE_SCHEDULE;
    RESCHEDULE_ASAP();
}

static void on_data_sent_cb(const app_lib_data_sent_status_t * status)
{
    LOGI("Block load profile sent with status: %d", status->success);
    if (status->success)
    {
        LOGI("Set start of interval to %u", m_param.next_start);
        // Last reading was sent, we can move forward and start a new reading
        m_param.start = m_param.next_start;
        Client_Attribute_Manager_writeBlockLoadData(m_param.start);
    }
}

static void on_meter_read_cb(profile_generic_result_e result,
                             gxByteBuffer * profile_data,
                             uint32_t endtime,
                             uint32_t period)
{
    uint32_t now;
    message push_message;
    block_load_prof_sfsm_state_e next_state = BLOCKLOAD_PROF_STATE_CLOSE;

    m_param.period_s = period;
    if (result == PROFILE_GENERIC_RESULT_OK || result == PROFILE_GENERIC_RESULT_MULTIPLE_ROW)
    {
        uint16_t push_cfg;

        Server_Attribute_Manager_readProfilePushConfig(&push_cfg);
        if (BLOCKLOAD_PUSH_PROF_IS_ENABLED(push_cfg))
        {
            mes_init(&push_message);
            if (Data_Notification_generatePushFromPull(MCM_AA_US, c_push_bl_ln,
                                                       profile_data,
                                                       &push_message))
            {
                // Remenber end of this reading as potential next read
                m_param.next_start = endtime;
                // Todo: check return code
                Wirepas_com_send_message(push_message.data[0]->data,
                                         push_message.data[0]->size,
                                         on_data_sent_cb,
                                         WC_TYPE_BLOCK_LOAD_PROF);
            }
            mes_clear(&push_message);
        }
        else
        {
            LOGI("Blockload push disabled");
            m_param.start = endtime;
            Client_Attribute_Manager_writeBlockLoadData(m_param.start);
        }

        // Check if there are potentially more reading available
        MeterClock_get(&now, NULL);
        // Some meters return data even if the period is ongoing
        if (((endtime < now) && m_param.og_period_support != BLOCKLOAD_ONGOING_PERIOD_NOT_SUPPORTED) ||
            ((endtime + period < now) && m_param.og_period_support == BLOCKLOAD_ONGOING_PERIOD_NOT_SUPPORTED))
        {
            // We are late (out of sync)
            LOGW("Late: %u vs %u", endtime + period, now);
            m_param.next_delay_s =  period / (MAX_NUMBER_OF_PUBLISH_PER_PERIOD);
            // No randomization yet but will be needed when we are not late
            m_param.new_randomization_needed = true;
        }
        else
        {
            if (m_param.new_randomization_needed)
            {
                // back in sync
                if (endtime >= now)
                {
                    m_param.next_delay_s = endtime - now + Rng_number(period * 0.9);
                    m_param.og_period_support = BLOCKLOAD_ONGOING_PERIOD_SUPPORTED;
                }
                else if (endtime + period >= now)
                {
                    m_param.next_delay_s = endtime + period - now + Rng_number(period * 0.9);
                }
                m_param.new_randomization_needed = false;
                LOGI("Randomization needed: next read in %u s", m_param.next_delay_s);
            }
            else
            {
                // Still in sync
                m_param.next_delay_s = period;
                LOGI("No randomization needed: next read in %u s", m_param.next_delay_s);
            }
        }
    }
    else if (result == PROFILE_GENERIC_RESULT_NO_DATA)
    {
        uint32_t start_time = endtime - period;
        // It should always happen on first boot
        MeterClock_get(&now, NULL);
        // Trying to read before the beginning of the period
        if (now < start_time)
        {
            m_param.next_delay_s = period;
            LOGE("Trying to read period in the future, now: %u period: %u - %u", now , start_time, endtime);
        }
        // Trying to read during the period, some meters return data, some meters don't
        else if (endtime >= now)
        {   // It is normal that nothing was read, as we are in the middle of the
            // profile to read.
            // It is probably first boot, we just have to randomize the next reading
            // based on its expected available time in meter.
            // Expected time is in: endtime  - now
            // Add randomization for the period
            m_param.og_period_support = BLOCKLOAD_ONGOING_PERIOD_NOT_SUPPORTED;
            m_param.next_delay_s = endtime - now + Rng_number(period * 0.9);
            LOGI("Next block load in %u s (%u)", m_param.next_delay_s, endtime - now);
            m_param.new_randomization_needed = false;
        }
        else
        {
            // Period ended but there is no data available
            LOGE("No new data, out of sync");
            // Reset the start time to resynchronize with the meter
            // in order to not get stuck here forever
            m_param.start = 0;
            // Retry immediately as the connection is already opened
            next_state = BLOCKLOAD_PROF_STATE_READ_PROFILE;
            m_param.new_randomization_needed = true;
        }
    }
    else
    {
        LOGE("on_meter_read_cb: %d", result);
        m_param.next_delay_s = period;
    }
    m_param.state = next_state;

    RESCHEDULE_ASAP();
}

static uint32_t read_block_load_profile_fsm(void)
{
    uint32_t delay = SFSM_DELAY_PAUSED;
    Dlms_Lock_return_code_e lock_ret;
    uint32_t now;

    SFSM_ENTRY(m_param.state);

    switch (m_param.state)
    {
        case BLOCKLOAD_PROF_STATE_TAKE_LOCK:
            LOGI("Reading block load profile");
            lock_ret = Dlms_lock_take(DLMS_LOCK_ID_BLOCKLOAD,
                                      on_lock_aquired_cb,
                                      DLMS_LOCK_TYPE_WITHOUT_TRAFFIC);

            if (lock_ret == DLMS_LOCK_RET_ACQUIRED)
            {
                // Move to next state immediatelly
                m_param.state = BLOCKLOAD_PROF_STATE_CONNECT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            else if (lock_ret != DLMS_LOCK_RET_WAITING_FOR_LOCK)
            {
                LOGE("Cannot take or wait for lock");
                delay = SFSM_DELAY_RETRY;
            }
            break;
        case BLOCKLOAD_PROF_STATE_CONNECT:
            if (Meter_Connection_Management_open(open_cb, MCM_SECURED_AA) !=
                                                    DLMS_ERROR_CODE_OK)
            {
                m_param.state = BLOCKLOAD_PROF_STATE_SCHEDULE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case BLOCKLOAD_PROF_STATE_READ_PROFILE:
            // We read the meter clock in the previous state so let's check
            // here that we are not trying to read a period in the future
            // start time may be null but this is not an issue here
            MeterClock_get(&now, NULL);
            if (now < m_param.start)
            {
                uint32_t period_s;
                LOGI("Period is in the future (now: %u < %u), skipping profile reading",
                     now , m_param.start);
                if (m_param.period_s)
                {
                    period_s = m_param.period_s;
                }
                else
                {
                    period_s = DEFAULT_BLOCKLOAD_PERIOD_S;
                }

                if (m_param.new_randomization_needed)
                {
                    m_param.next_delay_s = m_param.start - now + Rng_number(period_s * 0.9);
                    // We probably don't know it at this stage but it does not hurt to check
                    if (m_param.og_period_support == BLOCKLOAD_ONGOING_PERIOD_NOT_SUPPORTED)
                    {
                        m_param.next_delay_s += period_s;
                    }
                    m_param.new_randomization_needed = false;
                    LOGI("Randomization needed: next read in %u s", m_param.next_delay_s);
                }
                else
                {
                    // Still in sync
                    m_param.next_delay_s = period_s;
                    LOGI("No randomization needed: next read in %u s", m_param.next_delay_s);
                }
                m_param.state = BLOCKLOAD_PROF_STATE_CLOSE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            else
            {
                Profile_Generic_read_by_row_async(c_bl_ln,
                                                  m_param.start,
                                                  DEFAULT_BLOCKLOAD_PERIOD_S,
                                                  on_meter_read_cb);
            }
            break;

        case BLOCKLOAD_PROF_STATE_CLOSE: // close com
            if (Meter_Connection_Management_close(close_cb) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_param.state = BLOCKLOAD_PROF_STATE_SCHEDULE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case BLOCKLOAD_PROF_STATE_SCHEDULE:
            SFSM_CHECK();

            delay = m_param.next_delay_s * 1000;

            LOGI("Next reading in %d ms", delay);

            Dlms_lock_release(DLMS_LOCK_ID_BLOCKLOAD);
            m_param.state = BLOCKLOAD_PROF_STATE_TAKE_LOCK;
            break;
    } /* switch (_state) */

    SFSM_EXIT();

    return delay;
}

bool Blockload_isStartTimeValid(uint32_t * time_p)
{
    if (*time_p == INVALID_BLOCK_LOAD_START_TIME)
    {
        *time_p = 0;
        return false;
    }
    return true;
}

void Blockload_profile_start(void)
{
    uint32_t now;
    Client_Attribute_Manager_readBlockLoadData(&m_param.start);

    MeterClock_get(&now, NULL);
    // Start main FSM asap
    LOGI("Starting read_block_load_profile_fsm asap");
    LOGI("Next Block load: %u now: %u", m_param.start, now);
    App_Scheduler_addTask_execTime(read_block_load_profile_fsm,
                                   0,
                                   SFSM_EXECUTION_TIME_US);

    // When starting, we are out of sync and require a new randomization
    // that will be determined when period is known
    m_param.new_randomization_needed = true;
}
