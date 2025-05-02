/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    common.c
 * \brief
 */

#include <stdio.h>

#include "common.h"

//Gurux DLMS includes.
#include "include/bytebuffer.h"
#include "public_provisioning.h"
#include "app_persistent.h"

#define DEBUG_LOG_MODULE_NAME "COMMON  "
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

// DLMS client settings.
static dlmsSettings m_meter_settings;

/* ************************************ */
dlmsSettings * Common_getMeterSettings(void)
{
    return &m_meter_settings;
}

/* ************************************ */
#ifdef USE_SAFE_MALLOC

#define CONTEXT_DEPTH  8

static const char * contextLIFO[CONTEXT_DEPTH];
static uint16_t     contextLIFO_index;

void Common_pushContextName(const char * name)
{
    if (contextLIFO_index < CONTEXT_DEPTH)
    {
        contextLIFO[contextLIFO_index] = name;
    }
    contextLIFO_index++;
}

void Common_popContextName()
{
    contextLIFO_index--;
}

#endif

const char * Common_getContextName(void)
{
#ifdef USE_SAFE_MALLOC
    if (contextLIFO_index)
    {
        uint16_t index =  (contextLIFO_index < CONTEXT_DEPTH) ? contextLIFO_index : CONTEXT_DEPTH;
        return contextLIFO[index - 1];
    }
    else
#endif
    {
        return "None";
    }
}

DLMS_INTERFACE_TYPE Common_getNicInterfaceType(void)
{
    static int8_t nic_interface_type = -1;
    nic_provisioning_parameters_t nic_params = { 0 };

    if (nic_interface_type == DLMS_INTERFACE_TYPE_HDLC ||
        nic_interface_type == DLMS_INTERFACE_TYPE_WRAPPER)
    {
        // Interface type has already been read
        return nic_interface_type;
    }

    // We don't know the interface type, read it
    App_Persistent_init();

    // Read our needed parameters
    if (App_Persistent_read((uint8_t *) &nic_params,
                        sizeof(nic_provisioning_parameters_t)) != APP_PERSISTENT_RES_OK)
    {
        LOGE("Cannot obtain interface type, using wrapper");
        return DLMS_INTERFACE_TYPE_WRAPPER;
    }

    LOGI("Successfully read provisioned settings for interface type");
    nic_interface_type = nic_params.nic_interface_type;
    return nic_interface_type;
}

uint32_t Common_getNicBaudrate(void)
{
    static int32_t nic_baudrate = -1;
    nic_provisioning_parameters_t nic_params = { 0 };

    if (nic_baudrate > 0)
    {
        // Baudrate has already been read
        return nic_baudrate;
    }

    // We don't know the baudrate, read it
    App_Persistent_init();

    // Read our needed parameters
    if (App_Persistent_read((uint8_t *) &nic_params,
                        sizeof(nic_provisioning_parameters_t)) != APP_PERSISTENT_RES_OK)
    {
        LOGE("Cannot obtain baudrate, using 9600");
        return 9600;
    }

    nic_baudrate = nic_params.nic_baudrate;
    LOGI("Successfully read provisioned settings for baudrate: %d", nic_baudrate);
    return nic_baudrate;
}

void Common_formatSystemTitle(const uint8_t * sys_title_p, char * str_p, uint8_t str_size)
{
    uint32_t v = (sys_title_p[4] << 24) + (sys_title_p[5] << 16) +
                 (sys_title_p[6] << 8)  + (sys_title_p[7] << 0);

    snprintf(str_p, str_size, "%.3s%lu", sys_title_p, v);
}

static uint32_t stop_stack(void)
{
    // Restart the NIC
    lib_state->stopStack();
    return APP_SCHEDULER_STOP_TASK;
}

void Common_reboot(uint16_t delay_s)
{
    // Restart the NIC after specified delay
    LOGI("Stopping the stack in %u seconds", delay_s);
    App_Scheduler_addTask_execTime(stop_stack,
                                   delay_s * 1000,
                                   SFSM_EXECUTION_TIME_US);
}

void Common_wrapperFrameGetHeader(const uint8_t * bytes, wrapper_header_t * hdr_p)
{
    hdr_p->version = bytes[1] | (bytes[0] << 8);
    hdr_p->source = bytes[3] | (bytes[2] << 8);
    hdr_p->destination = bytes[5] | (bytes[4] << 8);
    hdr_p->length = bytes[7] | (bytes[6] << 8);
}
