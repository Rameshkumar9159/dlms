/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include "nic_server.h"
#include "nic_server_objects.h"
#include "common.h"
#include "include/serverevents.h"
#include "include/server.h"
#include "nic_system_title.h"

#include <malloc.h>

#include "api.h"
#include "shared_data.h"
#include "dlms_lock.h"
#include "dlms_com.h"
#include "wirepas_com.h"
#include "security_material.h"
#include "security_service.h"
#include "transparent_mode.h"
#include "meter_clock.h"

#define DEBUG_LOG_MODULE_NAME "NIC SVR "
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

//#define LOG_DUMP

#define SRV_TIMEOUT_MS          60 * 1000 /* Timeout for NIC server association session */

/* ****************** */

static uint32_t serverHandler(void);

#define RESCHEDULE_ASAP()   \
            App_Scheduler_addTask_execTime(serverHandler, \
                                           APP_SCHEDULER_SCHEDULE_ASAP,\
                                           SFSM_EXECUTION_TIME_US)
typedef enum
{
    SRV_STATE_PARSE_REQUEST,
    SRV_STATE_CHECK_FOR_SECURITY_MATERIAL_UPDATE,
    SRV_STATE_EXIT,
} srv_fsm_state_e;

typedef struct
{
    // Data set
    uint8_t        last_byte_from_hes; // last byte

    uint8_t *      server_frame; // FRAME_SIZE
    uint8_t *      server_pdu;   // PDU_BUFFER_SIZE

    gxByteBuffer   data_to_hes;

    dlmsServerSettings server_settings;

    uint32_t invocation_counter;

    mcm_aa_e aa_type;
    // Whether the invocation counters have to be reset
    bool ic_reset;
} srv_param_t;


static srv_fsm_state_e m_state;
// NIC registration status
static bool m_nic_registered;
// Indicate if the security material has been updated
static bool m_sm_updated;
static srv_param_t * mp_srv_param;

static uint32_t m_current_delay_ms = 0;

/* ************************************ */
/* PUBLIC API                           */

void Nic_Server_disconnect(void)
{
    // Server connection was closed by the client
    m_state = SRV_STATE_CHECK_FOR_SECURITY_MATERIAL_UPDATE;
    RESCHEDULE_ASAP();
}

void Nic_Server_invalid_connection(void)
{
    LOGE("Invalid connection");
    // Cannot establish the connection
    m_state = SRV_STATE_EXIT;
    RESCHEDULE_ASAP();
}

/* ************************************ */
/* FSM & HANDLER                        */


static void server_cleanup()
{
    if (mp_srv_param)
    {
        const dlmsSettings * ms_p = &mp_srv_param->server_settings.base;
        uint32_t old_ic;

        if (mp_srv_param->aa_type == MCM_AA_US)
        {
            if (mp_srv_param->ic_reset)
            {
                LOGI("The encryption key has changed, resetting the NIC server invocation counters");
                Server_Attribute_Manager_resetInvocationCounters();
            }
            else
            {
                bool commit = false;
                // Readback the previous decryption IC
                Server_Attribute_Manager_readDecryptInvocationCounter(&old_ic);

                // If the decryption IC has been incremented
                if (old_ic < mp_srv_param->invocation_counter)
                {
                    LOGI( "Updating NIC server decryption IC: %d", mp_srv_param->invocation_counter);
                    Server_Attribute_Manager_writeDecryptInvocationCounterNoCommit(mp_srv_param->invocation_counter);
                    commit = true;
                }

                // Readback the previous encryption IC
                Server_Attribute_Manager_readEncryptInvocationCounter(&old_ic);

                if (old_ic < ms_p->cipher.invocationCounter)
                {
                    LOGI("Encryption IC set to %u", ms_p->cipher.invocationCounter);
                    Server_Attribute_Manager_writeEncryptInvocationCounterNoCommit(ms_p->cipher.invocationCounter);
                    commit = true;
                }
                if (commit)
                {
                    Server_Attr_Manager_commit();
                }
            }
        }

        Dlms_Server_deleteObjects();
        bb_clear(&mp_srv_param->data_to_hes);
        svr_clear(&mp_srv_param->server_settings);
        gxfree(mp_srv_param->server_frame);
        gxfree(mp_srv_param->server_pdu);
        gxfree(mp_srv_param);
        mp_srv_param = NULL;
    }
}

static void release_lock()
{
    bool registered;

    // Release the lock only if transparent is not enabled
    if (!Transparent_mode_is_enabled())
    {
        Dlms_lock_release(DLMS_LOCK_ID_NIC_SERVER);
    }

    Server_Attribute_Manager_readNicRegistrationStatus(&registered);

    // If NIC registration status has changed
    // or
    // if the security material has been updated
    // let's reboot
    if (registered != m_nic_registered || m_sm_updated)
    {
        // Reboot without delay to be sure
        // another FSM opening another association has time to execute
        Common_reboot(0);
    }
}

uint32_t Nic_Server_get_current_message_delay_ms(void)
{
    return m_current_delay_ms;
}

bool Nic_Server_handle_message(const uint8_t * bytes, size_t num_bytes,
                               uint16_t destination, mcm_aa_e aa_type,
                               uint32_t delay_ms)
{
    m_current_delay_ms = delay_ms;
    if (mp_srv_param == NULL)
    {
        // There is no copy, so pointer will point to the storage directly
        uint8_t * us_secret_p = NULL;
        uint8_t us_secret_len = 0;
        uint8_t * enc_key_p = NULL;
        uint8_t enc_key_len = 0;
        uint8_t * auth_key_p = NULL;
        uint8_t auth_key_len = 0;
        const uint8_t * system_title_p = NULL;
        uint8_t system_title_len = 0;
        uint8_t * server_frame;
        uint8_t * server_pdu;
        Dlms_Lock_return_code_e lock_ret;

        // Server is not started yet, time to start it by first taking the lock
        LOGI("Starting NIC server");
        lock_ret = Dlms_lock_take(DLMS_LOCK_ID_NIC_SERVER,
                           NULL,
                           DLMS_LOCK_TYPE_WITH_TRAFFIC);
        if (lock_ret != DLMS_LOCK_RET_ACQUIRED && lock_ret != DLMS_LOCK_RET_ALREADY_OWNER)
        {
            LOGE("Cannot aquire the lock! Message is lost");
            return false;
        }

        // Read the NIC current registration status
        Server_Attribute_Manager_readNicRegistrationStatus(&m_nic_registered);

        // Set the security material status to not updated
        m_sm_updated = false;

        // Allocate memory
        mp_srv_param = gxcalloc(1, sizeof(srv_param_t));
        server_frame = gxcalloc(1, FRAME_SIZE);
        server_pdu = gxcalloc(1, PDU_BUFFER_SIZE);

        if (!mp_srv_param || !server_frame || !server_pdu)
        {
            if (mp_srv_param)
            {
                gxfree(mp_srv_param);
            }
            if (server_frame)
            {
                gxfree(server_frame);
            }
            if (server_pdu)
            {
                gxfree(server_pdu);
            }
            LOGE("Cannot allocate memory for server! Message is lost");
            release_lock(DLMS_LOCK_ID_NIC_SERVER);
            return false;
        }

        // Update struct
        mp_srv_param->server_frame = server_frame;
        mp_srv_param->server_pdu   = server_pdu;

        // Init server settings
        svr_init(&mp_srv_param->server_settings, 1, DLMS_INTERFACE_TYPE_WRAPPER,
                 FRAME_SIZE, PDU_BUFFER_SIZE, server_frame, FRAME_SIZE,
                 server_pdu, PDU_BUFFER_SIZE);

        // Readback invocation counter
        Server_Attribute_Manager_readDecryptInvocationCounter(&mp_srv_param->invocation_counter);
        // Remove 1 as GuruX tests vs expected +1
        mp_srv_param->invocation_counter--;

        mp_srv_param->server_settings.info.preEstablished = 0;

        mp_srv_param->server_settings.base.maxServerPDUSize = PDU_BUFFER_SIZE;
        mp_srv_param->server_settings.base.maxPduSize = PDU_BUFFER_SIZE;

        mp_srv_param->aa_type = aa_type;

        if (aa_type == MCM_AA_US)
        {
            Security_Material_get_us_secret(&us_secret_p, &us_secret_len);
            Security_Material_get_keys(&enc_key_p, &enc_key_len, &auth_key_p, &auth_key_len);

            // retrieve NIC system title
            Nic_ST_get(&system_title_p, &system_title_len);

            mp_srv_param->server_settings.base.authentication = DLMS_AUTHENTICATION_HIGH;

            // Ciphering configuration
            dlmsSettings * ms_p = &mp_srv_param->server_settings.base;
            ciphering * cipher_p = &ms_p->cipher;

            bb_clear(&ms_p->password);
            bb_set(&ms_p->password, us_secret_p, us_secret_len);

            cipher_p->security = DLMS_SECURITY_AUTHENTICATION_ENCRYPTION;
            cipher_p->suite = DLMS_SECURITY_SUITE_V0;
            Server_Attribute_Manager_readEncryptInvocationCounter(&cipher_p->invocationCounter);

            bb_clear(&cipher_p->authenticationKey);
            bb_set(&cipher_p->authenticationKey, auth_key_p, auth_key_len);
            bb_clear(&cipher_p->blockCipherKey);
            bb_set(&cipher_p->blockCipherKey, enc_key_p, enc_key_len);
            bb_clear(&cipher_p->systemTitle);
            bb_set(&cipher_p->systemTitle, system_title_p, system_title_len);

            mp_srv_param->server_settings.base.expectedInvocationCounter = &mp_srv_param->invocation_counter;
        }
        else if (aa_type == MCM_AA_PC)
        {
            mp_srv_param->server_settings.base.authentication = DLMS_AUTHENTICATION_NONE;
        }
        else
        {
            // TODO create an API in application_association.[ch] to print the AA type
            LOGE("Association %u not supported, exiting", aa_type);
            server_cleanup();
            release_lock();
            // TODO should we return some DLMS error message ?
            return false;
        }

        // Add COSEM objects.
        if (!Dlms_Server_createObjects(&mp_srv_param->server_settings, aa_type,
                                       &mp_srv_param->invocation_counter))
        {
            LOGE("Failed to allocate memory for nic server objects, exiting");
            server_cleanup();
            release_lock();
            return false;
        }
    }

    // At this point mp_srv_param is not NULL so we can parse the message
    LOGI("Rcvd %u bytes from HES", num_bytes);
#if defined(LOG_DUMP)
    LOGI("From HES:");
    LOG_BUFFER(LVL_INFO, bytes,  num_bytes);
#endif

    // Verify that the AA is same
    if (mp_srv_param->aa_type != aa_type)
    {
        LOGE("Received msg AA %u while first msg AA %u",
             aa_type, mp_srv_param->aa_type);
        server_cleanup();
        release_lock();
        return false;
    }

    bb_init(&mp_srv_param->data_to_hes);

    // feeding most of data now, in order to deal with gurux buffer management.
    // the last byte will trig the request procesing in the FSM.
    if (svr_handleRequest2(&mp_srv_param->server_settings, (uint8_t *)bytes, num_bytes-1, &mp_srv_param->data_to_hes) != 0)
    {
        LOGE("Data from HES is not valid");
        server_cleanup();
        release_lock();
        return false;
    }

    m_state = SRV_STATE_PARSE_REQUEST;
    mp_srv_param->last_byte_from_hes = bytes[num_bytes-1];
    // let the FSM finish the work
    RESCHEDULE_ASAP();
    return true;
}

static void update_security_cb(int32_t result)
{
    (void)result;

    // If the encryption key has changed, we reset the invocation counter
    if (Security_Material_is_enc_key_changed())
    {
        mp_srv_param->ic_reset = true;
    }

    // The clock is also updated in this FSM so check if there is a new security material first before to write in flash
    if (Security_Material_is_new_material_available())
    {
        // Update unconditionnaly the material in persistent storage
        Security_Material_switch_to_new_material();
        // Set the status of the security material to updated
        m_sm_updated = true;
    }
    m_state = SRV_STATE_EXIT;
    RESCHEDULE_ASAP();
}

static uint32_t serverHandler()
{
    // default delay is "reschedule ASAP"
    gxByteBuffer * data_p = &mp_srv_param->data_to_hes;
    uint32_t delay = APP_SCHEDULER_SCHEDULE_ASAP;
    uint32_t epoch;
    int16_t deviation;
    int ret;

    SFSM_ENTRY(m_state);
    switch (m_state)
    {
        case SRV_STATE_PARSE_REQUEST:
            ret = svr_handleRequest3(&mp_srv_param->server_settings, mp_srv_param->last_byte_from_hes, data_p);
            // Next state is always EXIT: immediately in case of error or with a timeout
            m_state = SRV_STATE_CHECK_FOR_SECURITY_MATERIAL_UPDATE;
            if (ret != 0)
            {
                LOGE("Data from HES is not valid");
            }
            else if (data_p->size == 0)
            {
                LOGE("No data to send to HES");
            }
            else
            {
                // Send response to HES:
                if (!Wirepas_com_send_message(data_p->data, data_p->size, NULL, WC_TYPE_ON_DEMAND))
                {
                    // TODO: Close server session or retry?
                    LOGE("Cannot send %u bytes to HES", data_p->size);
                }
                else
                {
                    // Wait for next request up to TIMEOUT_MS
                    // Or until session is explicitly closed
                    // TODO: check associationstatus
                    // if it is NONE, exit directly
                    delay = SRV_TIMEOUT_MS;
                    LOGI("send %u bytes to HES", data_p->size);
#if defined(LOG_DUMP)
                    LOG_BUFFER(LVL_INFO, data_p->data,  data_p->size);
#endif
                }
                // Clear it to release memory
                bb_clear(&mp_srv_param->data_to_hes);
                // Clear byte buffers used by Gurux as they won't be cleared
                // in case of errors (e.g. wrong invocation counter)
                // and the previous data will be used to answer the next client request
                mp_srv_param->server_settings.receivedData.size = 0;
                mp_srv_param->server_settings.receivedData.position = 0;
                mp_srv_param->server_settings.info.data.size = 0;
                mp_srv_param->server_settings.info.data.position = 0;
            }
            break;

        case SRV_STATE_CHECK_FOR_SECURITY_MATERIAL_UPDATE:
            if (Security_Material_is_new_material_available() ||
                MeterClock_get_new_meter_clock(&epoch, &deviation))
            {
                Dlms_Lock_return_code_e rc;

                LOGI("MSM or clock updated");
                // Update material in the meter
                // First, update lock type to prevent reentrency
                rc = Dlms_lock_changeType(DLMS_LOCK_ID_NIC_SERVER,
                                          DLMS_LOCK_TYPE_WITHOUT_TRAFFIC);
                if (rc == DLMS_LOCK_RET_ACQUIRED &&
                    Security_Service_updateSecurity(update_security_cb))
                {
                    // The switch to the new material will be performed
                    // in the callback
                    delay = SFSM_DELAY_PAUSED;
                }
                else
                {
                    // Update unconditionnaly the material in persistent storage
                    Security_Material_switch_to_new_material();
                }
            }
            m_state = SRV_STATE_EXIT;
            break;

        case SRV_STATE_EXIT:
            server_cleanup();
            LOGI("end of dlms server");
            SFSM_CHECK();

            release_lock();

            delay = APP_SCHEDULER_STOP_TASK;
            break;
    }

    SFSM_EXIT();
    return delay;
}
