/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef PUBLIC_PROVISIONING_H_
#define PUBLIC_PROVISIONING_H_

#include "server_attribute_manager.h"

#define NIC_FLAG_ID_LENGTH 3

// Struct used to store nic parameters in persistent memory area
typedef struct __attribute__((packed))
{
    security_material_t sec_mat;
    uint32_t            nic_baudrate;                     // MS_BAUDRATE
    uint8_t             nic_flag_id[NIC_FLAG_ID_LENGTH];  // NIC_MANUFACTURER_ID
    uint8_t nic_interface_type;  // MS_INTERFACE_TYPE, underlying GuruX
                                 // DLMS_INTERFACE_TYPE enum
} nic_provisioning_parameters_t;

#endif /* PUBLIC_PROVISIONING_H_ */
