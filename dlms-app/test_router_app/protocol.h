/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    protocol.h
 * \brief   Test router protocol helper function 
 */

#ifndef PROTOCOL_H_
#define PROTOCOL_H_

#ifdef EOL_MODE
#include "eol_testing_protocol.h"
#else
#include "dut_protocol.h"
#endif

// Message header used in every HDLC packet
typedef struct __attribute__((packed))
{
    uint8_t version;  // Should be 0
    uint8_t type;     // Use the msg_id_e values
} message_header_t;

// Structure holding the message's header and data
typedef struct __attribute__((packed))
{
    message_header_t header;
    uint8_t          data[BUFFER_MAX_SIZE - sizeof(message_header_t)];
} message_payload_t;

// Holds data received or that can be sent in HDLC
typedef struct __attribute__((packed))
{
    message_payload_t payload;  // Holds header + data
    uint16_t          size;     // Payload size
} message_t;

/**
 * \brief Sets the header of a given message
 *
 * \param[inout] msg Buffer holding the message
 * \param type Type of the message (from msg_id_e)
 */
void protocol_set_message_header(message_t * msg, msg_id_e type);

/**
 * \brief Sets the payload of a given message
 *
 * \param[inout] msg Buffer holding the message
 * \param[in] payload Pointer to the buffer holding the payload's data
 * \param payload_size Payload size in bytes
 */
void protocol_set_message_payload(message_t * msg, uint8_t * payload,
                                      uint16_t payload_size);

#endif  // PROTOCOL_H_