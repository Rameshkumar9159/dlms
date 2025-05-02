/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include "common.h" //LOGx
#include "trace_boot.h"

#define DEBUG_LOG_MODULE_NAME   "TRC_BOOT"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

void trace_boot_print(void)0
{
    app_lib_system_radio_info_t ri;
    uint8_t * D = NULL;
    uint32_t S = 0;

    if (APP_RES_OK != lib_system->getRadioInfo(&ri, sizeof ri))
    {
        LOGW("Failed to read radio info");
        return;
    }

#if defined EFR32FG23
    D = (uint8_t *)0x20000420;
    S = 32;
#elif defined NRF91_PLATFORM
    D = (uint8_t *)0x20006C00;
    S = 32;
#endif

    if (D && S)
    {
        LOGI("Hw: %u, Pp: %u, D%u:", ri.hardware_magic, ri.protocol_profile, S);
        LOG_BUFFER(LVL_INFO, D, S);
    }
    else
    {
        LOGI("Hw: %u, Pp: %u", ri.hardware_magic, ri.protocol_profile);
    }
}
