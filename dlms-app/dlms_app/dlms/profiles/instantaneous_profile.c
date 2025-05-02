/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#define DEBUG_LOG_MODULE_NAME "INSTANT "
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"


#include "dlms_com.h"
#include "common.h"
#include "meter_connection_management.h"
#include "profile_generic.h"
#include "dlms_lock.h"
#include "wirepas_com.h"
#include "profiles_config.h"
#include "data_notification.h"
#include "server_attribute_manager.h"
#include "rng.h"
#include "meter_clock.h"

#include <malloc.h>

static obis_code_t c_inst_prof_ln = { 1, 0, 94, 91, 0, 255 };
static obis_code_t c_push_inst_prof_ln = { 0, 0, 25, 9, 0, 255 };

/* ************************************ */
/* FSM  For instantaneous profile       */

/* FSM handler */
static uint32_t read_instantaneous_profile_fsm(void);

typedef enum
{
    INST_PROF_STATE_TAKE_LOCK,
    INST_PROF_STATE_CONNECT,
    INST_PROF_STATE_READ_INSTANTANEOUS,
    INST_PROF_STATE_CLOSE,
    INST_PROF_STATE_SEND,
    INST_PROF_STATE_SCHEDULE
} inst_prof_sfsm_state_e;

typedef struct
{
    uint32_t randomization_time_ms;
    uint8_t * instantaneous_buffer; // NULL, if nothing stored
    uint16_t instantaneous_size; // 0 if never read, meter size
    uint16_t current_period_min;
    inst_prof_sfsm_state_e state;
} inst_prof_fsm_param_t;

static inst_prof_fsm_param_t m_param;

#define RESCHEDULE_ASAP()   App_Scheduler_addTask_execTime(read_instantaneous_profile_fsm, \
                                                APP_SCHEDULER_SCHEDULE_ASAP,\
                                                SFSM_EXECUTION_TIME_US)
static uint32_t get_next_inst_delay_ms(void)
{
    uint16_t current_period_min;
    uint32_t current_period_s;
    uint32_t now_s;
    MeterClock_get(&now_s, NULL);

    uint32_t delay_s;

    Server_Attribute_Manager_readInstantaneousPushConfig(&current_period_min);

    if (m_param.current_period_min != current_period_min)
    {
        m_param.current_period_min = current_period_min;
        // Current period has changed, reset the randomization_time
        m_param.randomization_time_ms = 0;
    }

    if (now_s == 0) {
        LOGE("Cannot determine clock, return period");
        return current_period_min * 60 * 1000;
    }

    current_period_s = current_period_min * 60;

    // next is in period - delay since last ts
    delay_s = current_period_s - (now_s % (current_period_s));

    LOGI("Next instantaneous is in %d s", delay_s);

    return delay_s * 1000;
}

static void on_lock_aquired_cb(void)
{
    m_param.state = INST_PROF_STATE_CONNECT;
    RESCHEDULE_ASAP();
}

static void open_cb(int32_t result)
{
    if (result == DLMS_ERROR_CODE_OK)
    {
        m_param.state = INST_PROF_STATE_READ_INSTANTANEOUS;
    }
    else
    {
        m_param.state = INST_PROF_STATE_SCHEDULE;
    }
    RESCHEDULE_ASAP();
}

static bool is_time_to_read_inst_profile(void)
{
    uint32_t now_s = 0;
    uint16_t current_period_min;
    uint32_t current_period_s;
    uint32_t next_instantaneous_s;

    MeterClock_get(&now_s, NULL);
    Server_Attribute_Manager_readInstantaneousPushConfig(&current_period_min);
    current_period_s = current_period_min * 60;

    // if the clock drifts or due to rounding errors
    // we may try to read the instantaneous few seconds too early
    // Check if it is the case and schedule the read at the right time
    next_instantaneous_s = current_period_s - (now_s % (current_period_s));
    if (next_instantaneous_s && next_instantaneous_s < current_period_s / 10)
    {
        uint32_t delay_ms = next_instantaneous_s * 1000;
        LOGI("Inst read postponed in %" PRIu32 " ms", delay_ms);
        App_Scheduler_addTask_execTime(read_instantaneous_profile_fsm,
                                       delay_ms,
                                       SFSM_EXECUTION_TIME_US);
        return false;
    }
    return true;
}

static void schedule_sending()
{
    if (m_param.instantaneous_buffer != NULL)
    {
        uint32_t next_inst_ms;

        // Release the lock while waiting the random period
        Dlms_lock_release(DLMS_LOCK_ID_INSTANTANEOUS);
        m_param.state = INST_PROF_STATE_SEND;

        next_inst_ms = get_next_inst_delay_ms();

        // Generate a random delay if:
        //  - It is not set: the first time or interval has changed
        //  - We are too late
        if (m_param.randomization_time_ms == 0 ||
            m_param.randomization_time_ms > next_inst_ms)
        {
            // Generate a new randomization
            m_param.randomization_time_ms = Rng_number(next_inst_ms * 0.9); // Randomize on 90% of interval
            LOGI("Randomization time is %d ms", m_param.randomization_time_ms);
        }

        App_Scheduler_addTask_execTime(read_instantaneous_profile_fsm,
                                       m_param.randomization_time_ms,
                                       SFSM_EXECUTION_TIME_US);

    }
    else
    {
        LOGW("Nothing to send");
        m_param.state = INST_PROF_STATE_SCHEDULE;
        RESCHEDULE_ASAP();
    }
}

static void close_cb(int32_t result)
{
    LOGD("Close result = %d", result);
    schedule_sending();
}

static void on_data_sent_cb(const app_lib_data_sent_status_t * status)
{
    LOGI("Instant profile sent with status: %d", status->success);
    // TODO add logic here or in wirepas_com_module to handle the retry
}

// This is used as a callback, and we want to avoid modifying the caller's signature.
static void read_instantaneous_cb(profile_generic_result_e result,
// cppcheck-suppress constParameterCallback
                                  gxByteBuffer * profile_data,
                                  uint32_t endtime,
                                  uint32_t period)
{
    if (result == PROFILE_GENERIC_RESULT_OK)
    {
        if (profile_data->size > m_param.instantaneous_size && m_param.instantaneous_buffer != NULL)
        {
            LOGE("Static buffer is too small (%d vs %d)", profile_data->size, m_param.instantaneous_size);
            // Size has changed, free buffer to reallocate it
            free(m_param.instantaneous_buffer);
            m_param.instantaneous_buffer = NULL;
            m_param.instantaneous_size = 0;
        }

        // Check if buffer must be allocated
        if (m_param.instantaneous_buffer == NULL)
        {
            m_param.instantaneous_buffer = malloc(profile_data->size);
            if (m_param.instantaneous_buffer == NULL)
            {
                LOGE("Cannot allocate instantaneous buffer of %d bytes", m_param.instantaneous_size);
            }
            else
            {
                m_param.instantaneous_size = profile_data->size;
                LOGI("Inst buffer at: 0x%x (size = %d bytes)", m_param.instantaneous_buffer, m_param.instantaneous_size);
            }
        }

        // Final check to store the instantaneous
        if (m_param.instantaneous_buffer != NULL && m_param.instantaneous_size >= profile_data->size)
        {
            // Store result to be sent later
            memcpy(m_param.instantaneous_buffer, profile_data->data, profile_data->size);
            m_param.instantaneous_size = profile_data->size;
        }
    }
    else
    {
        LOGE("read_instantaneous_cb: %d", result);
        free(m_param.instantaneous_buffer);
        m_param.instantaneous_buffer = NULL;
    }

    m_param.state = INST_PROF_STATE_CLOSE;

    RESCHEDULE_ASAP();
}

static void send_buffered_instantaneous(void)
{
    message push_message;
    // This function is called
    gxByteBuffer profile_data;
    uint16_t push_cfg;

    if (m_param.instantaneous_buffer == NULL)
    {
        LOGE("No profile to send");
        return;
    }

    Server_Attribute_Manager_readProfilePushConfig(&push_cfg);
    if (INSTANTANEOUS_PUSH_PROF_IS_ENABLED(push_cfg))
    {
        mes_init(&push_message);

        // Attach our static buffer to gxByteBuffer
        bb_attach(&profile_data, m_param.instantaneous_buffer, m_param.instantaneous_size, m_param.instantaneous_size);

        if (Data_Notification_generatePushFromPull(MCM_AA_US,
                                                   c_push_inst_prof_ln,
                                                   &profile_data,
                                                   &push_message))
        {

            Wirepas_com_send_message(push_message.data[0]->data,
                                     push_message.data[0]->size,
                                     on_data_sent_cb,
                                     WC_TYPE_INST_PROF);
        }

        mes_clear(&push_message);
    }
    else
    {
        LOGI("Instantaneous push disabled");
    }
    free(m_param.instantaneous_buffer);
    m_param.instantaneous_buffer = NULL;
}

static void on_lock_aquired_for_sending_cb(void)
{
    // Retake the lock as we will need to malloc
    send_buffered_instantaneous();
    m_param.state = INST_PROF_STATE_SCHEDULE;
    RESCHEDULE_ASAP();
}

static uint32_t read_instantaneous_profile_fsm(void)
{
    uint32_t delay = SFSM_DELAY_PAUSED;
    Dlms_Lock_return_code_e lock_ret;

    SFSM_ENTRY(m_param.state);

    switch (m_param.state)
    {
        case INST_PROF_STATE_TAKE_LOCK:
            LOGI("Reading instantaneous profile");
            lock_ret = Dlms_lock_take(DLMS_LOCK_ID_INSTANTANEOUS,
                                      on_lock_aquired_cb,
                                      DLMS_LOCK_TYPE_WITHOUT_TRAFFIC);

            if (lock_ret == DLMS_LOCK_RET_ACQUIRED)
            {
                // Move to next state immediatelly
                m_param.state = INST_PROF_STATE_CONNECT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            else if (lock_ret != DLMS_LOCK_RET_WAITING_FOR_LOCK)
            {
                LOGE("Cannot take or wait for lock");
                delay = SFSM_DELAY_RETRY;
            }
            break;

        case INST_PROF_STATE_CONNECT:
            if (m_param.instantaneous_size != 0)
            {
                // We know the size of instantaneaous
                // Allocate it with malloc as we want to persist it outside the lock
                // Allocate it now as the head should be empty
                if (m_param.instantaneous_buffer != NULL)
                {
                    LOGE("Instantaneous buffer already exist, free it to reallocate it");
                    free(m_param.instantaneous_buffer);
                }
                m_param.instantaneous_buffer = malloc(m_param.instantaneous_size);
                if (m_param.instantaneous_buffer == NULL)
                {
                    LOGE("Cannot allocate instantaneous buffer");
                }
                else
                {
                    LOGI("Inst buffer at: 0x%x (size = %d bytes)", m_param.instantaneous_buffer, m_param.instantaneous_size);
                }
            }

            if (Meter_Connection_Management_open(open_cb, MCM_SECURED_AA) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_param.state = INST_PROF_STATE_SCHEDULE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case INST_PROF_STATE_READ_INSTANTANEOUS:
            if (is_time_to_read_inst_profile())
            {
                Profile_Generic_read_all_async(c_inst_prof_ln,
                                            read_instantaneous_cb);
            }
            break;

        case INST_PROF_STATE_CLOSE:
            if (Meter_Connection_Management_close(close_cb) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                // Cannot close connection but still send what we have read
                schedule_sending();
            }
            break;

        case INST_PROF_STATE_SEND:
            LOGI("Time to send instantaneous profile");
            lock_ret = Dlms_lock_take(DLMS_LOCK_ID_INSTANTANEOUS,
                                      on_lock_aquired_for_sending_cb,
                                      DLMS_LOCK_TYPE_WITHOUT_TRAFFIC);

            if (lock_ret == DLMS_LOCK_RET_ACQUIRED)
            {
                // Call the cb directly, next state is there
                on_lock_aquired_for_sending_cb();
            }
            else if (lock_ret != DLMS_LOCK_RET_WAITING_FOR_LOCK)
            {
                LOGE("Cannot take or wait for lock");
                delay = SFSM_DELAY_RETRY;
            }

            break;

        case INST_PROF_STATE_SCHEDULE:
            SFSM_CHECK();
            {
                delay = get_next_inst_delay_ms();
                LOGI("Next read in %u seconds", (delay/ 1000));
            }
            Dlms_lock_release(DLMS_LOCK_ID_INSTANTANEOUS);
            m_param.state = INST_PROF_STATE_TAKE_LOCK;
            break;
    } /* switch (_state) */

    SFSM_EXIT();

    return delay;
}
/*****************************/

void Instantaneous_profile_start(void)
{
    uint32_t delay_ms = get_next_inst_delay_ms();

    // Start main FSM
    LOGI("Starting read_instantaneous_profile_fsm in %u ms", delay_ms);
    App_Scheduler_addTask_execTime(read_instantaneous_profile_fsm,
                                   delay_ms,
                                   SFSM_EXECUTION_TIME_US);

    return;
}
