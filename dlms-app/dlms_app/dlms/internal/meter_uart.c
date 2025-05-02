/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    meter_uart.c
 * \brief   Low level uart handling
 */

#include "meter_uart.h"
#include "usart.h"
#include "app_scheduler.h"
#include "common.h"

#define DEBUG_LOG_MODULE_NAME "MET_UART"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

// Tested with 1 and it is enough so 2 should be fine
#define NUMBER_BYTES_TIMEOUT    2

// Execution measured to be up to 16ms for parsing billing
#define PROCESS_RX_BUFFER_EXEC_TIME_US (20 * 1000)

#define PRINT_TASK_EXEC_TIME_US 100

#define LOG_RX
//#define LOG_RX_DUMP
#define LOG_TX
//#define LOG_TX_DUMP

/**
 * Internal buffer used when no other module is registered
 * It allows to receive async traffic and potentially
 * handle it
*/
static uint8_t m_internal_buffer[512];

/**
 * Rx Buffer provided by caller
 */
static uint8_t * m_rx_buffer = m_internal_buffer;

/**
 * Max size for current RX buffer
 */
static size_t m_rx_max_size = sizeof(m_internal_buffer);

/**
 * Current index of rx buffer
 */
static size_t m_rx_index = 0;

static bool m_rx_overflow = false;

/**
 * Timeout to wait for the line to be silent before calling upper level
 * It is dependant on UART baudrate and computed in init function.
 */
static uint32_t m_timeout_ms = 0;

static on_frame_rx_cb_t m_rx_cb = NULL;

// Default handler that can be overloaded
size_t __attribute__((weak)) Custom_Uart_Prehook_handler(uint8_t * buffer, size_t size, bool * is_prehook_data_p)
{
    // By default we don't consume anything from the buffer
    *is_prehook_data_p = false;
    return 0;
}

/**
 * This task is executed as soon as rx_buffer is not empty and
 * line is silent for enough time
*/
static uint32_t process_rx_buffer_task(void)
{
    Meter_uart_rx_code_e res;
    size_t rx_bytes;
    size_t consumed_bytes;
    bool is_prehook_data = false;

    if (!m_rx_buffer)
    {
        LOGE("No RX buffer");
        return APP_SCHEDULER_STOP_TASK;
    }

#if defined(LOG_RX)
    LOGI("RX - %u", m_rx_index);
#endif
#if defined(LOG_RX_DUMP)
    LOG_BUFFER(LVL_INFO, m_rx_buffer, m_rx_index);
#endif

    // Call a prehook, in case someone want to intercept some data
    rx_bytes = m_rx_index;
    consumed_bytes = Custom_Uart_Prehook_handler(m_rx_buffer, m_rx_index, &is_prehook_data);
    // Custom_Uart_Prehook_handler can be overidden, its return is not known.
    // cppcheck-suppress knownConditionTrueFalse
    if (consumed_bytes)
    {
        if (consumed_bytes > m_rx_index)
        {
            LOGE("Cannot consume more than provided\n");
            consumed_bytes = m_rx_index;
        }
        memmove(m_rx_buffer, m_rx_buffer + consumed_bytes, consumed_bytes);
        m_rx_index = m_rx_index - consumed_bytes;

        if (m_rx_index == 0)
        {
            // All was consummed by prehook
            LOGD("All bytes consumed by the prehook");
            return APP_SCHEDULER_STOP_TASK;
        }
        else
        {
            LOGD("%u/%u bytes consumed by the prehook", consumed_bytes, rx_bytes);
            (void) rx_bytes;
        }
    }

    if (m_rx_buffer == m_internal_buffer)
    {
        if (! is_prehook_data)
        {
            // Not under critical section as just for debug purpose
            LOGW("Unexpected RX (%d)", m_rx_overflow);
            LOG_BUFFER(LVL_WARNING, m_internal_buffer, m_rx_index);
            m_rx_overflow = false;
            m_rx_index = 0;
        }
        else
        {
            LOGD("Prehook data in internal buffer");
        }
        return APP_SCHEDULER_STOP_TASK;
    }
    // Custom_Uart_Prehook_handler can be overidden, its return is not known.
    // cppcheck-suppress knownConditionTrueFalse
    if (consumed_bytes)
    {
#if defined(LOG_RX)
        LOGD("Post hook RX - %u", m_rx_index);
#endif
#if defined(LOG_RX_DUMP)
        LOG_BUFFER(LVL_DEBUG, m_rx_buffer, m_rx_index);
#endif
    }

    if (m_rx_overflow)
    {
        LOGE("Overflow on Rx");
        m_rx_overflow = false;
        // Do not stop execution, but it will fail later
    }

    // No need to process rx_buffer under critical section.
    // If called "too early" the upper level will reply METER_UART_RX_NOT_FULL
    if (m_rx_cb != NULL && m_rx_buffer != NULL)
    {
        rx_bytes = m_rx_index;
        res = m_rx_cb(m_rx_buffer, rx_bytes);
        if (res == METER_UART_RX_OK || res == METER_UART_RX_DISCARD)
        {
            // Check if bytes were received while handling message
            lib_system->enterCriticalSection();
            if (rx_bytes < m_rx_index)
            {
                memmove(m_rx_buffer, m_rx_buffer + rx_bytes, m_rx_index - rx_bytes);
                m_rx_index = m_rx_index - rx_bytes;
                // No need to reschedule, we are already rescheduled by IRQ handler
            }
            else
            {
                // All data is consumed
                m_rx_index = 0;
            }
            lib_system->exitCriticalSection();
        } // else METER_UART_RX_NOT_FULL, wait for more bytes
    }
    else
    {
        // Receiving byte with no recipient (Should never happen)
        LOGW("RX With no recipient %u",  m_rx_index);
        // Discard it
        m_rx_index = 0;
    }

    return APP_SCHEDULER_STOP_TASK;
}

/**
 * \brief   UART RX callback
 */
// This is used as a callback, and we want to avoid modifying the caller's signature.
// cppcheck-suppress constParameterCallback
static void uartReceiveCb(uint8_t * chars, size_t n)
{
    size_t free = n;
    uint8_t * buffer;

    if (! m_rx_buffer)
    {
        // We should not print log here so just return
        return;
    }
    buffer = m_rx_buffer;

    if ((n + m_rx_index) > m_rx_max_size)
    {
        free = m_rx_max_size - m_rx_index;
        m_rx_overflow = true;
    }

    // Copy the rx bytes to caller buffer
    memcpy(buffer + m_rx_index, chars, free);
    m_rx_index += free;

     /* Schedule the processing with a bit of delay.
       Each time a new byte is received, the processing will be postponed. So ideally,
       it should be processed only one time when the frame is full.
      */
    App_Scheduler_addTask_execTime(process_rx_buffer_task,
                                   m_timeout_ms,
                                   PROCESS_RX_BUFFER_EXEC_TIME_US);
}

void Meter_uart_init(uint32_t baudrate)
{
    Usart_init(baudrate, UART_FLOW_CONTROL_NONE);
    Usart_enableReceiver(uartReceiveCb);
    Usart_setEnabled(true);
    Usart_receiverOn();

    // 1 byte = 8 bit.
    // We use 8n1 so 1 byte is 10 bits (1 start bit/ no parity / 1 stop bit)
    // Timeout set as line being silent for 4 bytes at configured baudrate
    // Timeout must be at least 1ms
    m_timeout_ms = MAX(((10 * 1000) / baudrate) * NUMBER_BYTES_TIMEOUT, 1);

}

bool Meter_uart_register_rx_cb(on_frame_rx_cb_t cb, uint8_t * buffer, size_t max_size)
{
    bool res = false;
    LOGD("register_rx_cb 0x%08X", cb);
    lib_system->enterCriticalSection();
    if (m_rx_cb == NULL)
    {
        // Copy the remaining bytes in the customer buffer if any
        if (m_rx_index)
        {
            m_rx_index = MIN(m_rx_index, max_size);
            memcpy(buffer, m_rx_buffer, m_rx_index);
        }
        m_rx_buffer = buffer;
        m_rx_max_size = max_size;

        m_rx_cb = cb;
        res = true;
    }
    lib_system->exitCriticalSection();

    if (!res)
    {
        LOGE("Uart rx is already in use");
    }
    else if (m_rx_index)
    {
        LOGD("Copied %u bytes from internal buffer", m_rx_index);
    }
    return res;
}

bool Meter_uart_unregister_rx_cb(const on_frame_rx_cb_t cb)
{
    bool res = false;
    LOGD("unregister_rx_cb 0x%08X", cb);
    lib_system->enterCriticalSection();
    if (m_rx_cb == cb)
    {
        // Copy the remaining bytes in the customer buffer if any
        if (m_rx_index)
        {
            m_rx_index = MIN(m_rx_index, sizeof(m_internal_buffer));
            memcpy(m_internal_buffer, m_rx_buffer, m_rx_index);
        }
        // Switch to our own internal buffer
        m_rx_buffer = m_internal_buffer;
        m_rx_max_size = sizeof(m_internal_buffer);

        m_rx_cb = NULL;
        res = true;
    }

    lib_system->exitCriticalSection();
    if (!res)
    {
        LOGE("Cb is not the current registered one");
    }
    else if (m_rx_index)
    {
        LOGD("Copied %u bytes from registered buffer", m_rx_index);
    }
    return res;
}

uint32_t Meter_uart_send_frame(const uint8_t * bytes, size_t size)
{
#if defined(LOG_TX)
    LOGI("TX - %u", size);
#endif
#if defined(LOG_TX_DUMP)
    LOG_BUFFER(LVL_INFO, bytes, size);
#endif
    return Usart_sendBuffer(bytes, size);
}
