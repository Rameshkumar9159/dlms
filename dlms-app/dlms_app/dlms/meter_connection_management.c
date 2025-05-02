/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    meter_connection_management.c
 * \brief   functions handling the connection to the smart meters
 */

#include "meter_connection_management.h"
#include "common.h"
#include "dlms_com.h" // Should be modified later
#include "application_association.h"
#include "meter_clock.h"

// Gurux DLMS includes.
#include "include/cosem.h"
#include "include/client.h"
#include "include/dlmssettings.h"
#include "include/gxmem.h"

#define DEBUG_LOG_MODULE_NAME "CONN_MNG"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#define RESCHEDULE_ASAP(task)   \
                App_Scheduler_addTask_execTime(task, \
                                               APP_SCHEDULER_SCHEDULE_ASAP,\
                                               SFSM_EXECUTION_TIME_US)

static obis_code_t c_clock_ln = {0, 0, 1, 0, 0, 255};

static obis_code_t c_mr_ic_ln = {0, 0, 43, 1, 2, 255};
static obis_code_t c_us_ic_ln = {0, 0, 43, 1, 3, 255};
static obis_code_t c_fu_ic_ln = {0, 0, 43, 1, 5, 255};

// FSM handling the smart meter connection opening
typedef enum
{
    CONNECT_STATE_AA_PC_ESTABLISH,
    CONNECT_STATE_READ_IC,
    CONNECT_STATE_AA_PC_RELEASE,
    CONNECT_STATE_ESTABLISH,
    CONNECT_STATE_READ_CLOCK,
    CONNECT_STATE_EXIT,
} conn_open_fsm_state_e;

// FSM handling the smart meter connection closing
/* ** */
typedef enum
{
    CLOSE_STATE_RELEASE,
    CLOSE_STATE_EXIT,
} conn_close_fsm_state_e;

// Prototypes
static uint32_t open_connection_fsm(void);
static uint32_t close_connection_fsm(void);

typedef struct {
    message msg;
    gxReplyData reply;
    uint32_t invocation_counter;
    const uint8_t * ic_ln;
    // Input
    aa_type_e aa;
    // FSM
    int32_t result;
    // Caller
    operation_result_cb cb;
} conn_param_t;

// Static variables
// Connection status notification
static mcm_status_cb_f m_status_cb;
// FSM parameters
static conn_param_t * mp_param;
// MCM open FSM state
static conn_open_fsm_state_e m_open_state;
// MCM close FSM state
static conn_close_fsm_state_e m_close_state;

// Prototypes
static void mcm_send_notification(bool connected);

void Meter_Connection_Management_subscribeStatusCb(mcm_status_cb_f cb)
{
    LOGI("MCM status callback subscribed: 0x%08X", cb);
    m_status_cb = cb;
}

int Meter_Connection_Management_open(operation_result_cb cb,
                                     mcm_aa_e aa)
{
    if (cb == NULL)
    {
        LOGE("MCM open callback cannot be null");
        return DLMS_ERROR_CODE_UNKNOWN;
    }

   // if mp_param is already allocated, then this is a case of reentrency
    if (mp_param)
    {
        LOGE("Trying to re-enter in MCM open FSM");
        return DLMS_ERROR_CODE_UNKNOWN;
    }

    mp_param = gxcalloc(1, sizeof *mp_param);
    // Check that we are not out of memory
    if (! mp_param)
    {
        LOGE("No memory to allocate MCM open param");
        return DLMS_ERROR_CODE_OUTOFMEMORY;
    }

    mp_param->result = DLMS_ERROR_CODE_OK;
    mp_param->cb = cb;
    mp_param->invocation_counter = 0;

    // Initialize the FSM state
    // Default state for secured AA, the invocation counter has to be read first
    m_open_state = CONNECT_STATE_AA_PC_ESTABLISH;
    switch (aa)
    {
        case MCM_AA_PC:
            m_open_state = CONNECT_STATE_ESTABLISH;
            mp_param->aa = APPLICATION_ASSOCIATION_PC;
            break;
        case MCM_AA_MR:
            mp_param->ic_ln = c_mr_ic_ln;
            mp_param->aa = APPLICATION_ASSOCIATION_MR;
            break;
        case MCM_AA_US:
            mp_param->ic_ln = c_us_ic_ln;
            mp_param->aa = APPLICATION_ASSOCIATION_US;
            break;
        case MCM_AA_FU:
            mp_param->ic_ln = c_fu_ic_ln;
            mp_param->aa = APPLICATION_ASSOCIATION_FU;
            break;
    }

    // Start the task ASAP
    RESCHEDULE_ASAP(open_connection_fsm);

    return DLMS_ERROR_CODE_OK;
}

int Meter_Connection_Management_close(operation_result_cb cb)
{
    if (cb == NULL)
    {
        LOGE("MCM close callback cannot be null");
        return DLMS_ERROR_CODE_UNKNOWN;
    }

    // if mp_param is already allocated, then this is a case of reentrency
    if (mp_param)
    {
        LOGE("Trying to re-enter in MCM close FSM");
        return DLMS_ERROR_CODE_UNKNOWN;
    }
    mp_param = gxcalloc(1, sizeof *mp_param);
    // Check that we are not out of memory
    if (! mp_param)
    {
        LOGE("No memory to allocate MCM close param");
        return DLMS_ERROR_CODE_OUTOFMEMORY;
    }

    // Initialize the FSM state
    m_close_state = CLOSE_STATE_RELEASE;
    mp_param->result = DLMS_ERROR_CODE_OK;
    mp_param->cb = cb;

    // Start the task ASAP
    RESCHEDULE_ASAP(close_connection_fsm);

    return DLMS_ERROR_CODE_OK;
}

/* *************************************** */
/* CONNECT FSM & helpers                   */
/* ** */
static void mcm_aa_pc_open_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result)
    {
#ifdef METER_RETROFIT
        // No IC object for the retrofit meters
        m_open_state = CONNECT_STATE_READ_CLOCK;
#else
        m_open_state = CONNECT_STATE_READ_IC;
#endif
    }
    else
    {
        mp_param->result = result;
        m_open_state = CONNECT_STATE_EXIT;
    }

    RESCHEDULE_ASAP(open_connection_fsm);
}

static void mcm_read_invocation_counter_cb(int32_t result)
{
    gxReplyData * reply_p = &mp_param->reply;
    if (DLMS_ERROR_CODE_OK == result)
    {
        if (DLMS_DATA_TYPE_UINT32 == reply_p->dataValue.vt)
        {
            mp_param->invocation_counter = reply_p->dataValue.lVal;
            LOGI("Invocation counter: %u", reply_p->dataValue.lVal);
            if (mp_param->invocation_counter)
            {
                mp_param->invocation_counter++;
            }
        }
        else
        {
            // Initial value (0) will be used
            LOGE("Unsupported format for invocation counter: %u",
                reply_p->dataValue.vt);
        }
    }
    else
    {
        // The object may not exist
        LOGE("Failed to read invocation counter: %d", result);
    }
    mes_clear(&mp_param->msg);
    reply_clear(&mp_param->reply);
    m_open_state = CONNECT_STATE_READ_CLOCK;

    RESCHEDULE_ASAP(open_connection_fsm);
}

static void mcm_aa_pc_release_cb(int32_t result)
{
    (void) result;
    m_open_state = CONNECT_STATE_ESTABLISH;

    RESCHEDULE_ASAP(open_connection_fsm);
}

static void mcm_connect_cb(int32_t result)
{
    mp_param->result = result;

    m_open_state = CONNECT_STATE_EXIT;
    RESCHEDULE_ASAP(open_connection_fsm);
}

static void mcm_read_clock_cb(int32_t result)
{
    gxReplyData * reply_p = &mp_param->reply;
    dlmsVARIANT tmp;

    if (DLMS_ERROR_CODE_OK == result)
    {
        if (reply_p->dataValue.vt == DLMS_DATA_TYPE_OCTET_STRING && reply_p->dataValue.byteArr != NULL)
        {
            int ret;
            var_init(&tmp);
            ret = dlms_changeType2(&reply_p->dataValue, DLMS_DATA_TYPE_DATETIME, &tmp);
            if (ret != 0)
            {
                LOGE("Clock cannot change type: %u", ret);
            }
            else
            {
                LOGI("Clock: %u", tmp.dateTime->value);
                MeterClock_set(tmp.dateTime->value, tmp.dateTime->deviation);
            }
            var_clear(&tmp);
        }
        else
        {
            LOGE("Wrong Clock type: %u", reply_p->dataValue.vt);
        }
    }
    else
    {
        // The object may not exist
        LOGE("Failed to read clock: %d", result);
    }

    mes_clear(&mp_param->msg);
    reply_clear(&mp_param->reply);
    m_open_state = CONNECT_STATE_AA_PC_RELEASE;

    RESCHEDULE_ASAP(open_connection_fsm);
}


static void mcm_send_notification(bool connected)
{
    if (m_status_cb)
    {
        mcm_status_e status;
        if (mp_param->aa != APPLICATION_ASSOCIATION_PC)
        {
            if (connected)
            {
                status = MCM_AUTHENTIFIED;
            }
            else
            {
                status = MCM_AUTH_FAILED;
            }
        }
        else
        {
            if (connected)
            {
                status = MCM_CONNECTED;
            }
            else
            {
                status = MCM_CONN_FAILED;
            }
        }
        m_status_cb(status);
    }
}

static uint32_t open_connection_fsm(void)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    uint32_t delay = SFSM_DELAY_PAUSED;
    int res = -1;

    SFSM_ENTRY(m_open_state);

    switch (m_open_state)
    {
        case CONNECT_STATE_AA_PC_ESTABLISH:
            mp_param->result = AA_Establish(APPLICATION_ASSOCIATION_PC, 0,
                                            mcm_aa_pc_open_cb);
            if (mp_param->result != DLMS_ERROR_CODE_OK)
            {
                m_open_state = CONNECT_STATE_EXIT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case CONNECT_STATE_READ_IC:
            mes_init(&mp_param->msg);
            reply_init(&mp_param->reply);

            res = cl_readLN(ms_p, mp_param->ic_ln,
                            DLMS_OBJECT_TYPE_DATA,
                            2, NULL, &mp_param->msg);
            if (DLMS_ERROR_CODE_OK == res)
            {
                Dlms_Com_call_async(&mp_param->msg, &mp_param->reply, mcm_read_invocation_counter_cb);
                delay = SFSM_DELAY_PAUSED;
            }
            else
            {
                LOGE("cl_readLN for IC: %d", res);
                m_open_state = CONNECT_STATE_READ_CLOCK;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case CONNECT_STATE_READ_CLOCK:
            mes_init(&mp_param->msg);
            reply_init(&mp_param->reply);

            res = cl_readLN(ms_p, c_clock_ln,
                            DLMS_OBJECT_TYPE_CLOCK,
                            2, NULL, &mp_param->msg);

            if (DLMS_ERROR_CODE_OK == res)
            {
                Dlms_Com_call_async(&mp_param->msg, &mp_param->reply, mcm_read_clock_cb);
                delay = SFSM_DELAY_PAUSED;
            }
            else
            {
                LOGE("cl_readLN for CLK: %d", res);
                m_open_state = CONNECT_STATE_AA_PC_RELEASE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case CONNECT_STATE_AA_PC_RELEASE:
            AA_Release(mcm_aa_pc_release_cb);
            break;

       case CONNECT_STATE_ESTABLISH:
            mp_param->result = AA_Establish(mp_param->aa,
                                            mp_param->invocation_counter,
                                            mcm_connect_cb);
            if (mp_param->result != DLMS_ERROR_CODE_OK)
            {
                m_open_state = CONNECT_STATE_EXIT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case CONNECT_STATE_EXIT:
            mp_param->cb(mp_param->result);
            mcm_send_notification(DLMS_ERROR_CODE_OK == mp_param->result);
            m_open_state = CONNECT_STATE_AA_PC_ESTABLISH;

            // cleanup before next run
            gxfree(mp_param);
            mp_param = NULL;
            break;
    } /* switch() */

    SFSM_EXIT();

    return delay;
}

/* *************************************** */
/* CLOSE FSM & helpers                     */
static void mcm_release_cb(int32_t result)
{
    mp_param->result = result;
    m_close_state = CLOSE_STATE_EXIT;

    // Start the task ASAP
    RESCHEDULE_ASAP(close_connection_fsm);
}

static uint32_t close_connection_fsm(void)
{
    uint32_t delay = SFSM_DELAY_PAUSED;

    SFSM_ENTRY(m_close_state);

    switch (m_close_state)
    {
        case CLOSE_STATE_RELEASE:
            mp_param->result  = AA_Release(mcm_release_cb);
            if (mp_param->result != DLMS_ERROR_CODE_OK)
            {
                m_close_state = CLOSE_STATE_EXIT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case CLOSE_STATE_EXIT:
            mp_param->cb(mp_param->result);
            delay = SFSM_DELAY_PAUSED;
            m_close_state = CLOSE_STATE_RELEASE;

            // cleanup before next run
            gxfree(mp_param);
            mp_param = NULL;
            break;
    } /* switch() */

    SFSM_EXIT();
    return delay;
}

bool Meter_Connection_Management_addr_to_aa(uint16_t client_address,
                                            mcm_aa_e * aa_p)
{
    switch (client_address)
    {
        case PC_CLIENT_ADDRESS:
            *aa_p = MCM_AA_PC;
            break;

        case MR_CLIENT_ADDRESS:
            *aa_p = MCM_AA_MR;
            break;

        case US_CLIENT_ADDRESS:
            *aa_p = MCM_AA_US;
            break;

        case FU_CLIENT_ADDRESS:
            *aa_p = MCM_AA_FU;
            break;

        default:
            return false;
    }
    return true;
}

