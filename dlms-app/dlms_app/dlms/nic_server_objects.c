/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */
#include <stdio.h>

#include "nic_server_objects.h"
#include "security_material.h"
#include "common.h"
#include "server_attribute_manager.h"
#include "transparent_mode.h"
#include "meter_clock.h"
#include "fixed_day_billing_profile.h"

#include "include/serverevents.h"
#include "include/server.h"
#include "include/cosem.h"

#define DEBUG_LOG_MODULE_NAME "NIC OBJ "
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"
#include "nic_server.h"

#define LOG_WITH_OBIS(level, obis, fmt, ...) \
do { \
    LOG(level, "[%u.%u.%u.%u.%u.%u]" fmt, obis[0], obis[1], obis[2], obis[3], obis[4], obis[5], ##__VA_ARGS__); \
} while (0)

typedef bool (* security_material_store_f) (const uint8_t * key_p, uint8_t key_len);

// Initial Value size used in AES key wrapping
// Refer to https://www.ietf.org/rfc/rfc3394.txt
#define IV_SIZE                 8
#define WRAPPED_KEY_SIZE        (KEY_SIZE + IV_SIZE)

// Limits for the instantaneous period
#define INSTANTANEOUS_MINIMUN_PERIOD_MN   15
#define INSTANTANEOUS_MAXIMUM_PERIOD_MN   1440

typedef enum
{
    OBJ_PC_ASSOCIATION_LN,
    OBJ_US_ASSOCIATION_LN,
    OBJ_US_SECURITY_SETUP,
    OBJ_CLOCK,
    OBJ_US_INVOCATION_COUNTER,
    OBJ_MR_ASSOCIATION_SECRET,
    OBJ_US_ASSOCIATION_SECRET,
    OBJ_FU_ASSOCIATION_SECRET,
    OBJ_WRAPPED_ENCRYPTION_KEY,
    OBJ_WRAPPED_AUTHENTICATION_KEY,
    OBJ_WRAPPED_KEY_ENCRYPTION_KEY,
    OBJ_INSTANTANEOUS_PERIOD_CONFIG,
    OBJ_PUSH_ENABLE_CONFIG,
    OBJ_TRANSPARENT_MODE_CONFIG,
#ifdef METER_REGISTRATION_ENABLED
    OBJ_NIC_REGISTRATION_STATUS,
#endif
#ifdef FIXED_DAY_BILLING_ENABLED
    OBJ_FIXED_DAY_BILLING_DAY,
#endif
    OBJ_NB
} obj_list_e;

typedef int (* add_object_f) (gxObject * obj_p, uint32_t descriptor_index);

typedef struct
{
    // Object
    obj_list_e obj;
    // Object type
    DLMS_OBJECT_TYPE type;
    // Logical name
    obis_code_t ln;
    // Object presence in PC AA
    bool pc_presence;
    // Object presence in US AA
    bool us_presence;
    // Callback to add object
    add_object_f add_object;
} obj_descriptor_t;

// Prototypes
static int add_pc_association_ln(gxObject * obj_p, uint32_t descriptor_index);
static int add_us_association_ln(gxObject * obj_p, uint32_t descriptor_index);
static int add_us_security_setup(gxObject * obj_p, uint32_t descriptor_index);
static int add_transparent_cfg_object(gxObject * obj_p, uint32_t descriptor_index);
static int add_clock(gxObject * obj_p, uint32_t descriptor_index);
static int add_invocation_counter(gxObject * obj_p, uint32_t descriptor_index);
static int add_instantaneous_period_cfg_object(gxObject * obj_p, uint32_t descriptor_index);
static int add_push_enable_cfg_object(gxObject * obj_p, uint32_t descriptor_index);
#ifdef METER_REGISTRATION_ENABLED
static int add_nic_registration_status_object(gxObject * obj_p, uint32_t descriptor_index);
#endif
#ifdef FIXED_DAY_BILLING_ENABLED
static int add_fixed_day_billing_day_object(gxObject * obj_p, uint32_t descriptor_index);
#endif
static int add_data_object(gxObject * obj_p, uint32_t descriptor_index);

static const obj_descriptor_t nic_server_objects[OBJ_NB] =
{
    {
        OBJ_PC_ASSOCIATION_LN,
        DLMS_OBJECT_TYPE_ASSOCIATION_LOGICAL_NAME,
        { 0, 0, 40, 0, 101, 255 },
        true,
        true,
        add_pc_association_ln
    },
    {
        OBJ_US_ASSOCIATION_LN,
        DLMS_OBJECT_TYPE_ASSOCIATION_LOGICAL_NAME,
        { 0, 0, 40, 0, 103, 255 },
        false,
        true,
        add_us_association_ln
    },
    {
        OBJ_US_SECURITY_SETUP,
        DLMS_OBJECT_TYPE_SECURITY_SETUP,
        { 0, 0, 43, 0, 103, 255 },
        false,
        true,
        add_us_security_setup
    },
    {
        OBJ_CLOCK,
        DLMS_OBJECT_TYPE_CLOCK,
        { 0, 0, 1, 0, 0, 255 },
        false,
        true, // Only attribute 2 (time) is readable
        add_clock
    },
    {
        OBJ_US_INVOCATION_COUNTER,
        DLMS_OBJECT_TYPE_DATA,
        { 0, 0, 43, 1, 3, 255 },
        true,
        true,
        add_invocation_counter
    },
    {
        OBJ_MR_ASSOCIATION_SECRET,
        DLMS_OBJECT_TYPE_DATA,
        { 0, 0, 40, 0, 2, 250 },
        false,
        true,
        add_data_object
    },
    {
        OBJ_US_ASSOCIATION_SECRET,
        DLMS_OBJECT_TYPE_DATA,
        { 0, 0, 40, 0, 3, 250 },
        false,
        true,
        add_data_object
    },
    {
        OBJ_FU_ASSOCIATION_SECRET,
        DLMS_OBJECT_TYPE_DATA,
        { 0, 0, 40, 0, 5, 250 },
        false,
        true,
        add_data_object
    },
    {
        OBJ_WRAPPED_ENCRYPTION_KEY,
        DLMS_OBJECT_TYPE_DATA,
        { 0, 0, 43, 0, 0, 251 },
        false,
        true,
        add_data_object
    },
    {
        OBJ_WRAPPED_AUTHENTICATION_KEY,
        DLMS_OBJECT_TYPE_DATA,
        { 0, 0, 43, 0, 0, 253 },
        false,
        true,
        add_data_object
    },
    {
        OBJ_WRAPPED_KEY_ENCRYPTION_KEY,
        DLMS_OBJECT_TYPE_DATA,
        { 0, 0, 43, 0, 0, 254 },
        false,
        true,
        add_data_object
    },
    {
        OBJ_INSTANTANEOUS_PERIOD_CONFIG,
        DLMS_OBJECT_TYPE_DATA,
        { 0, 100, 25, 9, 0, 250 },
        true,
        true,
        add_instantaneous_period_cfg_object
    },
    {
        OBJ_PUSH_ENABLE_CONFIG,
        DLMS_OBJECT_TYPE_DATA,
        { 0, 101, 25, 9, 0, 250 },
        true,
        true,
        add_push_enable_cfg_object

    },
    {
        OBJ_TRANSPARENT_MODE_CONFIG,
        DLMS_OBJECT_TYPE_DATA,
        { 0, 200, 25, 9, 0, 250 },
        false,
        true,
        add_transparent_cfg_object
    },
#ifdef METER_REGISTRATION_ENABLED
    {
        OBJ_NIC_REGISTRATION_STATUS,
        DLMS_OBJECT_TYPE_DATA,
        { 0, 0, 96, 0, 1, 255 },
        false,
        true,
        add_nic_registration_status_object
    },
#endif
#ifdef FIXED_DAY_BILLING_ENABLED
    {
        OBJ_FIXED_DAY_BILLING_DAY,
        DLMS_OBJECT_TYPE_DATA,
        { 0, 102, 25, 9, 0, 250 },
        false,
        true,
        add_fixed_day_billing_day_object
    },
#endif
};

typedef struct
{
    // The list of available objects is rebuilt for each association
    gxObject * current_objects[OBJ_NB];
    uint32_t current_objects_index[OBJ_NB];
    uint32_t obj_nb;

    uint8_t client_system_title[SYSTEM_TITLE_SIZE];
    uint8_t server_system_title[SYSTEM_TITLE_SIZE];

    dlmsServerSettings * server_settings;

    uint32_t * invocation_counter_p;
} srvobj_param_t;

static srvobj_param_t * mp_srvobj_param;

static obj_list_e find_object(const gxObject * obj_p)
{
    for (uint32_t i = 0; i < OBJ_NB; i++)
    {
        if (! mp_srvobj_param->current_objects[i])
        {
           break;
        }

        if (obj_p == mp_srvobj_param->current_objects[i])
        {
            return nic_server_objects[mp_srvobj_param->current_objects_index[i]].obj;
        }
    }
    return OBJ_NB;
}

static int add_pc_association_ln(gxObject * obj_p, uint32_t descriptor_index)
{
    const obj_descriptor_t * desc_p = &nic_server_objects[descriptor_index];
    gxAssociationLogicalName * pc_aln_p = (gxAssociationLogicalName *) obj_p;
    int ret;

    if ((ret = INIT_OBJECT((*pc_aln_p), desc_p->type, desc_p->ln)) == 0)
    {
        // Attach the list of visible objects in PC AA
        oa_attach(&pc_aln_p->objectList, mp_srvobj_param->current_objects, mp_srvobj_param->obj_nb);
        pc_aln_p->authenticationMechanismName.mechanismId = DLMS_AUTHENTICATION_NONE;
        pc_aln_p->clientSAP = PC_CLIENT_ADDRESS;
        pc_aln_p->xDLMSContextInfo.maxSendPduSize = PDU_BUFFER_SIZE;
        pc_aln_p->xDLMSContextInfo.maxReceivePduSize = PDU_BUFFER_SIZE;
        pc_aln_p->xDLMSContextInfo.conformance = (DLMS_CONFORMANCE)(DLMS_CONFORMANCE_GET | DLMS_CONFORMANCE_SET);
    }
    return ret;
}

static gxSecuritySetup * get_us_security_setup(void)
{
    gxSecuritySetup * sec_setup_p = NULL;

    for (uint32_t i = 0; i < OBJ_NB; i++)
    {
        gxObject * obj_p = mp_srvobj_param->current_objects[i];
        uint32_t idx = mp_srvobj_param->current_objects_index[i];
        const obj_descriptor_t * desc_p = &nic_server_objects[idx];
        if (! obj_p)
        {
            break;
        }

        if (desc_p->obj == OBJ_US_SECURITY_SETUP)
        {
            sec_setup_p = (gxSecuritySetup *) obj_p;
            break;
        }
    }
    return sec_setup_p;
}

///////////////////////////////////////////////////////////////////////
//This method adds example Logical Name Association object for High authentication.
// UA in Indian standard.
///////////////////////////////////////////////////////////////////////
static int add_us_association_ln(gxObject * obj_p, uint32_t descriptor_index)
{
    const obj_descriptor_t * desc_p = &nic_server_objects[descriptor_index];
    gxAssociationLogicalName * us_aln_p = (gxAssociationLogicalName *) obj_p;
    gxSecuritySetup * sec_setup_p = NULL;
    // There is no copy, so pointer will point to the storage directly
    uint8_t * us_secret_p;
    uint8_t us_secret_len;
    // Note that Only the clientSAP is actually used */
    int ret;

    Security_Material_get_us_secret(&us_secret_p, &us_secret_len);
    if (! (sec_setup_p = get_us_security_setup()))
    {
        LOGE("Missing security setup object for US association");
        return DLMS_ERROR_CODE_FALSE;
    }

    if ((ret = INIT_OBJECT((*us_aln_p), desc_p->type, desc_p->ln)) == 0)
    {
        us_aln_p->authenticationMechanismName.mechanismId = DLMS_AUTHENTICATION_HIGH;
        oa_attach(&us_aln_p->objectList, mp_srvobj_param->current_objects, mp_srvobj_param->obj_nb);
        us_aln_p->clientSAP = US_CLIENT_ADDRESS;
        us_aln_p->xDLMSContextInfo.maxSendPduSize = PDU_BUFFER_SIZE;
        us_aln_p->xDLMSContextInfo.maxReceivePduSize = PDU_BUFFER_SIZE;
        us_aln_p->xDLMSContextInfo.conformance = (DLMS_CONFORMANCE)(DLMS_CONFORMANCE_BLOCK_TRANSFER_WITH_ACTION |
            DLMS_CONFORMANCE_BLOCK_TRANSFER_WITH_SET_OR_WRITE |
            DLMS_CONFORMANCE_BLOCK_TRANSFER_WITH_GET_OR_READ |
            DLMS_CONFORMANCE_SET |
            DLMS_CONFORMANCE_SELECTIVE_ACCESS |
            DLMS_CONFORMANCE_ACTION |
            DLMS_CONFORMANCE_MULTIPLE_REFERENCES |
            DLMS_CONFORMANCE_GET);
        bb_attach(&us_aln_p->secret, us_secret_p, us_secret_len, us_secret_len);

        us_aln_p->securitySetup = sec_setup_p;
    }
    return ret;
}

///////////////////////////////////////////////////////////////////////
//This method adds security setup object for High authentication.
///////////////////////////////////////////////////////////////////////
static int add_us_security_setup(gxObject * obj_p, uint32_t descriptor_index)
{
    const obj_descriptor_t * desc_p = &nic_server_objects[descriptor_index];
    gxSecuritySetup * us_ss_p = (gxSecuritySetup *) obj_p;
    int ret;

    if ((ret = INIT_OBJECT((*us_ss_p), desc_p->type, desc_p->ln)) == 0)
    {
        BB_ATTACH(us_ss_p->serverSystemTitle,
                  mp_srvobj_param->server_system_title, SYSTEM_TITLE_SIZE);
        BB_ATTACH(us_ss_p->clientSystemTitle,
                  mp_srvobj_param->client_system_title, SYSTEM_TITLE_SIZE);

        //Only Authenticated encrypted connections are allowed.
        us_ss_p->securityPolicy = DLMS_SECURITY_POLICY_AUTHENTICATED_ENCRYPTED;
        us_ss_p->securitySuite = DLMS_SECURITY_SUITE_V0;
    }
    return ret;
}

static int add_transparent_cfg_object(gxObject * obj_p, uint32_t descriptor_index)
{
    gxData * data_p = (gxData *) obj_p;
    int ret = 0;

    if ((ret = add_data_object(obj_p, descriptor_index)) == 0)
    {
        // Load initial value
        var_setBoolean(&data_p->value, Transparent_mode_is_enabled());
    }
    return ret;
}

// Add clock object
static int add_clock(gxObject * obj_p, uint32_t descriptor_index)
{
    const obj_descriptor_t * desc_p = &nic_server_objects[descriptor_index];
    gxClock * clock_p = (gxClock *) obj_p;
    int ret = 0;

    if ((ret = INIT_OBJECT((*clock_p), desc_p->type, desc_p->ln)) == 0)
    {
        // TODO: is it useful to initialize this clock object?
        //~ //Set default values.
        //~ time_init(&meterData.clock1.begin, -1, 3, 0, 0, 0, 0, 0, 0);
        //~ time_init(&meterData.clock1.end, -1, 9, 0, 0, 0, 0, 0, 0);
        //~ //Meter is using UTC time zone.
        //~ meterData.clock1.timeZone = 0;
        //~ //Deviation is 60 minutes.
        //~ meterData.clock1.deviation = 60;
        //~ meterData.clock1.clockBase = DLMS_CLOCK_BASE_FREQUENCY_50;
    }
    return ret;
}

// Add invocation counter object
static int add_invocation_counter(gxObject * obj_p, uint32_t descriptor_index)
{
    const obj_descriptor_t * desc_p = &nic_server_objects[descriptor_index];
    gxData * ic_p = (gxData *) obj_p;
    int ret;

    if ((ret = INIT_OBJECT((*ic_p), desc_p->type, desc_p->ln)) == 0)
    {
        // Initial invocation counter value.
        // TODO: fix reference
        ic_p->value.vt = (DLMS_DATA_TYPE) (DLMS_DATA_TYPE_BYREF | DLMS_DATA_TYPE_UINT32);
        ic_p->value.pulVal = mp_srvobj_param->invocation_counter_p;
    }

    return ret;
}

// Generic function to add an object of type data
static int add_data_object(gxObject * obj_p, uint32_t descriptor_index)
{
    const obj_descriptor_t * desc_p = &nic_server_objects[descriptor_index];
    gxData * data_p = (gxData *) obj_p;
    int ret;

    if ((ret = INIT_OBJECT((*data_p), desc_p->type, desc_p->ln)) != 0)
    {
        LOGE("Cannot add %d type object: %d", desc_p->type, ret);
    }
    return ret;
}

static int add_instantaneous_period_cfg_object(gxObject * obj_p, uint32_t descriptor_index)
{
    gxData * data_p = (gxData *) obj_p;
    int ret = 0;
    uint16_t period;

    if ((ret = add_data_object(obj_p, descriptor_index)) == 0)
    {
        // Load initial value
        Server_Attribute_Manager_readInstantaneousPushConfig(&period);
        var_setUInt16(&data_p->value, period);
    }
    return ret;
}

static int add_push_enable_cfg_object(gxObject * obj_p, uint32_t descriptor_index)
{
    gxData * data_p = (gxData *) obj_p;
    dlmsVARIANT * var_p = &data_p->value;
    int ret = 0;

    if ((ret = add_data_object(obj_p, descriptor_index)) == 0)
    {
        uint16_t cfg;
        uint8_t bcfg[PROF_PUSH_CFG_BYTE_SIZE];

        // Load initial value
        Server_Attribute_Manager_readProfilePushConfig(&cfg);
        bcfg[0] = cfg >> 8;
        bcfg[1] = cfg & 0xFF;

        var_p->vt = DLMS_DATA_TYPE_BIT_STRING;
        // This buffer will be freed by Gurux when the server is stopped
        var_p->bitArr = gxcalloc(1, sizeof *var_p->bitArr);
        if (var_p->bitArr)
        {
            ba_init(var_p->bitArr);
            ba_copy(var_p->bitArr, bcfg, PROF_PUSH_BIT_NB);
        }
    }
    return ret;
}

#ifdef METER_REGISTRATION_ENABLED
static int add_nic_registration_status_object(gxObject * obj_p, uint32_t descriptor_index)
{
    gxData * data_p = (gxData *) obj_p;
    int ret = 0;

    if ((ret = add_data_object(obj_p, descriptor_index)) == 0)
    {
        bool registered;

        // Load initial value
        Server_Attribute_Manager_readNicRegistrationStatus(&registered);
        var_setBoolean(&data_p->value, registered);
    }
    return ret;
}
#endif

#ifdef FIXED_DAY_BILLING_ENABLED
static int add_fixed_day_billing_day_object(gxObject * obj_p, uint32_t descriptor_index)
{
    gxData * data_p = (gxData *) obj_p;
    int ret = 0;

    if ((ret = add_data_object(obj_p, descriptor_index)) == 0)
    {
        uint32_t unused;
        uint8_t billing_day;

        // Load initial value
        Server_Attribute_readFixedDayBillingInfo(&billing_day, &unused);
        var_setUInt8(&data_p->value, billing_day);
    }
    return ret;
}
#endif

static gxObject * allocate_object(uint32_t index)
{
    const obj_descriptor_t * desc = &nic_server_objects[index];
    uint32_t size = 0;

    switch (desc->type)
    {
        case DLMS_OBJECT_TYPE_DATA:
            size = sizeof(gxData);
            break;

        case DLMS_OBJECT_TYPE_ASSOCIATION_LOGICAL_NAME:
            size = sizeof(gxAssociationLogicalName);
            break;

        case DLMS_OBJECT_TYPE_SECURITY_SETUP:
            size = sizeof(gxSecuritySetup);
            break;

        case DLMS_OBJECT_TYPE_CLOCK:
            size = sizeof(gxClock);
            break;

        default:
            LOGE("allocating unsupported object type %d", desc->type);
            break;
    }

    if (size)
    {
        return gxcalloc(1, size);
    }
    return NULL;
}

// Create objects and load values
bool Dlms_Server_createObjects(dlmsServerSettings* settings, mcm_aa_e aa_type,
                               uint32_t * invocation_counter_p)
{
    int ret;
    uint32_t i;

    if (! (mp_srvobj_param = gxcalloc(1, sizeof(srvobj_param_t))))
    {
        return false;
    }

    mp_srvobj_param->server_settings = settings;
    mp_srvobj_param->invocation_counter_p = invocation_counter_p;
    mp_srvobj_param->obj_nb = 0;

    for (i = 0; i < OBJ_NB; i++)
    {
        const obj_descriptor_t * desc = &nic_server_objects[i];
        if ((aa_type == MCM_AA_PC && desc->pc_presence) ||
            (aa_type == MCM_AA_US && desc->us_presence))
        {
            // Allocate memory for this type of object
            mp_srvobj_param->current_objects[mp_srvobj_param->obj_nb] = allocate_object(i);
            if (! mp_srvobj_param->current_objects[mp_srvobj_param->obj_nb])
            {
                LOGE("malloc failed");
                Dlms_Server_deleteObjects();
                return false;
            }
            // Store the index of the object in the table
            mp_srvobj_param->current_objects_index[mp_srvobj_param->obj_nb] = i;
            mp_srvobj_param->obj_nb++;
        }
    }

    LOGD("AA type %d => %d objects", aa_type, mp_srvobj_param->obj_nb);
    oa_attach(&settings->base.objects, mp_srvobj_param->current_objects, mp_srvobj_param->obj_nb);

    for (i = 0; i < mp_srvobj_param->obj_nb; i++)
    {
        gxObject * obj_p = mp_srvobj_param->current_objects[i];
        uint32_t idx = mp_srvobj_param->current_objects_index[i];
        const obj_descriptor_t * desc_p = &nic_server_objects[idx];
        if (0 != (ret = desc_p->add_object(obj_p, idx)))
        {
            LOGE("Failed to add NIC objects: %d", ret);
            Dlms_Server_deleteObjects();
            return false;
        }
    }

    if ((ret = oa_verify(&settings->base.objects)) != 0 ||
        (ret = svr_initialize(settings)) != 0)
    {
        LOGE("Failed to start the meter: %d", ret);
        Dlms_Server_deleteObjects();
        (void) ret;
        return false;
    }

    LOGI("Meter started");
    return true;
}

void Dlms_Server_deleteObjects()
{
    if (mp_srvobj_param)
    {
        for (uint32_t i = 0; i < OBJ_NB; i++)
        {
            gxObject * obj_p = mp_srvobj_param->current_objects[i];
            if (obj_p)
            {
                gxfree(obj_p);
            }
        }
        gxfree(mp_srvobj_param);
        mp_srvobj_param = NULL;
    }
}

/* ************************************ */
/* SERVER CALLBACKS                     */

//Get attribute access level for Push Setup.
static DLMS_ACCESS_MODE getPushSetupAttributeAccess(
    const dlmsSettings* settings,
    unsigned char index)
{
    //Write is allowed only for High authentication.
    if (settings->authentication > DLMS_AUTHENTICATION_LOW)
    {
        switch (index)
        {
        case 2://pushObjectList
        case 4://communicationWindow
            return DLMS_ACCESS_MODE_READ_WRITE;
        default:
            break;
        }
    }
    return DLMS_ACCESS_MODE_READ;
}

//Get attribute access level for register.
static DLMS_ACCESS_MODE getRegisterAttributeAccess(
    dlmsSettings* settings,
    unsigned char index)
{
    return DLMS_ACCESS_MODE_READ;
}

//Get attribute access level for data objects.
static DLMS_ACCESS_MODE getDataAttributeAccess(
    dlmsSettings* settings,
    unsigned char index)
{
    return DLMS_ACCESS_MODE_READ_WRITE;
}

//Get attribute access level for association LN.
static DLMS_ACCESS_MODE getAssociationAttributeAccess(
    const dlmsSettings* settings,
    unsigned char index)
{
    //If secret
    if (settings->authentication == DLMS_AUTHENTICATION_LOW && index == 7)
    {
        return DLMS_ACCESS_MODE_READ_WRITE;
    }
    return DLMS_ACCESS_MODE_READ;
}

//Get attribute access level for security setup.
static DLMS_ACCESS_MODE getSecuritySetupAttributeAccess(
    dlmsSettings* settings,
    unsigned char index)
{
    return DLMS_ACCESS_MODE_READ;
}

//Get attribute access for clock object.
static DLMS_ACCESS_MODE getClockAttributeAccess(
    dlmsSettings* settings,
    unsigned char index)
{

    if (index == 2)
    {
        return DLMS_ACCESS_MODE_WRITE;
    }
    return DLMS_ACCESS_MODE_NONE;
}

/**
 * Get attribute access level.
 */
// This function prototype is in a lib, we dont want to change its signature.
DLMS_ACCESS_MODE svr_getAttributeAccess(
        dlmsSettings* settings,
// cppcheck-suppress constParameterPointer
        gxObject* obj,
        unsigned char index)
{
    LOGD("svr_getAttributeAccess type:%d idx:%d", obj->objectType, index);

    if (obj->objectType == DLMS_OBJECT_TYPE_ASSOCIATION_LOGICAL_NAME)
    {
        return getAssociationAttributeAccess(settings, index);
    }

    // Exposed data object in PC AA are readable
    if (obj->objectType == DLMS_OBJECT_TYPE_DATA && index == 2 &&
        settings->authentication == DLMS_AUTHENTICATION_NONE)
    {
        return DLMS_ACCESS_MODE_READ;
    }

    /* check that the command is encrypted */
    if (!mp_srvobj_param->server_settings->info.encryptedCommand) {
        LOGE("Access without security");
        return DLMS_ACCESS_MODE_NONE;
    }

    // Only read is allowed if authentication is not used.
    if (index == 1 || settings->authentication == DLMS_AUTHENTICATION_NONE)
    {
        return DLMS_ACCESS_MODE_READ;
    }
    if (obj->objectType == DLMS_OBJECT_TYPE_ASSOCIATION_LOGICAL_NAME)
    {
        return getAssociationAttributeAccess(settings, index);
    }
    if (obj->objectType == DLMS_OBJECT_TYPE_PUSH_SETUP)
    {
        return getPushSetupAttributeAccess(settings, index);
    }
    if (obj->objectType == DLMS_OBJECT_TYPE_REGISTER)
    {
        return getRegisterAttributeAccess(settings, index);
    }
    if (obj->objectType == DLMS_OBJECT_TYPE_DATA)
    {
        return getDataAttributeAccess(settings, index);
    }
    if (obj->objectType == DLMS_OBJECT_TYPE_SECURITY_SETUP)
    {
        return getSecuritySetupAttributeAccess(settings, index);
    }
    if (obj->objectType == DLMS_OBJECT_TYPE_CLOCK)
    {
        return getClockAttributeAccess(settings, index);
    }

    // If not in one of the case above, no access!
    return DLMS_ACCESS_MODE_NONE;
}


/**
 * Get method access level.
 */
// This function prototype is in a lib, we dont want to change its signature.
DLMS_METHOD_ACCESS_MODE svr_getMethodAccess(
// cppcheck-suppress constParameterPointer
        dlmsSettings* settings,
        gxObject* obj,
        unsigned char index)
{
    LOGD("svr_getMethodAccess type:%d idx:%d", obj->objectType, index);

    // Methods are not allowed.
    if (settings->authentication == DLMS_AUTHENTICATION_NONE ||
        settings->authentication == DLMS_AUTHENTICATION_LOW)
    {
        return DLMS_METHOD_ACCESS_MODE_NONE;
    }
    return DLMS_METHOD_ACCESS_MODE_ACCESS;
}


/**
 * Read selected item(s).
 *
 * @param args
 *            Handled read requests.
 */
void svr_preRead(
        dlmsSettings* settings,
        gxValueEventCollection* args)
{
    gxValueEventArg* e;
    int pos;
    DLMS_OBJECT_TYPE type;
    for (pos = 0; pos != args->size; ++pos)
    {
        if (vec_getByIndex(args, pos, &e) != 0)
        {
            return;
        }

        LOG_WITH_OBIS(LVL_DEBUG, e->target->logicalName, "preRead type = %d",  e->target->objectType);

        //Let framework handle Logical Name read.
        if (e->index == 1)
        {
            continue;
        }

        //Get target type.
        type = (DLMS_OBJECT_TYPE)e->target->objectType;
        //Let Framework will handle Association objects and profile generic automatically.
        if (type == DLMS_OBJECT_TYPE_ASSOCIATION_LOGICAL_NAME ||
            type == DLMS_OBJECT_TYPE_ASSOCIATION_SHORT_NAME)
        {
            continue;
        }

        // We need to fix the reading of the invocation counter by ourselves
        // because of Gurux inconsistent checks of the invocation counter
        if ((find_object(e->target)) == OBJ_US_INVOCATION_COUNTER &&
            e->index == 2)
        {
            gxData * ic_p = (gxData *) e->target;
            e->value.ulVal = *ic_p->value.pulVal + 1;
            e->value.vt = DLMS_DATA_TYPE_UINT32;
            e->handled = 1;
        }
    }
}

typedef bool (* store_security_material_f) (gxByteBuffer * data_p,
                                            security_material_store_f store_fn,
                                            const char * secret_name_p);

static bool store_secret(gxByteBuffer * data_p,
                         security_material_store_f store_fn,
                         const char * secret_name_p)
{
    if (data_p->size > SECRET_MAX_SIZE)
    {
        LOGE("Attribute length too big (%d > %d)",
            data_p->size, SECRET_MAX_SIZE);
        return false;
    }

    if (! store_fn(data_p->data, data_p->size))
    {
        LOGE("failed to store %s secret", secret_name_p);
        return false;
    }
    LOGD("%s secret updated", secret_name_p);

    return true;
}

static bool unwrap_key(gxByteBuffer * wrapped_bb_p, gxByteBuffer * key_bb_p,
                       const char * key_name_p)
{
    uint8_t * kek_p;
    uint8_t kek_len;
    bool rc = false;

    if (wrapped_bb_p->size != WRAPPED_KEY_SIZE)
    {
        LOGE("Attribute length not matching (%d != %d)",
            wrapped_bb_p->size, WRAPPED_KEY_SIZE);
    }
    else if (! Security_Material_get_current_key_encryption_key(&kek_p,
                                                                &kek_len))
    {
        LOGE("Failed to get the current KEK key");

    }
    else if (cip_decryptKey(kek_p, kek_len, wrapped_bb_p, key_bb_p) !=
                                                            DLMS_ERROR_CODE_OK)
    {
        LOGE("Failed to unwrap %s key", key_name_p);
    }
    else
    {
        rc = true;
    }

    return rc;
}

static bool store_key(gxByteBuffer * wrapped_bb_p,
                      security_material_store_f store_fn,
                      const char * key_name_p)
{
    gxByteBuffer key_bb;
    bool rc = false;

    bb_init(&key_bb);
    if (unwrap_key(wrapped_bb_p, &key_bb, key_name_p) &&
        store_fn(key_bb.data, key_bb.size))
    {
        LOGD("%s key updated", key_name_p);
        rc = true;
    }
    bb_clear(&key_bb);

    return rc;
}

static void handle_profile_push_config_change(uint16_t cfg)
{
    uint16_t old_cfg;

    Server_Attribute_Manager_readProfilePushConfig(&old_cfg);

    if (cfg == old_cfg)
    {
        LOGI("ProfilePushConfig: no change");
    }
    else
    {
        Server_Attribute_Manager_writeProfilePushConfig(cfg);
    }
}

/**
 * Write selected item(s).
 *
 * @param args
 *            Handled write requests.
 */
void svr_preWrite(
        dlmsSettings* settings,
        gxValueEventCollection* args)
{
    LOGD("svr_preWrite");

    gxValueEventArg * e;
    int pos;
    dlmsVARIANT clock;
    bool transparent_mode_to_enable;
    bool clock_set = false;
    gxValueEventArg * tm_vae = NULL;

    for (pos = 0; pos != args->size; ++pos)
    {
        int ret;
        obj_list_e o;

        dlmsVARIANT * var;
        if (vec_getByIndex(args, pos, &e) != 0)
        {
            LOGE("Failed to retrieve object");
            return;
        }
        var = &e->value;
        LOG_WITH_OBIS(LVL_DEBUG, e->target->logicalName,
                      "preWrite type = %d",  e->target->objectType);

        switch ((o = find_object(e->target)))
        {
            case OBJ_MR_ASSOCIATION_SECRET:
                if (! store_secret(var->strVal,
                                   Security_Material_store_new_mr_secret, "MR"))
                {
                    e->error = DLMS_ERROR_CODE_UNMATCH_TYPE;
                    e->handled = 1;
                }
                break;

            case OBJ_US_ASSOCIATION_SECRET:
                if (! store_secret(var->strVal,
                                   Security_Material_store_new_us_secret, "US"))
                {
                    e->error = DLMS_ERROR_CODE_UNMATCH_TYPE;
                    e->handled = 1;
                }
                break;

            case OBJ_FU_ASSOCIATION_SECRET:
                if (! store_secret(var->strVal,
                                   Security_Material_store_new_fu_secret, "FU"))
                {
                    e->error = DLMS_ERROR_CODE_UNMATCH_TYPE;
                    e->handled = 1;
                }
                break;

            case OBJ_WRAPPED_ENCRYPTION_KEY:
                if (! store_key(var->strVal,
                                Security_Material_store_new_enc_key, "ek"))
                {
                    e->error = DLMS_ERROR_CODE_UNMATCH_TYPE;
                    e->handled = 1;
                }
                break;

            case OBJ_WRAPPED_AUTHENTICATION_KEY:
                if (! store_key(var->strVal,
                                Security_Material_store_new_auth_key, "ak"))
                {
                    e->error = DLMS_ERROR_CODE_UNMATCH_TYPE;
                    e->handled = 1;
                }
                break;

            case OBJ_WRAPPED_KEY_ENCRYPTION_KEY:
                if (! store_key(var->strVal,
                                Security_Material_store_new_key_encryption_key, "kek"))
                {
                    e->error = DLMS_ERROR_CODE_UNMATCH_TYPE;
                    e->handled = 1;
                }
                break;

            case OBJ_INSTANTANEOUS_PERIOD_CONFIG:
                LOGD("Checking inst period");
                if (e->value.uiVal >= INSTANTANEOUS_MINIMUN_PERIOD_MN &&
                    e->value.uiVal <= INSTANTANEOUS_MAXIMUM_PERIOD_MN)
                {
                    LOGI("Setting instantaneous period to %d mn", e->value.uiVal);
                    Server_Attribute_Manager_writeInstantaneousPushConfig(e->value.uiVal);
                }
                else
                {
                    LOGE("Unallowed value for instantaneous period %d", e->value.uiVal);
                    e->error = DLMS_ERROR_CODE_UNMATCH_TYPE;
                    e->handled = 1;
                }
                break;

            case OBJ_PUSH_ENABLE_CONFIG:
                LOGD("Sanity checking prof push config");
                if (e->value.vt != DLMS_DATA_TYPE_BIT_STRING)
                {
                    e->error = DLMS_ERROR_CODE_INCONSISTENT_CLASS_OR_OBJECT;
                    e->handled = 1;
                    LOGE("invalid type %d for prof push config",
                        e->value.vt);
                }
                else if (ba_getByteCount(e->value.bitArr->size) != PROF_PUSH_CFG_BYTE_SIZE ||
                         (e->value.bitArr->size != PROF_PUSH_BIT_NB))
                {
                    e->error = DLMS_ERROR_CODE_UNMATCH_TYPE;
                    e->handled = 1;
                    LOGE("invalid size for prof push config");
                }
                else
                {
                    uint16_t cfg = (e->value.bitArr->data[0] << 8) |
                                   e->value.bitArr->data[1];
                    handle_profile_push_config_change(cfg);
                }
                break;

            case OBJ_TRANSPARENT_MODE_CONFIG:
                LOGD("Sanity checking transparent config");
                if (e->value.vt != DLMS_DATA_TYPE_BOOLEAN)
                {
                    e->error = DLMS_ERROR_CODE_INCONSISTENT_CLASS_OR_OBJECT;
                    e->handled = 1;
                    LOGE("invalid type %d for transparent config",
                        e->value.vt);
                }
                else
                {
                    bool enabled = Transparent_mode_is_enabled();
                    transparent_mode_to_enable = e->value.boolVal;
                    // If the setting has changed, keep a pointer on the object for later action
                    if (enabled != transparent_mode_to_enable)
                    {
                        tm_vae = e;
                    }
                    else
                    {
                        LOGI("Transparent mode unchanged");
                    }
                }
                break;

            case OBJ_CLOCK:
                LOGD("Set the clock");
                if (e->value.vt == DLMS_DATA_TYPE_OCTET_STRING && e->value.byteArr != NULL)
                {
                    var_init(&clock);
                    ret = dlms_changeType2(&e->value, DLMS_DATA_TYPE_DATETIME, &clock);
                    if (ret != 0)
                    {
                        LOGE("Clock cannot change type: %u", ret);
                        (void) ret;
                        var_clear(&clock);
                    }
                    else
                    {
                        clock_set = true;
                    }
                }
                else
                {
                    e->error = DLMS_ERROR_CODE_INCONSISTENT_CLASS_OR_OBJECT;
                    e->handled = 1;
                    LOGE("Wrong Clock type: %u", e->value.vt);
                }
                break;

#ifdef METER_REGISTRATION_ENABLED
            case OBJ_NIC_REGISTRATION_STATUS:
                LOGD("Sanity checking NIC registration status");
                if (e->value.vt != DLMS_DATA_TYPE_BOOLEAN)
                {
                    e->error = DLMS_ERROR_CODE_INCONSISTENT_CLASS_OR_OBJECT;
                    e->handled = 1;
                    LOGE("invalid type %d for NIC registration status",
                         e->value.vt);
                }
                else
                {
                    bool registered;
                    Server_Attribute_Manager_readNicRegistrationStatus(&registered);
                    // If status has changed
                    if (registered != e->value.boolVal)
                    {
                        // Write the updated status
                        Server_Attribute_Manager_writeNicRegistrationStatus(e->value.boolVal);
                    }
                }
                break;
#endif

#ifdef FIXED_DAY_BILLING_ENABLED
            case OBJ_FIXED_DAY_BILLING_DAY:
                LOGD("Sanity checking fixed day billing day");
                if (e->value.vt != DLMS_DATA_TYPE_UINT8)
                {
                    e->error = DLMS_ERROR_CODE_INCONSISTENT_CLASS_OR_OBJECT;
                    e->handled = 1;
                    LOGE("invalid type %d for FDBD", e->value.vt);
                }
                else if (! Fixed_Day_Billing_Day_isValid(e->value.bVal))
                {
                    e->error = DLMS_ERROR_CODE_OTHER_REASON;
                    e->handled = 1;
                    LOGE("invalid value %d for FDBD", e->value.bVal);
                }
                else
                {
                    Fixed_Day_Billing_Profile_updateBillingDay(e->value.bVal);
                }
                break;
#endif

            default:
                LOGE("Attempt to write an unsupported object %d", o);
                break;
        }
    }

    // We do not update the clock or enable/disable the transparent mode if the NIC is unregistered
    if (clock_set || tm_vae)
    {
        bool registered;

        Server_Attribute_Manager_readNicRegistrationStatus(&registered);

        if (clock_set)
        {
            if (registered)
            {
                uint32_t delay_s = Nic_Server_get_current_message_delay_ms()/1000;
                LOGI("Clock: %u dev: %u, to be compensated by: %d", clock.dateTime->value, clock.dateTime->deviation, delay_s);
                MeterClock_set_new_meter_clock(clock.dateTime->value + delay_s, clock.dateTime->deviation);
            }
            else
            {
                LOGW("Clock can't be set while the NIC is unregistered");
            }
            var_clear(&clock);
        }
        if (tm_vae)
        {
            if (registered)
            {
                bool res;

                // Do it here as there is no transparent mode module as such
                if (transparent_mode_to_enable)
                {
                    res = Transparent_mode_enable();
                }
                else
                {
                    res = Transparent_mode_disable();
                }
                // Store the new mode only if successful
                if (!res)
                {
                    // Generate an error
                    tm_vae->error = DLMS_ERROR_CODE_TEMPORARY_FAILURE;
                    tm_vae->handled = 1;
                    LOGE("Cannot change transparent mode to %d",
                         transparent_mode_to_enable);
                }
            }
            else
            {
                LOGW("Transparent mode can't be enable or disable while the NIC is unregistered");
            }
        }
    }
}

/**
 * Action is occurred.
 *
 * @param args
 *            Handled action requests.
 */
void svr_preAction(
        dlmsSettings* settings,
        gxValueEventCollection* args)
{
    gxValueEventArg* e;
    int pos;
    for (pos = 0; pos != args->size; ++pos)
    {
        if (vec_getByIndex(args, pos, &e) != 0)
        {
            return;
        }

        LOG_WITH_OBIS(LVL_DEBUG, e->target->logicalName,
                      "preAction type = %d",  e->target->objectType);
    }
}

/**
 * Read selected item(s).
 *
 * @param args
 *            Handled read requests.
 */
void svr_postRead(
        dlmsSettings* settings,
        gxValueEventCollection* args)
{
    LOGD("svr_postRead");
    gxValueEventArg* e;
    int pos;
    for (pos = 0; pos != args->size; ++pos)
    {
        if (vec_getByIndex(args, pos, &e) != 0)
        {
            return;
        }
        LOG_WITH_OBIS(LVL_DEBUG, e->target->logicalName,
                      "postRead type = %d",  e->target->objectType);
    }
}

/**
 * Write selected item(s).
 *
 * @param args
 *            Handled write requests.
 */
void svr_postWrite(
        dlmsSettings* settings,
        gxValueEventCollection* args)
{
    // In Gurux server:
    //  - the srv_preWrite is called
    //  - then object from the list is updated
    //  - then srv_postWrite is called.
    // As we don’t really care of the internal list except for initial capture
    // object, we do everything in srv_preWrite, including the write
    // to our own storage
    (void)settings;
    (void)args;
}

/**
 * Action is occurred.
 *
 * @param args
 *            Handled action requests.
 */
void svr_postAction(
        dlmsSettings* settings,
        gxValueEventCollection* args)
{
    gxValueEventArg* e;
    int pos;
    for (pos = 0; pos != args->size; ++pos)
    {
        if (vec_getByIndex(args, pos, &e) != 0)
        {
            return;
        }
        LOG_WITH_OBIS(LVL_DEBUG, e->target->logicalName,
                      "preAction type = %d",  e->target->objectType);

        // TODO: code hereunder does nothing, just comment it for now
        //~ if (e->target == BASE(mp_srvobj_param->us_security_setup))
        //~ {
            //~ // TODO: Key was updated through action Error?
            //~ //Update block cipher key authentication key or broadcast key.
            //~ //Save settings to EEPROM.
        //~ }
        //~ //Check is client changing the settings with action.
        //~ else if (svr_isChangedWithAction(e->target->objectType, e->index))
        //~ {
            //~ // TODO: Update the persistent
            //~ // But for now, we don't have anything with Action
        //~ }
    }
}

/**
 * Find object.
 *
 * @param objectType
 *            Object type.
 * @param sn
 *            Short Name. In Logical name referencing this is not used.
 * @param ln
 *            Logical Name. In Short Name referencing this is not used.
 * @return Found object or NULL if object is not found.
 */
int svr_findObject(
        dlmsSettings* settings,
        DLMS_OBJECT_TYPE objectType,
        int sn,
        unsigned char* ln,
        gxValueEventArg* e)
{
    LOG_WITH_OBIS(LVL_DEBUG, ln, "findObject type = %d",  objectType);
    if (objectType == DLMS_OBJECT_TYPE_ASSOCIATION_LOGICAL_NAME)
    {
        objectArray objects;
        // 6 is a default value but could be smaller as we shouldn't support that many association
        gxObject* tmp[6];
        oa_attach(&objects, tmp, sizeof(tmp) / sizeof(tmp[0]));
        objects.size = 0;
        // Get subset of object of type Association_Logical_name
        if (oa_getObjects(&settings->objects, DLMS_OBJECT_TYPE_ASSOCIATION_LOGICAL_NAME, &objects) == 0)
        {
            uint16_t pos;
            gxAssociationLogicalName* a;
            for (pos = 0; pos != objects.size; ++pos)
            {
                if (oa_getByIndex(&objects, pos, (gxObject**)&a) == 0)
                {
                    LOGI("findObject %X-%X , %u-%u",
                            a->clientSAP, settings->clientAddress,
                            a->authenticationMechanismName.mechanismId, settings->authentication);
                    if (a->clientSAP == settings->clientAddress &&
                            a->authenticationMechanismName.mechanismId == settings->authentication)
                    {
                        e->target = (gxObject*) a;
                        break;
                    }
                }
            }
        }
    }
    if (e->target == NULL)
    {
        LOG_WITH_OBIS(LVL_WARNING, ln, "findObject Unknown type = %d",  objectType);
    }
    return 0;
}

/**
 * called when client clses connection to the server.
 */
int svr_disconnected(
        dlmsServerSettings* settings)
{
    LOGI("svr_disconnected");
    Nic_Server_disconnect();
    return 0;
}

/* ************************************ */
/* UNUSED SERVER CALLBACKS              */

/**
 * called when client makes connection to the server.
 */
int svr_connected(
        dlmsServerSettings* settings)
{
    LOGI("svr_connected");
    return 0;
}

/**
 * Client has try to made invalid connection. Password is incorrect.
 *
 * @param connectionInfo
 *            Connection information.
 */
int svr_invalidConnection(
        dlmsServerSettings* settings)
{
    LOGE("svr_invalidConnection");
    Nic_Server_invalid_connection();
    return 0;
}

/**
 * Check whether the authentication and password are correct.
 *
 * @param authentication
 *            Authentication level.
 * @param password
 *            Password.
 * @return Source diagnostic.
 */
DLMS_SOURCE_DIAGNOSTIC svr_validateAuthentication(
        dlmsServerSettings* settings,
        DLMS_AUTHENTICATION authentication,
        gxByteBuffer* password)
{
    /*GXTRACE(("svr_validateAuthentication"), NULL);
    if (authentication == DLMS_AUTHENTICATION_NONE)
    {
        //Uncomment this if authentication is always required.
        //return DLMS_SOURCE_DIAGNOSTIC_AUTHENTICATION_MECHANISM_NAME_REQUIRED;
        return DLMS_SOURCE_DIAGNOSTIC_NONE;
    }
    //Check Low Level security..
    if (authentication == DLMS_AUTHENTICATION_LOW)
    {
        if (bb_compare(password, associationLow.secret.data, associationLow.secret.size) == 0)
        {
            GXTRACE(("Invalid low level password."), (const char*)associationLow.secret.data);
            return DLMS_SOURCE_DIAGNOSTIC_AUTHENTICATION_FAILURE;
        }
    }*/
    // Hith authentication levels are check on phase two.
    return DLMS_SOURCE_DIAGNOSTIC_NONE;
}


/**
 * Check if data sent to this server.
 *
 * @param serverAddress
 *            Server address.
 * @param clientAddress
 *            Client address.
 * @return True, if data is sent to this server.
 */
unsigned char svr_isTarget(
        dlmsSettings* settings,
        uint32_t serverAddress,
        uint32_t clientAddress)
{
    LOGD("svr_isTarget Srv:%X Cl:%X", serverAddress, clientAddress);
    return 1;
}


void svr_preGet(
        dlmsSettings* settings,
        gxValueEventCollection* args)
{
    LOGD("svr_preGet");
}

void svr_postGet(
        dlmsSettings* settings,
        gxValueEventCollection* args)
{
    LOGD("svr_postGet");
}

/**
 * This is reserved for future use.
 *
 * @param args
 *            Handled data type requests.
 */
void svr_getDataType(
        dlmsSettings* settings,
        gxValueEventCollection* args)
{
    LOGD("svr_getData");
}

/* ************************************ */
/* TRACE  CALLBACKS                     */
#undef DEBUG_LOG_MODULE_NAME
#define DEBUG_LOG_MODULE_NAME "GURUX   "

//Returns the approximate processor time in ms.
uint32_t time_elapsed(void)
{
    return lib_time->getTimestampS() * 1000;
}

void svr_trace(
        const char* str,
        const char* data)
{
    LOGI("%s: %s", str, data);
}

void svr_notifyTrace(const char* str, int err)
{
#ifdef DLMS_DEBUG
    if (err != 0)
    {
        LOGE("%s: %d", str, err);
    }
    else
    {
        LOGI("%s: ok", str);
    }
#endif// DLMS_DEBUG
}

void svr_notifyTrace2(const char* str, const short ot, const unsigned char* ln, int err)
{
#ifdef DLMS_DEBUG
    if (err != 0)
    {
        LOGE("%s - %d: %d.%d.%d.%d.%d.%d error: %d", str, ot, ln[0], ln[1], ln[2], ln[3], ln[4], ln[5], err);
    }
    else
    {
        LOGI("%s: ok", str);
    }
#endif// DLMS_DEBUG
}
