/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    meter_connection_management.c
 * \brief   functions handling the connection to the smart meters
 */

#include "security_service.h"
#include "meter_clock.h"
#include "common.h"
#include "dlms_lock.h"
#include "dlms_com.h"
#include "meter_connection_management.h"
#include "security_material.h"

// Gurux DLMS includes.
#include "include/cosem.h"
#include "include/client.h"
#include "include/dlmssettings.h"
#include "include/gxmem.h"

#define DEBUG_LOG_MODULE_NAME "SEC_SRV "
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#define RESCHEDULE_ASAP()   \
                App_Scheduler_addTask_execTime(update_security_material_fsm, \
                                               APP_SCHEDULER_SCHEDULE_ASAP,\
                                               SFSM_EXECUTION_TIME_US)
#define ALN_ATTRIBUTE_ID_LLS_SECRET         7
#define ALN_METHOD_ID_CHANGE_HLS_SECRET     2
#define SS_METHOD_ID_KEY_TRANSFER           2

// Association Logical Name OBIS
static obis_code_t c_aln_mr_ln = { 0, 0, 40, 0, 2, 255 };
static obis_code_t c_aln_us_ln = { 0, 0, 40, 0, 3, 255 };
static obis_code_t c_aln_fu_ln = { 0, 0, 40, 0, 5, 255 };

// Security Setup OBIS (US only)
static obis_code_t c_ss_us_ln = { 0, 0, 43, 0, 3, 255 };

static obis_code_t c_clock_ln = { 0, 0, 1, 0, 0, 255};

// Key ids, refer to Security Setup object in EC 62056-6-2 COSEM interface classes
//~ typedef enum
//~ {
    //~ SEC_SRV_KEY_ID_GLOBAL_UNICAST_ENCRYPTION_KEY,
    //~ SEC_SRV_KEY_ID_GLOBAL_BROADCAST_ENCRYPTION_KEY,
    //~ SEC_SRV_KEY_ID_AUTHENTICATION_KEY,
    //~ SEC_SRV_KEY_ID_KEK
//~ } sec_srv_key_id_e;

// Bitfield
#define BIT_MR_SECRET               0x01
#define BIT_US_SECRET               0x02
#define BIT_FU_SECRET               0x04
#define BIT_AUTH_KEY                0x10
#define BIT_ENC_KEY                 0x20
#define BIT_KEK_KEY                 0x40

typedef uint8_t security_type_bitfield_t;

#define LLS_SECRET_MASK             (BIT_MR_SECRET)
#define HLS_SECRET_MASK             (BIT_US_SECRET | BIT_FU_SECRET)
#define KEY_MASK                    (BIT_AUTH_KEY | BIT_ENC_KEY | BIT_KEK_KEY)

#define UPDATE_LLS_SECRET(type)     ((type) & LLS_SECRET_MASK)
#define UPDATE_HLS_SECRET(type)     ((type) & HLS_SECRET_MASK)
#define UPDATE_KEYS(type)           ((type) & KEY_MASK)

// FSM handling the update of the security material in the smart meter
typedef enum
{
    SEC_SRV_STATE_CONNECT,
    SEC_SRV_STATE_UPDATE_CLOCK,
    SEC_SRV_STATE_UPDATE_LLS_SECRET,
    SEC_SRV_STATE_UPDATE_HLS_SECRET,
    SEC_SRV_STATE_UPDATE_KEY,
    SEC_SRV_STATE_DISCONNECT,
    SEC_SRV_STATE_EXIT
} ss_fsmp_param_state_e;

typedef struct {
    gxAssociationLogicalName association;
    gxSecuritySetup security_setup;
    gxByteBuffer bb;
    dlmsVARIANT var;

    message msg;
    gxReplyData reply;
    // Input
    security_type_bitfield_t type;
    const security_material_t * msm_p;
    // FSM
    int32_t result;
    ss_fsmp_param_state_e state;
    // Caller
    operation_result_cb cb;
} ss_param_t;

// Static variables
// FSM parameters
static ss_param_t * mp_param;

// Prototypes
static uint32_t update_security_material_fsm(void);
static void compute_next_state(void);
static void compute_operation_to_perform(void);

bool Security_Service_updateSecurity(operation_result_cb cb)
{
    if (! cb)
    {
        LOGE("Invalid parameters");
        return false;
    }

    // if mp_param is already allocated, then this is a case of reentrency
    if (mp_param)
    {
        LOGE("Reentrency");
        return false;
    }

    mp_param = (ss_param_t *) gxcalloc(1, sizeof(ss_param_t));
    // Check that we are not out of memory
    if (! mp_param)
    {
        LOGE("OOM");
        return false;
    }

    mp_param->result = DLMS_ERROR_CODE_OK;
    mp_param->cb = cb;
    mp_param->type = 0;
    mp_param->msm_p = Security_Material_get_new_material();
    compute_operation_to_perform();

    // Initialize the FSM state
    mp_param->state = SEC_SRV_STATE_CONNECT;

    // Start the task ASAP
    RESCHEDULE_ASAP();

    return true;
}

static void compute_operation_to_perform(void)
{
    const security_material_t * cur_msm_p;
    const security_material_t * new_msm_p;

    new_msm_p = Security_Material_get_new_material();
    cur_msm_p = Security_Material_get_current_material();

    if (new_msm_p->mr_secret_len!= cur_msm_p->mr_secret_len ||
        memcmp(new_msm_p->mr_secret, cur_msm_p->mr_secret, new_msm_p->mr_secret_len))
    {
        mp_param->type |= BIT_MR_SECRET;
    }

    if (new_msm_p->us_secret_len!= cur_msm_p->us_secret_len ||
        memcmp(new_msm_p->us_secret, cur_msm_p->us_secret, new_msm_p->us_secret_len))
    {
        mp_param->type |= BIT_US_SECRET;
    }

    if (new_msm_p->fu_secret_len!= cur_msm_p->fu_secret_len ||
        memcmp(new_msm_p->fu_secret, cur_msm_p->fu_secret, new_msm_p->fu_secret_len))
    {
        mp_param->type |= BIT_FU_SECRET;
    }

    if (memcmp(new_msm_p->encryption_key, cur_msm_p->encryption_key, KEY_SIZE))
    {
        mp_param->type |= BIT_ENC_KEY;
    }

    if (memcmp(new_msm_p->authentication_key, cur_msm_p->authentication_key, KEY_SIZE))
    {
        mp_param->type |= BIT_AUTH_KEY;
    }

    if (memcmp(new_msm_p->key_encryption_key, cur_msm_p->key_encryption_key, KEY_SIZE))
    {
        mp_param->type |= BIT_KEK_KEY;
    }
}

static void compute_next_state(void)
{
    if (UPDATE_LLS_SECRET(mp_param->type))
    {
        mp_param->state = SEC_SRV_STATE_UPDATE_LLS_SECRET;
    }
    else if (UPDATE_HLS_SECRET(mp_param->type))
    {
        mp_param->state = SEC_SRV_STATE_UPDATE_HLS_SECRET;
    }
    else if (UPDATE_KEYS(mp_param->type))
    {
        mp_param->state = SEC_SRV_STATE_UPDATE_KEY;
    }
    else
    {
        mp_param->state = SEC_SRV_STATE_DISCONNECT;
    }
}

static void open_cb(int32_t result)
{
    mp_param->result = result;

    if (DLMS_ERROR_CODE_OK == mp_param->result)
    {
        mp_param->state = SEC_SRV_STATE_UPDATE_CLOCK;
    }
    else
    {
        mp_param->state = SEC_SRV_STATE_EXIT;
    }
    RESCHEDULE_ASAP();
}

static void close_cb(int32_t result)
{
    (void)result;
    mp_param->state = SEC_SRV_STATE_EXIT;
    RESCHEDULE_ASAP();
}

static void init_buffers(void)
{
    mes_init(&mp_param->msg);
    reply_init(&mp_param->reply);
    bb_init(&mp_param->bb);
    var_init(&mp_param->var);
}

static void cleanup_buffers(void)
{
    mes_clear(&mp_param->msg);
    reply_clear(&mp_param->reply);
    bb_clear(&mp_param->bb);
    var_clear(&mp_param->var);
}

static bool add_key(uint8_t * kek_p, uint8_t kek_len, gxByteBuffer * bb_p,
                    DLMS_GLOBAL_KEY_TYPE key_type,
                    const uint8_t * key_p, uint8_t key_len)
{
    gxByteBuffer key_bb;
    gxByteBuffer wrapped_bb;
    bool rc = false;

    bb_init(&key_bb);
    bb_init(&wrapped_bb);

    if (bb_set(&key_bb, key_p, key_len) == DLMS_ERROR_CODE_OK &&
        cip_encryptKey(kek_p, kek_len, &key_bb, &wrapped_bb) ==
                                                        DLMS_ERROR_CODE_OK &&
        cosem_setStructure(bb_p, 2) == DLMS_ERROR_CODE_OK &&
        cosem_setEnum(bb_p, key_type) == DLMS_ERROR_CODE_OK &&
        // AES key wrapped using KEK and cip_encryptKey
        cosem_setOctetString(bb_p, &wrapped_bb) == DLMS_ERROR_CODE_OK)
    {
        rc = true;
    }

    bb_clear(&key_bb);
    bb_clear(&wrapped_bb);

    return rc;
}

/* Function to get no of set bits in binary representation of passed binary no */
static uint32_t count_set_bits(uint32_t n)
{
    uint32_t count = 0;
    while (n) {
        n &= (n - 1);
        count++;
    }
    return count;
}

static bool set_variant_for_key_update(void)
{
    dlmsVARIANT * var_p = &mp_param->var;
    gxByteBuffer * bb_p = &mp_param->bb;
    const security_material_t * msm_p = mp_param->msm_p;
    uint32_t  key_nb = count_set_bits(mp_param->type & KEY_MASK);
    uint8_t * kek_p;
    uint8_t kek_len;

    // Keys: 2. key_transfer (data) method of 5.3.7 Security Setup in
    //      IEC 62056-6-2 COSEM interface classes
    //      data::= array key_transfer_data
    //          key_transfer_data::= structure
    //          {
    //              key_id: enum:
    //                  (0) global unicast encryption key,
    //                  (1) global broadcast encryption key,
    //                  (2) authentication key,
    //                  (3) master key (KEK)
    //              key_wrapped: octet-string
    //          }
    if (! Security_Material_get_current_key_encryption_key(&kek_p, &kek_len))
    {
        return false;
    }

    if (cosem_setArray(bb_p, key_nb) != DLMS_ERROR_CODE_OK)
    {
        return false;
    }

    if ((mp_param->type & BIT_AUTH_KEY) &&
        ! add_key(kek_p, kek_len, bb_p, DLMS_GLOBAL_KEY_TYPE_AUTHENTICATION,
                  msm_p->authentication_key, KEY_SIZE))
    {
        return false;
    }

    if ((mp_param->type & BIT_ENC_KEY) &&
        ! add_key(kek_p, kek_len, bb_p, DLMS_GLOBAL_KEY_TYPE_UNICAST_ENCRYPTION,
                  msm_p->encryption_key, KEY_SIZE))
    {
        return false;
    }

    if ((mp_param->type & BIT_KEK_KEY) &&
        ! add_key(kek_p, kek_len, bb_p, DLMS_GLOBAL_KEY_TYPE_KEK,
                  msm_p->key_encryption_key, KEY_SIZE))
    {
        return false;
    }

    if (var_addBytes(var_p, bb_p->data, bb_p->size) != DLMS_ERROR_CODE_OK)
    {
        return false;
    }

    return true;
}

static void ss_set_clock_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result)
    {
        LOGI("Changing clock successfully");
    }
    else
    {
        // The object may not exist
        LOGE("Failed to change clock: %d", result);
    }
    // Set it to 0 to mark it done, so next time we will not try to set it again
    // Even if it was failing just above as it may fail always
    MeterClock_set_new_meter_clock(0, 0);

    cleanup_buffers();
    compute_next_state();

    RESCHEDULE_ASAP();
}

static void ss_write_lls_secret_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result)
    {
        LOGI("LLS secret changed successfully");
    }
    else
    {
        // The object may not exist
        LOGE("Failed to change LLS secret: %d", result);
    }
    cleanup_buffers();
    mp_param->type &= ~BIT_MR_SECRET;
    compute_next_state();

    RESCHEDULE_ASAP();
}

static void change_hls_secret_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result)
    {
        LOGI("%s HLS secret changed successfully",
            (mp_param->type & BIT_US_SECRET) ? "US" : "FU");
    }
    else
    {
        // The object may not exist
        LOGE("Failed to change %s HLS secret: %d",
            (mp_param->type & BIT_US_SECRET) ? "US" : "FU", result);
    }
    cleanup_buffers();
    // We clean the appropriate HLS bit:
    if (mp_param->type & BIT_US_SECRET)
    {
        mp_param->type &= ~BIT_US_SECRET;
    }
    else if (mp_param->type & BIT_FU_SECRET)
    {
        mp_param->type &= ~BIT_FU_SECRET;
    }
    compute_next_state();

    RESCHEDULE_ASAP();
}

static void key_transfer_method_cb(int32_t result)
{
    if (DLMS_ERROR_CODE_OK == result)
    {
        LOGI("Keys changed successfully");
    }
    else
    {
        // The object may not exist
        LOGE("Failed to change keys: %d", result);
    }
    cleanup_buffers();
    // We clean the key bits:
    mp_param->type &= ~KEY_MASK;
    compute_next_state();

    RESCHEDULE_ASAP();
}

static uint32_t update_security_material_fsm(void)
{
    dlmsSettings * ms_p = Common_getMeterSettings();
    const uint8_t * ln_p = NULL;
    uint32_t delay = SFSM_DELAY_PAUSED;
    const security_material_t * msm_p = mp_param->msm_p;
    dlmsVARIANT * var_p = &mp_param->var;
    gxByteBuffer * bb_p = &mp_param->bb;
    message  * msg_p = &mp_param->msg;
    gxReplyData * reply_p = &mp_param->reply;
    const uint8_t * secret_p = NULL;
    uint32_t secret_len = 0;
    uint32_t new_epoch = 0;
    int16_t new_deviation = 0;
    gxtime t;

    SFSM_ENTRY(mp_param->state);

    // Association LN Object has two options for SECRET change :
    // 1. LLS secret is changed by writing attribute 7
    // 2. HLS secret is changed by calling method 2

    switch (mp_param->state)
    {
        case SEC_SRV_STATE_CONNECT:
            if ((mp_param->result = Meter_Connection_Management_open(open_cb,
                                                                     MCM_AA_US))
                                                        != DLMS_ERROR_CODE_OK)
            {
                mp_param->state = SEC_SRV_STATE_EXIT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;


        case SEC_SRV_STATE_UPDATE_CLOCK:

            if (MeterClock_get_new_meter_clock(&new_epoch, &new_deviation))
            {
                LOGI("Clock must be updated to %u(%d)", new_epoch, new_deviation);
                init_buffers();

                time_initUnix(&t, new_epoch);
                t.deviation = new_deviation;

                // Section 4.6.1 Date and time formats - IEC 62056-6-2:2017
                if (cosem_setDateTimeAsOctetString(bb_p, &t) == DLMS_ERROR_CODE_OK &&
                    var_addOctetString(var_p, bb_p) == DLMS_ERROR_CODE_OK &&
                    cl_writeLN(ms_p, c_clock_ln,
                               DLMS_OBJECT_TYPE_CLOCK,
                               2, var_p, true, msg_p) == DLMS_ERROR_CODE_OK)
                {
                    Dlms_Com_call_async(msg_p, reply_p, ss_set_clock_cb);
                }
                else
                {
                    LOGE("Failed to set clock");
                    cleanup_buffers();
                    compute_next_state();
                    delay = APP_SCHEDULER_SCHEDULE_ASAP;
                }
            }
            else
            {
                LOGD("No clock update");
                compute_next_state();
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case SEC_SRV_STATE_UPDATE_LLS_SECRET:
            // LLS secret = MR association
            init_buffers();

            // LLS: attribute 7 of 5.3.4 Association LN in
            //      IEC 62056-6-2 COSEM interface classes
            //      7. secret octet-string
            if (cosem_setOctetString2(bb_p, msm_p->mr_secret,
                                msm_p->mr_secret_len) == DLMS_ERROR_CODE_OK &&
                var_addOctetString(var_p, bb_p) == DLMS_ERROR_CODE_OK &&
                cl_writeLN(ms_p, c_aln_mr_ln,
                           DLMS_OBJECT_TYPE_ASSOCIATION_LOGICAL_NAME,
                           ALN_ATTRIBUTE_ID_LLS_SECRET, var_p, true, msg_p)
                                                        == DLMS_ERROR_CODE_OK)
            {
                Dlms_Com_call_async(msg_p, reply_p, ss_write_lls_secret_cb);
            }
            else
            {
                LOGW("Failed to set LLS secret");
                cleanup_buffers();
                // We clean the LLS bit:
                mp_param->type &= ~BIT_MR_SECRET;
                compute_next_state();
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case SEC_SRV_STATE_UPDATE_HLS_SECRET:
            // HLS secret US or AA association
            if (mp_param->type & BIT_US_SECRET)
            {
                ln_p = c_aln_us_ln;
                secret_p = (uint8_t *) msm_p->us_secret;
                secret_len = msm_p->us_secret_len;
            }
            else if (mp_param->type & BIT_FU_SECRET)
            {
                ln_p = c_aln_fu_ln;
                secret_p = (uint8_t *) msm_p->fu_secret;
                secret_len = msm_p->fu_secret_len;
            }
            cosem_init2(BASE(mp_param->association),
                        DLMS_OBJECT_TYPE_ASSOCIATION_LOGICAL_NAME, ln_p);
            init_buffers();
            // HLS: change_HLS_secret method of 5.3.4 Association LN in
            //      IEC 62056-6-2 COSEM interface classes
            //      data::= octet-string new HLS secret
            if (bb_set(bb_p, secret_p, secret_len) == DLMS_ERROR_CODE_OK &&
                var_addOctetString(var_p, bb_p) == DLMS_ERROR_CODE_OK &&
                cl_method(ms_p, BASE(mp_param->association),
                          ALN_METHOD_ID_CHANGE_HLS_SECRET, var_p, msg_p)
                                                        == DLMS_ERROR_CODE_OK)
            {
                Dlms_Com_call_async(msg_p, reply_p, change_hls_secret_cb);
            }
            else
            {
                LOGW("Failed to set HLS secret");
                cleanup_buffers();
                // We clean the appropriate HLS bit:
                if (mp_param->type & BIT_US_SECRET)
                {
                    mp_param->type &= ~BIT_US_SECRET;
                }
                else if (mp_param->type & BIT_FU_SECRET)
                {
                    mp_param->type &= ~BIT_FU_SECRET;
                }
                compute_next_state();
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case SEC_SRV_STATE_UPDATE_KEY:
            cosem_init2(BASE(mp_param->security_setup),
                        DLMS_OBJECT_TYPE_SECURITY_SETUP, c_ss_us_ln);
            init_buffers();

            if (set_variant_for_key_update() &&
                cl_method2(ms_p, BASE(mp_param->security_setup),
                          SS_METHOD_ID_KEY_TRANSFER,
                          bb_p->data, bb_p->size, msg_p) == DLMS_ERROR_CODE_OK)
            {
                Dlms_Com_call_async(msg_p, reply_p, key_transfer_method_cb);
            }
            else
            {
                LOGW("Failed to set key");
                cleanup_buffers();
                // We clean the key bits:
                mp_param->type &= ~KEY_MASK;
                compute_next_state();
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

         case SEC_SRV_STATE_DISCONNECT:
            if (Meter_Connection_Management_close(close_cb) !=
                                                            DLMS_ERROR_CODE_OK)
            {
                mp_param->state = SEC_SRV_STATE_EXIT;
                delay = APP_SCHEDULER_SCHEDULE_ASAP;
            }
            break;

        case SEC_SRV_STATE_EXIT:
            mp_param->cb(mp_param->result);

            // cleanup before next run
            gxfree(mp_param);
            mp_param = NULL;
            break;
    } /* switch() */

    SFSM_EXIT();

    return delay;
}
