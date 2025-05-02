/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include <string.h>
#include "protocol.h"

void protocol_set_message_header(message_t * msg, msg_id_e type)
{
    if (msg == NULL)
    {
        return;
    }

    msg->payload.header
        = (message_header_t){ .type    = (uint8_t) type,
                              .version = METERING_TEST_PROTOCOL_VERSION };
    msg->size += sizeof(message_header_t);
}

void protocol_set_message_payload(message_t * msg, uint8_t * payload,
                                      uint16_t payload_size)
{
    if (msg == NULL || payload == NULL)
    {
        return;
    }

    if (payload_size)
    {
        memcpy(msg->payload.data, payload, payload_size);
        msg->size += payload_size;
    }
}