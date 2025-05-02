/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    nic_server_objects.h
 * \brief   NIC implementation of a DLMS server
 */

#ifndef NIC_SERVER_OBJECTS_H_
#define NIC_SERVER_OBJECTS_H_

#include "meter_connection_management.h"

#include "include/dlmssettings.h"

#define PDU_BUFFER_SIZE       1440
#define FRAME_SIZE            8 + PDU_BUFFER_SIZE

bool Dlms_Server_createObjects(dlmsServerSettings* settings, mcm_aa_e aa_type,
                               uint32_t * invocation_counter_p);
void Dlms_Server_deleteObjects(void);

#endif // NIC_SERVER_OBJECTS_H_
