/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    app.c
 * \brief   This file contains the entry points for the testing app
 */

#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "app_scheduler.h"
#include "led.h"
#include "node_configuration.h"

#include "dut_protocol.h"
#include "extflash_tests.h"
#include "factory_rf_tests_dut.h"
#include "hdlc.h"

#define DEBUG_LOG_MODULE_NAME "TEST APP"
#define DEBUG_LOG_MAX_LEVEL   LVL_INFO
#include "debug_log.h"

#define BLINK_TASK_EXEC_TIME_US                 100
#define INITIALIZE_DUT_TASK_EXEC_TIME_US        100
#define END_OF_LATEST_TESTS_TASK_EXEC_TIME_US   100
#define START_RF_TESTING_TASK_EXEC_TIME_US      100
#define STOP_EXTFLASH_TEST_FM_TASK_EXEC_TIME_US 100

#define WAIT_FOR_HDLC_ACK_NACK_TIMEOUT_DELAY_MS 550

#define BLINKING_TASK_EXEC_PERIOD_MS 500

static uint32_t stop_extflash_tests_fsm_task(void);
static uint32_t end_of_testing_task(void);

/**
 * \brief Holds the parameter received from the script to initialize the testing
 *
 */
static pl_req_test_init_t m_test_init;

/**
 * \brief Stores the External Flash Memory Area Info
 *
 */
static app_lib_mem_area_info_t m_extflash_info;

/**
 * \brief Summary to write in external flash
 *
 */
static testing_summary_t m_summary = { .extflash_status = TEST_STATUS_UNTESTED,
                                       .rf_status = TEST_STATUS_UNTESTED };

/**
 * @brief   periodic callback that toggles the LED
 * @return  the delay before the next execution of this callback
 */
static uint32_t blink_task(void)
{
    // Toggle all LEDs
    uint8_t leds_nb = Led_getNumber();

    for (uint8_t id = 0; id < leds_nb; id++)
    {
        Led_toggle(id);
    }
    return BLINKING_TASK_EXEC_PERIOD_MS;
}

/**
 * \brief Setup to Blink LEDs alternatively
 *
 */
static void start_alternative_led_blinking(void)
{
    uint8_t leds_nb = Led_getNumber();

    for (uint8_t id = 0; id < leds_nb; id++)
    {
        Led_set(id, false);
    }

    Led_set(0, true);
    App_Scheduler_addTask_execTime(blink_task,
                                   APP_SCHEDULER_SCHEDULE_ASAP,
                                   BLINK_TASK_EXEC_TIME_US);
}

/**
 * \brief Set the led to indicate tests success
 *
 */
static void set_led_to_indicate_tests_success(void)
{
    App_Scheduler_cancelTask(blink_task);

    uint8_t leds_nb = Led_getNumber();

    for (uint8_t id = 0; id < leds_nb; id++)
    {
        Led_set(id, true);
    }
}

/**
 * \brief Set the led to indicate tests failure
 *
 */
static void set_led_to_indicate_tests_failure(void)
{
    uint8_t leds_nb = Led_getNumber();

    for (uint8_t id = 0; id < leds_nb; id++)
    {
        Led_set(id, false);
    }
}

/**
 * \brief Set the led according to the test results
 *
 */
void set_led_according_to_test_results()
{
    if (m_summary.extflash_status == TEST_STATUS_SUCCESS
        && m_summary.rf_status == TEST_STATUS_SUCCESS)
    {
        set_led_to_indicate_tests_success();
    }
    else
    {
        set_led_to_indicate_tests_failure();
    }
}

/**
 * \brief Writes the testing summary in the last page of the External Flash
 *
 */
void write_testing_summary_in_extflash(void)
{
    size_t   page_size          = m_extflash_info.flash.write_page_size;
    size_t   page_nb            = m_extflash_info.area_size / page_size;
    uint16_t last_flash_page_id = page_nb - 1;

    LOG(LVL_INFO, "Writing Testing Summary in External Flash");

    if (lib_memory_area->startWrite(m_extflash_info.area_id,
                                    last_flash_page_id * page_size,
                                    (uint8_t *) &m_summary,
                                    sizeof(testing_summary_t))
        != APP_LIB_MEM_AREA_RES_OK)
    {
        LOG(LVL_ERROR,
            "Failed to write Testing Summary in address range 0x%08X--0x%08X "
            "in area id 0x%08X",
            last_flash_page_id * page_size,
            (last_flash_page_id + 1) * page_size - 1,
            m_extflash_info.area_id);
    }
}

/**
 * \brief Callback called when the External Flash Testing is ended
 *
 * \param status External Flash Test Status
 * \param info External Flash Memory Area Info
 */
static void flash_test_ended_cb(test_status_e           status,
                                app_lib_mem_area_info_t info)
{
    message_t rsp_test_ext_flash_msg = { 0 };
    // External Flash Test is successful
    pl_rsp_test_ext_flash_t pl = status;

    m_extflash_info = info;

    m_summary.extflash_status = status;

    if (status == TEST_STATUS_SUCCESS)
    {
        LOG(LVL_INFO, "✅ External Flash is OK ✅");
    }
    else
    {
        LOG(LVL_ERROR, "❌ External Flash is not OK ❌");

        // We stop the test procedure when a test fails
        App_Scheduler_addTask_execTime(end_of_testing_task,
                                       APP_SCHEDULER_SCHEDULE_ASAP,
                                       END_OF_LATEST_TESTS_TASK_EXEC_TIME_US);
    }

    dut_protocol_set_message_header(&rsp_test_ext_flash_msg,
                                    MSG_ID_RSP_TEST_EXT_FLASH);

    dut_protocol_set_message_payload(&rsp_test_ext_flash_msg,
                                     (uint8_t *) &pl,
                                     sizeof(pl));

    HDLC_send_message((uint8_t *) &rsp_test_ext_flash_msg.payload,
                      rsp_test_ext_flash_msg.size);

    // This callback is called from extflash_tests_fsm_task, so we have to
    // schedule another task that will kill it
    App_Scheduler_addTask_execTime(stop_extflash_tests_fsm_task,
                                   APP_SCHEDULER_SCHEDULE_ASAP,
                                   STOP_EXTFLASH_TEST_FM_TASK_EXEC_TIME_US);
}

/**
 * \brief Callback called once the RF Test is ended
 *
 * \param status RF Testing Status
 * \param rx_rssi Measured RX RSSI
 * \param tx_rssi Measured TX RSSI
 */
static void rf_test_ended_cb(test_status_e status,
                             int8_t        rx_rssi,
                             int8_t        tx_rssi)
{
    message_t        rsp_test_rf_msg = { 0 };
    pl_rsp_test_rf_t pl              = { .status          = status,
                                         .meas_rf.rx_rssi = rx_rssi,
                                         .meas_rf.tx_rssi = tx_rssi };

    m_summary.rf_status = status;
    m_summary.rf_results
        = (rf_testing_parameters_t){ .rx_rssi = rx_rssi, .tx_rssi = tx_rssi };

    if (status == TEST_STATUS_SUCCESS)
    {
        LOG(LVL_INFO, "✅ RF is OK ✅");
    }
    else
    {
        LOG(LVL_ERROR, "❌ RF is not OK ❌");
        // We stop the test process when a test fails
        App_Scheduler_addTask_execTime(end_of_testing_task,
                                       APP_SCHEDULER_SCHEDULE_ASAP,
                                       END_OF_LATEST_TESTS_TASK_EXEC_TIME_US);
    }

    LOG(LVL_INFO, "\t├ RX RSSI: %d dBm", rx_rssi);
    LOG(LVL_INFO, "\t└ TX RSSI: %d dBm", tx_rssi);

    dut_protocol_set_message_header(&rsp_test_rf_msg, MSG_ID_RSP_TEST_RF);

    dut_protocol_set_message_payload(&rsp_test_rf_msg,
                                     (uint8_t *) &pl,
                                     sizeof(pl));

    HDLC_send_message((uint8_t *) &rsp_test_rf_msg.payload,
                      rsp_test_rf_msg.size);

    // As RF Testing is the last performed test, we call the end_of_testing_task
    // task. If RF Test is not the last performed test anymore, move this
    // Scheduling Call where the last individual test results are obtained
    App_Scheduler_addTask_execTime(end_of_testing_task,
                                   APP_SCHEDULER_SCHEDULE_ASAP,
                                   END_OF_LATEST_TESTS_TASK_EXEC_TIME_US);
}

/**
 * \brief This task is responsible to stop the extflash_tests_fsm_task
 *
 * \return uint32_t Next Scheduled Time
 */
static uint32_t stop_extflash_tests_fsm_task(void)
{
    // Stop External Flash Testing Task
    App_Scheduler_cancelTask(extflash_tests_fsm_task);

    return APP_SCHEDULER_STOP_TASK;
}

static uint32_t initialize_dut_task(void)
{
    static bool valid_config_received_once = false;

    message_t rsp_test_init_msg = { 0 };
    // Initialization is successful
    pl_rsp_test_init_t pl = { .test_init_status = TEST_INIT_STATUS_OK,
                              .extflash_status  = m_summary.extflash_status,
                              .rf_status        = m_summary.rf_status };

    memcpy(m_summary.serial_number,
           m_test_init.serial_number,
           NIC_SERIAL_NUMBER_MAX_LENGTH);

    if (!valid_config_received_once)
    {
        if (lib_settings->setEncryptionKey(m_test_init.net_settings.enc_key)
            != APP_RES_OK)
        {
            pl.test_init_status = TEST_INIT_STATUS_CONFIG_ERROR;
        }

        if (lib_settings->setAuthenticationKey(
                m_test_init.net_settings.auth_key)
            != APP_RES_OK)
        {
            pl.test_init_status = TEST_INIT_STATUS_CONFIG_ERROR;
        }

        if (lib_settings->setNetworkAddress(
                m_test_init.net_settings.net_address)
            != APP_RES_OK)
        {
            pl.test_init_status = TEST_INIT_STATUS_CONFIG_ERROR;
        }

        if (lib_settings->setNetworkChannel(
                m_test_init.net_settings.net_channel)
            != APP_RES_OK)
        {
            pl.test_init_status = TEST_INIT_STATUS_CONFIG_ERROR;
        }

        if (lib_settings->setNodeAddress(getUniqueAddress()) != APP_RES_OK)
        {
            pl.test_init_status = TEST_INIT_STATUS_CONFIG_ERROR;
        }

        if (lib_settings->setNodeRole(APP_LIB_SETTINGS_ROLE_SUBNODE_LL)
            != APP_RES_OK)
        {
            pl.test_init_status = TEST_INIT_STATUS_CONFIG_ERROR;
        }

        LOG(LVL_INFO, "DUT address: 0x%08X", getUniqueAddress());
        LOG(LVL_INFO,
            "Router Address: 0x%08X",
            m_test_init.net_settings.router_address);

        start_alternative_led_blinking();

        if (pl.test_init_status == TEST_INIT_STATUS_OK)
        {
            valid_config_received_once = true;
        }
    }
    else
    {
        pl.test_init_status = TEST_INIT_STATUS_TESTING_ALREADY_DONE;
        // We set the LEDs according to the results of the already performed
        // test
        set_led_according_to_test_results();
    }

    dut_protocol_set_message_header(&rsp_test_init_msg, MSG_ID_RSP_TEST_INIT);

    dut_protocol_set_message_payload(&rsp_test_init_msg,
                                     (uint8_t *) &pl,
                                     sizeof(pl));

    HDLC_send_message((uint8_t *) &rsp_test_init_msg.payload,
                      rsp_test_init_msg.size);

    return APP_SCHEDULER_STOP_TASK;
}

/**
 * \brief This task must be executed once all tests are done
 *
 * \return uint32_t Next Scheduled Time
 */
static uint32_t end_of_testing_task(void)
{
    // Sets the LEDs at the end to show the result
    set_led_according_to_test_results();

    if (m_summary.extflash_status == TEST_STATUS_SUCCESS)
    {
        write_testing_summary_in_extflash();
    }
    else
    {
        LOG(LVL_ERROR,
            "External Flash is not tested successfully: Unable to write Testing Summary in External Flash");
    }

    return APP_SCHEDULER_STOP_TASK;
}

/**
 * \brief   Callback called when an HDLC DATA frame has been received
 *
 * \param message       Pointer to received frame
 * \param message_size  Frame size
 * \return  True: Message is valid to be processed; False: Message isn't
 * valid
 */
static bool message_rx_from_script_cb(const uint8_t * message,
                                      size_t          message_size)
{
    message_t rx_msg = { 0 };

    if (message_size < sizeof(message_header_t))
    {
        // Message must be bigger than a header
        LOGE("Message too short %d < %d",
             message_size,
             sizeof(message_header_t));
        return false;
    }

    rx_msg.size = message_size;
    memcpy(&rx_msg.payload, message, rx_msg.size);

    if (rx_msg.payload.header.version != METERING_TEST_PROTOCOL_VERSION)
    {
        // Discard message and return True for forward compatibility
        LOGE("Invalid protocol version %d", rx_msg.payload.header.version);
        return true;
    }

    switch (rx_msg.payload.header.type)
    {
        case MSG_ID_REQ_TEST_INIT:
            LOG(LVL_INFO, "Received MSG_ID_REQ_TEST_INIT");
            memcpy((uint8_t *) &m_test_init,
                   rx_msg.payload.data,
                   sizeof(pl_req_test_init_t));

            App_Scheduler_addTask_execTime(
                initialize_dut_task,
                WAIT_FOR_HDLC_ACK_NACK_TIMEOUT_DELAY_MS,
                INITIALIZE_DUT_TASK_EXEC_TIME_US);
            break;
        case MSG_ID_REQ_TEST_EXT_FLASH:
            LOG(LVL_INFO, "Received MSG_ID_REQ_TEST_EXT_FLASH");
            LOG(LVL_INFO, "💾 Starting External Flash Testing 💾");

            extflash_tests_set_end_cb(flash_test_ended_cb);
            extflash_tests_reset_fsm_state();

            // Schedule the task that will run the external flash tests
            App_Scheduler_addTask_execTime(
                extflash_tests_fsm_task,
                WAIT_FOR_HDLC_ACK_NACK_TIMEOUT_DELAY_MS,
                TEST_EXEC_TIME_US);
            break;
        case MSG_ID_REQ_TEST_RF:
            LOG(LVL_INFO, "📶 Starting RF Testing 📶");
            LOG_FLUSH(LVL_INFO);

            // Intialize the rf testing tasks
            factory_rf_tests_dut_init(m_test_init.rf_test_params,
                                      m_test_init.net_settings.router_address,
                                      rf_test_ended_cb);
            break;
        default:
            LOGW("Unsupported cmd %d", rx_msg.payload.header.type);
            // Still return true for forward compatibility
            break;
    }

    return true;
}

/**
 * \brief   Initialization callback for application
 *
 * This function is called after hardware has been initialized but the
 * stack is not yet running.
 *
 */
void App_init(const app_global_functions_t * functions)
{
    LOG_INIT();

    LOG(LVL_INFO, "Metering Test App");

    HDLC_init(message_rx_from_script_cb);
}
