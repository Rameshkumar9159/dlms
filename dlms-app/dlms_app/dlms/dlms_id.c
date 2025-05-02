/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    dlms_id.c
 * \brief   Implements reading of meter ids
 */
#include <stdio.h> // snprintf

#include "common.h"
#include "dlms_com.h"
#include "dlms_id.h"
#include "dlms_lock.h"
#include "meter_connection_management.h"
#include "server_attribute_manager.h"
#include "client_attribute_manager.h"
#include "wirepas_eol_testing.h"

//Gurux DLMS includes.
#include "include/gxmem.h" // gxcalloc
#include "include/cosem.h" // cosem2
#include "include/client.h" // cl_readLN

#define DEBUG_LOG_MODULE_NAME "DLMS_ID "
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#include <malloc.h>

// Definition
#define RETRY_PERIOD_MS           (60 * 1000)

#define RESCHEDULE_ASAP()   \
            App_Scheduler_addTask_execTime(read_meter_name_fsm, \
                                           APP_SCHEDULER_SCHEDULE_ASAP,\
                                           SFSM_EXECUTION_TIME_US)

typedef enum
{
    READID_STATE_TAKE_LOCK,
    READID_STATE_CONNECT_PC,
    READID_STATE_PC_READ_SERIAL_NUMBER,
    READID_STATE_CLOSE_PC,
    READID_STATE_CONNECT_MR,
    READID_STATE_CLOSE_MR,
    READID_STATE_CONNECT_US,
    READID_STATE_US_READ_DEVICE_ID,
    READID_STATE_CLOSE_US,
    READID_STATE_CONNECT_FU,
    READID_STATE_CLOSE_FU,
   READID_STATE_EXIT
} readid_fsm_state_e;

typedef struct
{
    bool serial_number_read;
    // Variable required to read object
    message  data;
    gxReplyData  reply;
} readid_param_t;

// State machine state
static readid_fsm_state_e m_state;

static dlms_id_read_meter_serial_number_cb m_cb;

// Variables
static readid_param_t * mp_param;

// Const data
// Serial number logical name
static const obis_code_t c_sn_ln = {0, 0, 96, 1, 0, 255};
// Device id logical name
static obis_code_t c_device_id_ln = { 0, 0, 96, 1, 2, 255 };

// Prototypes
static uint32_t read_meter_name_fsm(void);

void Dlms_Id_readSerialNumber(dlms_id_read_meter_serial_number_cb cb)
{
    if (cb == NULL)
    {
        LOGE("Dlms id callback cannot be null");
        // TODO: Must generate an error
        return;
    }

    // if mp_param is already allocated, then this is a case of reentrency
    if (mp_param)
    {
        LOGE("Trying to re-enter");
        cb(false);
        return;
    }

    // FSM
    m_state = READID_STATE_TAKE_LOCK;
    // Caller
    m_cb = cb;

    // Start main FSM
    RESCHEDULE_ASAP();
}

static void rid_lock_aquired_cb()
{
    m_state = READID_STATE_CONNECT_PC;

    RESCHEDULE_ASAP();
}

static void rid_open_pc_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result)
    {
        m_state = READID_STATE_PC_READ_SERIAL_NUMBER;
    }
    else
    {
        LOGE("PC association failed: %d", result);
        m_state = READID_STATE_CONNECT_MR;
    }

    RESCHEDULE_ASAP();
}

static void rid_open_mr_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result)
    {
        m_state = READID_STATE_CLOSE_MR;
    }
    else
    {
        LOGE("MR association failed: %d", result);
#ifndef METER_RETROFIT
        m_state = READID_STATE_CONNECT_US;
#else
        m_state = READID_STATE_EXIT;
#endif
    }

    RESCHEDULE_ASAP();
}

static void rid_open_us_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result)
    {
        m_state = READID_STATE_US_READ_DEVICE_ID;
    }
    else
    {
        LOGE("US association failed: %d", result);
        m_state = READID_STATE_CONNECT_FU;
    }

    RESCHEDULE_ASAP();
}

static void rid_open_fu_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result)
    {
        m_state = READID_STATE_CLOSE_FU;
    }
    else
    {
        LOGE("FU association failed: %d", result);
        m_state = READID_STATE_EXIT;
    }

    RESCHEDULE_ASAP();
}

static void read_serial_number_cb(int32_t result)
{
    gxReplyData * reply_p = &mp_param->reply;
    const uint8_t * sn_p = NULL;
    uint8_t sn_len = 0;
    char str[11];
    bool read = false;

    if (DLMS_ERROR_CODE_OK == result)
    {
        dlmsVARIANT * var_p = &reply_p->dataValue;
        read = true;
        if (DLMS_DATA_TYPE_STRING == var_p->vt)
        {
            sn_p = var_p->strVal->data;
            sn_len = var_p->strVal->size;
        }
        else if (DLMS_DATA_TYPE_OCTET_STRING == var_p->vt)
        {
            sn_p = var_p->byteArr->data;
            sn_len = var_p->byteArr->size;
        }
        else if (DLMS_DATA_TYPE_UINT32 == var_p->vt)
        {
            // UINT32MAX is 4 294 967 295 so char[11] is enough to hold the max value
            snprintf(str, sizeof(str), "%lu", (uint32_t)var_p->lVal);

            sn_p = (uint8_t *)str;
            sn_len = strlen(str);
        }
        else
        {
            read = false;
            LOGE("Unsupported format for meter SN: %u", reply_p->dataValue.vt);
        }

        // Notify EOL test module of successful com with meter
        Wirepas_eol_testing_notify_meter_com_ok();
    }
    else
    {
        LOGE("Failed to read meter SN: %d", result);
    }

    mp_param->serial_number_read = read;

    if (read)
    {
        const uint8_t * stored_sn_p;
        uint8_t stored_sn_len;

        // Read the persistent SN
        Server_Attribute_Manager_readMeterSerialNumber(&stored_sn_p, &stored_sn_len);

        // No SN yet, store the one just read
        if (! stored_sn_len)
        {
            Server_Attribute_Manager_writeMeterSerialNumber(sn_p, sn_len);
        }
        // If the SN has changed
        else if (memcmp(stored_sn_p, sn_p, stored_sn_len))
        {
            char * str_p = gxmalloc(sn_len + 1);
            // NIC swapping, reset the registration status
            if (str_p)
            {
                memcpy(str_p, sn_p, sn_len);
                str_p[sn_len] = '\0';
                // stored_snp is guaranteed to be null terminated
                LOGE("NIC swapping detected: stored SN: %s, cur SN: %s", stored_sn_p, str_p);
                gxfree(str_p);
            }
            else
            {
                LOGE("NIC swapping detected");
            }
#ifdef METER_REGISTRATION_ENABLED
            // "Emulated" factory reset (including the reset of the NIC registration status)
            LOGI("Factory reset");
            if (ATTR_SUCCESS != Server_Attr_Manager_reset())
            {
                LOGE("Failed to reset server data");
            }
            if (ATTR_SUCCESS != Client_Attr_Manager_reset())
            {
                LOGE("Failed to reset client data");
            }
#endif
            // Write the new SN
            Server_Attribute_Manager_writeMeterSerialNumber(sn_p, sn_len);
        }
        // Else it's the same SN, no need to update it
        // Currently, we may truncate the SN and not detect NIC swapping
        // We could add a CRC of the full SN to handle this case if needed
    }

    // Release the memory
    mes_clear(&mp_param->data);
    reply_clear(&mp_param->reply);

    m_state = READID_STATE_CLOSE_PC;

    RESCHEDULE_ASAP();
}

static void read_device_id_cb(int32_t result)
{
    gxReplyData * reply_p = &mp_param->reply;
    const uint8_t * devid_p = NULL;
    uint8_t devid_len = 0;
    // str must be kept in this scope as it is referenced by devid_p later.
    // cppcheck-suppress variableScope
    char str[11];
    bool read = false;

    if (DLMS_ERROR_CODE_OK == result)
    {
        dlmsVARIANT * var_p = &reply_p->dataValue;
        read = true;
        if (DLMS_DATA_TYPE_STRING == var_p->vt)
        {
            devid_p = var_p->strVal->data;
            devid_len = var_p->strVal->size;
        }mp_para
        else if (DLMS_DATA_TYPE_OCTET_STRING == var_p->vt)
        {
            devid_p = var_p->byteArr->data;
            devid_len = var_p->byteArr->size;
        }
        else if (DLMS_DATA_TYPE_UINT32 == var_p->vt)
        {
            // UINT32MAX is 4 294 967 295 so char[11] is enough to hold the max value
            snprintf(str, sizeof(str), "%lu", (uint32_t)var_p->lVal);
            devid_p = (uint8_t *)str;
            devid_len = strlen(str);
        }
        else
        {
            read = false;
            LOGW("Unsupported format for device id: %u", var_p->vt);
        }
    }
    else
    {
        LOGW("Failed to read device id: %d", result);
    }

    if (read)
    {
        const uint8_t * stored_devid_p;
        uint8_t stored_devid_len;

        // Read the persistent device id
        Server_Attribute_Manager_readMeterDeviceId(&stored_devid_p, &stored_devid_len);

        // No device id yet or device id has changed, store the new one
        // We should not check if NIC has been swapped again
        if (! stored_devid_len ||
            memcmp(stored_devid_p, devid_p, stored_devid_len))
        {
            Server_Attribute_Manager_writeMeterDeviceId(devid_p, devid_len);
        }
    }

    // Release the memory
    mes_clear(&mp_param->data);
    reply_clear(&mp_param->reply);

    m_state = READID_STATE_CLOSE_US;

    RESCHEDULE_ASAP();
}

static void rid_close_pc_cb(int32_t result)
{
    (void) result;
    m_state = READID_STATE_CONNECT_MR;

    RESCHEDULE_ASAP();
}

static void rid_close_mr_cb(int32_t result)
{
    (void) result;
#ifndef METER_RETROFIT
    m_state = READID_STATE_CONNECT_US;
#else
    m_state = READID_STATE_EXIT;
#endif

    RESCHEDULE_ASAP();
}

static void rid_close_us_cb(int32_t result)
{
    (void) result;
    m_state = READID_STATE_CONNECT_FU;

    RESCHEDULE_ASAP();
}

static void rid_close_fu_cb(int32_t result)
{
    (void) result;
    m_state = READID_STATE_EXIT;

    RESCHEDULE_ASAP();
}

static uint32_t read_meter_name_fsm(void)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    uint32_t delay = SFSM_DELAY_PAUSED;
    int res = -1;
    Dlms_Lock_return_code_e lock_ret;

    SFSM_ENTRY(m_state);

    switch (m_state)
    {
        case READID_STATE_TAKE_LOCK:
            LOGI("Checking associations and reading meter SN and device id");
            lock_ret = Dlms_lock_take(DLMS_LOCK_ID_ID,
                                      rid_lock_aquired_cb,
                                      DLMS_LOCK_TYPE_WITHOUT_TRAFFIC);

            if (lock_ret == DLMS_LOCK_RET_ACQUIRED)
            {
                // Move to next state immediatelly
                m_state = READID_STATE_CONNECT_PC;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            else if (lock_ret != DLMS_LOCK_RET_WAITING_FOR_LOCK)
            {
                LOGE("Cannot take or wait for lock");
                delay = RETRY_PERIOD_MS;
            }
            break;

        case READID_STATE_CONNECT_PC:
            mp_param = gxcalloc(1, sizeof *mp_param);
            // Check that we are not out of memory
            if (! mp_param)
            {
                LOGE("No memory to allocate params");
                m_state = READID_STATE_EXIT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            else
            {
                if (Meter_Connection_Management_open(rid_open_pc_cb, MCM_AA_PC) !=
                                                            DLMS_ERROR_CODE_OK)
                {
                    m_state = READID_STATE_EXIT;
                    delay = APP_SCHEDULER_SCHEDULE_ASAP;
                }
            }
            break;

        case READID_STATE_PC_READ_SERIAL_NUMBER:
            mes_init(&mp_param->data);
            reply_init(&mp_param->reply);

            res = cl_readLN(ms_p, c_sn_ln,
                            DLMS_OBJECT_TYPE_DATA,
                            2, NULL, &mp_param->data);

            if (DLMS_ERROR_CODE_OK == res)
            {
                Dlms_Com_call_async(&mp_param->data, &mp_param->reply,
                                    read_serial_number_cb);
                delay = SFSM_DELAY_PAUSED;
            }
            else
            {
                LOGE("cl_readLN: %d", res);
                m_state = READID_STATE_CLOSE_PC;
                // Release the memory
                mes_clear(&mp_param->data);
                reply_clear(&mp_param->reply);
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case READID_STATE_CLOSE_PC:
            // Close the connection
            if (Meter_Connection_Management_close(rid_close_pc_cb) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_state = READID_STATE_CONNECT_MR;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case READID_STATE_CONNECT_MR:
            if (Meter_Connection_Management_open(rid_open_mr_cb, MCM_AA_MR) !=
                                                        DLMS_ERROR_CODE_OK)
            {
                m_state = READID_STATE_CONNECT_US;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case READID_STATE_CLOSE_MR:
            // Close the connection
            if (Meter_Connection_Management_close(rid_close_mr_cb) !=
                                                            DLMS_ERROR_CODE_OK)
            {
#ifndef METER_RETROFIT
                m_state = READID_STATE_CONNECT_US;
#else
                m_state = READID_STATE_EXIT;
#endif
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
                }
            break;

        case READID_STATE_CONNECT_US:
            if (Meter_Connection_Management_open(rid_open_us_cb, MCM_AA_US) !=
                                                        DLMS_ERROR_CODE_OK)
            {
                m_state = READID_STATE_CONNECT_FU;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case READID_STATE_US_READ_DEVICE_ID:
            mes_init(&mp_param->data);
            reply_init(&mp_param->reply);

            res = cl_readLN(ms_p, c_device_id_ln,
                            DLMS_OBJECT_TYPE_DATA,
                            2, NULL, &mp_param->data);

            if (DLMS_ERROR_CODE_OK == res)
            {
                Dlms_Com_call_async(&mp_param->data, &mp_param->reply,
                                    read_device_id_cb);
                delay = SFSM_DELAY_PAUSED;
            }
            else
            {
                LOGE("cl_readLN: %d", res);
                m_state = READID_STATE_CLOSE_US;
                // Release the memory
                mes_clear(&mp_param->data);
                reply_clear(&mp_param->reply);
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case READID_STATE_CLOSE_US:
            // Close the connection
            if (Meter_Connection_Management_close(rid_close_us_cb) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_state = READID_STATE_CONNECT_FU;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case READID_STATE_CONNECT_FU:
            if (Meter_Connection_Management_open(rid_open_fu_cb, MCM_AA_FU) !=
                                                        DLMS_ERROR_CODE_OK)
            {
                m_state = READID_STATE_EXIT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case READID_STATE_CLOSE_FU:
            // Close the connection
            if (Meter_Connection_Management_close(rid_close_fu_cb) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_state = READID_STATE_EXIT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case READID_STATE_EXIT:
            Dlms_lock_release(DLMS_LOCK_ID_ID);
            if (mp_param != NULL && mp_param->serial_number_read)
            {
                // Stop the task
                delay = APP_SCHEDULER_STOP_TASK;
                m_cb(mp_param->serial_number_read);
                gxfree(mp_param);
                mp_param = NULL;
                SFSM_CHECK();
            }
            else
            {
                // Release mp_param as it will be allocated again once lock aquired
                if (mp_param != NULL)
                {
                    gxfree(mp_param);
                    mp_param = NULL;
                }

                // Retry after a delay
                // Do not call SFSM_CHECK
                m_state = READID_STATE_TAKE_LOCK;
                delay = RETRY_PERIOD_MS;
            }

            break;
    }
    SFSM_EXIT();

    return delay;
}
