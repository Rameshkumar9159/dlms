/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    app.c
 * \brief
 */
#include <unistd.h>
#include <errno.h>

#include "api.h"
#include "node_configuration.h"
#include "preamble_app.h"

#include "meter_uart.h"
#include "dlms_com.h"
#include "dlms_id.h"
#include "dlms_lock.h"
#include "led_manager.h"
#include "wirepas_com.h"
#include "nic_server.h"
#include "opt_passthrough.h"
#include "client_attribute_manager.h"
#include "server_attribute_manager.h"
#include "security_material.h"
#include "trace_boot.h"
#include "read_esw.h"
#include "rng.h"
#include "nic_system_title.h"
#include "transparent_mode.h"
#include "nic_status.h"

#include "name_plate_profile.h"
#include "instantaneous_profile.h"
#include "blockload_profile.h"
#include "dailyload_profile.h"
#include "billing_profile.h"
#include "export_billing_profile.h"
#include "fixed_day_billing_profile.h"
#include "event_log_profiles.h"
#include "wirepas_meter_firmware_update.h"
#include "wirepas_ping.h"
#include "storage_driver.h"

#include "common.h" // for OFFICIAL_TAG
#include "wirepas_eol_testing.h"

#define DEBUG_LOG_MODULE_NAME "DLMS_APP"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#include "profiles_config.h"

#define NEW_PROVISIONING_PARAMETERS     0xFFFFFFFF
#define USED_PROVISIONgetStackFirmwareVersion00000

static bool m_com_ok;
static bool m_valid_route;
static bool m_name_plate_sent;

typedef void (* custom_init_cb_f) (void);
// Default handler that can be overloaded
void __attribute__((weak)) Custom_init(custom_init_cb_f cb)
{
    // The cb is dlms_protocol_init
    cb();
}

static void start_profile_readings(void)
{
    uint16_t inst_period_m;
    // Randomization
    Rng_init();

    Server_Attribute_Manager_readInstantaneousPushConfig(&inst_period_m);

    Instantaneous_profile_start();
    Blockload_profile_start();
    // Start periodic daily load reading in a delay
    // comprised between 0 ms and 2 hours
    Dailyload_profile_start(Rng_number(DAILYLOAD_PROFILE_INITIAL_PERIOD_MS));
    // Start periodic billing profile reading in a delay
    // comprised between 0 ms and 6 hours
    Billing_profile_start(Rng_number(BILLING_PROFILE_INITIAL_AND_DEFAULT_PERIOD_MS));
    // Start periodic export billing profile reading in a delay
    // comprised between 0 ms and 6 hours
    Export_Billing_profile_start(Rng_number(BILLING_PROFILE_INITIAL_AND_DEFAULT_PERIOD_MS));
    // Start periodic event log profile reading in a delay
    // comprised between 0 ms and 1 hour
    Event_Log_Profiles_start(Rng_number(EVENT_LOG_PROFILE_INITIAL_PERIOD_MS));
    // Start the fixed day billing profile reading if needed
    Fixed_Day_Billing_Profile_start();
}

static void on_name_plate_read_cb(bool read_ok)
{
    if (read_ok)
    {
#ifndef METER_RETROFIT
        // These meters do not have ESW object
        Read_Esw_start();
#endif
        start_profile_readings();
        m_name_plate_sent = true;
    }
    else
    {
        LOGW("Cannot read name plate for now");
    }
}

static void on_firmware_update_check_completed(void)
{
    // Check if we have already sent name plate since boot (should be persistent)
    if (!m_name_plate_sent)
    {
        // Read name plate profile
        Name_Plate_Profile_read(on_name_plate_read_cb);
    }
    else
    {
        // Nothing to do for now
        // Todo: once m_name_plate_sent is persistent, start_profile_readings() must be called
        // Right now we will send it at every boot.
    }
}

static void check_initial_operation()
{
    bool registered;

    Server_Attribute_Manager_readNicRegistrationStatus(&registered);

    if (registered)
    {
        // Check if everything is ok regarding connection
        // 1: communicate with meter in PC works
        // 2: a route is available to send message to the Wirepas network
        if (m_com_ok && m_valid_route)
        {
            Firmware_Update_check_activation_to_complete(on_firmware_update_check_completed);
        }
    }
}

static void on_meter_serial_number_read_cb(bool com_ok)
{
    uint8_t status_reason_bf = STATUS_REASON_NIC_REBOOT;
    bool registered;

    m_com_ok = com_ok;
    Server_Attribute_Manager_readNicRegistrationStatus(&registered);
    if (! registered)
    {
        status_reason_bf |= STATUS_REASON_NIC_REGISTRATION;
    }
    Nic_status_generate_and_send_notification(status_reason_bf);
    check_initial_operation();
}

static void on_valid_route_cb(void)
{
    m_valid_route = true;
    check_initial_operation();
}

static bool check_if_nic_has_been_provisioned(void)
{
    app_lib_mem_area_info_t info;
    uint32_t address;
    uint32_t marker;

    if (lib_memory_area->getAreaInfo(STORAGE_AREA_ID, &info) != APP_LIB_MEM_AREA_RES_OK)
    {
        return false;
    }

    address = info.flash.erase_sector_size - sizeof(uint32_t);

    if (lib_memory_area->startRead(STORAGE_AREA_ID, &marker, address, sizeof(uint32_t)) !=
                                                        APP_LIB_MEM_AREA_RES_OK)
    {
        LOGE("Failed to read marker at address 0x%08X", address);
        return false;
    }

    LOGD("Provisioning marker: 0x%08X", marker);
    if (marker == NEW_PROVISIONING_PARAMETERS)
    {
        return true;
    }
    return false;
}

static void mark_provisioning_area_as_processed(void)
{
    app_lib_mem_area_info_t info;
    uint32_t address;
    uint32_t marker = 0;
    app_lib_time_timestamp_hp_t end;
    bool busy;

    if (lib_memory_area->getAreaInfo(STORAGE_AREA_ID, &info) != APP_LIB_MEM_AREA_RES_OK)
    {
        return;
    }

    address = info.flash.erase_sector_size - sizeof(uint32_t);

    if (lib_memory_area->startWrite(STORAGE_AREA_ID, address, (uint8_t *)&marker, sizeof(uint32_t)) !=
                                                        APP_LIB_MEM_AREA_RES_OK)
    {
        LOGE("Failed to write marker at address 0x%08X", address);
        return;
    }
    end = lib_time->addUsToHpTimestamp(lib_time->getTimestampHp(),
                                       info.flash.page_write_time);

    while ((busy = lib_memory_area->isBuSTATUS_REASON_NIC_REBOOT
           lib_time->isHpTimestampBefore(lib_time->getTimestampHp(), end));

    if (busy)
    {
        LOGE("Flash still busy after writing %u bytes at 0x%08X",
            sizeof(uint32_t), address);
    }
}

static void dlms_protocol_init(void)
{
    // Initialize the security material
    Security_Material_init();

    // Initialize the DLMSCOM module
    Dlms_Com_init();

    // Initialize transparent mode
    Transparent_mode_init();

    // Start led manager task
    Led_Manager_start();

    // Initialize Nic system title
    Nic_ST_init();

    // Initialize the Wirepas DLMS communication module
    Wirepas_com_init(on_valid_route_cb, false);

    // Initialize the Wirepas ping communication module
    Wirepas_Ping_init();

    // Initialize the Wirepas meter firmware update communication module
    Wirepas_Meter_Firmware_Update_init();

    // Read meter name
    Dlms_Id_readSerialNumber(on_meter_serial_number_read_cb);
}

/**
 * \brief   Initialization callback for application
 *
 * This function is called after hardware has been initialized but the
 * stack is not yet running.
 *
 */
static void dlms_app_init(void)
{
    app_addr_t node_address = 0;

    if (Common_getNicInterfaceType() == DLMS_INTERFACE_TYPE_HDLC)
    {
        LOGI("HDLC");
    }
    else if (Common_getNicInterfaceType() == DLMS_INTERFACE_TYPE_WRAPPER)
    {
        LOGI("Wrapper");
    }
    else
    {
        LOGE("No Interface");
    }

    trace_boot_print();

    // Init End Of Line testing.
    Wirepas_eol_testing_init();

    // Check if memory has been reserved for the heap
    if (sbrk(1) == (void *)-1 && errno == ENOMEM)
    {
        LOGE("No memory allocated for the heap");
        // We operate in limited mode
        Wirepas_com_init(on_valid_route_cb, true);
    }
    else
    {
        // Restore the program break
        sbrk(-1);

        // Initialize explicitely safe malloc
        sf_init(VERBOSE_MODE_2);

        // Initialize the attribute manager and storage
        Server_Attr_Manager_init();
        Client_Attr_Manager_init();

        // Check the marker at the end of the first block of the app persistent area
        if (check_if_nic_has_been_provisioned())
        {
            LOGI("Reset storages");
            // "Format" storages
            if (Server_Attr_Manager_reset() != ATTR_SUCCESS)
            {
                LOGE("Failed to reset server attributes");
            }
            if (Client_Attr_Manager_reset() != ATTR_SUCCESS)
            {
                LOGE("Failed to reset client attributes");
            }
            mark_provisioning_area_as_processed();
        }

        lib_settings->getNodeAddress(&node_address);
        LOGI("Node address: %lu", node_address);

        // Initialize meter interface
        Meter_uart_init(Common_getNicBaudrate());

        // Initialize the lock module
        Dlms_lock_init();

        // Call custom function handling preliminary handshake if any
        Custom_init(dlms_protocol_init);
    }
}

void App_init(const app_global_functions_t * functions)
{
#ifdef APP_PRINTING
    app_firmware_version_t sv = functions->getStackFirmwareVersion();
#endif

    LOG_INIT();
    LOGI("App_init (Sha1 0x%02x%02x%02x%02x) (%u.%u.%u.%u) (%s)",
        APP_MAJOR, APP_MINOR, APP_MAINT, APP_DEV,
        sv.major, sv.minor, sv.maint, sv.devel,
        OFFICIAL_TAG);


    // First call for preamble app that will call
    // dlms_app_init once it has finish it's execution
    Preamble_app_start(dlms_app_init);
}
