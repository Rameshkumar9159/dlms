/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    opt_passthrough.c
 * \brief
 */
#include <stdint.h>
#include <stdbool.h>

#include "opt_passthrough.h"
#include "meter_connection_management.h"
#include "dlms_com.h"
#include "dlms_lock.h"
#include "wirepas_com.h"
#include "application_association.h"
#include "security_material.h"
#include "nic_system_title.h"
#include <malloc.h>


#include "api.h"
#include "shared_data.h"

//Gurux DLMS includes.
#include "include/gxmem.h" // gxmalloc
#include "include/dlmssettings.h"
#include "include/variant.h"
#include "include/cosem.h"
#include "include/server.h"
#include "include/bytebuffer.h"
#include "include/client.h"

/* missing declaration */
extern unsigned char dlms_getGloMessage(dlmsSettings* settings,
                                         DLMS_COMMAND command,
                                         DLMS_COMMAND encryptedCommand);

#define DEBUG_LOG_MODULE_NAME "OPT_P-T "
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

//#define LOG_DUMP

#define DATA_RESEND_DELAY_MS    10 * 1000 /* retry to send data to HES in 10s */ // TODO, probably modify it as lock is kept during this time

#define KEY_SIZE                16

static uint32_t run_passthrough_fsm(void);

#define RESCHEDULE_ASAP()   \
            App_Scheduler_addTask_execTime(run_passthrough_fsm, \
                                           APP_SCHEDULER_SCHEDULE_ASAP,\
                                           SFSM_EXECUTION_TIME_US)

typedef enum
{
    OPT_STATE_CONN,
    OPT_STATE_SEND_REQUEST,
    OPT_STATE_PARSE_REPLY,
    OPT_STATE_RESEND_REPLY,
    OPT_STATE_DISC,
    OPT_STATE_EXIT,
} opt_fsm_state_e;

typedef struct
{
    // Data set
    gxByteBuffer   data_from_hes;
    message        data_to_meter;

    gxReplyData    data_from_meter;
    gxByteBuffer   data_to_hes;

    dlmsServerSettings server_settings;
    uint8_t        invokeId;
    DLMS_COMMAND   cmd;
    int32_t        meter_read_result;

    // Application association type for meter connection
    mcm_aa_e       aa_type;

    // NIC server invocation counter
    uint32_t       invocation_counter;
} opt_param_t;

static opt_fsm_state_e m_state;

static opt_param_t * mp_param;

static uint8_t JUNK_MEM[1];

// *************************************************

static void open_cb(int32_t result)
{
    m_state = (result == DLMS_ERROR_CODE_OK) ? OPT_STATE_SEND_REQUEST : OPT_STATE_DISC;
    RESCHEDULE_ASAP();
}

static void close_cb(int32_t result)
{
    m_state = OPT_STATE_EXIT;
    RESCHEDULE_ASAP();
}

static void read_meter_cb(int32_t result)
{
    mes_clear(&mp_param->data_to_meter);

    if (result != DLMS_ERROR_CODE_OK)
    {
        LOGE("Error from meter %u", result);
    }

    if (result <= DLMS_ERROR_CODE_OTHER_REASON)
    {
        // If there is an error, it is from the dlms layer, and can be forwarded
        mp_param->meter_read_result = result;
        m_state = OPT_STATE_PARSE_REPLY;
    }
    else
    {
        reply_clear(&mp_param->data_from_meter);
        m_state = OPT_STATE_DISC;
    }
    RESCHEDULE_ASAP();
}


/* ********************************************************* */
#define PRIORITY_INVOKE_ID_OFFSET 2
static uint8_t get_priority_invoke_id(gxByteBuffer * apdu_p)
{
    //reply_p->data contains the XDLMS APDU
    if (apdu_p->size > PRIORITY_INVOKE_ID_OFFSET)
    {
        // For most (maybe all) valid messages that the HES can send to the meter:
        // First byte is the command
        // Second byte is the type ?
        // Third byte is the priority and invoke-id
        return apdu_p->data[PRIORITY_INVOKE_ID_OFFSET];
    }
    return 0;
}


static bool _translate_HES_to_meter()
{
    gxByteBuffer * data_from_hes = &mp_param->data_from_hes;
    message * data_to_meter =  &mp_param->data_to_meter;

    dlmsServerSettings * server_settings = &mp_param->server_settings;
    dlmsSettings * meter_settings = Common_getMeterSettings();

    gxReplyData reply;
    bool translation_done = false;

    reply_init(&reply);
#if defined(LOG_DUMP)
    LOGI("#data_from_hes");
    LOG_BUFFER(LVL_INFO, data_from_hes->data, data_from_hes->size);
#endif

    // 1- Decode request from HES
    reply.ignoreValue = 1; // do not decode all the values
    server_settings->base.server = 0; // does not work as server
    int res = cl_getData(&server_settings->base, data_from_hes, &reply);
    server_settings->base.server = 1; // restore value

    if ((res == DLMS_ERROR_CODE_OK) && (reply.complete) &&
        (reply.moreData == DLMS_DATA_REQUEST_TYPES_NONE) &&
        (reply.command != DLMS_COMMAND_NONE) &&
        (reply.data.position > 0)) /* We check for 0 in case it is an association in passthrough*/
    {
#if defined(LOG_DUMP)
        LOGI("#getData");
        LOG_BUFFER(LVL_INFO, reply.data.data, reply.data.size);
#endif
        // 2- Prepare request for meter
        mp_param->invokeId = get_priority_invoke_id(&reply.data);  // save invokeID
        mp_param->cmd = (reply.encryptedCommand == DLMS_COMMAND_NONE) ? reply.command : reply.encryptedCommand;

        --reply.data.position;
#ifndef METER_RETROFIT
        // Retrofits support PC & MR only and MR is not encrypted
        if (mp_param->aa_type != MCM_AA_PC)
        {
            if (reply.encryptedCommand == DLMS_COMMAND_NONE)
            {
                LOGE("Request from HES was not encrypted. No right elevation allowed!");
                res = DLMS_ERROR_CODE_ACCESS_VIOLATED;
            }
            else
            {
                gxByteBuffer* key;
                if (dlms_useDedicatedKey(meter_settings) && (meter_settings->connected & DLMS_CONNECTION_STATE_DLMS) != 0)
                {
                    key = meter_settings->cipher.dedicatedKey;
                }
                else
                {
                    key = &meter_settings->cipher.blockCipherKey;
                }
                res = cip_encrypt(
                        &meter_settings->cipher,
                        meter_settings->cipher.security,
                        DLMS_COUNT_TYPE_PACKET,
                        meter_settings->cipher.invocationCounter,
                        dlms_getGloMessage(meter_settings, reply.command, reply.encryptedCommand),
                        meter_settings->cipher.systemTitle.data,
                        key,
                        &reply.data
                );
                // TODO: is it useful to increment the meter IC here?
                ++meter_settings->cipher.invocationCounter;
            }
        }
#endif // METER_RETROFIT

        if (res == DLMS_ERROR_CODE_OK)
        {
            // Create message
            switch (meter_settings->interfaceType)
            {
                case DLMS_INTERFACE_TYPE_WRAPPER:
                {
                    gxByteBuffer* f = gxmalloc(sizeof(gxByteBuffer));
                    bb_init(f);

                    res = dlms_getWrapperFrame(meter_settings, mp_param->cmd, &reply.data, f);
                    mes_push(data_to_meter, f);
                }
                break;

                case DLMS_INTERFACE_TYPE_HDLC:
                {
                    dlms_addLLCBytes(meter_settings, &reply.data);

                    unsigned char frame = 0;
                    while (res == 0 && bb_available(&reply.data) != 0)
                    {
                        gxByteBuffer* f = gxmalloc(sizeof(gxByteBuffer));
                        bb_init(f);

                        res = dlms_getHdlcFrame(meter_settings, frame, &reply.data, f);
                        mes_push(data_to_meter, f);

                        if (res == 0 && bb_available(&reply.data) != 0)
                        {
                            frame = getNextSend(meter_settings, 0);
                        }
                    }
                }
                break;

                default:
                    res = DLMS_ERROR_CODE_INVALID_PARAMETER;
                    break;
            }
        }

        if (res == DLMS_ERROR_CODE_OK)
        {
            translation_done = true;
        }
        else
        {
            LOGE("HES request translation failure %u", res);
            mes_clear(data_to_meter);
            // TODO: reply to meter
        }
    }
    else
    {
        LOGE("HES request decoding failure %u", res);
        mes_clear(data_to_meter);
    }

    reply_clear(&reply);
    bb_clear(data_from_hes);

    return translation_done;
}

static bool _translate_meter_to_HES()
{
    gxByteBuffer * data_to_hes = &mp_param->data_to_hes;
    gxReplyData  * data_from_meter =  &mp_param->data_from_meter;

    dlmsServerSettings * server_settings = &mp_param->server_settings;
    bool translation_done = false;
    uint8_t encrypted_cmd = DLMS_COMMAND_NONE;

    if (mp_param->meter_read_result == DLMS_ERROR_CODE_OK)
    {
#if defined(LOG_DUMP)
        LOGI("#data_from_meter");
        LOG_BUFFER(LVL_INFO, data_from_meter->data.data, data_from_meter->data.size);
#endif
    }
    else
    {
        bb_clear(&data_from_meter->data);
    }

    // translate command
    switch (mp_param->cmd)
    {
        case DLMS_COMMAND_GET_REQUEST:
            mp_param->cmd = DLMS_COMMAND_GET_RESPONSE;
            break;
        case DLMS_COMMAND_SET_REQUEST:
            mp_param->cmd = DLMS_COMMAND_SET_RESPONSE;
            break;
        case DLMS_COMMAND_METHOD_REQUEST:
            mp_param->cmd = DLMS_COMMAND_METHOD_RESPONSE;
            break;
        case DLMS_COMMAND_GLO_GET_REQUEST:
            mp_param->cmd = DLMS_COMMAND_GET_RESPONSE;
            encrypted_cmd = DLMS_COMMAND_GLO_GET_RESPONSE;
            break;
        case DLMS_COMMAND_GLO_SET_REQUEST:
            mp_param->cmd = DLMS_COMMAND_SET_RESPONSE;
            encrypted_cmd = DLMS_COMMAND_GLO_SET_RESPONSE;
           break;
        case DLMS_COMMAND_GLO_METHOD_REQUEST:
            mp_param->cmd = DLMS_COMMAND_METHOD_RESPONSE;
            encrypted_cmd = DLMS_COMMAND_GLO_METHOD_RESPONSE;
           break;
        default:
            mp_param->cmd = DLMS_COMMAND_NONE;
            break;
    }

    if (mp_param->cmd != DLMS_COMMAND_NONE)
    {
        gxLNParameters p;
        gxByteBuffer bb;
        gxByteBuffer * attr_desc_p = &data_from_meter->data;
        bb_init(&bb);
        int ret;

        // If this test is true, then it means that the meter response doesn't contain any data
        if (data_from_meter->data.size == data_from_meter->data.position)
        {
            attr_desc_p = NULL;
        }

        params_initLN(&p, &server_settings->base, mp_param->invokeId,
                      mp_param->cmd, 1, attr_desc_p, NULL,
                      mp_param->meter_read_result,
                      encrypted_cmd, 0, 0);
        if ((ret = dlms_getLNPdu(&p, &bb)) == DLMS_ERROR_CODE_OK)
        {
            server_settings->base.server = 0; // does not work as server
            if ((ret = dlms_getWrapperFrame(&server_settings->base,
                                            mp_param->cmd, &bb, data_to_hes)) ==
                                                            DLMS_ERROR_CODE_OK)
            {
#if defined(LOG_DUMP)
                LOGI("#dlms_getWrapperFrame");
                LOG_BUFFER(LVL_INFO, data_to_hes->data,  data_to_hes->size);
#endif
                translation_done = true;
            }
            else
            {
                LOGE("HES reply wrapping failure %u", ret);
            }
            server_settings->base.server = 1;
        }
        else
        {
            LOGE("HES reply encoding failure %u", ret);
        }

        (void) ret;
        bb_clear(&bb);
    }
    else
    {
        LOGE("Meter reply translation failure");
    }

    reply_clear(data_from_meter);

    return translation_done;
}

// *************************************************

static uint32_t run_passthrough_fsm()
{
    // default delay is "reschedule ASAP"
    uint32_t delay = APP_SCHEDULER_SCHEDULE_ASAP;
    uint8_t * secret_p = NULL;
    uint8_t secret_len = 0;
    DLMS_AUTHENTICATION authentication_level;
    const uint8_t * system_title_p = NULL;
    uint8_t system_title_len = 0;

    SFSM_ENTRY(m_state);
    switch (m_state)
    {
        case OPT_STATE_CONN:
            // Secret
            switch (mp_param->aa_type)
            {
                case MCM_AA_PC:
                    // Nothing to do
                    authentication_level = DLMS_AUTHENTICATION_NONE;
                    break;
                case MCM_AA_MR:
                    Security_Material_get_mr_secret(&secret_p, &secret_len);
                    authentication_level = DLMS_AUTHENTICATION_LOW;
                    break;
                case MCM_AA_US:
                    Security_Material_get_us_secret(&secret_p, &secret_len);
                    authentication_level = DLMS_AUTHENTICATION_HIGH;
                    break;
                case MCM_AA_FU:
                    Security_Material_get_fu_secret(&secret_p, &secret_len);
                    authentication_level = DLMS_AUTHENTICATION_HIGH;
                    break;
                default:
                    LOGE("Wrong aa level");
                    m_state = OPT_STATE_EXIT;
                    authentication_level = DLMS_AUTHENTICATION_NONE;
                    // Cannot do return here but should be tested before
            }

            // retrieve NIC system title
            Nic_ST_get(&system_title_p, &system_title_len);

            // Init Meter buffers
            mes_init(&(mp_param->data_to_meter));
            reply_init(&(mp_param->data_from_meter));

            // Init server settings (we do not really use server api in passthrough, so memory parameters are dummy)
            svr_init(&mp_param->server_settings, 1, DLMS_INTERFACE_TYPE_WRAPPER,
                     1, 1, JUNK_MEM, sizeof(JUNK_MEM), JUNK_MEM, sizeof(JUNK_MEM));

            // Override part of the default init
            dlmsSettings * ms_p = &mp_param->server_settings.base;

            // Before next cl_init, release allocated buffers that will be lost otherwise
            cip_clear(&ms_p->cipher);

            cl_init(ms_p, 1, 0, 0, authentication_level,
                    (char *)secret_p, DLMS_INTERFACE_TYPE_WRAPPER);

            ms_p->server = 1;

            mp_param->invocation_counter = 0;

#ifndef METER_RETROFIT
            if (mp_param->aa_type != MCM_AA_PC)
            {
                uint8_t * enc_key_p = NULL;
                uint8_t * auth_key_p = NULL;
                uint8_t enc_key_len = 0;
                uint8_t auth_key_len = 0;

                // Readback invocation counter
                Server_Attribute_Manager_readDecryptInvocationCounter(&mp_param->invocation_counter);
                LOGD("NIC server dec IC: %u", mp_param->invocation_counter);

                // TODO: check that is was encrypted
                // Retrieve the keys
                Security_Material_get_keys(&enc_key_p, &enc_key_len,
                                           &auth_key_p, &auth_key_len);

                // Ciphering configuration
                ciphering * cipher_p = &ms_p->cipher;
                cipher_p->security = DLMS_SECURITY_AUTHENTICATION_ENCRYPTION;
                cipher_p->suite = DLMS_SECURITY_SUITE_V0;

                bb_clear(&cipher_p->authenticationKey);
                bb_set(&cipher_p->authenticationKey, auth_key_p, auth_key_len);
                bb_clear(&cipher_p->blockCipherKey);
                bb_set(&cipher_p->blockCipherKey, enc_key_p, enc_key_len);
                bb_clear(&cipher_p->systemTitle);
                bb_attach(&cipher_p->systemTitle, (uint8_t *) system_title_p, system_title_len, system_title_len);

                // Set the reference of the expected IC
                ms_p->expectedInvocationCounter = &mp_param->invocation_counter;
                // Set the encryption invocation counter
                Server_Attribute_Manager_readEncryptInvocationCounter(&cipher_p->invocationCounter);
            }
#endif

            ms_p->preEstablishedSystemTitle = gxmalloc(sizeof(gxByteBuffer));

            bb_init(ms_p->preEstablishedSystemTitle);
            bb_attach(ms_p->preEstablishedSystemTitle, (uint8_t *) system_title_p, system_title_len, system_title_len);
            ms_p->negotiatedConformance = DLMS_CONFORMANCE_GET |
                                          DLMS_CONFORMANCE_SET |
                                          DLMS_CONFORMANCE_ACTION;

            if (Meter_Connection_Management_open(open_cb, mp_param->aa_type) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_state = OPT_STATE_EXIT;
            }
            else
            {
                delay = SFSM_DELAY_PAUSED;
            }
            break;

        case OPT_STATE_SEND_REQUEST:
            // check if data from HES is available
            if (mp_param->data_from_hes.size)
            {
                if (_translate_HES_to_meter())
                {
                    mp_param->data_from_meter.ignoreValue = 1; // do not decode all the values
                    Dlms_Com_call_async(&mp_param->data_to_meter,
                                        &mp_param->data_from_meter,
                                        read_meter_cb);
                    delay = SFSM_DELAY_PAUSED;
                }
                else
                {
                    LOGW("dropping message from HES");
                    m_state = OPT_STATE_DISC;
                }
            }
            else
            {
                LOGE("No data from HES when passthrough is ready");
                m_state = OPT_STATE_DISC;
            }
            break;


        case OPT_STATE_PARSE_REPLY:
            if (_translate_meter_to_HES()) // will clear mp_param->data_from_meter
            {
                LOGI("send %u bytes to HES", mp_param->data_to_hes.size);
                if (!Wirepas_com_send_message(mp_param->data_to_hes.data,
                                              mp_param->data_to_hes.size,
                                              NULL,
                                              WC_TYPE_ON_DEMAND))
                {
                    LOGW("Not able to send data to HES now, retry later");
                    m_state = OPT_STATE_RESEND_REPLY;
                    delay = DATA_RESEND_DELAY_MS;
                    break;
                }
            }
            else
            {
                LOGW("dropping message from meter");
            }

            bb_clear(&mp_param->data_to_hes);
            m_state = OPT_STATE_DISC;
            break;

        case OPT_STATE_RESEND_REPLY:
            if (!Wirepas_com_send_message(mp_param->data_to_hes.data,
                                          mp_param->data_to_hes.size,
                                          NULL,
                                          WC_TYPE_ON_DEMAND))
            {
                LOGE("Not able to send data to HES, drop message");
            }

            bb_clear(&mp_param->data_to_hes);
            m_state = OPT_STATE_DISC;
            break;

        case OPT_STATE_DISC:
            if (Meter_Connection_Management_close(close_cb) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                m_state = OPT_STATE_EXIT;
            }
            else
            {
                delay = SFSM_DELAY_PAUSED;
            }
            break;

        case OPT_STATE_EXIT:
            if (mp_param)
            {
                if (mp_param->aa_type != MCM_AA_PC)
                {
                    uint32_t old_ic;
                    bool commit = false;

                    // Readback the last stored IC
                    Server_Attribute_Manager_readDecryptInvocationCounter(&old_ic);
                    // If the IC has been incremented
                    if (old_ic < mp_param->invocation_counter)
                    {
                        LOGI("Updating NIC server invocation counter: %d",
                             mp_param->invocation_counter);
                        Server_Attribute_Manager_writeDecryptInvocationCounterNoCommit(mp_param->invocation_counter);
                        commit = true;
                    }

                    // Readback the last stored encryption IC
                    Server_Attribute_Manager_readEncryptInvocationCounter(&old_ic);
                    if (old_ic < mp_param->server_settings.base.cipher.invocationCounter)
                    {
                        Server_Attribute_Manager_writeEncryptInvocationCounterNoCommit(mp_param->server_settings.base.cipher.invocationCounter);
                        commit = true;
                    }
                    if (commit)
                    {
                        Server_Attr_Manager_commit();
                    }
                }
                // If opening the association fails, some of these buffers are "leaked"
                mes_clear(&mp_param->data_to_meter);
                reply_clear(&mp_param->data_from_meter);
                bb_clear(&mp_param->data_from_hes);

                svr_clear(&mp_param->server_settings);
                gxfree(mp_param);
                mp_param = NULL;
            }
            SFSM_CHECK();

            LOGI("end of passthrough");
            Dlms_lock_release(DLMS_LOCK_ID_PASSTHROUGH);

            delay = APP_SCHEDULER_STOP_TASK;
            break;
    }

    SFSM_EXIT();
    return delay;
}


// *************************************************

bool Opt_Passthrough_handle_passthrough_message(const uint8_t * message,
                                                size_t size,
                                                mcm_aa_e aa_type)
{
    bool registered;

    Server_Attribute_Manager_readNicRegistrationStatus(&registered);

    if (! registered)
    {
        LOGE("Passthrough is disabled when NIC is unregistered");
        return false;
    }

    // If we have receive a message, we should be able to take the lock
    if (Dlms_lock_take(DLMS_LOCK_ID_PASSTHROUGH,
                       NULL,
                       DLMS_LOCK_TYPE_WITHOUT_TRAFFIC) != DLMS_LOCK_RET_ACQUIRED)
    {
        LOGE("Cannot aquire the lock! Message is lost");
        return false;
    }

    LOGI("start of passthrough");
    // We have the lock, we can allocate parameters

    // Allocate memory first
    mp_param = gxcalloc(1, sizeof *mp_param);
    if (!mp_param)
    {
        LOGE("Failed to allocate memory for passthrough, exiting");
        return false;
    }

    // Save the AA type
    mp_param->aa_type = aa_type;

    // Init buffers
    bb_init(&(mp_param->data_from_hes));
    bb_init(&(mp_param->data_to_hes));

    // Copy the message
    bb_set(&mp_param->data_from_hes, message, size);

    // Time to connect to meter
    m_state = OPT_STATE_CONN;
    RESCHEDULE_ASAP();
    return true;
}
