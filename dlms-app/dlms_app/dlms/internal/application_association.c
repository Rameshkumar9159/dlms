/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    application_association.c
 * \brief   manage the application association establishment and release
 */

#include "application_association.h"
#include "common.h"
#include "random.h" // Random_get32
#include "dlms_com.h" // Should be modified later
#include "security_material.h"
#include "nic_system_title.h"
#include "nic_status.h"

// Gurux DLMS includes.
#include "include/client.h"
#include "include/dlmssettings.h"
#include "include/gxmem.h"

#define DEBUG_LOG_MODULE_NAME "APP_ASSO"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#define RESCHEDULE_ASAP(task)   \
                App_Scheduler_addTask_execTime(task, \
                                               APP_SCHEDULER_SCHEDULE_ASAP,\
                                               SFSM_EXECUTION_TIME_US)
// Default SAPs defined in IS15959 part 2
#define SERVER_ADDRESS                  0x0001

// Timeout to wait for meter's response in seconds
#define AA_TIMEOUT_S                    5

// States of the FSM handling the application association establishment
typedef enum
{
    AA_ESTABLISHMENT_INIT,
    AA_ESTABLISHMENT_SNRM,
    AA_ESTABLISHMENT_AARQ,
    AA_ESTABLISHMENT_HLS,
    AA_ESTABLISHMENT_DISC,
    AA_ESTABLISHMENT_EXIT,
} establish_aa_fsm_state_e;

// States of the FSM handling the application association release
/* ** */
typedef enum
{
    AA_RELEASE_STATE_RELEASE,
    AA_RELEASE_STATE_DISC,
    AA_RELEASE_STATE_EXIT,
} release_aa_fsm_state_e;

#define PWD_SIZE                        64
#define CHALLENGE_SIZE                  16
#define KEY_SIZE                        16

//Space for client challenge.
static unsigned char C2S_CHALLENGE[CHALLENGE_SIZE];
//Space for server challenge.
static unsigned char S2C_CHALLENGE[CHALLENGE_SIZE];

#ifdef APP_PRINTING
static const char* aa_to_str[] = {"PC", "MR", "US", "FU"};
#endif

// Prototypes
static uint32_t establish_aa_fsm(void);
static uint32_t release_aa_fsm(void);

typedef struct {
    message msg;
    gxReplyData reply;
    // Input
    aa_type_e aa_type;
    uint32_t invocation_counter;
    // FSM
    int32_t result;
    // Caller
    operation_result_cb cb;
} aa_establishment_param_t;

// Static variables
// FSM parameters
static aa_establishment_param_t * mp_param;
// AA establishment FSM state
static establish_aa_fsm_state_e m_establish_state;
// AA release FSM state
static release_aa_fsm_state_e m_release_state;

int AA_Establish(aa_type_e aa_type, uint32_t invocation_counter,
                  operation_result_cb cb)
{
    if (cb == NULL)
    {
        LOGE("AA establishment callback cannot be null");
        return DLMS_ERROR_CODE_UNKNOWN;
    }

    // if mp_param is already allocated, then this is a case of reentrency
    if (mp_param)
    {
        LOGE("Trying to re-enter in AA establishment FSM");
        return DLMS_ERROR_CODE_UNKNOWN;
    }

    mp_param = gxcalloc(1, sizeof *mp_param);
    // Check that we are not out of memory
    if (! mp_param)
    {
        LOGE("No memory to allocate AA establishment param");
        return DLMS_ERROR_CODE_OUTOFMEMORY;
    }

    // Initialize the FSM state
    m_establish_state = AA_ESTABLISHMENT_INIT;
    mp_param->result = DLMS_ERROR_CODE_OK;
    mp_param->cb = cb;
    mp_param->aa_type = aa_type;
    mp_param->invocation_counter = invocation_counter;

    // Start the task ASAP
    RESCHEDULE_ASAP(establish_aa_fsm);

    return DLMS_ERROR_CODE_OK;
}

int AA_Release(operation_result_cb cb)
{
    const dlmsSettings * ms_p = Common_getMeterSettings();

    if (cb == NULL)
    {
        LOGE("AA release callback cannot be null");
        return DLMS_ERROR_CODE_UNKNOWN;
    }

    // if mp_param is already allocated, then this is a case of reentrency
    if (mp_param)
    {
        LOGE("Trying to re-enter in AA release FSM");
        return DLMS_ERROR_CODE_UNKNOWN;
    }
    mp_param = gxcalloc(1, sizeof *mp_param);
    // Check that we are not out of memory
    if (! mp_param)
    {
        LOGE("No memory to allocate AA release param");
        return DLMS_ERROR_CODE_OUTOFMEMORY;
    }

    // Initialize the FSM state
    if (ms_p->interfaceType == DLMS_INTERFACE_TYPE_HDLC)
    {
        m_release_state = AA_RELEASE_STATE_DISC;
    }
    else
    {
        m_release_state = AA_RELEASE_STATE_RELEASE;
    }
    mp_param->result = DLMS_ERROR_CODE_OK;
    mp_param->cb = cb;

    // Start the task ASAP
    RESCHEDULE_ASAP(release_aa_fsm);

    return DLMS_ERROR_CODE_OK;
}

/* *************************************** */
/* AA establishment FSM & helpers          */
/* ** */
static void snrm_result_cb(int32_t result)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    message * msg_p = &mp_param->msg;
    gxReplyData * reply_p = &mp_param->reply;

    mp_param->result = result;

    if (DLMS_ERROR_CODE_OK != mp_param->result  ||
        DLMS_ERROR_CODE_OK != (mp_param->result =
                            cl_parseUAResponse(ms_p, &reply_p->data)))
    {
        LOGE("SNRM failed: %u", mp_param->result);
        m_establish_state = AA_ESTABLISHMENT_EXIT;
    }
    else
    {
        m_establish_state = AA_ESTABLISHMENT_AARQ;
    }
    mes_clear(msg_p);
    reply_clear(reply_p);

    // Start the task ASAP
    RESCHEDULE_ASAP(establish_aa_fsm);
}

static void aarq_result_cb(int32_t result)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    message * msg_p = &mp_param->msg;
    gxReplyData * reply_p = &mp_param->reply;

    mp_param->result = result;

    if (DLMS_ERROR_CODE_OK != mp_param->result  ||
        DLMS_ERROR_CODE_OK != (mp_param->result =
                            cl_parseAAREResponse(ms_p, &reply_p->data)))
    {
        LOGE("AARQ failed: %u", mp_param->result);
        if (ms_p->interfaceType == DLMS_INTERFACE_TYPE_HDLC)
        {
            m_establish_state = AA_ESTABLISHMENT_DISC;
        }
        else
        {
            m_establish_state = AA_ESTABLISHMENT_EXIT;
        }
    }
    else
    {
        if (ms_p->authentication > DLMS_AUTHENTICATION_LOW)
        {
            m_establish_state = AA_ESTABLISHMENT_HLS;
        }
        else
        {
            m_establish_state = AA_ESTABLISHMENT_EXIT;
        }
    }
    mes_clear(msg_p);
    reply_clear(reply_p);

    // Start the task ASAP
    RESCHEDULE_ASAP(establish_aa_fsm);
}

static void hls_result_cb(int32_t result)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    message * msg_p = &mp_param->msg;
    gxReplyData * reply_p = &mp_param->reply;

    mp_param->result = result;

    if (DLMS_ERROR_CODE_OK != mp_param->result  ||
        DLMS_ERROR_CODE_OK !=  (mp_param->result =
        cl_parseApplicationAssociationResponse(ms_p, &reply_p->data)))
    {
        LOGE("AUTH failed: %u", mp_param->result);
        m_establish_state = AA_ESTABLISHMENT_EXIT;
    }
    else
    {
        m_establish_state = AA_ESTABLISHMENT_EXIT;
    }
    mes_clear(msg_p);
    reply_clear(reply_p);

    // Start the task ASAP
    RESCHEDULE_ASAP(establish_aa_fsm);
}

static void get_address_and_auth_type(uint16_t * srv_addr_p,
                                      uint8_t * cl_addr_p,
                                      DLMS_AUTHENTICATION * auth_p)
{
    *srv_addr_p = SERVER_ADDRESS;

    switch (mp_param->aa_type)
    {
        case APPLICATION_ASSOCIATION_PC:
            *cl_addr_p = PC_CLIENT_ADDRESS;
            *auth_p = DLMS_AUTHENTICATION_NONE;
            break;
        case APPLICATION_ASSOCIATION_MR:
            *cl_addr_p = MR_CLIENT_ADDRESS;
            *auth_p = DLMS_AUTHENTICATION_LOW;
            break;
        case APPLICATION_ASSOCIATION_US:
            *cl_addr_p = US_CLIENT_ADDRESS;
            *auth_p = DLMS_AUTHENTICATION_HIGH;
            break;
        case APPLICATION_ASSOCIATION_FU:
            *cl_addr_p = FU_CLIENT_ADDRESS;
            *auth_p = DLMS_AUTHENTICATION_HIGH;
            break;
    }
}

static gxByteBuffer * generate_dedicated_key(void)
{
#ifdef USE_DEDICATED_KEY
    uint8_t dk[KEY_SIZE];
    gxByteBuffer * dedicated_key_p = NULL;

    // This buffer will be freed by Gurux library in cip_clear
    // which is called in cl_clear (cf exit state of release_aa_fsm)
    // Thus, we can use a variable on the stack to allocate it
    dedicated_key_p = gxcalloc(1, sizeof *dedicated_key_p);
    if (! dedicated_key_p)
    {
        LOGW("No memory to allocate dedicated_key");
        return NULL;
    }
    for (uint8_t i = 0; i < sizeof(dk); i++)
    {
        // The random generator is already initialized in app.c
        dk[i] = Random_get8();
    }
    memset(dedicated_key_p, 0, sizeof(gxByteBuffer));
    bb_set(dedicated_key_p, dk, sizeof(dk));

    return dedicated_key_p;
#else // USE_DEDICATED_KEY
    return NULL;
#endif // USE_DEDICATED_KEY
}

static void setup_security_parameters(void)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    ciphering * cipher_p = &ms_p->cipher;
    const uint8_t * system_title_p = NULL;
    uint8_t * secret_p = NULL;
    uint8_t * enc_key_p = NULL;
    uint8_t * auth_key_p = NULL;
    uint8_t system_title_len = 0;
    uint8_t secret_len = 0;
    uint8_t enc_key_len = 0;
    uint8_t auth_key_len = 0;

    switch (mp_param->aa_type)
    {
        case APPLICATION_ASSOCIATION_PC:
            // Nothing to setup
            return;

        case APPLICATION_ASSOCIATION_MR:
            Security_Material_get_mr_secret(&secret_p, &secret_len);
            break;

        case APPLICATION_ASSOCIATION_US:
            Security_Material_get_us_secret(&secret_p, &secret_len);
            break;

        case APPLICATION_ASSOCIATION_FU:
            Security_Material_get_fu_secret(&secret_p, &secret_len);
            break;
    }

    // Set the password
    bb_clear(&ms_p->password);
    bb_set(&ms_p->password, secret_p, secret_len);

    // Ciphering configuration
#ifdef METER_RETROFIT
    cipher_p->security = DLMS_SECURITY_NONE;
#else
    cipher_p->security = DLMS_SECURITY_AUTHENTICATION_ENCRYPTION;
#endif
    cipher_p->suite = DLMS_SECURITY_SUITE_V0;

    //Allocate space for client challenge.
    BB_ATTACH(ms_p->ctoSChallenge, C2S_CHALLENGE, 0);
    //Allocate space for server challenge.
    BB_ATTACH(ms_p->stoCChallenge, S2C_CHALLENGE, 0);

    // Invocation counter
    cipher_p->invocationCounter = mp_param->invocation_counter;

    // Keys
    Security_Material_get_keys(&enc_key_p, &enc_key_len, &auth_key_p, &auth_key_len);
    bb_clear(&cipher_p->authenticationKey);
    bb_set(&cipher_p->authenticationKey, auth_key_p, auth_key_len);
    bb_clear(&cipher_p->blockCipherKey);
    bb_set(&cipher_p->blockCipherKey, enc_key_p, enc_key_len);

    // System title
    Nic_ST_get(&system_title_p, &system_title_len);
    bb_clear(&cipher_p->systemTitle);
    bb_set(&cipher_p->systemTitle, system_title_p, system_title_len);

    // Dedicated key (session key)
    cipher_p->dedicatedKey = generate_dedicated_key();
}

static void disc_from_estab_result_cb(int32_t result)
{
    message * msg_p = &mp_param->msg;
    gxReplyData * reply_p = &mp_param->reply;

    if (DLMS_ERROR_CODE_OK != result)
    {
        LOGE("Disconnect ko: %u", result);
    }
    reply_clear(reply_p);
    mes_clear(msg_p);
    m_establish_state = AA_ESTABLISHMENT_EXIT;

    // Start the task ASAP
    RESCHEDULE_ASAP(establish_aa_fsm);
}

// Establish connection to the meter.
#define INVALID_ADDRESS     0x00
static uint32_t establish_aa_fsm(void)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    message * msg_p = &mp_param->msg;
    gxReplyData * reply_p = &mp_param->reply;
    // default delay is "reschedule ASAP"
    uint32_t delay = APP_SCHEDULER_SCHEDULE_ASAP;
    uint16_t srv_addr = INVALID_ADDRESS;
    uint8_t cl_addr = INVALID_ADDRESS;
    DLMS_AUTHENTICATION auth = DLMS_AUTHENTICATION_NONE;

    SFSM_ENTRY(m_establish_state);

    switch (m_establish_state)
    {
        case AA_ESTABLISHMENT_INIT:
            LOGI("Connecting %s", aa_to_str[mp_param->aa_type]);
            // Initialize variables
            mes_init(msg_p);
            reply_init(reply_p);

            get_address_and_auth_type(&srv_addr, &cl_addr, &auth);

            cl_init(ms_p, 1, cl_addr, srv_addr, auth, NULL, Common_getNicInterfaceType());

            // Hdlc
            ms_p->maxInfoTX = DLMS_COM_PDU_SIZE;
            ms_p->maxInfoRX = DLMS_COM_PDU_SIZE;
            // Wrapper
            ms_p->maxPduSize = DLMS_COM_PDU_SIZE;

            setup_security_parameters();

            if (ms_p->interfaceType == DLMS_INTERFACE_TYPE_HDLC)
            {
                m_establish_state = AA_ESTABLISHMENT_SNRM;
            }
            else
            {
                m_establish_state = AA_ESTABLISHMENT_AARQ;
            }
            break;

        case AA_ESTABLISHMENT_SNRM:
            // HDLC only
            // Get meter's send and receive buffers size.
            mp_param->result = cl_snrmRequest(ms_p, msg_p);
            if (DLMS_ERROR_CODE_OK == mp_param->result)
            {
                Dlms_Com_call_async_with_timeout(msg_p, reply_p, snrm_result_cb, AA_TIMEOUT_S);
                delay = SFSM_DELAY_PAUSED;
            }
            else
            {
                LOGE("cl_snrmRequest failed: %u", mp_param->result);
                mes_clear(msg_p);
                reply_clear(reply_p);
                m_establish_state = AA_ESTABLISHMENT_EXIT;
            }
            break;

        case AA_ESTABLISHMENT_AARQ:
            mp_param->result = cl_aarqRequest(ms_p, msg_p);
            if (DLMS_ERROR_CODE_OK == mp_param->result)
            {
                Dlms_Com_call_async_with_timeout(msg_p, reply_p, aarq_result_cb, AA_TIMEOUT_S);
                delay = SFSM_DELAY_PAUSED;
            }
            else
            {
                LOGE("cl_aarqRequest failed: %u", mp_param->result);
                mes_clear(msg_p);
                reply_clear(reply_p);
                m_establish_state = AA_ESTABLISHMENT_EXIT;
            }
            break;

        case AA_ESTABLISHMENT_HLS:
            // Get challenge if HLS authentication is used.
            mp_param->result = cl_getApplicationAssociationRequest(ms_p, msg_p);
            if (DLMS_ERROR_CODE_OK == mp_param->result)
            {
                Dlms_Com_call_async_with_timeout(msg_p, reply_p, hls_result_cb, AA_TIMEOUT_S);
                delay = SFSM_DELAY_PAUSED;
            }
            else
            {
                LOGE("cl_getApplicationAssociationRequest failed: %u",
                    mp_param->result);
                mes_clear(msg_p);
                reply_clear(reply_p);
                m_establish_state = AA_ESTABLISHMENT_EXIT;
            }
            break;

        case AA_ESTABLISHMENT_DISC:
            {
                int result;
                mes_init(msg_p);
                reply_init(reply_p);
                // We don't overwrite the result of the failed operation
                result = cl_disconnectRequest(ms_p, msg_p);
                if (DLMS_ERROR_CODE_OK == result)
                {
                    if (msg_p->size == 0)
                    {
                        // Nothing to be done (Wrapper mode probably)
                        LOGI("No disconnect to do");
                        reply_clear(reply_p);
                        mes_clear(msg_p);
                        m_establish_state = AA_ESTABLISHMENT_EXIT;
                    }
                    else
                    {
                        LOGI("Sending Disconnect request");
                        Dlms_Com_call_async_with_timeout(msg_p, reply_p, disc_from_estab_result_cb, AA_TIMEOUT_S);
                        delay = SFSM_DELAY_PAUSED;
                    }
                }
                else
                {
                    LOGE("Disconnect req failed: %u", result);
                    reply_clear(reply_p);
                    mes_clear(msg_p);
                    m_establish_state = AA_ESTABLISHMENT_EXIT;
                }
            }
            break;

        case AA_ESTABLISHMENT_EXIT:
            if (DLMS_ERROR_CODE_OK == mp_param->result)
            {
                LOGI("AA established %s", aa_to_str[mp_param->aa_type]);
                if (ms_p->authentication > DLMS_AUTHENTICATION_LOW)
                {
                    char st[PRINTABLE_SYS_TITLE_MAX_SIZE];
                    char source_st[PRINTABLE_SYS_TITLE_MAX_SIZE];

                    Common_formatSystemTitle(ms_p->cipher.systemTitle.data, st, sizeof(st));
                    Common_formatSystemTitle(ms_p->sourceSystemTitle, source_st, sizeof(source_st));

                    LOGI("SystemTitle: %s, SourceSystemTitle: %s", st, source_st);
                }
            }
            else
            {
                cl_clear(ms_p);

            }
            // Set the connection status, to be populated in NIC status
            // (if connection is not possible, could be a reason to trigger sending of nic status)
            Nic_status_set_aa_state(mp_param->aa_type, DLMS_ERROR_CODE_OK == mp_param->result);
            mp_param->cb(mp_param->result);
            delay = SFSM_DELAY_PAUSED;
            m_establish_state = AA_ESTABLISHMENT_INIT;

            gxfree(mp_param);
            mp_param = NULL;
            break;
    } /* switch() */

    SFSM_EXIT();

    return delay;
}

/* *************************************** */
/* AA release FSM & helpers                */
static void release_result_cb(int32_t result)
{
    message * msg_p = &mp_param->msg;
    gxReplyData * reply_p = &mp_param->reply;

    mp_param->result = result;

    if (DLMS_ERROR_CODE_OK != mp_param->result)
    {
        LOGE("Release ko: %u", mp_param->result);
    }
    reply_clear(reply_p);
    mes_clear(msg_p);
    m_release_state = AA_RELEASE_STATE_DISC;

    // Start the task ASAP
    RESCHEDULE_ASAP(release_aa_fsm);
}

static void disc_result_cb(int32_t result)
{
    message * msg_p = &mp_param->msg;
    gxReplyData * reply_p = &mp_param->reply;

    mp_param->result = result;

    if (DLMS_ERROR_CODE_OK != mp_param->result)
    {
        LOGE("Disconnect ko: %u", mp_param->result);
    }
    reply_clear(reply_p);
    mes_clear(msg_p);
    m_release_state = AA_RELEASE_STATE_EXIT;

    // Start the task ASAP
    RESCHEDULE_ASAP(release_aa_fsm);
}

static uint32_t release_aa_fsm(void)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    message * msg_p = &mp_param->msg;
    gxReplyData * reply_p = &mp_param->reply;
    // default delay is "reschedule ASAP"
    uint32_t delay = APP_SCHEDULER_SCHEDULE_ASAP;

    SFSM_ENTRY(m_release_state);

    switch (m_release_state)
    {
        case AA_RELEASE_STATE_RELEASE:
            // Initialize variables
            mes_init(msg_p);
            reply_init(reply_p);

            LOGI("Releasing AA");
            // Restore the original max PDU size
            // This is mandatory for some meters requesting protected release
            ms_p->maxPduSize = DLMS_COM_PDU_SIZE;
            mp_param->result = cl_releaseRequest2(ms_p, msg_p, USE_PROTECTED_RELEASE);
            if (DLMS_ERROR_CODE_OK == mp_param->result)
            {
                Dlms_Com_call_async_with_timeout(msg_p, reply_p, release_result_cb, AA_TIMEOUT_S);
                delay = SFSM_DELAY_PAUSED;
            }
            else
            {
                LOGE("Release req failed: %u", mp_param->result);
                reply_clear(reply_p);
                mes_clear(msg_p);
                m_release_state = AA_RELEASE_STATE_DISC;
            }
            break;

        case AA_RELEASE_STATE_DISC:
            // Initialize variables
            mes_init(msg_p);
            reply_init(reply_p);

            mp_param->result = cl_disconnectRequest(ms_p, msg_p);
            if (DLMS_ERROR_CODE_OK == mp_param->result)
            {
                if (msg_p->size == 0)
                {
                    // Nothing to be done (Wrapper mode probably)
                    LOGI("No disconnect to do");
                    reply_clear(reply_p);
                    mes_clear(msg_p);
                    m_release_state = AA_RELEASE_STATE_EXIT;
                }
                else
                {
                    LOGI("Sending Disconnect request");
                    Dlms_Com_call_async_with_timeout(msg_p, reply_p, disc_result_cb, AA_TIMEOUT_S);
                    delay = SFSM_DELAY_PAUSED;
                }
            }
            else
            {
                LOGE("Disconnect req failed: %u", mp_param->result);
                reply_clear(reply_p);
                mes_clear(msg_p);
                m_release_state = AA_RELEASE_STATE_EXIT;
            }
            break;

        case AA_RELEASE_STATE_EXIT:
            mp_param->cb(mp_param->result);
            delay = SFSM_DELAY_PAUSED;
            m_release_state = AA_RELEASE_STATE_RELEASE;

            // cleanup before next run
            cl_clear(ms_p);
            gxfree(mp_param);
            mp_param = NULL;
            break;
    } /* switch() */

    SFSM_EXIT();
    return delay;
}
