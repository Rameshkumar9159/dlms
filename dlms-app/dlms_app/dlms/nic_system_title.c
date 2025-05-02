/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */
#include <string.h>

#define DEBUG_LOG_MODULE_NAME "NIC SYST"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#include "api.h"
#include "common.h" //LOGx
#include "nic_system_title.h"
#include "public_provisioning.h"
#include "app_persistent.h"


// Initialize nic system title with NIC manufacturer id
static uint8_t nic_system_title[8];

void Nic_ST_init(void)
{
    app_addr_t node_address;
    nic_provisioning_parameters_t nic_params = { 0 };
    App_Persistent_init();

    // Read our needed parameters
    if (App_Persistent_read((uint8_t *) &nic_params,
                        sizeof(nic_provisioning_parameters_t)) != APP_PERSISTENT_RES_OK)
    {
        LOGE("Cannot get NIC Manufacturer ID: default to TBC");
        // Default it to TBC (To Be Changed)
        memcpy(nic_system_title, "TBC", NIC_FLAG_ID_LENGTH);
    }
    else
    {
        memcpy(nic_system_title, nic_params.nic_flag_id, NIC_FLAG_ID_LENGTH);
    }

    if (lib_settings->getNodeAddress(&node_address) != APP_RES_OK)
    {
        // Should never happen as we will execute this code only once device is connected to network
        node_address = 0;
        LOGE("Cannot read node address, set it to 0");
    }

    nic_system_title[3] = 0;
    // Add the node address in Big Endian
    nic_system_title[4] = (node_address >> 24) & 0xff;
    nic_system_title[5] = (node_address >> 16) & 0xff;
    nic_system_title[6] = (node_address >>  8) & 0xff;
    nic_system_title[7] = node_address & 0xff;

    LOGI("Nic system title: %.3s%u", nic_system_title, node_address);
    LOG_BUFFER(LVL_INFO, nic_system_title, 8);

}

/**
 * @brief   Get Nic System Title
 */
bool Nic_ST_get(const uint8_t ** title_pp, uint8_t * title_len_p)
{
    *title_pp = (const uint8_t *) nic_system_title;
    *title_len_p = sizeof(nic_system_title);

    return true;
}

