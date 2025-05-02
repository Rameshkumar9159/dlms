/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    app.c
 * \brief   An application that receives and applies provisioning data received
 * on the UART using the HDLC Protocol.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "app_persistent.h"
#include "app_scheduler.h"
#include "node_configuration.h"
#include "usart.h"

#include "hdlc.h"
#include "provisioning_data.h"
#include "nic_provisioning.h"
#include "public_provisioning.h"

#define DEBUG_LOG_MODULE_NAME "NIC_PROV"
#define DEBUG_LOG_MAX_LEVEL   LVL_INFO
#include "debug_log.h"

// Wait for more than the ACK_NACK_TIMEOUT_MS
#define PERFORM_PROVISIONING_TASK_DELAY_MS  750
#define PERFORM_PROVISIONING_TASK_EXEC_TIME 100

#define VALIDATE_PROVISIONING_DATA_TASK_EXEC_TIME 100

#define READ_PARAMETERS_TASK_EXEC_TIME 100

// Provisioning Protocol Version used in the message header
#define PROVISIONING_PROTOCOL_VERSION 0

#define TIMEOUT_100MS_TASK_DELAY_MS     100
#define TIMEOUT_100MS_TASK_EXEC_TIME_US 100

#define CHALLENGE_BUF_SIZE          64
#define PROVISIONING_CHALLENGE      "WPP\n"
#define PROVISIONING_CHALLENGE_RESP "OK\n"
#define MSG_END_CHAR                '\n'

// Message type used in the message header
typedef enum
{
    MESSAGE_TYPE_RECEIVE_PROVISIONING_PACKET     = 0,
    MESSAGE_TYPE_SEND_PROVISIONING_RETURN_CODE   = 1,
    MESSAGE_TYPE_RECEIVE_DEVICE_REBOOT_REQUEST   = 2,
    MESSAGE_TYPE_RECEIVE_READ_PARAMETERS_REQUEST = 3,
    MESSAGE_TYPE_SEND_READ_PARAMETERS_RESPONSE   = 4
} message_type_e;

// Message header used in every HDLC packet
typedef struct __attribute__((packed))
{
    uint8_t version;  // Should be 0
    uint8_t type;     // Use the message_type_e values
} message_header_t;

// Structure holding the message's header and data
typedef struct __attribute__((packed))
{
    message_header_t header;
    uint8_t          data[BUFFER_MAX_SIZE - sizeof(message_header_t)];
} message_payload_t;

// Holds data received or that can be sent in HDLC
typedef struct
{
    message_payload_t payload;  // Holds header + data
    uint16_t          size;     // Payload size
} message_t;

static message_t m_provisioning_message;

static provisioning_ret_e m_validation_run_ret = PROV_RET_INTERNAL_ERROR;

/** Buffers for RX */
static char     m_rx_buffer[CHALLENGE_BUF_SIZE];
static uint32_t m_rx_buffer_idx;
static uint32_t current_cmd_idx = 0;

// Callback if challenge wasn't received (if NIC already provisioned)
static void (*m_timeout_cb)(void) = NULL;

static bool message_rx_from_script_cb(const uint8_t * message,
                                      size_t          message_size);

__STATIC_INLINE void resetRxBuffer(void)
{
    memset(&m_rx_buffer, 0, CHALLENGE_BUF_SIZE);
    m_rx_buffer_idx = 0;
    current_cmd_idx = 0;
}

static uint32_t timeout_100ms_task(void)
{
    LOGI("No Challenge Received on the UART, skipping Provisioning Process");
    Usart_setEnabled(false);
    m_timeout_cb();
    return APP_SCHEDULER_STOP_TASK;
}

/**
 * Store received characters into RX buffer and check if it contains
 * end-character to indicate that full command is received. Call
 * handleRxCommands() to implement requested command as many times there
 * are commands in the RX buffer.
 *
 */
// This is used as a callback, and we want to avoid modifying the caller's signature.
// cppcheck-suppress constParameterCallback
void uartRxCb(uint8_t * chars, size_t n)
{
    if ((m_rx_buffer_idx + n) > (CHALLENGE_BUF_SIZE - 1))
    {
        // Chars received don't fit in the buffer
        resetRxBuffer();
        return;
    }

    // Store received characters into buffer
    memcpy(&m_rx_buffer[m_rx_buffer_idx], chars, n);
    m_rx_buffer_idx += n;

    // Check if buffer contains full commands
    for (uint8_t i = current_cmd_idx; i < m_rx_buffer_idx; i++)
    {
        if (m_rx_buffer[i] == MSG_END_CHAR)
        {
            const uint8_t * ptr = (uint8_t *) &m_rx_buffer[current_cmd_idx];

            if (0
                == memcmp(ptr,
                          PROVISIONING_CHALLENGE,
                          strlen(PROVISIONING_CHALLENGE)))
            {
                Usart_sendBuffer(PROVISIONING_CHALLENGE_RESP,
                                 strlen(PROVISIONING_CHALLENGE_RESP));
                App_Scheduler_cancelTask(timeout_100ms_task);

                Usart_setEnabled(false);

                LOGI("Received challenge on the UART");
                LOG_FLUSH(LVL_INFO);

                HDLC_init(message_rx_from_script_cb);

                break;
            }

            current_cmd_idx = i + 1;
            // If all commands in buffer handled, clean the buffer
            if (i >= m_rx_buffer_idx - 1)
            {
                resetRxBuffer();
            }
        }
    }
}


/**
 * \brief Sends Provisioning Return Code to Python Script
 *
 * \param ret_code Provisioning Return Code
 */
static void send_prov_ret_code(uint8_t ret_code)
{
    CborEncoder encoder;
    CborError   err = CborNoError;
    message_t   prov_ret_cod_resp;

    prov_ret_cod_resp.payload.header
        = (message_header_t){ .version = PROVISIONING_PROTOCOL_VERSION,
                              .type
                              = MESSAGE_TYPE_SEND_PROVISIONING_RETURN_CODE };

    prov_ret_cod_resp.size = sizeof(message_header_t);

    cbor_encoder_init(&encoder,
                      prov_ret_cod_resp.payload.data,
                      BUFFER_MAX_SIZE - sizeof(message_header_t),
                      0);

    err = cbor_encode_uint(&encoder, ret_code);
    if (err != CborNoError)
    {
        LOGD("Error when encoding NIC Provisioning Return Code");
    }

    prov_ret_cod_resp.size
        += cbor_encoder_get_buffer_size(&encoder,
                                        prov_ret_cod_resp.payload.data);

    HDLC_send_message((uint8_t *) &prov_ret_cod_resp.payload,
                      prov_ret_cod_resp.size);
}

/**
 * \brief Performs a validation run on the provisioning data and send the return
 * code
 * \note This is not a dry run, however, the parameters are discarded and not
 * applied to the device yet. We want to check the user parameters, which a dry
 * run doesn't do.
 * \return uint32_t delay in ms to be scheduled (0 to be scheduled asap)
 */
static uint32_t validate_provisioning_data_task(void)
{
    provisioning_data_conf_t prov_data_conf
        = { .end_cb       = NULL,
            .user_data_cb = NULL,
            .buffer       = (uint8_t *) m_provisioning_message.payload.data,
            .length = m_provisioning_message.size - sizeof(message_header_t) };

    m_validation_run_ret = Provisioning_Data_decode(&prov_data_conf, true);

    if (m_validation_run_ret != PROV_RET_OK)
    {
        LOGE("Error in the given parameters! Provisioning_res = %u",
             m_validation_run_ret);
    }

    LOGI("Sending Provisioning Return Code");
    send_prov_ret_code((uint8_t) m_validation_run_ret);

    return APP_SCHEDULER_STOP_TASK;
}

/**
 * \brief   The end provisioning callback. This function is called at the end
 *          of the provisioning process.
 * \param   result
 *          Result of the provisioning process.
 * \return  True: Apply received network parameters and reboot; False: discard
 *          data and end provisioning process.
 */
static bool provisioning_end_cb(provisioning_res_e result)
{
    if (result != PROV_RES_SUCCESS)
    {
        LOGE("Could not apply provisioning parameters! Provisioning_res = %u",
             result);
        return false;
    }

    return true;
}

/**
 * \brief   Applies the provisioning parameters previously received and tested
 *          during the dry run
 *
 * \return  uint32_t delay in ms to be scheduled (0 to be scheduled asap)
 */
static uint32_t perform_provisioning_task(void)
{
    provisioning_ret_e       ret = PROV_RET_OK;
    provisioning_data_conf_t prov_data_conf
        = { .end_cb       = provisioning_end_cb,
            .user_data_cb = NULL,
            .buffer       = (uint8_t *) m_provisioning_message.payload.data,
            .length = m_provisioning_message.size - sizeof(message_header_t) };

    // Applying parameters to the NIC
    ret = Provisioning_Data_decode(&prov_data_conf, false);

    /* We go past this point only if the provisioning was not successful
     * (i.e. error returned)
     */
    if (ret != PROV_RET_OK)
    {
        LOGD(
            "Provisioning Data Decode was not successful when decoding NIC Provisioning Data");
    }

    return APP_SCHEDULER_STOP_TASK;
}

/**
 * \brief Reads and sends the Wirepas and NIC parameters to the Python
 * Script
 *
 * \return uint32_t delay in ms to be scheduled (0 to be scheduled asap)
 */
static uint32_t read_parameters_task(void)
{
    message_t read_prov_data_resp = { 0 };

    read_prov_data_resp.payload.header
        = (message_header_t){ .version = PROVISIONING_PROTOCOL_VERSION,
                              .type
                              = MESSAGE_TYPE_SEND_READ_PARAMETERS_RESPONSE };
    read_prov_data_resp.size = sizeof(message_header_t);

    uint16_t parameters_length = BUFFER_MAX_SIZE - sizeof(message_header_t);
    Provisioning_Data_read_parameters(read_prov_data_resp.payload.data, &parameters_length);

    read_prov_data_resp.size += parameters_length;

    LOGI("Sending Read Parameters Response");

    HDLC_send_message((uint8_t *) &read_prov_data_resp.payload,
                      read_prov_data_resp.size);

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

    if (rx_msg.payload.header.version != PROVISIONING_PROTOCOL_VERSION)
    {
        // Discard message and return True for forward compatibility
        LOGE("Invalid protocol version %d", rx_msg.payload.header.version);
        return true;
    }

    switch (rx_msg.payload.header.type)
    {
        case MESSAGE_TYPE_RECEIVE_PROVISIONING_PACKET:
            LOGI("Received Provisioning Packet");
            // Copy received message to process it in an other task
            memcpy(&m_provisioning_message, &rx_msg, rx_msg.size);

            App_Scheduler_addTask_execTime(
                validate_provisioning_data_task,
                APP_SCHEDULER_SCHEDULE_ASAP,
                VALIDATE_PROVISIONING_DATA_TASK_EXEC_TIME);
            break;
        case MESSAGE_TYPE_RECEIVE_DEVICE_REBOOT_REQUEST:
            LOGI("Received Device Reboot Request");
            // We apply parameters only if the dry run didn't return an
            // error
            if (m_validation_run_ret == PROV_RET_OK)
            {
                // If a second ACK is received, we don't schedule task
                m_validation_run_ret = PROV_RET_INTERNAL_ERROR;

                App_Scheduler_addTask_execTime(
                    perform_provisioning_task,
                    PERFORM_PROVISIONING_TASK_DELAY_MS,
                    PERFORM_PROVISIONING_TASK_EXEC_TIME);
            }
            break;
        case MESSAGE_TYPE_RECEIVE_READ_PARAMETERS_REQUEST:
            LOGI("Received Read Parameters Request");
            App_Scheduler_addTask_execTime(read_parameters_task,
                                           APP_SCHEDULER_SCHEDULE_ASAP,
                                           READ_PARAMETERS_TASK_EXEC_TIME);
            break;
        default:
            LOGW("Unsupported cmd %d", rx_msg.payload.header.type);
            // Still return true for forward compatibility
            break;
    }

    return true;
}

void Nic_Provisioning_start(void (*timeout_cb)(void))
{
    Provisioning_Data_init();

    // Add the call to your app here
    if (Provisioning_Data_is_nic_provisioned())
    {
        // If NIC is already provisioned, we open a 100 ms window to read or
        // provision parameters, otherwise we wait until the parameters are
        // provisioned
        App_Scheduler_addTask_execTime(timeout_100ms_task,
                                       TIMEOUT_100MS_TASK_DELAY_MS,
                                       TIMEOUT_100MS_TASK_EXEC_TIME_US);

        LOGI("NIC is already provisioned");
    }
    else
    {
        LOGI("NIC is not provisioned, waiting for the challenge on the UART!");
    }

    // Initialize Serial
    LOGI("Initializing UART at %u baudrate", PROVISIONING_UART_BAUDRATE);
    Usart_init(PROVISIONING_UART_BAUDRATE, UART_FLOW_CONTROL_NONE);
    Usart_enableReceiver(uartRxCb);
    Usart_setEnabled(true);
    Usart_receiverOn();

    m_timeout_cb = timeout_cb;
}
