/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 */

/**
 * @file hdlc.h
 *
 * HDLC module to handle the communication with the other side
 */

#ifndef _HDLC_H_
#define _HDLC_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define BUFFER_MAX_SIZE 512 // It should be able to contain max HDLC frame

typedef void (*hdlc_message_sent_cb)(bool success);

typedef bool (*hdlc_message_rx_cb)(const uint8_t * message, size_t len);

/**
 * \brief   List of return code
 */
typedef enum
{
    /** Operation is successful */
    HDLC_RES_OK            = 0,
    HDLC_REX_TX_QUEUE_FULL = 1,
} hdlc_res_e;

hdlc_res_e HDLC_init(hdlc_message_rx_cb rx_cb);

hdlc_res_e HDLC_send_message(const uint8_t * message, size_t len);

#endif  //_HDLC_H_
