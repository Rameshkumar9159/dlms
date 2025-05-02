/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include <string.h>  // For memcpy

#include "hdlc.h"
#include "api.h"
#include "yahdlc.h"
#include "usart.h"
#include "app_scheduler.h"

#ifndef PROVISIONING_UART_BAUDRATE
#error Please define the provisioning uart baudrate in your makefile
#endif

#define DEBUG_LOG_MODULE_NAME "HDLC    "
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#define MAX_TX_ATTEMPT 3

#define ACK_NACK_TIMEOUT_MS 500

#define TX_QUEUE_SIZE 8  // Same as max sequence (could be lower)

// Uncomment to check RX sequence and directly acknowledge same sequence (case
// of corrupted Ack/Nack)
// #define CHECK_SEQUENCE

static char   m_rx_buffer[BUFFER_MAX_SIZE];
static size_t m_rx_index = 0;

// Buffer used to decrypt HDLC message
static char m_rx_message[BUFFER_MAX_SIZE];  // Could be a little be smaller as
                                            // it is without HDLC headers

static hdlc_message_rx_cb m_rx_cb;

typedef struct
{
    uint8_t last_seq;  // Valid if in [0:7] (only 3 bits)
    bool    ack;       // True, last was a ack, False was a nack
} rx_context;

static rx_context m_rx_context;

// Buffer queue used to send messages to meter (TAP, sent status)
typedef struct
{
    app_lib_time_timestamp_hp_t timeout_ts;
    uint8_t                     message[BUFFER_MAX_SIZE];
    size_t                      size;
    uint8_t                     retries;
    bool                        ack_received;
    bool                        nack_received;
} message_to_meter_t;

static message_to_meter_t m_tx_queue[TX_QUEUE_SIZE];
static uint8_t            m_tx_in_index  = 0;
static uint8_t            m_tx_out_index = 0;


static uint32_t process_tx_queue_task(void)
{
    message_to_meter_t * message_p;
    size_t               hdlc_message_len = 0;
    // Allocated on stack, on purpose. We know we have room (4K and WP stack
    // only use a bit of it)
    char hdlc_message[BUFFER_MAX_SIZE * 2];

    LOGD("Processing Tx queue: IN=%d, out=%d", m_tx_in_index, m_tx_out_index);

    message_p = m_tx_queue + m_tx_out_index;

    if (m_tx_in_index == m_tx_out_index && message_p->size == 0)
    {
        LOGD("Nothing more to send");
        // Queue is empty, nothing to do
        return APP_SCHEDULER_STOP_TASK;
    }

    // Do some check on it
    if (message_p->size == 0)
    {
        LOGE("Len cannot be 0 %u", m_tx_out_index);
        m_tx_out_index = (m_tx_out_index + 1) % TX_QUEUE_SIZE;
        return APP_SCHEDULER_SCHEDULE_ASAP;
    }

    // Check if ack received
    if (message_p->ack_received)
    {
        LOGD("Process ack for %u", m_tx_out_index);
        // Remove buffer and reschedule to process next
        m_tx_out_index  = (m_tx_out_index + 1) % TX_QUEUE_SIZE;
        message_p->size = 0;
        return APP_SCHEDULER_SCHEDULE_ASAP;
    }

    // Check if it is a timeout (time elapsed and no nack)
    if (message_p->retries > 0 && !message_p->nack_received)
    {
        // Message is already sent
        // Compute delay and check timeout
        app_lib_time_timestamp_hp_t now = lib_time->getTimestampHp();
        if (message_p->timeout_ts > now)
        {
            // Tiemout is not over, wait a bit
            uint32_t delay_ms
                = lib_time->getTimeDiffUs(now, message_p->timeout_ts) / 1000;
            LOGD("Reschedule in %u ms", delay_ms);
            return delay_ms;
        }
    }

    message_p->retries++;

    // Check if too much retries
    if (message_p->retries > MAX_TX_ATTEMPT)
    {
        // Too much attemps
        LOGE("Too much attempts, discard %u", m_tx_out_index);
        m_tx_out_index = (m_tx_out_index + 1) % TX_QUEUE_SIZE;
        // Reset message
        message_p->size = 0;
        return APP_SCHEDULER_SCHEDULE_ASAP;
    }

    // Try to send a new one
    yahdlc_control_t ctrl_tx = {
        .frame  = YAHDLC_FRAME_DATA,
        .seq_no = m_tx_out_index,
    };

    if ((yahdlc_frame_data(&ctrl_tx,
                           (const char *) message_p->message,
                           message_p->size,
                           hdlc_message,
                           &hdlc_message_len))
        != 0)
    {
        // Cannot send message!! Drop it
        LOGE("Cannot generate TAP message for meter");
        m_tx_out_index = (m_tx_out_index + 1) % TX_QUEUE_SIZE;
        // Reset message
        message_p->size = 0;
        return APP_SCHEDULER_SCHEDULE_ASAP;
    }

    // LOG_BUFFER(LVL_DEBUG, hdlc_message, hdlc_message_len);
    Usart_sendBuffer(hdlc_message, hdlc_message_len);
    message_p->timeout_ts
        = lib_time->addUsToHpTimestamp(lib_time->getTimestampHp(),
                                       ACK_NACK_TIMEOUT_MS * 1000);

    LOGD("Sending %u attempt %u", m_tx_out_index, message_p->retries);

    // At least reschedule in timeout
    return ACK_NACK_TIMEOUT_MS;
}

/**
 * /brief   This function handle the HDLC part
 *          It checks that message is correct and generate
 */
static uint32_t process_rx_buffer_task(void)
{
    yahdlc_control_t ctrl_rx;
    size_t           message_size = 0;
    int              ret;
    size_t           bytes_to_discard = 0;

    // Process rx_buffer under critical section to avoid new bytes to be queued
    // As it takes 1 ms / bytes, even if processing takes 2 or 3 ms, bytes will
    // be queued on HW buffer
    lib_system->enterCriticalSection();

    LOGD("Process meter frame");
    // We fully process the message each time so reset the last state
    yahdlc_get_data_reset();

    ret = yahdlc_get_data(&ctrl_rx,
                          m_rx_buffer,
                          m_rx_index,
                          m_rx_message,
                          &message_size);
    if (ret == -ENOMSG)
    {
        // Return directly and wait for new bytes
        lib_system->exitCriticalSection();
        return APP_SCHEDULER_STOP_TASK;
    }

    if (ret == -EIO)
    {
        LOGE("Invalid CRC");
        // It will be a timeout on other side
        bytes_to_discard = message_size;
    }
    else
    {
        bytes_to_discard = ret;
        // Is it ack or nack
        if (ctrl_rx.frame == YAHDLC_FRAME_ACK
            || ctrl_rx.frame == YAHDLC_FRAME_NACK)
        {
            // Check that id is correct one
            if (ctrl_rx.seq_no != m_tx_out_index)
            {
                // Wrong id
                LOGE("Wrong Seq for ack %u vs %u",
                     m_tx_out_index,
                     ctrl_rx.seq_no);
            }
            else
            {
                message_to_meter_t * message_p = m_tx_queue + m_tx_out_index;
                // Update status and reschedule the TX task
                if (ctrl_rx.frame == YAHDLC_FRAME_ACK)
                {
                    LOGD("ACK for %d", m_tx_out_index);
                    message_p->ack_received = true;
                }
                else
                {
                    LOGD("NACK for %d", m_tx_out_index);
                    message_p->nack_received = true;
                }
                // Reschedule tx task
                App_Scheduler_addTask_execTime(process_tx_queue_task,
                                               APP_SCHEDULER_SCHEDULE_ASAP,
                                               100);
            }
        }
        else if (ctrl_rx.frame == YAHDLC_FRAME_DATA)
        {
            yahdlc_control_t ctrl_tx;
            // Ack or nack are small packets than can be allocated on stack
            char   hdlc_ack_nack[64];
            size_t hdlc_ack_nack_len;
            ctrl_tx.seq_no = ctrl_rx.seq_no;
            LOGD("Seq %d vs %d", ctrl_rx.seq_no, m_rx_context.last_seq);
#ifdef CHECK_SEQUENCE
            if (ctrl_rx.seq_no == m_rx_context.last_seq)
            {
                // Same seq, do not call the callback
                LOGI("Receive same sequence, resend ack/nack %d vs %d",
                     ctrl_rx.seq_no,
                     m_rx_context.last_seq);
                ctrl_tx.frame
                    = m_rx_context.ack ? YAHDLC_FRAME_ACK : YAHDLC_FRAME_NACK;
            }
            else
            {
#endif  // CHECK_SEQUENCE
        // Now the frame is complete, we can call upper level
                if (m_rx_cb((const uint8_t *) m_rx_message, message_size))
                {
                    ctrl_tx.frame = YAHDLC_FRAME_ACK;
                    LOGD("Generating ACK for %u", ctrl_rx.seq_no);
                }
                else
                {
                    ctrl_tx.frame = YAHDLC_FRAME_NACK;
                    LOGW("Generating NACK for %u", ctrl_rx.seq_no);
                }
#ifdef CHECK_SEQUENCE
            }
#endif  // CHECK_SEQUENCE
            if (yahdlc_frame_data(&ctrl_tx,
                                  NULL,
                                  0,
                                  hdlc_ack_nack,
                                  &hdlc_ack_nack_len)
                == 0)
            {
                Usart_sendBuffer(hdlc_ack_nack, hdlc_ack_nack_len);
                m_rx_context.last_seq = ctrl_rx.seq_no;
                m_rx_context.ack      = (ctrl_tx.frame == YAHDLC_FRAME_ACK);
            }
            else
            {
                // Cannot generate ACK/NACK
                // Fatal error, nothing we can really do
                LOGE("Cannot generate ACK/NACK");
            }
        }
    }

    // Keep what was not handled yet,
    // m_rx_index - bytes_to_discard should be equal to 0 always
    memmove(m_rx_buffer,
            m_rx_buffer + bytes_to_discard,
            m_rx_index - bytes_to_discard);
    m_rx_index -= bytes_to_discard;

    lib_system->exitCriticalSection();
    if (m_rx_index)
    {
        // Still something to process
        return APP_SCHEDULER_SCHEDULE_ASAP;
    }

    // All done
    return APP_SCHEDULER_STOP_TASK;
}

/**
 * \brief   UART RX callback
 */
// This is used as a callback, and we want to avoid modifying the caller's signature.
// cppcheck-suppress constParameterCallback
static void uartReceiveCb(uint8_t * chars, size_t n)
{
    // Copy the rx bytes to our current buffer
    memcpy(m_rx_buffer + m_rx_index, chars, n);
    m_rx_index += n;

    /* Schedule the processing with a bit of delay.
       Each time a new byte is received, the processing will be postpone. So
       ideally, it should be processed only one time when the frame is full. So
       in other words, data is processed as soon as line is silent for more than
       2ms. At 9600 baudrate, it takes ~1ms to be transfered, so 2ms is a good
       delay to postpone.
    */
    App_Scheduler_addTask_execTime(process_rx_buffer_task, 2, 100);
}

hdlc_res_e HDLC_init(hdlc_message_rx_cb rx_cb)
{
    LOGI("PROVISIONING_UART_BAUDRATE=%u", PROVISIONING_UART_BAUDRATE);
    /* Initialize Serial */
    Usart_init(PROVISIONING_UART_BAUDRATE, UART_FLOW_CONTROL_NONE);
    Usart_enableReceiver(uartReceiveCb);
    Usart_setEnabled(true);
    Usart_receiverOn();

    m_rx_cb = rx_cb;

    m_rx_context.last_seq = (uint8_t) (-1);

    return HDLC_RES_OK;
}

static message_to_meter_t * get_tx_buffer()
{
    message_to_meter_t * message_p;
    message_p = m_tx_queue + m_tx_in_index;
    if (m_tx_in_index == m_tx_out_index)
    {
        // It is either full, or empty!
        if (message_p->size != 0)
        {
            // All entries are in use
            LOGE("TX queue full");
            return NULL;
        }
    }

    m_tx_in_index = (m_tx_in_index + 1) % TX_QUEUE_SIZE;

    // Reset message
    memset(message_p, 0, sizeof(message_to_meter_t));

    return message_p;
}

hdlc_res_e HDLC_send_message(const uint8_t * message, size_t len)
{
    message_to_meter_t * message_p;

    message_p = get_tx_buffer();
    if (message_p == NULL)
    {
        return HDLC_REX_TX_QUEUE_FULL;
    }

    memcpy(message_p->message, message, len);
    message_p->size = len;

    // Reschedule tx task
    App_Scheduler_addTask_execTime(process_tx_queue_task,
                                   APP_SCHEDULER_SCHEDULE_ASAP,
                                   100);

    return HDLC_RES_OK;
}
