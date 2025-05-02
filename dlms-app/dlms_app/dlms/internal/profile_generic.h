/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    profile_generic.h
 * \brief   interface to read profile generic
 */

#ifndef PROFILE_GENERIC_H_
#define PROFILE_GENERIC_H_

#include <stdint.h>

#include "common.h"
#include "include/dlmssettings.h"

typedef enum
{
    PROFILE_GENERIC_RESULT_OK = 0,
    PROFILE_GENERIC_RESULT_MULTIPLE_ROW,
    PROFILE_GENERIC_RESULT_NO_DATA,
    PROFILE_GENERIC_RESULT_CANNOT_READ_ROW,
    PROFILE_GENERIC_RESULT_BAD_FORMAT,
    PROFILE_GENERIC_RESULT_REENTRENCY,
    PROFILE_GENERIC_RESULT_NO_MEMORY,
    PROFILE_GENERIC_RESULT_WRONG_NUMBER_OF_ROW,
    PROFILE_GENERIC_RESULT_OTHER_ERROR
} profile_generic_result_e;

typedef void (* profile_generic_read_cb) (profile_generic_result_e result,
                                          gxByteBuffer * profile_data,
                                          uint32_t endtime,
                                          uint32_t period);


/**
 * @brief Read all profile
 * @param profile_obis
 *        Obis of the profile to read.
 * @param cb
 *        callback called once profile row is read or when an error happens
 */
void Profile_Generic_read_all_async(obis_code_t profile_obis,
                                    profile_generic_read_cb cb);

/**
 * @brief Read last row from profile with history
 * @param profile_obis
 *        Obis of the profile to read.
 * @param start_time
 *        Start time of the row. So read period will be [start_time; start_time + period of profile]
 *        If 0, current time of meter is used and interval is [now - period; now] so a full period is done
 * @param cb
 *        callback called once profile row is read or when an error happens
 */
void Profile_Generic_read_by_row_async(obis_code_t profile_obis,
                                       uint32_t start_time,
                                       uint32_t fallback_period,
                                       profile_generic_read_cb cb);


/**
 * @brief Read entry from profile generic with history
 * @param profile_obis
 *        Obis of the profile to read
 * @param entry_to_read
 *        id of the first entry to read, 0 means read current entry
 *        (given by the entriesInUse field of the profile generic)
 * @param entry_count
 *        count of entries to read starting from index given by entry_to_read
 * @param cb
 *        callback called once profile row is read or when an error happens
 */
void Profile_Generic_read_by_entry_async(obis_code_t profile_obis,
                                        uint16_t entry_to_read,
                                        uint16_t entry_count,
                                        profile_generic_read_cb cb);

#endif // PROFILE_GENERIC_H_
