/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include "dlms_lock.h"
#include <string.h> // for memset
#include <stdio.h> // snprintf

#include "api.h"
#include "common.h"

#include "firmware_update.h"
#include "wirepas_meter_firmware_update.h"
#include "meter_connection_management.h"
#include "dlms_com.h"
#include "meter_clock.h"
#include "nic_status.h"

#include <malloc.h>

#include "include/cosem.h" // cosem2
#include "include/gxmem.h" // gxcalloc
#include "include/helpers.h" // hlp_setObjectCount
#include "include/client.h" // cl_method

#define DEBUG_LOG_MODULE_NAME "FWUPDATE"
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

// Refer to [area:other_firmware_area] in dlms_app.ini
#define METER_FIRMWARE_AREA     0x860AE280

// Limit for the number of retries (get image verification/activation status)
#define LOOP_LIMIT_RETRIES      3

#define DEFAULT_DELAY_BEFORE_RECONNECTION_MS    (5 * 60 * 1000)

// OBIS of the ImageTransfer
static obis_code_t c_image_transfer_ln = { 0, 0, 44, 0, 0, 255 };

// States of the firmware update state machine
typedef enum
{
    FU_STATE_TAKE_LOCK,
    FU_STATE_CHECK_UPDATE_AVAILABILITY,
    FU_STATE_CONNECT,
    FU_STATE_CHECK_IF_IMAGE_TRANSFER_IS_ENABLED,
    FU_STATE_READ_BLOCK_SIZE,
    FU_STATE_CHECK_IMAGE_TRANSFER_STATUS,
    FU_STATE_CALL_IMAGE_TRANSFER_INITIATE_METHOD,
    FU_STATE_CALL_IMAGE_BLOCK_TRANSFER_METHOD,
    FU_STATE_CALL_IMAGE_VERIFY_METHOD,
    FU_STATE_CALL_IMAGE_ACTIVATE_METHOD,
    FU_STATE_CLOSE,
    FU_STATE_RECONNECT,
    FU_STATE_CHECK_IMAGE_TRANSFER_STATUS_AFTER_ACTIVATION,
    FU_STATE_EXIT
} firmware_update_sfsm_state_e;

// The header of the firmware update
typedef struct __attribute__ ((__packed__))
{
    uint32_t magic;
    uint32_t status;
    uint32_t size;
    uint32_t rfu;
} meter_firmware_header_t;

typedef struct
{
    uint8_t * id;
    uint8_t len;
} image_identifier_t;

typedef struct
{
    message msg;
    gxReplyData reply;
    gxByteBuffer bb;
    dlmsVARIANT var;
    gxImageTransfer imt;
    // image block size for transfer
    uint32_t block_size;
    // Image First Not Transferred Block Number is not used
    // as some meters do not support it and transfer can not be resumed
    uint32_t block_nb;
    uint32_t total_block_nb;
    meter_firmware_header_t meter_firmware_header;
    uint8_t * buf_p;
    uint8_t retries;
    bool marked_processed;
} firmware_update_data_t;

// Magic number for the firmware header
#define METER_FIRMWARE_HEADER_MAGIC     0xACB3D8C9

// Value indicating that the firmware update has already been processed
#define METER_FIRMWARE_PROCESSED        0x00000000

#define FU_OPERATION_COMPLETION_DELAY_MS    (30 * 1000)
#define RESCHEDULE_ASAP()   \
                App_Scheduler_addTask_execTime(firmwareUpdateHandler, \
                                               APP_SCHEDULER_SCHEDULE_ASAP,\
                                               SFSM_EXECUTION_TIME_US)
// Static variables
static firmware_update_sfsm_state_e m_state;
static image_identifier_t m_img_id;
static firmware_update_data_t * mp_data;
static firmware_update_status_t m_status;
static on_firmware_update_check_completed_cb_f m_activation_completed_cb;

static uint32_t firmwareUpdateHandler(void);

static void init_variables(void)
{
    // Beware that mes_init(...) is allocating a buffer thus init_variables()
    // should be called once only before usage of these variables
    mes_init(&mp_data->msg);
    reply_init(&mp_data->reply);
    var_init(&mp_data->var);
    bb_init(&mp_data->bb);
}

static void clear_variables(void)
{
    bb_clear(&mp_data->bb);
    var_clear(&mp_data->var);
    reply_clear(&mp_data->reply);
    mes_clear(&mp_data->msg);
}

static void on_lock_aquired_cb()
{
    if (! m_activation_completed_cb)
    {
        m_state = FU_STATE_CHECK_UPDATE_AVAILABILITY;
    }
    else
    {
        m_state = FU_STATE_RECONNECT;
    }
    RESCHEDULE_ASAP();
}

static void open_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result)
    {
        m_state = FU_STATE_CHECK_IF_IMAGE_TRANSFER_IS_ENABLED;
    }
    else
    {
        m_status.result = FIRMWARE_UPDATE_RESULT_CONNECTION_ERROR;
        LOGE("Connect: %d", result);
        m_state = FU_STATE_EXIT;
    }
    RESCHEDULE_ASAP();
}

static void reopen_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result)
    {
        m_state = FU_STATE_CHECK_IMAGE_TRANSFER_STATUS_AFTER_ACTIVATION;
    }
    else
    {
        Server_Attribute_Manager_writeMeterFirmwareUpdateActivationOngoing(false);
        m_status.result = FIRMWARE_UPDATE_RESULT_CONNECTION_ERROR;
        LOGE("Reconnect: %d", result);
        m_state = FU_STATE_EXIT;
    }
    RESCHEDULE_ASAP();
}

static bool check_meter_update_availability(void)
{
    meter_firmware_header_t * mfh_p = &mp_data->meter_firmware_header;

    if (lib_memory_area->startRead(METER_FIRMWARE_AREA, mfh_p,
                                    0, sizeof(meter_firmware_header_t)) !=
                                                    APP_LIB_MEM_AREA_RES_OK)
    {
        // Cannot read the meter firmware area
        LOGE("Failed to read METER_FIRMWARE_AREA");
        m_status.step = FIRMWARE_UPDATE_STEP_READ_UPDATE;
        m_status.result = FIRMWARE_UPDATE_RESULT_INTERNAL_ERROR;
        return false;
    }

    // After this point, the meter header is valid, check magic and status
    if (METER_FIRMWARE_HEADER_MAGIC != mfh_p->magic)
    {
        m_status.result = FIRMWARE_UPDATE_RESULT_NO_AVAILABLE_UPDATE;
        LOGI("No firmware");
        return false;
    }
    if (METER_FIRMWARE_PROCESSED == mfh_p->status)
    {
        m_status.result = FIRMWARE_UPDATE_RESULT_ALREADY_PROCESSED;
        LOGI("Firmware already processed");
        return false;
    }

    // Status will be updated when the transfer is completed
    m_status.result = FIRMWARE_UPDATE_RESULT_UPDATE_AVAILABLE;
    LOGI("Firmware available (%u bytes)", mfh_p->size);
    return true;
}

static void check_if_image_transfer_is_enabled_cb(int32_t result)
{
    const gxReplyData * reply_p = &mp_data->reply;

    m_state = FU_STATE_CLOSE;
    if (DLMS_ERROR_CODE_OK == result)
    {
        if (DLMS_DATA_TYPE_BOOLEAN == reply_p->dataValue.vt)
        {
            if (reply_p->dataValue.boolVal == true)
            {
                m_status.result = FIRMWARE_UPDATE_RESULT_ENABLED;
                LOGI("Image xfer enabled");
                m_state = FU_STATE_READ_BLOCK_SIZE;
            }
            else
            {
                m_status.result = FIRMWARE_UPDATE_RESULT_DISABLED;
                LOGI("Image xfer disabled");
            }
        }
        else
        {
            m_status.result = FIRMWARE_UPDATE_RESULT_METER_ERROR;
            LOGE("CIITIE: invalid type: %d", reply_p->dataValue.vt);
        }
    }
    else
    {
        m_status.result = FIRMWARE_UPDATE_RESULT_METER_ERROR;
        LOGE("CIITIE: failed to read: %d", result);
    }
    clear_variables();

    RESCHEDULE_ASAP();
}

static void read_block_size_cb(int32_t result)
{
    const meter_firmware_header_t * mfh_p = &mp_data->meter_firmware_header;
    gxReplyData * reply_p = &mp_data->reply;

    m_state = FU_STATE_CLOSE;
    if (DLMS_ERROR_CODE_OK == result)
    {
        if (DLMS_DATA_TYPE_UINT32 == reply_p->dataValue.vt)
        {
            mp_data->block_size = reply_p->dataValue.ulVal;
            mp_data->total_block_nb = (mfh_p->size / mp_data->block_size) +
                                ((mfh_p->size % mp_data->block_size) ? 1 : 0);
            LOGI("Block size: %u, block #: %u", mp_data->block_size, mp_data->total_block_nb);

            mp_data->buf_p = gxcalloc(mp_data->block_size, sizeof(uint8_t));
            if (mp_data->buf_p)
            {
                m_status.result = FIRMWARE_UPDATE_RESULT_OK;
                m_state = FU_STATE_CALL_IMAGE_TRANSFER_INITIATE_METHOD;
            }
            else
            {
                m_status.result = FIRMWARE_UPDATE_RESULT_INTERNAL_ERROR;
                LOGE("malloc failed");
            }
        }
        else
        {
            m_status.result = FIRMWARE_UPDATE_RESULT_METER_ERROR;
            LOGE("RBS: invalid type: %d", reply_p->dataValue.vt);
        }
    }
    else
    {
        m_status.result = FIRMWARE_UPDATE_RESULT_METER_ERROR;
        LOGE("RBS: failed to read block size: %d", result);
    }
    clear_variables();
    RESCHEDULE_ASAP();
}

static void read_image_transfer_status_cb(int32_t result)
{
    const gxReplyData * reply_p = &mp_data->reply;
    uint32_t delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;

    m_state = FU_STATE_CLOSE;

    if (result == DLMS_ERROR_CODE_OK)
    {
        if (DLMS_DATA_TYPE_ENUM == reply_p->dataValue.vt)
        {
            m_status.result = FIRMWARE_UPDATE_RESULT_OK;
            switch (reply_p->dataValue.bVal)
            {
                case DLMS_IMAGE_TRANSFER_STATUS_NOT_INITIATED:
                    m_state = FU_STATE_CALL_IMAGE_TRANSFER_INITIATE_METHOD;
                    break;

                case DLMS_IMAGE_TRANSFER_STATUS_INITIATED:
                    m_state = FU_STATE_CALL_IMAGE_BLOCK_TRANSFER_METHOD;
                    break;

                case DLMS_IMAGE_TRANSFER_STATUS_VERIFICATION_INITIATED:
                    // We need to wait for the completion of the ongoing operation
                    // State is unchanged
                    delay_ms = FU_OPERATION_COMPLETION_DELAY_MS;
                    if (mp_data->retries)
                    {
                        m_state = FU_STATE_CHECK_IMAGE_TRANSFER_STATUS;
                        mp_data->retries--;
                    }
                    else
                    {
                        m_status.result = FIRMWARE_UPDATE_RESULT_OPERATION_FAILED;
                    }
                    break;

                case DLMS_IMAGE_TRANSFER_STATUS_VERIFICATION_SUCCESSFUL:
                    // Verification has been successful
                    LOGI("Firmware verification completed successfully");
                    m_status.result = FIRMWARE_UPDATE_RESULT_OK;
                    m_state = FU_STATE_CALL_IMAGE_ACTIVATE_METHOD;
                    break;

                case DLMS_IMAGE_TRANSFER_STATUS_VERIFICATION_FAILED:
                    // TODO: is it possible to disable image transfer ?
                    LOGE("Firmware verification failed");
                    m_status.result = FIRMWARE_UPDATE_RESULT_OPERATION_FAILED;
                    break;

                case DLMS_IMAGE_TRANSFER_STATUS_ACTIVATION_INITIATED:
                    // We need to wait for the completion of the ongoing operation
                    // State is unchanged
                    LOGI("Firmware activation initiated");
                    delay_ms = FU_OPERATION_COMPLETION_DELAY_MS;
                    if (mp_data->retries)
                    {
                        m_state = FU_STATE_CHECK_IMAGE_TRANSFER_STATUS;
                        mp_data->retries--;
                    }
                    else
                    {
                        m_status.result = FIRMWARE_UPDATE_RESULT_OPERATION_FAILED;
                    }
                    break;

                case DLMS_IMAGE_TRANSFER_STATUS_ACTIVATION_SUCCESSFUL:
                    LOGI("Firmware activation completed successfully");
                    m_status.result = FIRMWARE_UPDATE_RESULT_OK;
                    break;

                case DLMS_IMAGE_TRANSFER_STATUS_ACTIVATION_FAILED:
                    // TODO: is it possible to disable image transfer ?
                    LOGE("Firmware activation failed");
                    m_status.result = FIRMWARE_UPDATE_RESULT_OPERATION_FAILED;
                    break;

            }
        }
        else
        {
            m_status.result = FIRMWARE_UPDATE_RESULT_METER_ERROR;
            LOGE("Invalid type for image xfer status: %d",
                reply_p->dataValue.vt);
        }
    }
    else
    {
        m_status.result = FIRMWARE_UPDATE_RESULT_METER_ERROR;
        LOGE("Failed to read image xfer status  %d", result);
    }

    clear_variables();
    App_Scheduler_addTask_execTime(firmwareUpdateHandler, delay_ms,
                                   SFSM_EXECUTION_TIME_US);
}

static void read_image_transfer_status_after_activation_cb(int32_t result)
{
    const gxReplyData * reply_p = &mp_data->reply;
    uint32_t delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;

    m_state = FU_STATE_CLOSE;

    if (result == DLMS_ERROR_CODE_OK)
    {
        if (DLMS_DATA_TYPE_ENUM == reply_p->dataValue.vt)
        {
            m_status.result = FIRMWARE_UPDATE_RESULT_OK;
            switch (reply_p->dataValue.bVal)
            {
                case DLMS_IMAGE_TRANSFER_STATUS_ACTIVATION_INITIATED:
                    // We need to wait for the completion of the ongoing operation
                    // State is unchanged
                    LOGI("Firmware activation initiated");
                    delay_ms = FU_OPERATION_COMPLETION_DELAY_MS;
                    if (mp_data->retries)
                    {
                        m_state = FU_STATE_CHECK_IMAGE_TRANSFER_STATUS_AFTER_ACTIVATION;
                        mp_data->retries--;
                    }
                    else
                    {
                        m_status.result = FIRMWARE_UPDATE_RESULT_OPERATION_FAILED;
                    }
                    break;

                case DLMS_IMAGE_TRANSFER_STATUS_ACTIVATION_SUCCESSFUL:
                    LOGI("Firmware activation completed successfully");
                    m_status.result = FIRMWARE_UPDATE_RESULT_OK;
                    break;

                case DLMS_IMAGE_TRANSFER_STATUS_ACTIVATION_FAILED:
                    // TODO: is it possible to disable image transfer ?
                    LOGE("Firmware activation failed");
                    m_status.result = FIRMWARE_UPDATE_RESULT_OPERATION_FAILED;
                    break;

                default:
                    m_status.result = FIRMWARE_UPDATE_RESULT_OPERATION_FAILED;
                     break;
           }
        }
        else
        {
            m_status.result = FIRMWARE_UPDATE_RESULT_METER_ERROR;
            LOGE("Invalid type for image xfer status: %d",
                reply_p->dataValue.vt);
        }
    }
    else
    {
        m_status.result = FIRMWARE_UPDATE_RESULT_METER_ERROR;
        LOGE("Failed to read image xfer status  %d", result);
    }

    if (m_state == FU_STATE_CLOSE)
    {
        Server_Attribute_Manager_writeMeterFirmwareUpdateActivationOngoing(false);
    }
    clear_variables();
    App_Scheduler_addTask_execTime(firmwareUpdateHandler, delay_ms,
                                   SFSM_EXECUTION_TIME_US);
}

static bool set_variant_for_image_transfer_initiate_method(void)
{
    dlmsVARIANT * var_p = &mp_data->var;
    gxByteBuffer * bb_p = &mp_data->bb;
    const meter_firmware_header_t * mf_p = &mp_data->meter_firmware_header;

    if (bb_setInt8(bb_p, DLMS_DATA_TYPE_STRUCTURE) != DLMS_ERROR_CODE_OK ||
        bb_setInt8(bb_p, 2) != DLMS_ERROR_CODE_OK ||
        bb_setInt8(bb_p, DLMS_DATA_TYPE_OCTET_STRING) != DLMS_ERROR_CODE_OK ||
        hlp_setObjectCount(m_img_id.len, bb_p) != DLMS_ERROR_CODE_OK ||
        bb_set(bb_p, m_img_id.id, m_img_id.len) != DLMS_ERROR_CODE_OK ||
        bb_setInt8(bb_p, DLMS_DATA_TYPE_UINT32) != DLMS_ERROR_CODE_OK ||
        bb_setInt32(bb_p, mf_p->size) != DLMS_ERROR_CODE_OK ||
        var_addBytes(var_p, bb_p->data, bb_p->size) != DLMS_ERROR_CODE_OK)
    {
        return false;
    }
    return true;
}

static void image_transfer_initiate_method_cb(int32_t result)
{
    if (result == DLMS_ERROR_CODE_OK)
    {
        m_state = FU_STATE_CHECK_IMAGE_TRANSFER_STATUS;
        m_status.result = FIRMWARE_UPDATE_RESULT_OK;
    }
    else
    {
        m_status.result = FIRMWARE_UPDATE_RESULT_METER_ERROR;
        m_state = FU_STATE_CLOSE;
        LOGE("Error calling image xfer initiate method %d",
            result);
    }
    clear_variables();
    RESCHEDULE_ASAP();
}

static bool set_variant_for_image_block_transfer_method(void)
{
    dlmsVARIANT * var_p = &mp_data->var;
    gxByteBuffer * bb_p = &mp_data->bb;
    uint32_t block_nb = mp_data->block_nb;
    uint32_t block_size = mp_data->block_size;
    uint32_t read_size = block_size;
    uint32_t fw_size = mp_data->meter_firmware_header.size;
    uint32_t offset = sizeof(meter_firmware_header_t);
    uint8_t * buf_p = mp_data->buf_p;

    // There are still data to send
    offset += block_nb * block_size;

    if (fw_size - (block_nb * block_size) < block_size)
    {
        read_size = fw_size - (block_nb * block_size);
    }
    if (lib_memory_area->startRead(METER_FIRMWARE_AREA, buf_p, offset,
                                   read_size) != APP_LIB_MEM_AREA_RES_OK)
    {
        LOGE("Failed to read memory area at offset %u, size %u",
           offset, block_size);
        return false;
    }

    if (bb_setInt8(bb_p, DLMS_DATA_TYPE_STRUCTURE) != DLMS_ERROR_CODE_OK ||
        bb_setInt8(bb_p, 2) != DLMS_ERROR_CODE_OK ||
        bb_setInt8(bb_p, DLMS_DATA_TYPE_UINT32) != DLMS_ERROR_CODE_OK ||
        bb_setInt32(bb_p, block_nb) != DLMS_ERROR_CODE_OK ||
        bb_setInt8(bb_p, DLMS_DATA_TYPE_OCTET_STRING) != DLMS_ERROR_CODE_OK ||
        hlp_setObjectCount(read_size, bb_p) != DLMS_ERROR_CODE_OK ||
        bb_set(bb_p, buf_p, read_size) != DLMS_ERROR_CODE_OK ||
        var_addBytes(var_p, bb_p->data, bb_p->size) != DLMS_ERROR_CODE_OK)
    {
        return false;
    }

#if DEBUG_LOG_MAX_LEVEL == LOG_DEBUG
    char msg[60];
    snprintf(msg, sizeof(msg), "Block %3lu, offset %lu, size %lu", block_nb,
            offset, (block_nb + 1) * block_size);
    LOG_BUFFER(msg, LOG_DEBUG, buf_p, block_size);
#endif

    return true;
}

static void image_block_transfer_method_cb(int32_t result)
{
    if (result == DLMS_ERROR_CODE_OK)
    {
        uint32_t fw_size = mp_data->meter_firmware_header.size;

        // State does not change until the whole image has been transfered
        mp_data->block_nb++;
        if (fw_size <= mp_data->block_nb * mp_data->block_size)
        {
            m_state = FU_STATE_CALL_IMAGE_VERIFY_METHOD;
        }
        m_status.result = FIRMWARE_UPDATE_RESULT_OK;
    }
    else
    {
        m_status.result = FIRMWARE_UPDATE_RESULT_METER_ERROR;
        m_state = FU_STATE_CLOSE;
        LOGE("Error calling image block xfer method %d", result);
    }
    clear_variables();
    RESCHEDULE_ASAP();
}

static void image_verify_method_cb(int32_t result)
{
    if (result == DLMS_ERROR_CODE_OK)
    {
        m_status.result = FIRMWARE_UPDATE_RESULT_OK;
        m_state = FU_STATE_CHECK_IMAGE_TRANSFER_STATUS;
    }
    else if (result == DLMS_ERROR_CODE_TEMPORARY_FAILURE)
    {
        m_status.result = FIRMWARE_UPDATE_RESULT_TEMPORARY_FAILURE;
        m_state = FU_STATE_CHECK_IMAGE_TRANSFER_STATUS;
    }
    else
    {
        m_status.result = FIRMWARE_UPDATE_RESULT_METER_ERROR;
        m_state = FU_STATE_CLOSE;
        LOGE("Error calling method image verify %d", result);
    }
    clear_variables();
    RESCHEDULE_ASAP();
}

static void image_activate_method_cb(int32_t result)
{
    if (result == DLMS_ERROR_CODE_OK)
    {
        Server_Attribute_Manager_writeMeterFirmwareUpdateActivationOngoing(true);
        m_status.result = FIRMWARE_UPDATE_RESULT_OK;
        m_state = FU_STATE_CLOSE;
    }
    else if (result == DLMS_ERROR_CODE_TEMPORARY_FAILURE)
    {
        m_status.result = FIRMWARE_UPDATE_RESULT_TEMPORARY_FAILURE;
        m_state = FU_STATE_CHECK_IMAGE_TRANSFER_STATUS;
    }
    else
    {
        m_status.result = FIRMWARE_UPDATE_RESULT_METER_ERROR;
        m_state = FU_STATE_CLOSE;
        LOGE("Error calling method image activate %d", result);
    }
    clear_variables();
    RESCHEDULE_ASAP();
}

static void close_cb(int32_t result)
{
    (void)result;
    bool ongoing;

    Server_Attribute_Manager_readMeterFirmwareUpdateActivationOngoing(&ongoing);

    if (ongoing)
    {
        m_state = FU_STATE_RECONNECT;
        App_Scheduler_addTask_execTime(firmwareUpdateHandler,
                                       DEFAULT_DELAY_BEFORE_RECONNECTION_MS,
                                       SFSM_EXECUTION_TIME_US);
    }
    else
    {
        m_state = FU_STATE_EXIT;
        RESCHEDULE_ASAP();
    }
}

static bool dlms_read_attribute_async(obis_code_t ln,
                                      DLMS_OBJECT_TYPE object_type,
                                      uint8_t attribute, message * msg_p,
                                      gxReplyData * reply_p,
                                      operation_result_cb cb)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    int res;

    init_variables();

    res = cl_readLN(ms_p, ln, object_type, attribute, NULL, msg_p);
    if (DLMS_ERROR_CODE_OK == res)
    {
        Dlms_Com_call_async(msg_p, reply_p, cb);
    }
    else
    {
        clear_variables();
        m_status.result = FIRMWARE_UPDATE_RESULT_LIB_ERROR;
        LOGE("cl_readLN: %d", res);
    }
    return DLMS_ERROR_CODE_OK == res;
}

/**
 * @brief:
 *
 *  FSM handling the firmware update process
 *
 */

static uint32_t firmwareUpdateHandler(void)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    uint32_t delay_ms = SFSM_DELAY_PAUSED;
    gxByteBuffer * bb_p = &mp_data->bb;
    Dlms_Lock_return_code_e lock_ret;

    SFSM_ENTRY(m_state);

    switch (m_state)
    {
        case FU_STATE_TAKE_LOCK:
            // Take the lock with traffic as we may receive QUERY_FIRMWARE_UPDATE_STATUS commands
            lock_ret = Dlms_lock_take(DLMS_LOCK_ID_FIRMWARE_UPDATE,
                                      on_lock_aquired_cb,
                                      DLMS_LOCK_TYPE_WITH_TRAFFIC);

            if (lock_ret == DLMS_LOCK_RET_ACQUIRED)
            {
                // Move to next state immediatelly
                if (! m_activation_completed_cb)
                {
                    m_state = FU_STATE_CHECK_UPDATE_AVAILABILITY;
                }
                else
                {
                    m_state = FU_STATE_RECONNECT;
                }
                delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            else if (lock_ret != DLMS_LOCK_RET_WAITING_FOR_LOCK)
            {
                LOGE("Cannot take or wait for lock");
                delay_ms = SFSM_DELAY_RETRY;
            }
            break;

        case FU_STATE_CHECK_UPDATE_AVAILABILITY:
            m_status.step = FIRMWARE_UPDATE_STEP_CHECK_AVAILABILITY;
            if ((mp_data = gxcalloc(1, sizeof(firmware_update_data_t))))
            {
                if (check_meter_update_availability())
                {
                    m_state = FU_STATE_CONNECT;
                }
                else
                {
                    m_state = FU_STATE_EXIT;
                }
                delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            else
            {
                LOGE("Malloc failed");
                m_status.result = FIRMWARE_UPDATE_RESULT_INTERNAL_ERROR;
                m_state = FU_STATE_CLOSE;
                delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case FU_STATE_CONNECT:
            m_status.step = FIRMWARE_UPDATE_STEP_CONNECT;
            if (Meter_Connection_Management_open(open_cb, MCM_AA_FU) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_status.result = FIRMWARE_UPDATE_RESULT_CONNECTION_ERROR;
                m_state = FU_STATE_EXIT;
                delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case FU_STATE_CHECK_IF_IMAGE_TRANSFER_IS_ENABLED:
            m_status.step = FIRMWARE_UPDATE_STEP_CHECK_IF_IMAGE_TRANSFER_IS_ENABLED;
            // IEC 62056-6-2 COSEM interface classes section 5.3.6.3
            if (! dlms_read_attribute_async(c_image_transfer_ln,
                                            DLMS_OBJECT_TYPE_IMAGE_TRANSFER, 5,
                                            &mp_data->msg, &mp_data->reply,
                                            check_if_image_transfer_is_enabled_cb))
            {
                LOGW("CIITIE failed");
                m_state = FU_STATE_CLOSE;
                delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case FU_STATE_READ_BLOCK_SIZE:
            m_status.step = FIRMWARE_UPDATE_STEP_READ_BLOCK_SIZE;
            if (! dlms_read_attribute_async(c_image_transfer_ln,
                                            DLMS_OBJECT_TYPE_IMAGE_TRANSFER, 2,
                                            &mp_data->msg, &mp_data->reply,
                                            read_block_size_cb))
            {
                LOGW("RBS failed");
                m_state = FU_STATE_CLOSE;
                delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case FU_STATE_CHECK_IMAGE_TRANSFER_STATUS:
            if (! dlms_read_attribute_async(c_image_transfer_ln,
                                            DLMS_OBJECT_TYPE_IMAGE_TRANSFER, 6,
                                            &mp_data->msg, &mp_data->reply,
                                            read_image_transfer_status_cb))
            {
                LOGW("CITS failed");
                m_state = FU_STATE_CLOSE;
                delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case FU_STATE_CALL_IMAGE_TRANSFER_INITIATE_METHOD:
            m_status.step = FIRMWARE_UPDATE_STEP_INITIATE_TRANSFER;
            cosem_init2(BASE(mp_data->imt), DLMS_OBJECT_TYPE_IMAGE_TRANSFER,
                        c_image_transfer_ln);
            init_variables();
            if (set_variant_for_image_transfer_initiate_method() &&
                cl_method2(ms_p, BASE(mp_data->imt), 1, bb_p->data,
                           bb_p->size, &mp_data->msg) == DLMS_ERROR_CODE_OK)
            {
                Dlms_Com_call_async(&mp_data->msg, &mp_data->reply,
                                    image_transfer_initiate_method_cb);
            }
            else
            {
                clear_variables();
                m_status.result = FIRMWARE_UPDATE_RESULT_LIB_ERROR;
                LOGW("CITIM failed");
                m_state = FU_STATE_CLOSE;
                delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case FU_STATE_CALL_IMAGE_BLOCK_TRANSFER_METHOD:
            m_status.step = FIRMWARE_UPDATE_STEP_TRANSFER_BLOCK;
            LOGI("xferring block #%3u / %3u",
                mp_data->block_nb, mp_data->total_block_nb - 1);
            cosem_init2(BASE(mp_data->imt), DLMS_OBJECT_TYPE_IMAGE_TRANSFER,
                        c_image_transfer_ln);
            init_variables();
            if (set_variant_for_image_block_transfer_method() &&
                cl_method2(ms_p, BASE(mp_data->imt), 2, bb_p->data,
                           bb_p->size, &mp_data->msg) == DLMS_ERROR_CODE_OK)
            {
                Dlms_Com_call_async(&mp_data->msg, &mp_data->reply,
                                    image_block_transfer_method_cb);
            }
            else
            {
                clear_variables();
                m_status.result = FIRMWARE_UPDATE_RESULT_LIB_ERROR;
                m_state = FU_STATE_CLOSE;
                delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case FU_STATE_CALL_IMAGE_VERIFY_METHOD:
            m_status.step = FIRMWARE_UPDATE_STEP_VERIFY_IMAGE;
            // Set the max number of retries to get the transfer verification status
            mp_data->retries = LOOP_LIMIT_RETRIES;
            LOGI("Calling image verification method");
            cosem_init2(BASE(mp_data->imt), DLMS_OBJECT_TYPE_IMAGE_TRANSFER,
                        c_image_transfer_ln);
            init_variables();
            GX_INT8(mp_data->var) = 0;
            if (cl_method(ms_p, BASE(mp_data->imt), 3,
                          &mp_data->var, &mp_data->msg) == DLMS_ERROR_CODE_OK)
            {
                 Dlms_Com_call_async_with_timeout(&mp_data->msg, &mp_data->reply,
                                                  image_verify_method_cb, 90);
            }
            else
            {
                clear_variables();
                m_status.result = FIRMWARE_UPDATE_RESULT_LIB_ERROR;
                m_state = FU_STATE_CLOSE;
                delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case FU_STATE_CALL_IMAGE_ACTIVATE_METHOD:
            m_status.step = FIRMWARE_UPDATE_STEP_ACTIVATE_IMAGE;
            // Set the max number of retries to get the transfer activate status
            mp_data->retries = LOOP_LIMIT_RETRIES;
            LOGI("Calling image activation method");
            cosem_init2(BASE(mp_data->imt), DLMS_OBJECT_TYPE_IMAGE_TRANSFER,
                        c_image_transfer_ln);
            init_variables();
            GX_INT8(mp_data->var) = 0;
            if (cl_method(ms_p, BASE(mp_data->imt), 4,
                          &mp_data->var, &mp_data->msg) == DLMS_ERROR_CODE_OK)
            {
                Dlms_Com_call_async(&mp_data->msg, &mp_data->reply,
                                    image_activate_method_cb);
            }
            else
            {
                clear_variables();
                m_status.result = FIRMWARE_UPDATE_RESULT_LIB_ERROR;
                m_state = FU_STATE_CLOSE;
                delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case FU_STATE_CLOSE:
            clear_variables();
            Meter_Connection_Management_close(close_cb);
            delay_ms = SFSM_DELAY_PAUSED;
            break;

        case FU_STATE_RECONNECT:
            m_status.step = FIRMWARE_UPDATE_STEP_RECONNECT;
            if (Meter_Connection_Management_open(reopen_cb, MCM_AA_FU) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                Server_Attribute_Manager_writeMeterFirmwareUpdateActivationOngoing(false);
                m_status.result = FIRMWARE_UPDATE_RESULT_CONNECTION_ERROR;
                m_state = FU_STATE_EXIT;
                delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case FU_STATE_CHECK_IMAGE_TRANSFER_STATUS_AFTER_ACTIVATION:
            m_status.step = FIRMWARE_UPDATE_STEP_ACTIVATE_IMAGE;
            mp_data->retries = LOOP_LIMIT_RETRIES;
            if (! dlms_read_attribute_async(c_image_transfer_ln,
                                            DLMS_OBJECT_TYPE_IMAGE_TRANSFER, 6,
                                            &mp_data->msg, &mp_data->reply,
                                            read_image_transfer_status_after_activation_cb))
            {
                Server_Attribute_Manager_writeMeterFirmwareUpdateActivationOngoing(false);
                LOGW("CITSAA failed");
                m_state = FU_STATE_CLOSE;
                delay_ms = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case FU_STATE_EXIT:
            // Free and zeroed the image identifier
            if (m_img_id.id && m_img_id.len)
            {
                free(m_img_id.id);
            }
            memset(&m_img_id, 0x00, sizeof(m_img_id));

            if (mp_data)
            {
                if (mp_data->buf_p)
                {
                    gxfree(mp_data->buf_p);
                }
                gxfree(mp_data);
                mp_data = NULL;
            }
            SFSM_CHECK();
            Dlms_lock_release(DLMS_LOCK_ID_FIRMWARE_UPDATE);
            delay_ms = SFSM_DELAY_PAUSED;
            Wirepas_Meter_Firmware_Update_notify_status(&m_status);
            if (m_activation_completed_cb)
            {
                m_activation_completed_cb();
                m_activation_completed_cb = NULL;
            }
            break;
    }
    SFSM_EXIT();

    return delay_ms;
}

bool Firmware_Update_process_firmware_update(const uint8_t * image_identifier,
                                             uint8_t image_identifier_len)
{
    if (! image_identifier || ! image_identifier_len)
    {
        LOGE("Missing image id");
        return false;
    }
    // We didn't grab the lock yet so let's use standard malloc
    if (! (m_img_id.id = calloc(1, image_identifier_len)))
    {
        LOGE("malloc failed");
        return false;
    }
    memcpy(m_img_id.id, image_identifier, image_identifier_len);
    m_img_id.len = image_identifier_len;
    m_activation_completed_cb = NULL;
    m_state = FU_STATE_TAKE_LOCK;

    RESCHEDULE_ASAP();

    return true;
}

void Firmware_Update_check_activation_to_complete(on_firmware_update_check_completed_cb_f cb)
{
    bool ongoing;

    Server_Attribute_Manager_readMeterFirmwareUpdateActivationOngoing(&ongoing);

    LOGD("Firmware activation ongoing: %d", ongoing);

    if (ongoing)
    {
        LOGI("Firmware Update activation to complete");
        if ((mp_data = gxcalloc(1, sizeof(firmware_update_data_t))))
        {
            m_activation_completed_cb = cb;
            m_state = FU_STATE_TAKE_LOCK;
            RESCHEDULE_ASAP();
            return;
        }
        else
        {
            LOGE("Malloc failed");
            Server_Attribute_Manager_writeMeterFirmwareUpdateActivationOngoing(false);
            m_status.result = FIRMWARE_UPDATE_RESULT_INTERNAL_ERROR;
        }
    }
    cb();
}

void Firmware_Update_query_firmware_update_status(firmware_update_status_t * status_p)
{
    bool ongoing;

    Server_Attribute_Manager_readMeterFirmwareUpdateActivationOngoing(&ongoing);

    memcpy(status_p, &m_status, sizeof * status_p);

    // If the activation is ongoing or if the FSM is running, set the result as ongoing
    if (ongoing || mp_data)
    {
        status_p->result = FIRMWARE_UPDATE_RESULT_ONGOING;
    }
}
