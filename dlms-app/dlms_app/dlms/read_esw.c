/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#define DEBUG_LOG_MODULE_NAME "READ_ESW"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#include "gpio.h"
#include "board.h"
#include "common.h"
#include "dlms_lock.h"
#include "dlms_com.h"
#include "data_notification.h"

#include "wirepas_com.h"

#include "include/cosem.h"
#include "include/helpers.h"
#include "include/client.h"
#include "include/bitarray.h"

#include <malloc.h>

#define DECODE_ESW

#ifdef ESW_POLLING_PERIOD_S
// Period in seconds configured at build time
#if ESW_POLLING_PERIOD_S < 10
#error "ESW polling period can't be smaller than 10 seconds"
#endif
#define ESW_POLLING_PERIOD_MS               (ESW_POLLING_PERIOD_S * 1000)
#else
// ESW polling period: 1 minute by default
#define ESW_POLLING_PERIOD_MS               (60 * 1000)
#endif // ESW_POLLING_PERIOD_S

// Position of the last gasp bit in the Event Status Word
// We need to manually set this bit to 1 in the last read ESW when the smart
// meter notifies the NIC of a power failure before to send it
#define ESW_LAST_GASP_BIT                   85
// Position of the first breath bit
#define ESW_FIRST_BREATH_BIT                86

static const uint32_t m_period_ms = ESW_POLLING_PERIOD_MS;

// ESW OBIS
static obis_code_t c_esw_ln = { 0, 0, 94, 91, 18, 255 };

// OBIS of the generated push for the ESW
static obis_code_t c_push_esw_ln = { 0, 4, 25, 9, 0, 255 };

// Event Status Word (ESW): 128 bit size
#define ESW_ARRAY_SIZE              (128 >> 3)
typedef uint8_t esw_data_t[ESW_ARRAY_SIZE];

/* FSM handler */
static uint32_t read_esw_fsm(void);

typedef enum
{
    ESW_STATE_TAKE_LOCK,
    ESW_STATE_CONNECT,
    ESW_STATE_READ_ESW,
    ESW_STATE_CLOSE,
    ESW_STATE_SCHEDULE
} esw_fsm_state_e;

typedef struct
{
    message msg;
    gxReplyData reply;
} esw_dyn_param_t;

typedef struct
{
    esw_fsm_state_e state;
    uint32_t start_ts;
    esw_data_t esw;
    esw_data_t new_esw;
    bool running;
    bool run_again;
} esw_stat_param_t;

static esw_stat_param_t m_param;
static esw_dyn_param_t * mp_dyn_param;

#define RESCHEDULE_ASAP()   \
                App_Scheduler_addTask_execTime(read_esw_fsm, \
                                               APP_SCHEDULER_SCHEDULE_ASAP,\
                                               SFSM_EXECUTION_TIME_US)
// Prototypes
static void send_esw(bool last_gasp);

#if defined DECODE_ESW & defined APP_PRINTING
typedef struct
{
    uint8_t idx;
    const char * desc;
} id2desc_t;

static const id2desc_t mc_esw_desc[] =
{
    {  0, "R Phase - Voltage missing" },
    {  1, "Y Phase - Voltage missing" },
    {  2, "B Phase - Voltage missing" },
    {  3, "Over voltage in any phase" },
    {  4, "Low voltage in any phase" },
    {  5, "Voltage unbalance" },
    {  6, "R Phase current reverse (Import type only)" },
    {  7, "Y Phase current reverse (Import type only)" },
    {  8, "B Phase current reverse (Import type only)" },
    {  9, "Current unbalance" },
    { 10, "Current bypass/short" },
    { 11, "Over current in any phase" },
    { 12, "Very low PF" },
    { 51, "Earth Loading" },
    { 81, "Influence of permanent magnet or ac/dc electromagnet" },
    { 82, "Neutral disturbance - HF, dc or alternate method" },
    { 83, "Meter cover opening" },
    { 84, "Meter load disconnected/Meter load connected" },
    { 85, "Last Gasp - Occurrence" },
    { 86, "First Breath - Restoration" },
    { 87, "Increment in billing counter (Manual/MRI reset)" }
};

static const char * bit2desc(uint8_t idx)
{
    for (uint8_t i = 0; i < ARRAY_SIZE(mc_esw_desc); i++)
    {
        if (idx == mc_esw_desc[i].idx)
        {
            return mc_esw_desc[i].desc;
        }
    }
    return "unspecified";
}
#endif

static void print_esw(void)
{
    LOGI("ESW:");
    LOG_BUFFER(LVL_INFO, m_param.new_esw, ESW_ARRAY_SIZE);

#if defined DECODE_ESW
    for (uint8_t i = 0; i < ESW_ARRAY_SIZE; i++)
    {
        if (m_param.new_esw[i])
        {
            for (uint8_t j = 0; j < 7; j++)
            {
                if ((m_param.new_esw[i] & (1 << (7 - j))))
                {
                    uint8_t idx = (i << 3) + j;
                    (void) idx;
                    LOGI("  - %u: %s", idx, bit2desc(idx));
                }
            }
        }
    }
#endif
}

static void on_lock_aquired_cb()
{
    m_param.state = ESW_STATE_CONNECT;
    RESCHEDULE_ASAP();
}

static void open_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result)
    {
        m_param.state = ESW_STATE_READ_ESW;
    }
    else
    {
        m_param.state = ESW_STATE_SCHEDULE;
    }
    RESCHEDULE_ASAP();
}

static void close_cb(int32_t result)
{
    m_param.state = ESW_STATE_SCHEDULE;
    RESCHEDULE_ASAP();
}

static void on_data_sent_cb(const app_lib_data_sent_status_t * status)
{
    LOGI("ESW push sent with status: %d", status->success);
    if (status->success)
    {
        LOGI("Save ESW data");
        // Last reading was sent, we can move forward and start a new reading
        memcpy(&m_param.esw, &m_param.new_esw, ESW_ARRAY_SIZE);
    }
    else
    {
        // Let's try to resend the ESW
        send_esw(false);
    }
}

static void send_esw(bool last_gasp)
{
    message push_message;
    gxByteBuffer bb;
    int32_t res;

    bb_init(&bb);
    mes_init(&push_message);

    LOGI("Sending ESW (last gasp = %d)", last_gasp);
    if ((res = bb_setUInt8(&bb, DLMS_DATA_TYPE_BIT_STRING))
                                                    == DLMS_ERROR_CODE_OK &&
        (res = hlp_setObjectCount((ESW_ARRAY_SIZE << 3), &bb))
                                                    == DLMS_ERROR_CODE_OK &&
        (res = bb_set(&bb, m_param.new_esw, ESW_ARRAY_SIZE))
                                                    == DLMS_ERROR_CODE_OK &&
        Data_Notification_generatePushFromPull(MCM_AA_US, c_push_esw_ln,
                                               &bb, &push_message))
    {
        // Todo: check return code
        Wirepas_com_send_message(push_message.data[0]->data,
                                 push_message.data[0]->size,
                                 on_data_sent_cb,
                                 last_gasp ? WC_TYPE_LAST_GASP : WC_TYPE_ESW);
    }
    else
    {
        // Something went wrong
        // TODO: make multiple checks to know where the problem is
        LOGE("Cannot generate ESW data notification %d", res);
        (void) res;
    }

    mes_clear(&push_message);
    // Clear our temporary buffer
    bb_clear(&bb);
}

static void on_esw_read_cb(int32_t result)
{
    gxReplyData * reply_p = &mp_dyn_param->reply;
    if (DLMS_ERROR_CODE_OK == result)
    {
        const dlmsVARIANT * var = &reply_p->dataValue;
        if (DLMS_DATA_TYPE_BIT_STRING == var->vt)
        {
            const bitArray * ba = var->bitArr;
            uint32_t byte_size = (ba->size >> 3);
            if (byte_size == ESW_ARRAY_SIZE)
            {
                if (memcmp(m_param.esw, ba->data, ESW_ARRAY_SIZE))
                {
                    // ESW has changed, let's send it to the HES
                    memcpy(m_param.new_esw, ba->data, ESW_ARRAY_SIZE);
                    print_esw();
                    send_esw(false);
                }
                else
                {
                    LOGI("ESW: no change");
                }
            }
            else
            {
                LOGW("Unexpected size for ESW: %u bits", ba->size);
            }
        }
        else
        {
            LOGE("Unsupported format for ESW: %u", var->vt);
        }
    }
    else
    {
        // The object may not exist
        LOGE("Failed to read ESW: %d", result);
    }
    m_param.state = ESW_STATE_CLOSE;

    // Release the memory
    mes_clear(&mp_dyn_param->msg);
    reply_clear(&mp_dyn_param->reply);
    gxfree(mp_dyn_param);
    mp_dyn_param = NULL;

    RESCHEDULE_ASAP();
}

static uint32_t read_esw_fsm(void)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    uint32_t delay = SFSM_DELAY_PAUSED;
    Dlms_Lock_return_code_e lock_ret;

    SFSM_ENTRY(m_param.state);

    switch (m_param.state)
    {
        case ESW_STATE_TAKE_LOCK:
            m_param.running = true;
            LOGI("Reading ESW");
            m_param.start_ts = lib_time->getTimestampHp();
            lock_ret = Dlms_lock_take(DLMS_LOCK_ID_ESW,
                                      on_lock_aquired_cb,
                                      DLMS_LOCK_TYPE_WITHOUT_TRAFFIC);

            if (lock_ret == DLMS_LOCK_RET_ACQUIRED)
            {
                // Move to next state immediatelly
                m_param.state = ESW_STATE_CONNECT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            else if (lock_ret != DLMS_LOCK_RET_WAITING_FOR_LOCK)
            {
                LOGE("Cannot take or wait for lock");
                delay = SFSM_DELAY_RETRY;
            }
            break;

        case ESW_STATE_CONNECT:
            if (Meter_Connection_Management_open(open_cb, MCM_SECURED_AA) !=
                                                 DLMS_ERROR_CODE_OK)
            {
                m_param.state = ESW_STATE_SCHEDULE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case ESW_STATE_READ_ESW:
            if ((mp_dyn_param = gxcalloc(1, sizeof *mp_dyn_param)))
            {
                int res;
                mes_init(&mp_dyn_param->msg);
                reply_init(&mp_dyn_param->reply);

                res = cl_readLN(ms_p, c_esw_ln, DLMS_OBJECT_TYPE_DATA,
                                2, NULL, &mp_dyn_param->msg);

                if (DLMS_ERROR_CODE_OK == res)
                {
                    Dlms_Com_call_async(&mp_dyn_param->msg, &mp_dyn_param->reply, on_esw_read_cb);
                    delay = SFSM_DELAY_PAUSED;
                }
                else
                {
                    LOGE("cl_readLN: %d", res);
                    m_param.state = ESW_STATE_CLOSE;
                    delay = APP_SCHEDULER_SCHEDULE_ASAP;
                    // Release the memory
                    mes_clear(&mp_dyn_param->msg);
                    reply_clear(&mp_dyn_param->reply);
                    gxfree(mp_dyn_param);
                    mp_dyn_param = NULL;
                }
            }
            else
            {
                m_param.state = ESW_STATE_CLOSE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case ESW_STATE_CLOSE: // close com
            if (Meter_Connection_Management_close(close_cb) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_param.state = ESW_STATE_SCHEDULE;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case ESW_STATE_SCHEDULE:
            SFSM_CHECK();
            const uint32_t stop_ts = lib_time->getTimestampHp();
            const uint32_t delay_ms = lib_time->getTimeDiffUs(m_param.start_ts, stop_ts) / 1000;

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

            Dlms_lock_release(DLMS_LOCK_ID_ESW);
            m_param.state = ESW_STATE_TAKE_LOCK;
            m_param.running = false;
            if (m_param.run_again)
            {
                LOGI("Rescheduling immediately");
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
                m_param.run_again = false;
            }
            break;
    } /* switch (_state) */

    SFSM_EXIT();

    return delay;
}

#ifdef BOARD_GPIO_ID_SMART_METER_PUSH
static uint32_t read_esw_from_notification_task(void)
{
    LOGI("Event push");
    if (m_param.running)
    {
        // The FSM is already running
        LOGI("read ESW FSM is already running");
        // Run again the ESW FSM after current run (event sent before meter is ready to communicate)
        m_param.run_again = true;
    }
    else
    {
        m_param.state = ESW_STATE_TAKE_LOCK;
        RESCHEDULE_ASAP();
    }
    return APP_SCHEDULER_STOP_TASK;
}
#endif

#ifdef BOARD_GPIO_ID_SMART_METER_POWER_FAIL
static uint32_t send_last_gasp_task(void)
{

    // Set last gasp bit in the last read ESW
    memcpy(&m_param.new_esw, &m_param.esw, ESW_ARRAY_SIZE);
    m_param.new_esw[ESW_LAST_GASP_BIT >> 3] |= (1 << (7 - (ESW_LAST_GASP_BIT % 8)));
    // Clear the first breath bit (DSA-167)
    m_param.new_esw[ESW_FIRST_BREATH_BIT >> 3] &= ~(1 << (7 - (ESW_FIRST_BREATH_BIT % 8)));
    print_esw();

    // Don't send the ESW with last gasp bit set if it has already been sent
    if (memcmp(m_param.new_esw, m_param.esw, ESW_ARRAY_SIZE))
    {
        LOGI("Event power failure, send ESW with last gasp bit set");
        // Send ESW
        send_esw(true);
    }
    else
    {
        LOGI("Event power failure, ESW already sent");
    }
    return APP_SCHEDULER_STOP_TASK;
}
#endif

#if defined BOARD_GPIO_ID_SMART_METER_PUSH | defined BOARD_GPIO_ID_SMART_METER_POWER_FAIL
static void on_smart_meter_push_event_cb(gpio_id_t id, gpio_in_event_e event)
{
#ifdef BOARD_GPIO_ID_SMART_METER_PUSH
    if (id == BOARD_GPIO_ID_SMART_METER_PUSH)
    {
        App_Scheduler_addTask_execTime(read_esw_from_notification_task,
                                       APP_SCHEDULER_SCHEDULE_ASAP,
                                       SFSM_EXECUTION_TIME_US);
    }
#endif
#ifdef BOARD_GPIO_ID_SMART_METER_POWER_FAIL
    if (id == BOARD_GPIO_ID_SMART_METER_POWER_FAIL)
    {
        App_Scheduler_addTask_execTime(send_last_gasp_task,
                                       APP_SCHEDULER_SCHEDULE_ASAP,
                                       SFSM_EXECUTION_TIME_US);
    }
#endif
}
#endif

static void push_notifications_init(void)
{
#if defined BOARD_GPIO_ID_SMART_METER_PUSH | defined BOARD_GPIO_ID_SMART_METER_POWER_FAIL
    gpio_in_cfg_t evt_cfg =
    {
        .event_cb = on_smart_meter_push_event_cb,
        .event_cfg = GPIO_IN_EVENT_FALLING_EDGE,
        .in_mode_cfg = GPIO_IN_PULL_UP
    };
#endif

#ifdef BOARD_GPIO_ID_SMART_METER_PUSH
    if (GPIO_RES_OK != Gpio_inputSetCfg(BOARD_GPIO_ID_SMART_METER_PUSH,
                                        &evt_cfg))
    {
        LOGE("Failed to configure GPIO for smart meter push events");
    }
#endif

#ifdef BOARD_GPIO_ID_SMART_METER_POWER_FAIL
    if (GPIO_RES_OK != Gpio_inputSetCfg(BOARD_GPIO_ID_SMART_METER_POWER_FAIL,
                                        &evt_cfg))
    {
        LOGE("Failed to configure GPIO for smart meter power "
            "fail events");
    }
#endif
}

void Read_Esw_start(void)
{
    // Listen to smart meter push notifications if enabled
    push_notifications_init();

    // Start main FSM
    LOGI("Starting read_esw_fsm");
    App_Scheduler_addTask_execTime(read_esw_fsm, APP_SCHEDULER_SCHEDULE_ASAP,
                                   SFSM_EXECUTION_TIME_US);
}

void Read_Esw_reschedule(void)
{
    if (! m_param.running)
    {
        // Start main FSM
        LOGI("Reshedule ESW fsm");
        App_Scheduler_addTask_execTime(read_esw_fsm, APP_SCHEDULER_SCHEDULE_ASAP,
                                       SFSM_EXECUTION_TIME_US);
    }
    else
    {
        m_param.run_again = true;
    }
}
