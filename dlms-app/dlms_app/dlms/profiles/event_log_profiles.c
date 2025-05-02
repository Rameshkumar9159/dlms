/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#define DEBUG_LOG_MODULE_NAME "EVTS_LOG"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#include "crc.h"
#include "wms_data.h"

#include "dlms_com.h"
#include "common.h"
#include "meter_connection_management.h"
#include "event_log_profiles.h"
#include "dlms_lock.h"
#include "wirepas_com.h"
#include "profile_generic.h"
#include "client_attribute_manager.h"
#include "data_notification.h"
#include "server_attribute_manager.h"
#include "rng.h"
#include "profiles_config.h"

#include "include/client.h" // cl_readLN
#include "include/gxmem.h" // gxmalloc

#include <malloc.h>

// Max number of fragments for event log push
#define MAX_FRAGMENT_NB                 3

/* FSM handler */
static uint32_t read_event_log_fsm(void);
static uint32_t read_event_logs_task(void);

#define ENTRY_INVALID_ID                0xFFFF

typedef enum
{
    EVENT_LOG_STATE_TAKE_LOCK,
    EVENT_LOG_STATE_CONNECT,
    EVENT_LOG_STATE_READ_PROFILE_ENTRIES,
    EVENT_LOG_STATE_READ_ENTRIES_IN_USE,
    EVENT_LOG_STATE_CHECK_ENTRY_SIZE,
    EVENT_LOG_STATE_SEARCH_ENTRY_BY_CRC,
    EVENT_LOG_STATE_READ_LAST_ENTRY,
    EVENT_LOG_STATE_READ_ENTRIES,
    EVENT_LOG_STATE_CLOSE,
    EVENT_LOG_STATE_EXIT
} evt_log_fsm_state_e;

typedef struct
{
    event_log_type_e type;
    obis_code_t prof_ln;
    obis_code_t push_ln;
    const char * desc;
} evt_log_descriptor_t;

typedef struct
{
    // Working set
    message msg;
    gxReplyData reply;
    // Number of profile entries in the Profile Generic
    uint16_t profile_entries;
    // Number of entries in use in the Profile Generic
    uint16_t entries_in_use;
    // Identification of entries to read (from first to last)
    uint16_t first_entry;
    uint16_t last_entry;
    // Estimated number of entries that are sure to fit in a single Wirepas message
    uint16_t entries_per_msg;
    // the CRC of the EIU entry
    uint16_t eiu_crc;
    // the CRC of the last read entry
    uint16_t last_entry_crc;
    // boolean tracking if a message has been sent
    bool message_posted;
} evt_log_prof_fsm_params_t;

static const evt_log_descriptor_t m_eld[EVENT_LOG_TYPE_NB] =
{
    // Voltage events event log
    {
        EVENT_LOG_TYPE_VOLTAGE,
        { 0, 0, 99, 98, 0, 255},
        { 0, 120, 25, 9, 0, 255},
        "voltage"
    },
    // Current events event log
    {
        EVENT_LOG_TYPE_CURRENT,
        { 0, 0, 99, 98, 1, 255},
        { 0, 121, 25, 9, 0, 255},
        "current"
    },
    // Power events event log
    {
        EVENT_LOG_TYPE_POWER,
        { 0, 0, 99, 98, 2, 255},
        { 0, 122, 25, 9, 0, 255},
        "power"
    },
    // Transaction events event log
    {
        EVENT_LOG_TYPE_TRANSACTION,
        { 0, 0, 99, 98, 3, 255},
        { 0, 123, 25, 9, 0, 255},
        "transaction"
    },
    // Other events event log
    {
        EVENT_LOG_TYPE_OTHER,
        { 0, 0, 99, 98, 4, 255},
        { 0, 124, 25, 9, 0, 255},
        "other"
    },
    // Non-rollover events event log
    {
        EVENT_LOG_TYPE_NON_ROLLOVER,
        { 0, 0, 99, 98, 5, 255},
        { 0, 125, 25, 9, 0, 255},
        "non_rollover"
    },
#ifndef METER_RETROFIT
    // Control events event log
    {
        EVENT_LOG_TYPE_CONTROL,
        { 0, 0, 99, 98, 6, 255},
        { 0, 126, 25, 9, 0, 255},
        "control"
    }
#endif
};

// FSM parameter, FSM is executed for each type of event log
static evt_log_prof_fsm_params_t * mp_param;
// Copy of event log data saved in flash
static evt_log_data_t m_data;
// If enabled, event logs are read once every hour
static const uint32_t m_period_ms = 3600 * 1000;
// State of the FSM
static evt_log_fsm_state_e m_state;
static uint32_t m_start_ts;
static event_log_type_e m_current_log;

#define RESCHEDULE_ASAP()   \
                App_Scheduler_addTask_execTime(read_event_log_fsm, \
                                               APP_SCHEDULER_SCHEDULE_ASAP,\
                                               SFSM_EXECUTION_TIME_US)
// -----------------------------------------------------------------------------
// FSM
// -----------------------------------------------------------------------------
static void on_lock_aquired_cb(void)
{
    m_state = EVENT_LOG_STATE_CONNECT;

    RESCHEDULE_ASAP();
}

static void open_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result)
    {
        m_state = EVENT_LOG_STATE_READ_PROFILE_ENTRIES;
    }
    else
    {
        LOGE("Connect : %d", result);
        m_state = EVENT_LOG_STATE_EXIT;
    }

    RESCHEDULE_ASAP();
}

static void close_cb(int32_t result)
{
    (void) result;
    m_state = EVENT_LOG_STATE_EXIT;

    RESCHEDULE_ASAP();
}

static uint32_t compute_next_reading_delay(void)
{
    uint32_t delay;

    // Schedule the next reading
    const uint32_t stop_ts = lib_time->getTimestampHp();
    const uint32_t delay_ms = lib_time->getTimeDiffUs(m_start_ts, stop_ts) / 1000;

    if (m_period_ms > delay_ms)
    {
        delay = m_period_ms - delay_ms;
        LOGI("Next read in %u seconds", (delay / 1000));
    }
    else
    {
        LOGW("Next read NOW, processing time is too long");
        delay = APP_SCHEDULER_SCHEDULE_ASAP;
    }
    return delay;
}

// Callback
static void read_evt_log_fsm_end_cb(bool message_posted)
{
    m_current_log = ((m_current_log + 1) % EVENT_LOG_TYPE_NB);

    // Not yet synced with the meter or nothing new and
    // whole loop not performed yet  => read next event log type
    if ((m_data.state != EVENT_LOG_STATE_SYNCED_WITH_METER || ! message_posted) &&
        m_current_log != m_data.first_log)
    {
        RESCHEDULE_ASAP();
    }
    else
    {
        uint32_t delay;

        // If we have sent some event logs, the update in flash will be done
        // in the message sent callback

        // If we are syncing with the meter
        if (m_data.state != EVENT_LOG_STATE_SYNCED_WITH_METER)
        {
            // We are synced with the meter
            m_data.state = EVENT_LOG_STATE_SYNCED_WITH_METER;
            // Write data in flash
            Client_Attribute_Manager_writeEventLogData(&m_data);
        }

        // Schedule the next reading
        delay = compute_next_reading_delay();
        App_Scheduler_addTask_execTime(read_event_logs_task,
                                       delay,
                                       SFSM_EXECUTION_TIME_US);
    }
}

static void read_profile_entries_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result &&
        mp_param->reply.dataValue.vt == DLMS_DATA_TYPE_UINT32)
    {
        evt_log_status_t * log_status_p = &m_data.log_status[m_current_log];
        uint32_t profiles_entries = mp_param->reply.dataValue.ulVal;

        // We successfully read the number of profile entries in the buffer.
        LOGD("Number of %s event log profile entries: %u",
            m_eld[m_current_log].desc, profiles_entries);
        // Sanity check
        if (profiles_entries > UINT16_MAX)
        {
            LOGE("uint16_t type too small to store profile entries value");
            m_state = EVENT_LOG_STATE_CLOSE;
        }
        else
        {
            mp_param->profile_entries = profiles_entries;
            log_status_p->profile_entries = profiles_entries;
            m_state = EVENT_LOG_STATE_READ_ENTRIES_IN_USE;
        }
    }
    else
    {
        LOGE("Read event_log PE result: %d", result);
        m_state = EVENT_LOG_STATE_CLOSE;
    }
    // Clear memory for request/reply
    mes_clear(&mp_param->msg);
    reply_clear(&mp_param->reply);

    RESCHEDULE_ASAP();
}

static void read_entries_in_use_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result &&
        mp_param->reply.dataValue.vt == DLMS_DATA_TYPE_UINT32)
    {
        const evt_log_status_t * log_status_p = &m_data.log_status[m_current_log];
        uint32_t entries_in_use = mp_param->reply.dataValue.ulVal;

        mp_param->entries_in_use = entries_in_use;
        // We succesfuly read the number of entries in use in the buffer.
        LOGI("%s entries in use: %u (prev. %u) on %u",
            m_eld[m_current_log].desc, entries_in_use,
            log_status_p->entries_in_use, log_status_p->profile_entries);

        // If the entries in use is zero, there is no entry
        if (! entries_in_use)
        {
            m_state = EVENT_LOG_STATE_CLOSE;
        }
        // Sanity check
        else if (log_status_p->current_entry > log_status_p->profile_entries)
        {
            LOGW("EIU > PE");
            m_state = EVENT_LOG_STATE_CLOSE;
        }
        else
        {
            // Let's check if there are new entries
            if (entries_in_use < log_status_p->current_entry)
            {
                LOGW("EIU < prev EIU, is sort method FIFO?");
                m_state = EVENT_LOG_STATE_CLOSE;
            }
            else if (entries_in_use > log_status_p->current_entry ||
                     entries_in_use == log_status_p->profile_entries)
            {
                // We have or may have some new entries, check entry size first
                // for potential batch sending
                m_state = EVENT_LOG_STATE_CHECK_ENTRY_SIZE;
            }
            else
            {
                // The entry list is not full and we already read this entry
                LOGI("No new %s entry", m_eld[m_current_log].desc);
                m_state = EVENT_LOG_STATE_CLOSE;
            }
        }
    }
    else
    {
        LOGE("Read event log EIU result: %d", result);
        m_state = EVENT_LOG_STATE_CLOSE;
    }
    // Clear memory for request/reply
    mes_clear(&mp_param->msg);
    reply_clear(&mp_param->reply);


    RESCHEDULE_ASAP();
}

static void get_entry_size_cb(profile_generic_result_e result,
                              gxByteBuffer * profile_data, uint32_t endtime,
                              uint32_t period)
{
    const evt_log_descriptor_t * desc_p = &m_eld[m_current_log];
    evt_log_status_t * log_status_p = &m_data.log_status[m_current_log];
    evt_log_fsm_state_e next_state = EVENT_LOG_STATE_CLOSE;

    // In this state, we have:
    // - either mp_param->entries_in_use > log_status_p->current_entry
    // - or mp_param->entries_in_use == log_status_p->profile_entries)

    if (result == PROFILE_GENERIC_RESULT_OK)
    {
        uint8_t * data = profile_data->data;
        uint16_t len = profile_data->size;
        message push_message;
        uint16_t overhead = 0;
        app_lib_data_data_size_t fragment = lib_data->getDataMaxNumBytes();
        uint16_t max_msg_size = MAX_FRAGMENT_NB * fragment.max_fragment_size;

        // Let's compute the size of the overhead
        mes_init(&push_message);
        if (Data_Notification_generatePushFromPull(MCM_AA_US, desc_p->push_ln,
                                                   profile_data,
                                                   &push_message) &&
            profile_data->size < push_message.data[0]->size)
        {
            overhead = push_message.data[0]->size - profile_data->size;
        }
        mes_clear(&push_message);

        // Now, compute the number of entries that can fit in a push message
        mp_param->entries_per_msg = (max_msg_size - overhead) / len;
        // Let's make sure that we can send one event at least
        if (! mp_param->entries_per_msg)
        {
             mp_param->entries_per_msg = 1;
        }

        LOGI("%s entry size: %u B, overhead: %u B -> %u entries per message",
             desc_p->desc, len, overhead, mp_param->entries_per_msg);

        // Compute the EIU CRC
        mp_param->eiu_crc = Crc_fromBuffer(data, len);

        // This is the first boot
        if (m_data.state != EVENT_LOG_STATE_SYNCED_WITH_METER)
        {
            // We just need to save the current state for this type of event
            log_status_p->current_entry = mp_param->entries_in_use;
            log_status_p->entries_in_use = mp_param->entries_in_use;
            log_status_p->crc = mp_param->eiu_crc;
        }
        else
        {
            // Now set target to read
            if (mp_param->entries_in_use > log_status_p->current_entry)
            {
                // We are sure that there are new entries
                mp_param->first_entry = log_status_p->current_entry + 1;
                // We must not read more than entries_per_msg
                mp_param->last_entry = MIN((mp_param->first_entry +
                                               mp_param->entries_per_msg - 1),
                                           (uint16_t)mp_param->entries_in_use);

                next_state = EVENT_LOG_STATE_READ_LAST_ENTRY;
            }
            // here mp_param->entries_in_use == log_status_p->current_entry
            else if (mp_param->entries_in_use == log_status_p->profile_entries)
            {
                // All entries are in use
                // We already read EIU entry and compute its CRC, check
                // if it's matching our last known or sent entry CRC
                if (log_status_p->crc == mp_param->eiu_crc)
                {
                    LOGI("No new %s entry", desc_p->desc);
                }
                else
                {
                    // We need to search for our entry starting from the end
                    mp_param->first_entry = mp_param->entries_in_use - 1;
                    next_state = EVENT_LOG_STATE_SEARCH_ENTRY_BY_CRC;
                }
            }
        }
    }
    else
    {
        LOGW("Failed to retrieve the %s log entry size: %d",
             desc_p->desc, result);
    }

    m_state = next_state;
    RESCHEDULE_ASAP();
}

static void on_event_log_push_sent_cb(const app_lib_data_sent_status_t * status)
{
    LOGI("Event log sent with status: %d", status->success);
    if (status->success)
    {
        // Round-robin scheduling: next log to read is the first one after
        // the one whose events have just been sent
        m_data.first_log = ((m_current_log + 1) % EVENT_LOG_TYPE_NB);

        // Update data in flash
        Client_Attribute_Manager_writeEventLogData(&m_data);
    }
}

static void search_event_log_entry_cb(profile_generic_result_e result,
                                      gxByteBuffer * profile_data,
                                      uint32_t endtime, uint32_t period)
{
    // Default next state
    evt_log_fsm_state_e next_state = EVENT_LOG_STATE_CLOSE;

    if (result == PROFILE_GENERIC_RESULT_OK)
    {
        uint8_t * data = profile_data->data;
        uint16_t len = profile_data->size;
        const evt_log_status_t * log_status_p = &m_data.log_status[m_current_log];
        uint16_t crc =  Crc_fromBuffer(data, len);

        // We don't know where is the last entry we read
        // Compute the CRC and if it is not equal to the last saved CRC
        // and if we have not reached the max number of entries to read
        // keep reading the previous entry
        if (crc == log_status_p->crc)
        {
            // This is the entry we were looking for
            // Re-read entries
            mp_param->first_entry++;
            mp_param->last_entry = MIN((mp_param->first_entry +
                                        mp_param->entries_per_msg - 1),
                                       mp_param->entries_in_use);
            next_state = EVENT_LOG_STATE_READ_LAST_ENTRY;
        }
        else
        {
            // Not found yet
            // We have not reached the first entry or the max number of
            // entries yet
            if (mp_param->first_entry - 1 > 0)
            {
                mp_param->first_entry--;
                next_state = EVENT_LOG_STATE_SEARCH_ENTRY_BY_CRC;
            }
            else
            {
                // We have reached the first entry and not found
                // our last read entry => re-read all
                mp_param->last_entry = MIN((mp_param->first_entry +
                                        mp_param->entries_per_msg - 1),
                                            mp_param->entries_in_use);
                next_state = EVENT_LOG_STATE_READ_LAST_ENTRY;
            }
        }
    }
    else
    {
        LOGW("Error reading %s event log entry %u: %d",
             m_eld[m_current_log].desc, mp_param->first_entry, result);
    }

    m_state = next_state;
    RESCHEDULE_ASAP();
}

static void read_last_entry_cb(profile_generic_result_e result,
                               gxByteBuffer * profile_data,
                               uint32_t endtime, uint32_t period)
{
    if (result == PROFILE_GENERIC_RESULT_OK)
    {
        mp_param->last_entry_crc = Crc_fromBuffer(profile_data->data,
                                                  profile_data->size);
        LOGD("%s event last entry (%u) CRC : 0x%04X", m_eld[m_current_log].desc,
             mp_param->last_entry, mp_param->last_entry_crc);
        m_state = EVENT_LOG_STATE_READ_ENTRIES;
    }
    else
    {
        LOGW("Error reading %s event log entry %u: %d",
             m_eld[m_current_log].desc, mp_param->last_entry, result);
        m_state = EVENT_LOG_STATE_CLOSE;
    }

    RESCHEDULE_ASAP();
}

static void read_event_log_entries_cb(profile_generic_result_e result,
                                      gxByteBuffer * profile_data,
                                      uint32_t endtime, uint32_t period)
{
    const evt_log_descriptor_t * desc_p = &m_eld[m_current_log];

    if (result == PROFILE_GENERIC_RESULT_OK)
    {
        evt_log_status_t * log_status_p = &m_data.log_status[m_current_log];
        evt_log_status_t prev_status;
        message push_message;
        uint16_t push_cfg;

        // Update the log data for this event log type
        memcpy(&prev_status, log_status_p, sizeof prev_status);
        log_status_p->profile_entries = mp_param->profile_entries;
        log_status_p->entries_in_use = mp_param->entries_in_use;
        log_status_p->current_entry = mp_param->last_entry;
        log_status_p->crc = mp_param->last_entry_crc;

        // Check if this event log is enabled
        Server_Attribute_Manager_readProfilePushConfig(&push_cfg);
        if (PUSH_PROF_IS_ENABLED(push_cfg,
                                 PROF_PUSH_FIRST_EVENT_LOG_BIT + m_current_log))
        {
            mes_init(&push_message);
            if (Data_Notification_generatePushFromPull(MCM_AA_US, desc_p->push_ln,
                                                profile_data, &push_message) &&
                Wirepas_com_send_message(push_message.data[0]->data,
                                         push_message.data[0]->size,
                                         on_event_log_push_sent_cb,
                                         WC_TYPE_EVENT_LOGS))
            {
                mp_param->message_posted = true;
            }
            else
            {
                LOGW("Failed to generate and send %s event log push",
                     desc_p->desc);
                // Restore the previous event data for this event log type
                memcpy(log_status_p, &prev_status, sizeof *log_status_p);
                mp_param->message_posted = false;
            }
            mes_clear(&push_message);
        }
        else
        {
            LOGI("Push disabled for %s event log", desc_p->desc);
            // Update data in flash
            Client_Attribute_Manager_writeEventLogData(&m_data);
        }
    }
    else
    {
        if (mp_param->first_entry != mp_param->last_entry)
        {
            LOGW("Error reading %s event log entries %u to %u: %d",
                 desc_p->desc, mp_param->first_entry,
                 mp_param->last_entry, result);
        }
        else
        {
            LOGW("Error reading %s event log entry %u: %d",
                 desc_p->desc, mp_param->first_entry, result);
        }
    }

    m_state = EVENT_LOG_STATE_CLOSE;
    RESCHEDULE_ASAP();
}

static uint32_t read_event_log_fsm(void)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    uint32_t delay = SFSM_DELAY_PAUSED;
    const evt_log_descriptor_t * desc_p = &m_eld[m_current_log];
    Dlms_Lock_return_code_e lock_ret;
    int res;

    SFSM_ENTRY(m_state);

    switch (m_state)
    {
        case EVENT_LOG_STATE_TAKE_LOCK:
            LOGI("Reading %s event log profile", desc_p->desc);

            // It's time to read some logs
            lock_ret = Dlms_lock_take(DLMS_LOCK_ID_EVENT_LOG,
                                      on_lock_aquired_cb,
                                      DLMS_LOCK_TYPE_WITHOUT_TRAFFIC);

            if (lock_ret == DLMS_LOCK_RET_ACQUIRED)
            {
                // Move to next state immediately
                m_state = EVENT_LOG_STATE_CONNECT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            else if (lock_ret != DLMS_LOCK_RET_WAITING_FOR_LOCK)
            {
                LOGE("Cannot take or wait for lock");
                delay = SFSM_DELAY_RETRY;
            }
            break;

        case EVENT_LOG_STATE_CONNECT:
            if ((mp_param = gxcalloc(1, sizeof(evt_log_prof_fsm_params_t))))
            {
                // Identification of entries to read (from first to last)
                mp_param->first_entry = ENTRY_INVALID_ID;
                mp_param->last_entry = ENTRY_INVALID_ID;
                mp_param->entries_per_msg = 0;
                mp_param->eiu_crc = 0xFFFF;
                mp_param->last_entry_crc = 0xFFFF;
                mp_param->message_posted = false;

                Meter_Connection_Management_open(open_cb, MCM_SECURED_AA);
            }
            else
            {
                LOGW("Failed to allocate memory");
                m_state = EVENT_LOG_STATE_EXIT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case EVENT_LOG_STATE_READ_PROFILE_ENTRIES:
            LOGI("Reading %s profile entries attribute", desc_p->desc);
            mes_init(&mp_param->msg);
            reply_init(&mp_param->reply);

            res = cl_readLN(ms_p, desc_p->prof_ln,
                            DLMS_OBJECT_TYPE_PROFILE_GENERIC,
                            8, NULL, &mp_param->msg);
            if (res == DLMS_ERROR_CODE_OK)
            {
                Dlms_Com_call_async(&mp_param->msg, &mp_param->reply,
                                    read_profile_entries_cb);
            }
            else
            {
                LOGE("cl_readLN event_log RPE: %d", res);
                mes_clear(&mp_param->msg);
                reply_clear(&mp_param->reply);
                m_state = EVENT_LOG_STATE_CLOSE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case EVENT_LOG_STATE_READ_ENTRIES_IN_USE:
            LOGI("Reading %s entries in use attribute", desc_p->desc);
            mes_init(&mp_param->msg);
            reply_init(&mp_param->reply);

            res = cl_readLN(ms_p, desc_p->prof_ln,
                            DLMS_OBJECT_TYPE_PROFILE_GENERIC,
                            7, NULL, &mp_param->msg);
            if (res == DLMS_ERROR_CODE_OK)
            {
                Dlms_Com_call_async(&mp_param->msg, &mp_param->reply,
                                    read_entries_in_use_cb);
            }
            else
            {
                LOGE("cl_readLN event_log EIU: %d", res);
                mes_clear(&mp_param->msg);
                reply_clear(&mp_param->reply);
                m_state = EVENT_LOG_STATE_CLOSE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case EVENT_LOG_STATE_CHECK_ENTRY_SIZE:
            LOGI("Checking %s entry size", desc_p->desc);
            Profile_Generic_read_by_entry_async(desc_p->prof_ln,
                                                mp_param->entries_in_use,
                                                1,
                                                get_entry_size_cb);
            break;

        case EVENT_LOG_STATE_SEARCH_ENTRY_BY_CRC:
            LOGI("Searching %s entry with 0x%04X CRC", desc_p->desc,
                m_data.log_status[m_current_log].crc);
            Profile_Generic_read_by_entry_async(desc_p->prof_ln,
                                                mp_param->first_entry,
                                                1,
                                                search_event_log_entry_cb);
            break;

        case EVENT_LOG_STATE_READ_LAST_ENTRY:
            LOGI("Reading %s last sent entry %u", desc_p->desc,
                mp_param->last_entry);
            Profile_Generic_read_by_entry_async(desc_p->prof_ln,
                                                mp_param->last_entry,
                                                1,
                                                read_last_entry_cb);
            break;

        case EVENT_LOG_STATE_READ_ENTRIES:
            if (mp_param->first_entry < mp_param->last_entry)
            {
                LOGI("Reading %s event log entries %u to %u",
                    desc_p->desc, mp_param->first_entry,
                    mp_param->last_entry);
            }
            else
            {
                LOGI("Reading %s event log entry %u",
                    desc_p->desc, mp_param->first_entry);
            }
            Profile_Generic_read_by_entry_async(desc_p->prof_ln,
                                                mp_param->first_entry,
                                                mp_param->last_entry -
                                                    mp_param->first_entry + 1,
                                                read_event_log_entries_cb);
            break;

        case EVENT_LOG_STATE_CLOSE:
            Meter_Connection_Management_close(close_cb);
            break;

        case EVENT_LOG_STATE_EXIT:
            if (mp_param)
            {
                read_evt_log_fsm_end_cb(mp_param->message_posted);
                gxfree(mp_param);
                mp_param = NULL;
            }
            else
            {
                read_evt_log_fsm_end_cb(false);
            }
            SFSM_CHECK();
            // No need to be very accurate for this scheduling as we are reading
            // many times "for nothing"
            delay = m_period_ms;
            Dlms_lock_release(DLMS_LOCK_ID_EVENT_LOG);
            m_state = EVENT_LOG_STATE_TAKE_LOCK;
            break;
    } /* switch (_state) */

    SFSM_EXIT();

    return delay;
}

static uint32_t read_event_logs_task(void)
{
    // Readback the event logs data stored in flash
    Client_Attribute_Manager_readEventLogData(&m_data);

    // If data have not been initialized, do it now
    if (! Event_Log_Profiles_checkDataValidity(&m_data))
    {
        Client_Attribute_Manager_writeEventLogData(&m_data);
    }

    m_start_ts = lib_time->getTimestampHp();

    m_current_log = m_data.first_log;

    RESCHEDULE_ASAP();

    return APP_SCHEDULER_STOP_TASK;
}

// -----------------------------------------------------------------------------
// Public functions
// -----------------------------------------------------------------------------
bool Event_Log_Profiles_checkDataValidity(evt_log_data_t * data_p)
{
    LOGD("Event log data check");

    if (data_p->state != EVENT_LOG_STATE_INITIALIZED &&
        data_p->state != EVENT_LOG_STATE_SYNCED_WITH_METER)
    {
        for (uint8_t t = 0; t < EVENT_LOG_TYPE_NB; t++)
        {
            evt_log_status_t * ls_p = &data_p->log_status[t];

            ls_p->profile_entries = 0;
            ls_p->entries_in_use = 0;
            ls_p->current_entry = 0;
            ls_p->crc = 0xFFFF;
        }
        data_p->first_log = EVENT_LOG_TYPE_VOLTAGE;
        data_p->state = EVENT_LOG_STATE_INITIALIZED;
        return false;
    }
    return true;
}

void Event_Log_Profiles_start(uint32_t delay_ms)
{
    LOGI("Starting read_event_logs_task in %u ms", delay_ms);

    App_Scheduler_addTask_execTime(read_event_logs_task,
                                   delay_ms,
                                   SFSM_EXECUTION_TIME_US);
}
