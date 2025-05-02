/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    meter_connection_management.h
 * \brief   interface for the connection to the smart meters
 */
#ifndef METER_CONNECTION_MANAGEMENT_H_
#define METER_CONNECTION_MANAGEMENT_H_

#include <stdint.h>

#include "common.h"

typedef enum
{
    MCM_AA_PC, // NO SECURITY
    MCM_AA_MR, // LOW SECURITY
    MCM_AA_US, // HIGH SECURITY
    MCM_AA_FU, // HIGH SECURITY
#ifndef METER_RETROFIT
    MCM_SECURED_AA = MCM_AA_US
#else
    MCM_SECURED_AA = MCM_AA_MR
#endif
} mcm_aa_e;

typedef enum
{
    MCM_NOT_CONNECTED,
    MCM_CONN_FAILED,
    MCM_CONNECTED,
    MCM_AUTH_FAILED,
    MCM_AUTHENTIFIED
} mcm_status_e;

typedef void (* mcm_status_cb_f)(mcm_status_e status);

bool Meter_Connection_Management_addr_to_aa(uint16_t client_address,
                                            mcm_aa_e * aa_p);
void Meter_Connection_Management_subscribeStatusCb(mcm_status_cb_f cb);
int Meter_Connection_Management_open(operation_result_cb cb, mcm_aa_e aa);
int Meter_Connection_Management_close(operation_result_cb cb);

#endif // METER_CONNECTION_MANAGEMENT_H_
