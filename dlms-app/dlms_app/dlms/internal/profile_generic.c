/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    dlms_read_profile_generic.c
 * \brief   implementation of the code to read profile generic
 */

#include "common.h"
#include "dlms_com.h"
#include "profile_generic.h"
#include "security_material.h"
#include "data_notification.h"
#include "meter_clock.h"

//Gurux DLMS includes.
#include "include/gxmem.h" // gxcalloc
#include "include/client.h" // cl_readLN
#include "include/cosem.h" // cosem_checkArray

#define DEBUG_LOG_MODULE_NAME "PROF_GEN"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#define RESCHEDULE_ASAP()   \
    App_Scheduler_addTask_execTime(profile_generic_read_fsm, \
                                   APP_SCHEDULER_SCHEDULE_ASAP,\
                                   SFSM_EXECUTION_TIME_US)

/* ************************************ */
/* FSM HANDLER                          */

static uint32_t profile_generic_read_fsm(void);

/* ************************************ */
/* FSM                                  */

typedef enum
{
    PROFILE_GENERIC_READ_PERIOD_STATE,
    PROFILE_GENERIC_READ_PROFILE_STATE,
    PROFILE_GENERIC_EXIT_STATE,
} profile_generic_fsm_state_e;

typedef enum
{
    PROFILE_GENERIC_READ_TYPE_ALL,
    PROFILE_GENERIC_READ_TYPE_BY_ROW,
    PROFILE_GENERIC_READ_TYPE_BY_ENTRY
} profile_generic_read_type;

typedef struct
{
    uint8_t * profile_obis;

    // Data set
    message data;
    gxReplyData reply;
    // FSM
    profile_generic_fsm_state_e state;
    profile_generic_result_e result;
    // Caller
    profile_generic_read_cb cb;
    profile_generic_read_type type;

    // Attribute of profile that can be read
    // Only needed when reading by row
    uint32_t period;
    uint32_t start_time;
    uint32_t end_time;
    uint32_t fallback_period;

    // Only needed when reading by entry
    uint16_t entry_to_read;
    uint16_t entry_count;
} read_prof_gen_param_t;

static read_prof_gen_param_t * mp_param;

static bool initialization(obis_code_t profile_obis, profile_generic_read_cb cb)
{
    if (cb == NULL)
    {
        LOGE("Cb cannot be null");
        return false;
    }

    // if mp_param is already allocated, then this is a case of reentrency
    if (mp_param)
    {
        LOGE("Trying to re-enter in Read Profile Generic FSM");
        cb(PROFILE_GENERIC_RESULT_REENTRENCY, NULL, 0, 0);
        return false;
    }
    mp_param = gxcalloc(1, sizeof *mp_param);
    // Check that we are not out of memory
    if (! mp_param)
    {
        LOGE("No memory to allocate Read Profile Generic params");
        cb(PROFILE_GENERIC_RESULT_NO_MEMORY, NULL, 0, 0);
        return false;
    }

    // Set the callback, obis and initial state
    mp_param->cb = cb;
    mp_param->profile_obis = (uint8_t *) profile_obis;
    mp_param->result = PROFILE_GENERIC_RESULT_OK;

    mp_param->period = 0;
    mp_param->start_time = 0;
    mp_param->end_time = 0;
    return true;
}

static void print_info(obis_code_t profile_obis, profile_generic_read_type type)
{
    LOGI("Read GP %u.%u.%u.%u.%u.%u (t=%d)",
        profile_obis[0], profile_obis[1], profile_obis[2],
        profile_obis[3], profile_obis[4], profile_obis[5],
        type);
}

void Profile_Generic_read_all_async(obis_code_t profile_obis,
                                    profile_generic_read_cb cb)
{
    if (!initialization(profile_obis, cb))
    {
        return;
    }

    mp_param->type = PROFILE_GENERIC_READ_TYPE_ALL;
    // Switch directly to read profile
    mp_param->state = PROFILE_GENERIC_READ_PROFILE_STATE;
    print_info(profile_obis, mp_param->type);
    RESCHEDULE_ASAP();
}


void Profile_Generic_read_by_row_async(obis_code_t profile_obis,
                                       uint32_t start_time,
                                       uint32_t fallback_period,
                                       profile_generic_read_cb cb)
{
    if (!initialization(profile_obis, cb))
    {
        return;
    }

    mp_param->type = PROFILE_GENERIC_READ_TYPE_BY_ROW;
    // Need to read period first
    mp_param->state = PROFILE_GENERIC_READ_PERIOD_STATE;

    mp_param->start_time = start_time;
    mp_param->fallback_period = fallback_period;

    print_info(profile_obis, mp_param->type);
    RESCHEDULE_ASAP();
}

void Profile_Generic_read_by_entry_async(obis_code_t profile_obis,
                                        uint16_t entry_to_read,
                                        uint16_t entry_count,
                                        profile_generic_read_cb cb)
{
    if (!initialization(profile_obis, cb))
    {
        return;
    }

    mp_param->type = PROFILE_GENERIC_READ_TYPE_BY_ENTRY;
    // Switch directly to read profile
    mp_param->state = PROFILE_GENERIC_READ_PROFILE_STATE;

    mp_param->entry_to_read = entry_to_read;
    mp_param->entry_count = entry_count;

    print_info(profile_obis, mp_param->type);
    RESCHEDULE_ASAP();
}

/* *************************************** */
/* READ PROFILE FSM & helpers              */

static void read_result_cb(int32_t result)
{
    uint16_t count = (uint16_t) -1;
    mp_param->state = PROFILE_GENERIC_EXIT_STATE;
    int pos;

    mes_clear(&mp_param->data);
    if (result != DLMS_ERROR_CODE_OK) {
        LOGE("read_result_cb %d", result);
        mp_param->result = PROFILE_GENERIC_RESULT_OTHER_ERROR;
        RESCHEDULE_ASAP();
        return;
    }

    // Keep position to restort it after check
    pos = mp_param->reply.data.position;
    // Check that we can parse the data
    if ((result = cosem_checkArray(&mp_param->reply.data, &count)) != 0)
    {
        LOGE("read_result_cb, cannot parse %d", result);
        mp_param->result = PROFILE_GENERIC_RESULT_OTHER_ERROR;
        RESCHEDULE_ASAP();
        (void) result;
        return;
    }

    // Restore position to keep array id
    mp_param->reply.data.position = pos;

    LOGD("Profile read count = %d", count);

    // Check the count and adapt result code
    if (count > 1)
    {
        LOGD("Multiple rows");
        if (mp_param->type != PROFILE_GENERIC_READ_TYPE_BY_ENTRY)
        {
            // Only reading by row can have multiple entries
            // Other cases is an error
            mp_param->result = PROFILE_GENERIC_RESULT_MULTIPLE_ROW;
            RESCHEDULE_ASAP();
            return;
        }

        // Check that number of rows is coherent
        if (count != mp_param->entry_count)
        {
            mp_param->result = PROFILE_GENERIC_RESULT_WRONG_NUMBER_OF_ROW;
            RESCHEDULE_ASAP();
            return;
        }
    }
    else if (count == 0)
    {
        LOGD("No Data");
        mp_param->result = PROFILE_GENERIC_RESULT_NO_DATA;
        RESCHEDULE_ASAP();
        return;
    }

    mp_param->result = PROFILE_GENERIC_RESULT_OK;
    RESCHEDULE_ASAP();
}

static void read_period_cb(int32_t result)
{
    gxReplyData * reply_p = &mp_param->reply;

    if (DLMS_ERROR_CODE_OK == result && reply_p->dataValue.vt == DLMS_DATA_TYPE_UINT32)
    {
        // Request was for attribute 4 of profile generic (period) so a uint32_t
        mp_param->period = reply_p->dataValue.ulVal;
        LOGI("Period is: %d", mp_param->period);
        mp_param->state = PROFILE_GENERIC_READ_PROFILE_STATE;
    }
    else
    {
        LOGE("Cannot read period: %d, using fallback", result);
        mp_param->period = mp_param->fallback_period;
        mp_param->state = PROFILE_GENERIC_READ_PROFILE_STATE;
    }

    RESCHEDULE_ASAP();
}

//Read profile generic object
static uint32_t profile_generic_read_fsm(void)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    const uint8_t *  ln_p = (uint8_t *) mp_param->profile_obis;
    message * data_p = &mp_param->data;
    gxtime start_gxtime, end_gxtime;
    int res = -1;
    gxProfileGeneric pg; // Use temporarly to generate request

    gxReplyData * reply_p = &mp_param->reply;
    // default delay is "reschedule ASAP"
    uint32_t delay = APP_SCHEDULER_SCHEDULE_ASAP;
    uint32_t meter_epoch;


    SFSM_ENTRY(mp_param->state);

    switch (mp_param->state)
    {
        case PROFILE_GENERIC_READ_PERIOD_STATE:
            mes_init(data_p);
            reply_init(reply_p);

            res = cl_readLN(ms_p, ln_p,
                            DLMS_OBJECT_TYPE_PROFILE_GENERIC,
                            4, NULL, data_p);

            if (DLMS_ERROR_CODE_OK == res)
            {
                Dlms_Com_call_async(data_p, reply_p, read_period_cb);
                delay = SFSM_DELAY_PAUSED;
            }
            else
            {
                LOGE("cl_readLN: %d", res);
                mp_param->state = PROFILE_GENERIC_EXIT_STATE;
            }
            break;


        case PROFILE_GENERIC_READ_PROFILE_STATE:
            // Initialize variables
            // Clear in case we executed previous state
            mes_clear(data_p);
            reply_clear(reply_p);

            // Initialize again
            mes_init(data_p);
            reply_init(reply_p);

            // Use ignoreValue flag to limit heap memory usage
            // Without this flag, the heap consumption is around 20 times
            // the size of the data
            reply_p->ignoreValue = 1;

            cosem_init2(BASE(pg), DLMS_OBJECT_TYPE_PROFILE_GENERIC,
                        mp_param->profile_obis);

            switch (mp_param->type) {
                case PROFILE_GENERIC_READ_TYPE_ALL:
                    res = cl_readLN(ms_p, ln_p,
                                    DLMS_OBJECT_TYPE_PROFILE_GENERIC, 2,
                                    NULL, data_p);

                    break;
                case PROFILE_GENERIC_READ_TYPE_BY_ROW:
                    //Read a row from start time with a length of capturePeriod
                    if (mp_param->start_time == 0 && MeterClock_get(&meter_epoch, NULL))
                    {
                        // Start time was not specified so read
                        // [now - period; now[
                        mp_param->start_time = meter_epoch - mp_param->period;
                    }

                    // Align start time on period boundary
                    mp_param->start_time = mp_param->start_time - (mp_param->start_time % mp_param->period);
                    // Remove one to have only one row each time
                    mp_param->end_time = mp_param->start_time + mp_param->period - 1;

                    LOGI("Start time: %u, end time: %u (period: %u)",
                        mp_param->start_time, mp_param->end_time, mp_param->period);

                    time_initUnix(&start_gxtime, mp_param->start_time);
                    time_initUnix(&end_gxtime, mp_param->end_time);
                    res = cl_readRowsByRange2(ms_p, &pg, &start_gxtime,
                                              &end_gxtime, data_p);
                break;
                case PROFILE_GENERIC_READ_TYPE_BY_ENTRY:
                    // mp_param->entry_count is converted by Gurux to the index
                    // of the last entry to retrieve (to_entry field in DLMS spec).
                    // entry_count = 0 is converted as to_entry = 0 which can be used
                    // to read the highest possible entry
                    // Unfortunately, it is not supported by all meters
                    LOGI("Read %u entr%s from entry %u",
                        mp_param->entry_count, mp_param->entry_count > 1 ? "ies" : "y",
                        mp_param->entry_to_read);
                    res = cl_readRowsByEntry(ms_p, &pg,
                                             mp_param->entry_to_read,
                                             mp_param->entry_count,
                                             data_p);
                break;
                default:
                LOGE("Wrong type");
                res = -1;

            }

            if (res != DLMS_ERROR_CODE_OK)
            {
                LOGE("Cannot generate request %d", res);
                mp_param->result = PROFILE_GENERIC_RESULT_OTHER_ERROR;
                mp_param->state = PROFILE_GENERIC_EXIT_STATE;
            }
            else
            {

                Dlms_Com_call_async(data_p, reply_p, read_result_cb);
                delay = SFSM_DELAY_PAUSED;
            }
            break;

        case PROFILE_GENERIC_EXIT_STATE:
            // Notify caller
            mp_param->cb(mp_param->result,
                         &mp_param->reply.data,
                         mp_param->start_time + mp_param->period,
                         mp_param->period); // Do not use mp_param->end_time that is 1 sec before next start_time

            delay = SFSM_DELAY_PAUSED;

            // cleanup
            mes_clear(data_p);
            reply_clear(reply_p);
            gxfree(mp_param);
            mp_param = NULL;
            break;

        default:
            LOGE("Invalid state %d", mp_param->state);
            break;
    } /* switch */

    SFSM_EXIT();

    return delay;
}
