/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    nic_server.h
 * \brief   NIC implementation of a DLMS server
 */

#ifndef NIC_SERVER_H_
#define NIC_SERVER_H_

#include <stdint.h>
#include <stdbool.h>

#include "meter_connection_management.h"

bool Nic_Server_handle_message(const uint8_t * message, size_t size,
                               uint16_t destination, mcm_aa_e aa_type,
                               uint32_t delay_ms);

void Nic_Server_disconnect(void);

void Nic_Server_invalid_connection(void);

uint32_t Nic_Server_get_current_message_delay_ms(void);

#endif // NIC_SERVER_H_
