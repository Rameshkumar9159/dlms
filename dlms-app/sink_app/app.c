/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    app.c
 * \brief   Preconfigured dual MCU
 */
#include <stdlib.h>

#include "dualmcu_lib.h"
#include "api.h"
#include "stack_state.h"

#if defined(NETWORK_CHANNEL)
#if (NETWORK_CHANNEL != 1)
#error "Network channel must be 1"
#endif
#endif

#ifndef MONO_SINK_GATEWAY
// To avoid bad performances on Gateway with two sinks,
// sink cluster channels must be isolated enough one from the other.
// It was measured that best allocation is as followed:
//  Channel |  Usage
// ______________________________________________
//     1    |  Network channel
//     2    |  Not used
//     3    |  Possible cluster channel for Sink1
//     4    |  Possible cluster channel for Sink1
//     5    |  Possible cluster channel for Sink1
//     6    |  Possible cluster channel for Sink1
//     7    |  Not used
//     8    |  Not used
//     9    |  Possible cluster channel for Sink2
//     10   |  Possible cluster channel for Sink2
//     11   |  Possible cluster channel for Sink2
//     12   |  Possible cluster channel for Sink2
//
// As there is no explicit API to tell who is sink1 or sink2,
// this information is determined by following convention:
//
//    If node address is odd : sink is considered as Sink1
//    If node address is even: sink is considered as Sink2
//
// Sink 1 must reserve channels: 2, 7, 8, 9, 10, 11, 12
// Sink 2 must reserve channels: 2, 3, 4, 5, 6, 7, 8
static const uint8_t sink1_reserved_channels[2] = {0xc2, 0x0f};
static const uint8_t sink2_reserved_channels[2] = {0xfe, 0x00};
#endif /* MONO_SINK_GATEWAY */

// Those symbols are defined on SDK side in start.c
extern const uint8_t * authen_key_p;
extern const uint8_t * cipher_key_p;

static void configure_sink()
{
    app_lib_settings_net_addr_t temp_net_add;
    app_lib_settings_net_channel_t temp_net_ch;

    // Set role as LL sink always
    lib_settings->setNodeRole(APP_LIB_SETTINGS_ROLE_SINK_LL);

    // Set key to default if not set yet
    if (authen_key_p != NULL
        && lib_settings->getAuthenticationKey(NULL) == APP_RES_INVALID_CONFIGURATION)
    {
        // Not set, set default
        lib_settings->setAuthenticationKey(authen_key_p);
    }

    if (cipher_key_p != NULL
        && lib_settings->getEncryptionKey(NULL) == APP_RES_INVALID_CONFIGURATION)
    {
        // Not set, set default
        lib_settings->setEncryptionKey(cipher_key_p);
    }

#if defined(NETWORK_ADDRESS)
    // Check network address
    if (lib_settings->getNetworkAddress(&temp_net_add) != APP_RES_OK)
    {
        // Do not check return code, if it fails, it will not be set
        lib_settings->setNetworkAddress(NETWORK_ADDRESS);
    }
#endif

    // Network channel is hardcoded to 1 (important)
    if (lib_settings->getNetworkChannel(&temp_net_ch) != APP_RES_OK)
    {
        // Not set, initialize it to 1
        lib_settings->setNetworkChannel(1);
        // Set the diagnostic frequency to 30 minutes
        lib_data->writeDiagnosticInterval(1800);
    }
}

#ifndef MONO_SINK_GATEWAY
static void reserve_sink_channels()
{
    app_addr_t node_address;

    if (lib_settings->getNodeAddress(&node_address) != APP_RES_OK)
    {
        // This is an error that should never happen.
        // In fact we call this fonction when stack has started
        // so node address MUST be set
        return;
    }

    if (node_address & 0x1)
    {
        // Odd, so it is sink 1
        lib_settings->setReservedChannels(sink1_reserved_channels,
                                          sizeof(sink1_reserved_channels));
    }
    else
    {
        // Even, so it is sink 2
        lib_settings->setReservedChannels(sink2_reserved_channels,
                                          sizeof(sink2_reserved_channels));
    }
}

static void onStackStarted(app_lib_stack_event_e event, void * param)
{
    /*We registered only for stack_started so test is useless*/
    if (event != APP_LIB_STATE_STACK_EVENT_STACK_STARTED)
    {
        return;
    }

    reserve_sink_channels();
}
#endif /* MONO_SINK_GATEWAY */

/**
 * \brief   Initialization callback for application
 *
 * This function is called after hardware has been initialized but the
 * stack is not yet running.
 *
 */
void App_init(const app_global_functions_t * functions)
{
    // Configure sink with predefined value, if present
    configure_sink();

#ifndef MONO_SINK_GATEWAY
    Stack_State_addEventCb(onStackStarted, 1 << APP_LIB_STATE_STACK_EVENT_STACK_STARTED);
#endif /* MONO_SINK_GATEWAY */

    Dualmcu_lib_init(UART_BAUDRATE, UART_FLOWCONTROL);
}
