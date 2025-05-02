/* Copyright 2019 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>

#include "uart_print.h"

#include "hal_api.h"
#include "board.h"
#include "api.h"
#include "gpio.h"
#include "em_eusart.h"
#include "em_cmu.h"

// Storage for debug messages.
#define BUFFER_SIZE     256
// Minimum free size of the buffer to append truncation mark when truncated
#define MIN_TRUNC_BUF_SIZE    2

#include "ringbuffer.h"
static volatile ringbuffer_t            m_uart_trace_tx_buffer;

static volatile uint32_t                m_bis_enabled;
static volatile bool                    m_bis_tx_active;

static uint32_t                         m_flush_delay_ms;

void __attribute__((__interrupt__))     EUSART1_TX_IRQHandler(void);

static bool Uart_Trace_init(uint32_t baudrate)
{
    EUSART_UartInit_TypeDef EUSART_init = EUSART_UART_INIT_DEFAULT_HF;

    // Module variables
    Ringbuffer_reset(m_uart_trace_tx_buffer);
    m_bis_tx_active = false;
    m_bis_enabled = 0;

    // Disable for RX
    Sys_disableAppIrq(EUSART1_RX_IRQn);
    Sys_clearFastAppIrq(EUSART1_RX_IRQn);
    // Disable for TX
    Sys_disableAppIrq(EUSART1_TX_IRQn);
    Sys_clearFastAppIrq(EUSART1_TX_IRQn);

    CMU_ClockEnable(cmuClock_EUSART1, true);
    CMU_ClockSelectSet(cmuClock_EM01GRPCCLK, cmuSelect_HFXO);

    EUSART_init.enable = eusartDisable;
    EUSART_init.baudrate = baudrate;
    EUSART_init.oversampling = EUSART_CFG0_OVS_X4;
    EUSART_UartInitHf(EUSART1, &EUSART_init);

    // Set UART TX GPIO
    hal_gpio_set_mode(BOARD_TRACE_TX_PORT,
                      BOARD_TRACE_TX_PIN,
                      GPIO_MODE_DISABLED);
    hal_gpio_clear(BOARD_TRACE_TX_PORT,
                   BOARD_TRACE_TX_PIN);

    GPIO->EUSARTROUTE[1].ROUTEEN = GPIO_EUSART_ROUTEEN_TXPEN;
    GPIO->EUSARTROUTE[1].TXROUTE = (BOARD_TRACE_TX_PORT << _GPIO_EUSART_TXROUTE_PORT_SHIFT) | (BOARD_TRACE_TX_PIN << _GPIO_EUSART_TXROUTE_PIN_SHIFT);

    EUSART_IntDisable(EUSART1, _EUSART_IF_MASK);
    EUSART_IntClear(EUSART1, _EUSART_IF_MASK);

    // APP IRQ
    Sys_clearFastAppIrq(EUSART1_TX_IRQn);
    Sys_enableFastAppIrq(EUSART1_TX_IRQn,
                         APP_LIB_SYSTEM_IRQ_PRIO_HI,
                         EUSART1_TX_IRQHandler);

    EUSART_Enable(EUSART1, eusartEnableTx);

    // Enable clock
    CMU_ClockEnable(cmuClock_EUSART1, true);
    // Disable deep sleep
    DS_Disable(DS_SOURCE_USART);
    // Set output
    hal_gpio_set_mode(BOARD_TRACE_TX_PORT,
                      BOARD_TRACE_TX_PIN,
                      GPIO_MODE_OUT_PP);

    return true;
}

static uint32_t Uart_Trace_sendBuffer(const void * buffer, uint32_t length)
{
    bool empty = false;
    uint32_t size_in = length;
    const uint8_t * data_out = (uint8_t *)buffer;

    Sys_enterCriticalSection();
    while ((uint32_t)Ringbuffer_free(m_uart_trace_tx_buffer) < length)
    {
        Sys_exitCriticalSection();
        return 0;
    }
    empty = (Ringbuffer_usage(m_uart_trace_tx_buffer) == 0);
    while (length--)
    {
        Ringbuffer_getHeadByte(m_uart_trace_tx_buffer) = *data_out++;
        Ringbuffer_incrHead(m_uart_trace_tx_buffer, 1);
    }
    if (empty)
    {
        EUSART_IntEnable(EUSART1, EUSART_IF_TXC);
        while ((EUSART_StatusGet(EUSART1) & EUSART_STATUS_TXFL) && (Ringbuffer_usage(m_uart_trace_tx_buffer) != 0))
        {
            EUSART_Tx(EUSART1, Ringbuffer_getTailByte(m_uart_trace_tx_buffer));
            Ringbuffer_incrTail(m_uart_trace_tx_buffer, 1);
        }
        m_bis_tx_active = true;
    }
    Sys_exitCriticalSection();
    return size_in;
}

static void Uart_Trace_flush(void)
{
    app_lib_time_timestamp_hp_t end;
    end = lib_time->addUsToHpTimestamp(lib_time->getTimestampHp(), 2000); // 2ms max

    while (m_bis_tx_active && lib_time->isHpTimestampBefore(lib_time->getTimestampHp(), end));
}

void __attribute__((__interrupt__)) EUSART1_TX_IRQHandler(void)
{
    EUSART_IntClear(EUSART1, EUSART_IF_TXC);

    if (Ringbuffer_usage(m_uart_trace_tx_buffer) == 0)
    {
        // when buffer becomes empty, reset indexes
        EUSART_IntDisable(EUSART1, EUSART_IF_TXC);
        m_bis_tx_active = false;
        return;
    }

    while ((EUSART_StatusGet(EUSART1) & EUSART_STATUS_TXFL) && (Ringbuffer_usage(m_uart_trace_tx_buffer) != 0))
    {
        EUSART_Tx(EUSART1, Ringbuffer_getTailByte(m_uart_trace_tx_buffer));
        Ringbuffer_incrTail(m_uart_trace_tx_buffer, 1);
    }
}

void UartPrint_init(uint32_t baudrate)
{
    // Initialize the hardware module
    Uart_Trace_init(baudrate);

    // Compute the delay in ms to flush a BUFFER_SIZE bytes buffer at the provided baudrate
    m_flush_delay_ms = ((BUFFER_SIZE * 10 * 1000) / baudrate) + 1;
}

int UartPrint_printf(const char * fmt, ...)
{
    uint8_t  buffer[BUFFER_SIZE];

    int len;
    va_list args;

    va_start(args, fmt);

    len = vsnprintf((char *)&buffer[0], BUFFER_SIZE, fmt, args);
    if (len >= BUFFER_SIZE)
    {
        // Log is truncated, add a special char, to let the user know
        buffer[BUFFER_SIZE - 2] = '#';
        buffer[BUFFER_SIZE - 1] = '\n';
        len = BUFFER_SIZE - 1;
    }

    // Try sending
    uint32_t lenw = Uart_Trace_sendBuffer(buffer, len);
    if (!lenw)
    {
        // buffer full, try to flush before giving up
        Uart_Trace_flush();
        lenw = Uart_Trace_sendBuffer(buffer, len);
        if (!lenw)
        {
            // Log is dropped, try to send a special char, to let the user know
            Uart_Trace_sendBuffer("?", 1);
        }
    }

    va_end(args);

    return len;
}

int UartPrintTrace(char lvl, const char * module_p, const char * fmt, ...)
{
    char buffer[BUFFER_SIZE];
    char full_fmt[BUFFER_SIZE];

    int len;
    va_list args;

    va_start(args, fmt);

    snprintf(full_fmt, sizeof(full_fmt), "[%s][%09lu] %c: %s\n", module_p,
             (lib_time->getTimestampCoarse() * 125) >> 4, lvl, fmt);
    len = vsnprintf(buffer, BUFFER_SIZE, full_fmt, args);

    if (len >= BUFFER_SIZE)
    {
        // Log is truncated, add a special char, to let the user know
        buffer[BUFFER_SIZE - 2] = '#';
        buffer[BUFFER_SIZE - 1] = '\n';
        len = BUFFER_SIZE - 1;
    }

    // Try sending
    uint32_t lenw = Uart_Trace_sendBuffer(buffer, len);
    if (!lenw)
    {
        // buffer full, try to flush before giving up
        Uart_Trace_flush();
        lenw = Uart_Trace_sendBuffer(buffer, len);
        if (!lenw)
        {
            // Log is dropped, try to send a special char, to let the user know
            Uart_Trace_sendBuffer("?", 1);
        }
    }

    va_end(args);

    return len;
}

void UartPrintDump(const uint8_t * buffer, uint16_t size)
{
    for (uint16_t i = 0; i < size; i++)
    {
        UartPrint_printf("%02X ", buffer[i]);
        if ((i & 0xF) == 0xF && i != size - 1)
        {
            UartPrint_printf("\n");
        }
    }
    UartPrint_printf("\n");
}

void UartPrintFlush(void)
{
    app_lib_time_timestamp_hp_t end;

    end = lib_time->addUsToHpTimestamp(lib_time->getTimestampHp(),
                                       m_flush_delay_ms * 1000);
    while (lib_time->isHpTimestampBefore(lib_time->getTimestampHp(),end));

}
