/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef METER_UART_H_
#define METER_UART_H_

#include <stdint.h>

// Parameter used during connection
#define MAX_UART_FRAME  512

typedef enum {
    METER_UART_RX_OK, // Message was consumed and can be erased
    METER_UART_RX_NOT_FULL, // Message was not full on receiver point of view, so keep it and wait for more bytes
    METER_UART_RX_DISCARD, // Message is not correct from receiver point of view and can be discarded
} Meter_uart_rx_code_e;

typedef Meter_uart_rx_code_e (*on_frame_rx_cb_t)(uint8_t * bytes, size_t size);

void Meter_uart_init(uint32_t baudrate);

bool Meter_uart_register_rx_cb(on_frame_rx_cb_t cb, uint8_t * buffer, size_t max_size);

bool Meter_uart_unregister_rx_cb(const on_frame_rx_cb_t cb);

uint32_t Meter_uart_send_frame(const uint8_t * bytes, size_t size);

#endif /* METER_UART_H_ */