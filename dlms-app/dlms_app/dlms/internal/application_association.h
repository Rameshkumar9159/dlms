/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * @file    application_association.h
 * @brief   interface for the establishment and release of Application
 *          Association (AA) with the smart meter
 */
#ifndef APPLICATION_ASSOCIATION_H_
#define APPLICATION_ASSOCIATION_H_

#include <stdint.h>

#include "common.h"

// Client address
// --------------
// Public Client
#define PC_CLIENT_ADDRESS               0x10
// Meter Reader
#define MR_CLIENT_ADDRESS               0x20
// Utility Setting
#define US_CLIENT_ADDRESS               0x30
// Firmware Upgrade
#define FU_CLIENT_ADDRESS               0x50

typedef enum
{
    APPLICATION_ASSOCIATION_FIRST = 0,
    APPLICATION_ASSOCIATION_PC = APPLICATION_ASSOCIATION_FIRST, // NO SECURITY
    APPLICATION_ASSOCIATION_MR, // LOW SECURITY
    APPLICATION_ASSOCIATION_US, // HIGH SECURITY
    APPLICATION_ASSOCIATION_FU, // HIGH SECURITY
#ifdef METER_RETROFIT
    APPLICATION_ASSOCIATION_LAST = APPLICATION_ASSOCIATION_MR,
#else
    APPLICATION_ASSOCIATION_LAST = APPLICATION_ASSOCIATION_FU,
#endif
} aa_type_e;

int AA_Establish(aa_type_e aa, uint32_t invocation_counter,
                  operation_result_cb cb);
int AA_Release(operation_result_cb cb);

#endif // APPLICATION_ASSOCIATION_H_
