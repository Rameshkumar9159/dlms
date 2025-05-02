/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */
#include "app_scheduler.h"
#include "common.h"
#include "hal_api.h"
#include "board.h"
#include "meter_uart.h"
#include "dlms_com.h"

//Gurux DLMS includes.
#include "include/client.h"
#include "include/bytebuffer.h"
#include "include/dlmssettings.h"
#include "include/variant.h"
#include "include/cosem.h"
#include "include/client.h"
#include "include/converters.h"
#include "include/gxobjects.h"
#include "include/notify.h"
#include "include/gxmem.h" // gxmalloc

#define DEBUG_LOG_MODULE_NAME "DLMS_COM"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

// Uncomment to have some preliminary checks on frame
// It is only informal info, no change in behavior
#define CHECK_RX_BUFFER

// Default timeout to wait for RX
// Some meters are really slow e.g. reading a row of the blockload profile
// can take more than 8s
#define DEFAULT_TIMEOUT_S           15

// Estimated execution time for timeout task
#define TIMEOUT_TASK_EXEC_TIME_US   100

#define RESCHEDULE_TIMEOUT_TASK()   App_Scheduler_addTask_execTime(timeout_task, \
                                                                   m_param.timeout_ms,\
                                                                   TIMEOUT_TASK_EXEC_TIME_US)

#define CANCEL_TIMEOUT_TASK()       App_Scheduler_cancelTask(timeout_task)

#define SCHEDULE_EXIT_TASK_ASAP()   App_Scheduler_addTask_execTime(notify_caller_and_exit_task, \
                                                                   APP_SCHEDULER_SCHEDULE_ASAP,\
                                                                   TIMEOUT_TASK_EXEC_TIME_US)


typedef struct
{
    message * msg_p;
    gxReplyData * reply_p;
    int pos;
    gxByteBuffer rr;
    // Caller
    operation_result_cb cb;
    uint8_t * rx_buffer;
    uint32_t timeout_ms;
    int result;
} dlms_com_param_t;

// Global variables
static dlms_com_param_t m_param;

#define HDLC_HEADER_SIZE 17
#define FRAME_SIZE (HDLC_HEADER_SIZE + DLMS_COM_PDU_SIZE)

/* Forward declaration */
static uint32_t timeout_task(void);

static Meter_uart_rx_code_e rx_data_cb(uint8_t * bytes, size_t size);

// Very basic sanity check in order to print a warning
// GuruX library may return a misleading error if buffer is corrupted

#define HDLC_FRAME_OVERHEAD    6  // not able to find where HDLC_HEADER_SIZE == 17 come from

#ifdef CHECK_RX_BUFFER
static void check_rx_buffer(const uint8_t * bytes, size_t size)
{
    DLMS_INTERFACE_TYPE type = Common_getNicInterfaceType();
    if ( type == DLMS_INTERFACE_TYPE_HDLC)
    {
        if (size < HDLC_FRAME_OVERHEAD)
        {
            LOGW("HDLC frame is invalid (too short)");
        }
        else if ((bytes[0] != 0x7E) || (bytes[size-1] != 0x7E))
        {
            // Frame may be corrupted, but if there is garbage before and after the frame
            // library should be able to parse it.
            LOGW("HDLC frame may be corrupted");
        }

    }
    else if (type == DLMS_INTERFACE_TYPE_WRAPPER)
    {
        if (size < sizeof(wrapper_header_t))
        {
            LOGW("Wrapper frame is invalid (too short)");
        }
        else
        {
            wrapper_header_t hdr;

            Common_wrapperFrameGetHeader(bytes, &hdr);
            if (hdr.version != WRAPPER_VERSION)
            {
                // Frame may be corrupted, but if there is garbage before and after the frame
                // library should be able to parse it.
                LOGW("Wrapper frame may be corrupted");
            }
            else if (hdr.length > (size - sizeof(wrapper_header_t)))
            {
                LOGI("Incomplete wrapper frame %d < %d", size - sizeof(wrapper_header_t), hdr.length);
            }
            else if (hdr.length < (size - sizeof(wrapper_header_t)))
            {
                LOGE("Wrapper frame is too big %d > %d", size - sizeof(wrapper_header_t), hdr.length);
            }
            else
            {
                LOGD("Wrapper frame complete");
            }
        }
    }
}
#endif

static uint32_t notify_caller_and_exit_task(void)
{
    Meter_uart_unregister_rx_cb(rx_data_cb);
    bb_clear(&m_param.rr);
    m_param.cb(m_param.result);
    gxfree(m_param.rx_buffer);
    // Cancel our timeout task
    CANCEL_TIMEOUT_TASK();
    return APP_SCHEDULER_STOP_TASK;
}

static Meter_uart_rx_code_e rx_data_cb(uint8_t * bytes, size_t size)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    gxReplyData  * reply_p = m_param.reply_p;
    const message * msg_p = m_param.msg_p;
    gxByteBuffer frameData;
    int32_t result;

#ifdef CHECK_RX_BUFFER
    check_rx_buffer(bytes, size);
#endif

    // Convert RX buffer to bytebuffer
    bb_attach(&frameData, bytes, size, size);
    result = cl_getData(ms_p, &frameData, reply_p);

    // In MR association, some meters send an encrypted response to
    // an authenticated and encrypted request. Gurux will return an error
    // in cl_getData() if the security level does not match
    if (result == DLMS_ERROR_CODE_INVALID_DECIPHERING_ERROR &&
        ms_p->authentication == DLMS_AUTHENTICATION_LOW &&
        ms_p->cipher.security == DLMS_SECURITY_AUTHENTICATION_ENCRYPTION)
    {
        LOGI("Retry get data from meter's response with ENCRYPTION instead of AUTHENTICATION_ENCRYPTION");
        ms_p->cipher.security = DLMS_SECURITY_ENCRYPTION;
        bb_attach(&frameData, bytes, size, size);
        result = cl_getData(ms_p, &frameData, reply_p);
    }

    if (result != DLMS_ERROR_CODE_OK)
    {
        // Problem with the data
        m_param.result = result;
        SCHEDULE_EXIT_TASK_ASAP();
        return METER_UART_RX_DISCARD;
    }

    if (!reply_p->complete)
    {
        // Not enough data yet
        return METER_UART_RX_NOT_FULL;
    }

    // At this stage data should be ok!

    // Check if RX was splited and require new iteration
    if (reply_isMoreData(reply_p))
    {
        result = cl_receiverReady(ms_p, reply_p->moreData, &m_param.rr);
        if (DLMS_ERROR_CODE_OK != result)
        {
            bb_clear(&m_param.rr);
            LOGE("Cannot gen next: %u", result);
            m_param.result = result;
            SCHEDULE_EXIT_TASK_ASAP();
        }
        else
        {
            LOGD("Next RX packet");
            Meter_uart_send_frame(m_param.rr.data, m_param.rr.size);
            RESCHEDULE_TIMEOUT_TASK();
        }
    }
    else
    {
        // Move to next TX packet
        m_param.pos++;
        if (m_param.pos != msg_p->size)
        {
            LOGD("Next %d/%d", m_param.pos, msg_p->size );
            Meter_uart_send_frame(msg_p->data[m_param.pos]->data, msg_p->data[m_param.pos]->size);
            RESCHEDULE_TIMEOUT_TASK();
        }
        else
        {
            // This is the end
            m_param.result = result;
            SCHEDULE_EXIT_TASK_ASAP();
        }
    }
    return METER_UART_RX_OK;
}

/* *************************************** */
/* READ DATA BLOCK FSM & helpers           */

static uint32_t timeout_task(void)
{
    LOGE("Rx Timeout");
    m_param.result = DLMS_ERROR_CODE_RECEIVE_FAILED;
    SCHEDULE_EXIT_TASK_ASAP();

    return APP_SCHEDULER_STOP_TASK;
}

/* ************************************ */
/* PUBLIC API                           */
void Dlms_Com_init(void)
{
    // Nothing to do
}

void Dlms_Com_call_async(message * msg_p, gxReplyData * reply_p,
                         operation_result_cb cb)
{
    Dlms_Com_call_async_with_timeout(msg_p, reply_p, cb, DEFAULT_TIMEOUT_S);
}

void Dlms_Com_call_async_with_timeout(message * msg_p, gxReplyData * reply_p,
                                      operation_result_cb cb, uint16_t timeout_s)
{
    if (cb == NULL)
    {
        LOGE("No CB provided");
        return;
    }
    m_param.cb = cb;

    if (msg_p->size == 0)
    {
        LOGW("No data");
        // Considered as ok for now.
        m_param.cb(DLMS_ERROR_CODE_OK);
        return;
    }

    m_param.rx_buffer = gxmalloc(FRAME_SIZE);
    if (m_param.rx_buffer == NULL)
    {
        LOGE("Cannot allocate rx buffer");
        m_param.cb(DLMS_ERROR_CODE_TEMPORARY_FAILURE);
        return;
    }

    if (!Meter_uart_register_rx_cb(rx_data_cb, m_param.rx_buffer, FRAME_SIZE))
    {
        LOGE("Cannot register rx_cb");
        m_param.cb(DLMS_ERROR_CODE_TEMPORARY_FAILURE);
        gxfree(m_param.rx_buffer);
        return;
    }

    m_param.msg_p = msg_p;
    m_param.reply_p = reply_p;
    m_param.pos = 0;
    memset(&m_param.rr, 0x00, sizeof(m_param.rr));
    if (! timeout_s)
    {
        m_param.timeout_ms = DEFAULT_TIMEOUT_S * 1000;
    }
    else
    {
        m_param.timeout_ms = timeout_s * 1000;
    }

    // Send first request part, then all is managed in the uart rx callback
    LOGD("Block count=%d", msg_p->size);
    Meter_uart_send_frame(msg_p->data[0]->data, msg_p->data[0]->size);

    RESCHEDULE_TIMEOUT_TASK();
}
