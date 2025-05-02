/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef EVENT_LOG_PROFILES_H_
#define EVENT_LOG_PROFILES_H_

#include <stdint.h>

/**
 * @brief  List of event log types
 */
typedef enum
{
    EVENT_LOG_TYPE_VOLTAGE,
    EVENT_LOG_TYPE_CURRENT,
    EVENT_LOG_TYPE_POWER,
    EVENT_LOG_TYPE_TRANSACTION,
    EVENT_LOG_TYPE_OTHER,
    EVENT_LOG_TYPE_NON_ROLLOVER,
#ifndef METER_RETROFIT
    EVENT_LOG_TYPE_CONTROL,
#endif
    EVENT_LOG_TYPE_NB
} event_log_type_e;

// State of the event log data stored in flash
typedef enum
{
    EVENT_LOG_STATE_INITIALIZED = 0x01,
    EVENT_LOG_STATE_SYNCED_WITH_METER = 0x02,
    // After erase, all bits of the NOR flash are set to 1
    EVENT_LOG_STATE_UNINITIALIZED = 0xFF
} evt_log_state_e;

// Event log status (one per type of logs)
typedef struct
{
    // The number of entries in this profile
    uint16_t profile_entries;
    // The number of entries in use
    uint16_t entries_in_use;
    // The last entry that has been read
    uint16_t current_entry;
    // The CRC of the last entry that has been read
    uint16_t crc;
} evt_log_status_t;

// Event log data that are persisted in flash
typedef struct
{
    // Status for each type of event log
    evt_log_status_t log_status[EVENT_LOG_TYPE_NB];
    // Type of the first log to check
    event_log_type_e first_log;
    // State of the event log data stored in flash
    evt_log_state_e state;
} evt_log_data_t;

/**
 * @brief   Check validity of event log data persisted in flash
 * @param   data_p: pointer on data stored in flash
 * @return  true if data are valid, false if they have been modified
 */
bool Event_Log_Profiles_checkDataValidity(evt_log_data_t * data_p);

/**
 * @brief   Start the event log profiles reading
 * @param   delay_ms
 *          Delay before the first reading in ms
 */
void Event_Log_Profiles_start(uint32_t delay_ms);

#endif /* EVENT_LOG_PROFILES_H_ */
