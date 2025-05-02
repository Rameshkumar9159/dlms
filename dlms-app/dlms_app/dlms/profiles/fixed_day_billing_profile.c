/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#define DEBUG_LOG_MODULE_NAME "F.D.BILL"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#include "fixed_day_billing_profile.h"
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
#include "meter_clock.h"

#include "wirepas_com.h"

#include <malloc.h>

#include "include/cosem.h" // cosem2
#include "include/gxmem.h" // gxcalloc
#include "include/client.h" // cl_readLN

// Fixed day billing profile has to be pushed within the 6 first hours of the fixed day billing day
#define FIXED_DAY_BILLING_PROFILE_PERIOD_MS     (6 * 60 * 60 * 1000)
#define MAX_DAYS_IN_MONTH                       31

// This is the default reschedule time, but it should never happen as
// a new reschedule period will be computed asynchronously when
// previous profile is succesfully pushed.
#define RETRY_RESCHEDULE_MS                   (20 * 60 * 1000)

/* FSM handler */
static uint32_t fixed_day_read_billing_profile_fsm(void);

typedef enum
{
    FIXED_DAY_BILLING_PROF_STATE_TAKE_LOCK,
    FIXED_DAY_BILLING_PROF_STATE_CONNECT,
    FIXED_DAY_BILLING_PROF_STATE_READ_ENTRIES_IN_USE,
    FIXED_DAY_BILLING_PROF_STATE_READ_CURRENT_ENTRY,
    FIXED_DAY_BILLING_PROF_STATE_CLOSE,
    FIXED_DAY_BILLING_PROF_STATE_SCHEDULE
} fdbp_fsm_state_e;

typedef struct
{
    // Variable required to read object
    message msg;
    gxReplyData reply;
    uint32_t entry_id;
} fdbp_data_t;

static fdbp_data_t * m_data_p;
static fdbp_fsm_state_e m_state;
static uint32_t m_next_delay_ms;
static bool m_is_running;

#define RESCHEDULE_ASAP()   \
                App_Scheduler_addTask_execTime(fixed_day_read_billing_profile_fsm, \
                                               APP_SCHEDULER_SCHEDULE_ASAP,\
                                               SFSM_EXECUTION_TIME_US)

// Billing Profile OBIS
static obis_code_t c_bp_ln = { 1, 0, 98, 1, 0, 255 };
// OBIS of the generated push for the billing profile
static obis_code_t c_push_bp_ln = { 0, 106, 25, 9, 0, 255 };

static uint32_t initialize_billing_date(uint8_t billing_day)
{
    // Initialize based on current time
    gxtime gxepoch, gxfdbd;
    uint32_t epoch;
    uint32_t init_date = 0;

    if (! MeterClock_get(&epoch, NULL))
    {
        LOGE("Clock not ready");
        return 0;
    }
    time_initUnix(&gxepoch, epoch);
    time_clearTime(&gxepoch);

    // This is the first time we compute the next date, we need to start from current date
    uint16_t target_year = time_getYears(&gxepoch);
    uint8_t target_month = time_getMonths(&gxepoch);
    uint8_t epoch_day = time_getDays(&gxepoch);
    uint8_t last_day = date_daysInMonth(target_year, target_month);
    uint8_t target_day = billing_day;

    // If the chosen day is biger than the number of days in this month
    if (target_day > last_day)
    {
        target_day = last_day;
    }
    if (target_day <= epoch_day)
    {
        // Switc to next month
        target_month++;
        // Restore target_day
        target_day = billing_day;
        if (target_month > 12)
        {
            target_year++;
            target_month = 1;
        }
        // We need to check again if the target day is in the target month
        last_day = date_daysInMonth(target_year, target_month);
        if (target_day > last_day)
        {
            target_day = last_day;
        }
    }
    // Finally, generate the next fixed day billing date as an epoch
    time_init(&gxfdbd, target_year, target_month, target_day, 0, 0, 0, 0, 0);
    init_date = time_toUnixTime2(&gxfdbd);
    LOGI("Date initialized to %u", init_date);

    return init_date;
}

static uint32_t compute_next_billing_date(uint8_t billing_day, uint32_t billing_date)
{
    gxtime  gxfdbd;

    uint32_t fdbd = billing_date;

    if (! Fixed_Day_Billing_Day_isValid(billing_day))
    {
        LOGE("Invalid date for fixed day billing day %d", billing_day);
        return 0;
    }
    else if (billing_day == FIXED_DAY_BILLING_DAY_DISABLED)
    {
        return 0;
    }
    else if (! fdbd)
    {
        fdbd = initialize_billing_date(billing_day);
    }
    else
    {
        uint32_t epoch;
        if (! MeterClock_get(&epoch, NULL))
        {
            LOGE("Clock not ready");
            return 0;
        }
        // We update the first day billing date until it's later than the current time
        while (fdbd <= epoch)
        {
            uint16_t target_year;
            uint8_t target_month;
            uint8_t last_day;
            uint8_t target_day = billing_day;

            time_initUnix(&gxfdbd, fdbd);

            target_year = time_getYears(&gxfdbd);
            target_month = time_getMonths(&gxfdbd);

            // Increment month
            target_month++;
            if (target_month > 12)
            {
                target_year++;
                target_month = 1;
            }

            // We need to check if the day is in the target month
            last_day = date_daysInMonth(target_year, target_month);
            if (target_day > last_day)
            {
                target_day = last_day;
            }
            // Finally, get the next fixed day billing date
            time_init(&gxfdbd, target_year, target_month, target_day, 0, 0, 0, 0, 0);
            fdbd = time_toUnixTime2(&gxfdbd);
        }
    }
    LOGI("Next fixed day billing date: %u", fdbd);
    return fdbd;
}

static uint32_t compute_delay(uint32_t billing_date)
{
    uint32_t epoch;
    uint32_t delay_ms;

    if (! billing_date)
    {
        LOGW("Fixed day billing date unset, disabling FSM");
        return APP_SCHEDULER_STOP_TASK;
    }
    // Schedule the next run
    if (! MeterClock_get(&epoch, NULL))
    {
        LOGE("Clock not ready");
        return APP_SCHEDULER_STOP_TASK;
    }
    // If we are late, check if we can still randomize the reading
    if (epoch >= billing_date)
    {
        uint32_t late_ms;
        late_ms = (epoch - billing_date) * 1000;
        if (late_ms >= FIXED_DAY_BILLING_PROFILE_PERIOD_MS)
        {
            delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;
        }
        else
        {
            delay_ms = Rng_number(FIXED_DAY_BILLING_PROFILE_PERIOD_MS - late_ms);
        }
    }
    // It's not yet time to send the push, let's schedule the reading
    else
    {
        uint32_t delay = billing_date - epoch;
        delay_ms = (delay * 1000) + Rng_number(FIXED_DAY_BILLING_PROFILE_PERIOD_MS);
        // Check for overflow, that should not happen
        if (delay_ms < delay)
        {
            LOGE("Delay overflow");
            return APP_SCHEDULER_SCHEDULE_ASAP;
        }
    }
    return delay_ms;
}

static void reschedule_state_machine(uint32_t new_date)
{
    uint32_t delay_ms = compute_delay(new_date);

    // Start main FSM
    LOGI("Starting fixed_day_read_billing_profile_fsm in %u ms", delay_ms);
    App_Scheduler_addTask_execTime(fixed_day_read_billing_profile_fsm,
                                   delay_ms, SFSM_EXECUTION_TIME_US);

}

static void on_lock_aquired_cb()
{
    m_state = FIXED_DAY_BILLING_PROF_STATE_CONNECT;
    RESCHEDULE_ASAP();
}

static void open_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result)
    {
        m_state = FIXED_DAY_BILLING_PROF_STATE_READ_ENTRIES_IN_USE;
    }
    else
    {
        m_state = FIXED_DAY_BILLING_PROF_STATE_SCHEDULE;
    }
    RESCHEDULE_ASAP();
}

static void close_cb(int32_t result)
{
    m_state = FIXED_DAY_BILLING_PROF_STATE_SCHEDULE;
    RESCHEDULE_ASAP();
}

static void read_entries_in_use_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result && m_data_p->reply.dataValue.vt == DLMS_DATA_TYPE_UINT32)
    {
        uint32_t entries_in_use = m_data_p->reply.dataValue.ulVal;
        // We succesfuly read the number of entries in use in the buffer.
        LOGI("Entries in use: %u", entries_in_use);

        // If the entries in use is zero, there is no entry
        if (! entries_in_use)
        {
            LOGI("No entry");
            m_state = FIXED_DAY_BILLING_PROF_STATE_CLOSE;
        }
        else
        {
            // We are ready to read the current entry
            m_state = FIXED_DAY_BILLING_PROF_STATE_READ_CURRENT_ENTRY;
            m_data_p->entry_id = entries_in_use;
        }
    }
    else
    {
        LOGE("Read entries in use attr: %d", result);
        m_state = FIXED_DAY_BILLING_PROF_STATE_CLOSE;
    }

    // Clear memory for request/reply
    mes_clear(&m_data_p->msg);
    reply_clear(&m_data_p->reply);

    RESCHEDULE_ASAP();
}

static void on_push_sent_cb(const app_lib_data_sent_status_t * status)
{
    LOGI("Fixed Day Billing Profile sent with status: %d",
        status->success);

    if (status->success)
    {
        uint32_t billing_date;
        uint8_t billing_day;
        Server_Attribute_readFixedDayBillingInfo(&billing_day, &billing_date);
        billing_date = compute_next_billing_date(billing_day, billing_date);
        Server_Attribute_writeFixedDayBillingInfo(billing_day, billing_date);

        // reschedule for next reading
        if (! m_is_running)
        {
            reschedule_state_machine(billing_date);
        }
        else
        {
            m_next_delay_ms = compute_delay(billing_date);
        }
    }
}

static bool generate_and_send_push(gxByteBuffer * profile_data,
                                   app_lib_data_data_sent_cb_f sent_cb)
{
    message push_message;
    bool done = false;

    mes_init(&push_message);
    if (Data_Notification_generatePushFromPull(MCM_AA_US, c_push_bp_ln,
                                               profile_data,
                                               &push_message) &&
        Wirepas_com_send_message(push_message.data[0]->data,
                                 push_message.data[0]->size,
                                 sent_cb,
                                 WC_TYPE_BILLING_PROF))
    {
        done = true;
    }
    mes_clear(&push_message);

    return done;
}

static void on_meter_read_cb(profile_generic_result_e result,
                            gxByteBuffer * profile_data,
                            uint32_t endtime,
                            uint32_t period)
{
    uint16_t push_cfg;

    if (result == PROFILE_GENERIC_RESULT_OK)
    {
        Server_Attribute_Manager_readProfilePushConfig(&push_cfg);
        if (BILLING_PUSH_PROF_IS_ENABLED(push_cfg))
        {
            // We send data unconditionnaly, they are different every time
            // they are read
            LOGI("Sending current billing period data");
            (void)generate_and_send_push(profile_data, on_push_sent_cb);
        }
    }
    else
    {
        LOGI("Read entry from ProfileGeneric: %d", result);
    }

    m_state = FIXED_DAY_BILLING_PROF_STATE_CLOSE;

    RESCHEDULE_ASAP();
}

static uint32_t fixed_day_read_billing_profile_fsm(void)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    uint32_t delay = SFSM_DELAY_PAUSED;
    Dlms_Lock_return_code_e lock_ret;

    SFSM_ENTRY(m_state);

    switch (m_state)
    {
        case FIXED_DAY_BILLING_PROF_STATE_TAKE_LOCK:
            m_is_running = true;
            m_next_delay_ms = RETRY_RESCHEDULE_MS;
            LOGI("Reading fixed day billing profile");
            lock_ret = Dlms_lock_take(DLMS_LOCK_ID_FIXED_DAY_BILLING,
                                      on_lock_aquired_cb,
                                      DLMS_LOCK_TYPE_WITHOUT_TRAFFIC);

            if (lock_ret == DLMS_LOCK_RET_ACQUIRED)
            {
                // Move to next state immediatelly
                m_state = FIXED_DAY_BILLING_PROF_STATE_CONNECT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            else if (lock_ret != DLMS_LOCK_RET_WAITING_FOR_LOCK)
            {
                LOGE("Cannot take or wait for lock");
                delay = SFSM_DELAY_RETRY;
            }
            break;

        case FIXED_DAY_BILLING_PROF_STATE_CONNECT:
            if (Meter_Connection_Management_open(open_cb, MCM_SECURED_AA) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_state = FIXED_DAY_BILLING_PROF_STATE_SCHEDULE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case FIXED_DAY_BILLING_PROF_STATE_READ_ENTRIES_IN_USE:
            if (! (m_data_p = gxcalloc(1, sizeof *m_data_p)))
            {
                LOGE("Failed to allocate memory to read FDBP");
                m_state = FIXED_DAY_BILLING_PROF_STATE_CLOSE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            else
            {
                int res;
                // Read entries in use attribute of the profile generic
                LOGI("Reading billing profile attribute 'entries in use'");

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
                    m_state = FIXED_DAY_BILLING_PROF_STATE_CLOSE;
                    delay = APP_SCHEDULER_SCHEDULE_ASAP;
                }
            }
            break;

        case FIXED_DAY_BILLING_PROF_STATE_READ_CURRENT_ENTRY:
            Profile_Generic_read_by_entry_async(c_bp_ln,
                                                m_data_p->entry_id,
                                                1,
                                                on_meter_read_cb);
            break;

        case FIXED_DAY_BILLING_PROF_STATE_CLOSE: // close com
            if (m_data_p)
            {
                gxfree(m_data_p);
                m_data_p = NULL;
            }
            if (Meter_Connection_Management_close(close_cb) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_state = FIXED_DAY_BILLING_PROF_STATE_SCHEDULE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case FIXED_DAY_BILLING_PROF_STATE_SCHEDULE:
            SFSM_CHECK();

            m_state = FIXED_DAY_BILLING_PROF_STATE_TAKE_LOCK;
            Dlms_lock_release(DLMS_LOCK_ID_FIXED_DAY_BILLING);
            delay = m_next_delay_ms;
            m_is_running = false;
            break;
    } /* switch (_state) */

    SFSM_EXIT();

    return delay;
}

void Fixed_Day_Billing_Profile_start(void)
{
    uint32_t billing_date;
    uint8_t billing_day;

    Server_Attribute_readFixedDayBillingInfo(&billing_day, &billing_date);

    if (billing_day == FIXED_DAY_BILLING_DAY_DISABLED)
    {
        LOGI("Fixed day billing profile push is disabled");
    }
    else if (! Fixed_Day_Billing_Day_isValid(billing_day))
    {
        LOGE("Invalid date for fixed day billing day %d, reset to disabled", billing_day);
        // Reset it in flash
        Server_Attribute_writeFixedDayBillingInfo(FIXED_DAY_BILLING_DAY_DISABLED, 0);
    }
    else
    {
        LOGI("Fixed data billing profile push is enabled");

        if (! billing_date)
        {
            LOGW("Fixed data billing date has not been computed");
            billing_date = initialize_billing_date(billing_day);
        }

        reschedule_state_machine(billing_date);
    }
}

bool Fixed_Day_Billing_Day_isValid(uint8_t billing_day)
{
    if (billing_day == FIXED_DAY_BILLING_DAY_DISABLED || billing_day <= MAX_DAYS_IN_MONTH)
    {
        return true;
    }
    return  false;
}

void Fixed_Day_Billing_Profile_updateBillingDay(uint8_t new_day)
{
    uint32_t billing_date;
    uint32_t new_date = 0;
    uint8_t billing_day;

    // Validity of the new_day has already been verified by the NIC server
    if (! Fixed_Day_Billing_Day_isValid(new_day))
    {
        LOGE("Invalid date for fixed day billing day %d, reset to disabled", new_day);
        // Fix it
        new_day = FIXED_DAY_BILLING_DAY_DISABLED;
    }

    // Read the previous day and date
    Server_Attribute_readFixedDayBillingInfo(&billing_day, &billing_date);

    if (new_day == FIXED_DAY_BILLING_DAY_DISABLED)
    {
        LOGI("Fixed day billing is disabled");
        if (! m_is_running)
        {
            App_Scheduler_cancelTask(fixed_day_read_billing_profile_fsm);
        }
        // Else let it run and do not reschedule it after current run is completed
        m_next_delay_ms = APP_SCHEDULER_STOP_TASK;
    }
    else
    {
        // If day has not changed, let's check the previous date
        if (billing_day == new_day)
        {
            new_date = billing_date;
        }
        new_date = compute_next_billing_date(new_day, new_date);

        reschedule_state_machine(new_date);
    }

    // Save info in persistent storage
    Server_Attribute_writeFixedDayBillingInfo(new_day, new_date);
}
