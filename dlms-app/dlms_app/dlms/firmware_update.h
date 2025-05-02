/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    firmware_update.h
 * \brief   interface for the smart meter firmware update management
 */

#ifndef FIRMWARE_UPDATE_H_
#define FIRMWARE_UPDATE_H_

typedef enum
{
    FIRMWARE_UPDATE_STEP_NO_OP,
    FIRMWARE_UPDATE_STEP_CHECK_AVAILABILITY,
    FIRMWARE_UPDATE_STEP_READ_UPDATE,
    FIRMWARE_UPDATE_STEP_CONNECT,
    FIRMWARE_UPDATE_STEP_CHECK_IF_IMAGE_TRANSFER_IS_ENABLED,
    FIRMWARE_UPDATE_STEP_READ_BLOCK_SIZE,
    FIRMWARE_UPDATE_STEP_CHECK_IMAGE_TRANSFER_STATUS,
    FIRMWARE_UPDATE_STEP_INITIATE_TRANSFER,
    FIRMWARE_UPDATE_STEP_TRANSFER_BLOCK,
    FIRMWARE_UPDATE_STEP_VERIFY_IMAGE,
    FIRMWARE_UPDATE_STEP_ACTIVATE_IMAGE,
    FIRMWARE_UPDATE_STEP_RECONNECT
} firmware_update_step_e;

typedef enum
{
    FIRMWARE_UPDATE_RESULT_OK,
    FIRMWARE_UPDATE_RESULT_ONGOING,
    FIRMWARE_UPDATE_RESULT_NO_AVAILABLE_UPDATE,
    FIRMWARE_UPDATE_RESULT_ALREADY_PROCESSED,
    FIRMWARE_UPDATE_RESULT_UPDATE_AVAILABLE,
    FIRMWARE_UPDATE_RESULT_ENABLED,
    FIRMWARE_UPDATE_RESULT_DISABLED,
    FIRMWARE_UPDATE_RESULT_INTERNAL_ERROR,
    FIRMWARE_UPDATE_RESULT_CONNECTION_ERROR,
    FIRMWARE_UPDATE_RESULT_LIB_ERROR,
    FIRMWARE_UPDATE_RESULT_METER_ERROR,
    FIRMWARE_UPDATE_RESULT_OPERATION_FAILED,
    FIRMWARE_UPDATE_RESULT_INVALID_OPERATION,
    FIRMWARE_UPDATE_RESULT_TEMPORARY_FAILURE
} firmware_update_result_e;

typedef struct
{
    firmware_update_step_e step;
    firmware_update_result_e result;
} firmware_update_status_t;

typedef void (* on_firmware_update_check_completed_cb_f) (void);

bool Firmware_Update_process_firmware_update(const uint8_t * image_identifier,
                                             uint8_t image_identifier_len);

void Firmware_Update_check_activation_to_complete(on_firmware_update_check_completed_cb_f cb);

void Firmware_Update_query_firmware_update_status(firmware_update_status_t * status_p);

#endif // FIRMWARE_UPDATE_H_
