/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    led_manager.c
 * \brief   handles management of the Leds
 */
#include "api.h"
#include "led.h"

#include "common.h"
#include "wirepas_com.h"
#include "meter_connection_management.h"

#define DEBUG_LOG_MODULE_NAME "LED_MNGR"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#define NETWORK_LED_TASK_EXEC_TIME_US   200
#define LED_TASK_INTERVAL_MS            500
#define NETWORK_LED                     0
#define METER_LED                       1

#ifndef DLMS_APP_NO_LED
// Definitions
typedef struct
{
    bool valid_route;
    mcm_status_e meter_status;
} led_mngr_param_t;

// Prototypes
static void update_nwk_conn_status(bool valid_route);
static void update_meter_conn_status(mcm_status_e status);
static uint32_t update_leds_task(void);
static uint32_t update_meter_led_task(void);

// Global variables
static led_mngr_param_t m_led_mngr;

//  LED 0 led indicates the network connection status
//      *   BLINK: no route
//      *   ON   : ready

//  LED 1 led indicates the meter connection status
//     *   OFF        : no communication
//     *   BLINK SLOW : unauthentified connection failed
//     *   BLINK FAST : wrong credential
//     *   ON         : ready
void Led_Manager_start(void)
{
    Led_set(NETWORK_LED, false);
    Led_set(METER_LED, false);

    Wirepas_com_subscribeStatusChangeCb(update_nwk_conn_status);
    Meter_Connection_Management_subscribeStatusCb(update_meter_conn_status);

    // Start task managing the LED update
    App_Scheduler_addTask_execTime(update_leds_task,
                                   APP_SCHEDULER_SCHEDULE_ASAP,
                                   NETWORK_LED_TASK_EXEC_TIME_US);
}

static void update_nwk_conn_status(bool valid_route)
{
    m_led_mngr.valid_route = valid_route;
}

static void update_meter_conn_status(mcm_status_e status)
{
    m_led_mngr.meter_status = status;
}

/* *************************************** */
static uint32_t update_leds_task(void)
{
//  LED 0 led indicates the network connection status
//      *   BLINK: no route
//      *   ON   : ready

    if (! m_led_mngr.valid_route)
    {
        Led_toggle(NETWORK_LED);
    }
    else
    {
        Led_set(NETWORK_LED, true);
    }

//  LED 1 led indicates the meter connection status
//     *   OFF        : no communication
//     *   BLINK SLOW : unauthentified connection failed
//     *   BLINK FAST : wrong credential
//     *   ON         : ready
    switch (m_led_mngr.meter_status)
    {
        case MCM_NOT_CONNECTED:
        // MCM_CONNECTED is a shorted lived temporary state not worth a blink
        case MCM_CONNECTED:
            Led_set(METER_LED, false);
            break;
        case MCM_CONN_FAILED:
            Led_toggle(METER_LED);
            break;
        case MCM_AUTH_FAILED:
            Led_toggle(METER_LED);
            // We blink twice faster than for MCM_CONN_FAILED using a dedicated
            // task
            App_Scheduler_addTask_execTime(update_meter_led_task,
                                           LED_TASK_INTERVAL_MS / 2,
                                           NETWORK_LED_TASK_EXEC_TIME_US);
            break;
        case MCM_AUTHENTIFIED:
            Led_set(METER_LED, true);
            break;
    }

    return LED_TASK_INTERVAL_MS;
}

/* *************************************** */
static uint32_t update_meter_led_task(void)
{
    if (m_led_mngr.meter_status == MCM_AUTH_FAILED)
    {
        Led_toggle(METER_LED);
    }
    return APP_SCHEDULER_STOP_TASK;
}
#else
void Led_Manager_start(void)
{
}
#endif // DLMS_APP_NO_LED
