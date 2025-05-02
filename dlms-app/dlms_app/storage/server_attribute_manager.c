/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cmsis_compiler.h" //__PACKED_STRUCT
#include <assert.h> // static_assert

#include "common.h" //LOGx
#include "attribute_manager.h"
#include "server_attribute_manager.h"

#include "public_provisioning.h"
#include "app_persistent.h"

#include "storage_driver.h"
#include "storage_configuration.h"

#define DEBUG_LOG_MODULE_NAME "SRV_ATTR"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

// V1 & V2 have the same size, there is one more bit in profilePushConfig
// for the export billing profile
#define V1                                      1
#define V2                                      2
#define V1_SIZE                                 (sizeof(storage_control_t) + sizeof(server_attribute_table_t))
#define V2_SIZE                                 (V1_SIZE)

/**
 * Versionning of the image
 */
#define SERVER_IMAGE_VERSION                    V2
#define SERVER_IMAGE_SIZE                       sizeof(m_server_image)

// Default values for invocation counters
#define DEFAULT_DECRYPT_INVOCATION_COUNTER      1
#define DEFAULT_ENCRYPT_INVOCATION_COUNTER      0

/**
 * Local (RAM) storage variables.
 * To maintain SW compatibility:
 *   - Do not change, move or delete existing definitions.
 *   - Always add new variables to the end.
 *   - Bump the image version and update the storage upgrade code
 * New parameters that are not found from FLASH shall be filled with 0xFF.
 */
typedef __PACKED_STRUCT
{
    // ------------- Invocation counters -------------
    //
    uint32_t encryption_invocation_counter;
    uint32_t decryption_invocation_counter;
    // ----------- Meter security material -----------
    security_material_t security_material_new; // security material received by NIC
    security_material_t security_material_cur; // security material written to metter
    // ---------- Instantaneous Push config ----------
    uint16_t            instantaneousPushConfig;
    // ------------ Profile Push config --------------
    uint16_t            profilePushConfig;
    // ----------- Transparent mode switch -----------
    bool                transparent_mode;
    // ------------ Meter identification -------------
    meter_identification_t meter_identification;
    // ------------- Registration state --------------
    bool nic_registration_status;
    // --- Meter firmware update activation status ---
    bool meter_firmware_update_activation_ongoing;
    // ----------- Fixed day billing day -------------
    uint8_t             billing_day;  // day to generate the billing
    uint32_t            billing_date; // next billing date computed from billing day and epoch
    // Add new stuff to the end (just above this line).
} server_attribute_table_t;

/**
 * The actual permanent storage bank image.
 */
typedef __PACKED_STRUCT server_attribute_image_t
{
    storage_control_t   _ctrl; // Special field for storage driver.
    server_attribute_table_t   attributes;
} server_attribute_image_t;

/**
 * If this test fails then the variables do not fit to the bank any more.
 */
_Static_assert(sizeof(server_attribute_image_t) <= SERVER_STORAGE_SECTOR_SIZE,
               "sizeof(server_attribute_image_t) > SERVER_STORAGE_SECTOR_SIZE");

// Storage descriptor
static storage_context_t m_server_context;

// RAM table
static server_attribute_image_t __attribute__((aligned(4)))  m_server_image;

static bool m_server_storage_available;

// Prototypes
/**
 * @brief       Check if the version of the last valid image read from
 *              persistent storage needs to be upgraded to a new version
 *              and perform the upgrade
 * @return      true : image has been modified
 *              false : image has not been modified
 */
static bool attr_upgrade_needed(void);

/**
 * @brief   Commit new attribute values to storage
 * @return  true : commited successfully
 *          false : failed to commit
 */
static bool attr_commit(void);

/**
 * @brief   After initialization, handle values that require default value other
 *          than 0xff to be set
 * @return  true
 *          Parameters changed: should commit
 *          false
 *          Nothing was changed
 */
static bool attr_set_defaults(void);

/** Attribute manager init */
attribute_result_e Server_Attr_Manager_init(void)
{
    if (STORAGE_DRIVER_RES_OK != Storage_Driver_contextInit(&m_server_context,
                                                    SERVER_STORAGE_BLOCK_OFFSET,
                                                    SERVER_STORAGE_BLOCK_NB,
                                                    SERVER_STORAGE_SECTOR_SIZE,
                                                    SERVER_IMAGE_VERSION))
    {
        return ATTR_INV_VALUE;
    }
 
    if (STORAGE_DRIVER_RES_OK != Storage_Driver_init(&m_server_context,
                                                (uint8_t *) &m_server_image,
                                                SERVER_IMAGE_SIZE))
    {
        return ATTR_STORAGE_ERROR;
    }

    m_server_storage_available = true;

    // Check if image need to be upgraded to a new version
    if (attr_upgrade_needed())
    {
        // Commit to persistent storage
        if (! attr_commit())
        {
            return ATTR_STORAGE_ERROR;
        }
    }
    return ATTR_SUCCESS;
}

attribute_result_e Server_Attr_Manager_reset(void)
{
   // Reset everything
    memset(&m_server_image.attributes, 0xFF, sizeof(server_attribute_table_t));

    // Set default values
    attr_set_defaults();

    // Commit new image
    if (attr_commit())
    {
        return ATTR_SUCCESS;
    }
    return ATTR_STORAGE_ERROR;
}

attribute_result_e Server_Attr_Manager_commit(void)
{
    // Commit new image
    if (attr_commit())
    {
        return ATTR_SUCCESS;
    }
    return ATTR_STORAGE_ERROR;
}

static bool v1_to_v2_upgrade_handler(uint16_t size)
{
    bool to_upgrade = false;

    if (size != V2_SIZE)
    {
        // PVIS: Previous Version Image Size mismatch
        LOGI("Resetting server image (PVIS)");
        (void)Server_Attr_Manager_reset();
    }
    else
    {
        LOGI("Upgrading server image from v1 to v2");
        server_attribute_table_t * attr_p = &m_server_image.attributes;
         // Profile push config: disable the export billing bit by default
        PUSH_PROF_UNSET_BIT(attr_p->profilePushConfig, PROF_PUSH_EXPORT_BILLING_BIT);
        to_upgrade = true;
    }
    return to_upgrade;
}

/** storage variables upgrade */
static bool attr_upgrade_needed(void)
{
    uint16_t version, size;
    bool valid = Storage_Driver_getImageInfo(&m_server_image, &version, &size);
    bool to_upgrade = false;

    // If not valid, reset everything
    if (! valid)
    {
        // This function already commits to persistent storage so no need
        // to return true. II: Invalid Image
        LOGI("Resetting server image (II)");
        (void)Server_Attr_Manager_reset();
    }
    else
    {
        if ((version != EMPTY_IMAGE_VERSION && version != SERVER_IMAGE_VERSION))
        {
            switch (version)
            {
                case V1:
                    to_upgrade = v1_to_v2_upgrade_handler(size);
                    break;

                default:
                    // IVU: Invalid Version Upgrade
                    LOGI("Resetting server image (IVU)");
                    (void)Server_Attr_Manager_reset();
                    break;
            }
        }
        else if (size != SERVER_IMAGE_SIZE)
        {
            // This error should not happen outside of a development cycle
            LOGE("Persistent storage and app server image size differ: %u vs %u, "
                 "did you forget to bump the version?", size, SERVER_IMAGE_SIZE);
            // IV: Inconsistent Version
            LOGI("Resetting server image (IV)");
            (void)Server_Attr_Manager_reset();
        }
    }
    return to_upgrade;
}

static bool attr_commit(void)
{
    if (! m_server_storage_available)
    {
        LOGW("server attributes are not saved in persistent memory");
        return false;
    }

    if (STORAGE_DRIVER_RES_OK != Storage_Driver_write(&m_server_context,
                                                    (uint8_t *) &m_server_image,
                                                    SERVER_IMAGE_SIZE))
    {
        LOGW("disabling server attributes saving in persistent"
            "memory");
        m_server_storage_available = false;
        return false;
    }
    return true;
}

static void reset_invocation_counters(void)
{
    server_attribute_table_t * attr_p = &m_server_image.attributes;

    attr_p->encryption_invocation_counter = DEFAULT_ENCRYPT_INVOCATION_COUNTER;
    attr_p->decryption_invocation_counter = DEFAULT_DECRYPT_INVOCATION_COUNTER;
}

static bool attr_set_defaults(void)
{
    server_attribute_table_t * attr_p = &m_server_image.attributes;

    security_material_t default_msm = {0};

    App_Persistent_init();

    // Read them from provisionned one
    if (App_Persistent_read((uint8_t *) &default_msm,
                        sizeof(security_material_t)) == APP_PERSISTENT_RES_OK)
    {
        LOGI("NIC default MSM from prov");
    }
    else
    {
        LOGE("Cannot get NIC default MSM");
        // Default it to all 0
    }

    attr_p->security_material_cur = default_msm;
    attr_p->security_material_new = default_msm;

    attr_p->instantaneousPushConfig = 15; // 15 mins by default

    attr_p->transparent_mode = false;

    reset_invocation_counters();

    // Meter identification
    memset(&attr_p->meter_identification, 0x00, sizeof(attr_p->meter_identification));

    // NIC-meter registration
#ifdef METER_REGISTRATION_ENABLED
    attr_p->nic_registration_status = false;
#else
    attr_p->nic_registration_status = true;
#endif

    attr_p->meter_firmware_update_activation_ongoing = false;
    attr_p->billing_day = FIXED_DAY_BILLING_DAY_DISABLED;
    attr_p->billing_date = 0;

    // Profile push config
    attr_p->profilePushConfig = 0x00;
    for (uint8_t i = PROF_PUSH_BIT_FIRST; i < PROF_PUSH_BIT_NB; i++)
    {
        if (i != PROF_PUSH_EXPORT_BILLING_BIT)
        {
            PUSH_PROF_SET_BIT(attr_p->profilePushConfig, i);
        }
    }

    return true;
}

bool Server_Attribute_Manager_writeMeterSecurityMaterial(Server_Attr_Manager_meterSecurityMaterial_type_t type,
                                                         const security_material_t * msm_p)
{
    server_attribute_table_t * attr_p = &m_server_image.attributes;
    switch (type) {
        case SERVER_ATTRIBUTE_MSM_TYPE_CURRENT:
            attr_p->security_material_cur = *msm_p;
            return attr_commit();
        break;
        case SERVER_ATTRIBUTE_MSM_TYPE_NEW:
            attr_p->security_material_new = *msm_p;
            return attr_commit();
        break;
        default:
            LOGE("Wrong MSM type to write");
            return false;
    }
}

bool Server_Attribute_Manager_readMeterSecurityMaterial(Server_Attr_Manager_meterSecurityMaterial_type_t type,
                                                        security_material_t ** msm_p)
{
    LOGD("Reading Secu Mat type %d", type);
    server_attribute_table_t * attr_p = &m_server_image.attributes;
    switch (type) {
        case SERVER_ATTRIBUTE_MSM_TYPE_CURRENT:
            *msm_p = &attr_p->security_material_cur;
            return true;
        break;
        case SERVER_ATTRIBUTE_MSM_TYPE_NEW:
            *msm_p = &attr_p->security_material_new;
            return true;
        break;
        default:
            LOGE("Wrong MSM type to read");
            return false;
    }
}

void Server_Attribute_Manager_writeInstantaneousPushConfig(uint16_t period_m)
{
    LOGD("Saving Instantaneous Push config: %d min", period_m);
    m_server_image.attributes.instantaneousPushConfig = period_m;
    attr_commit();
}

void Server_Attribute_Manager_readInstantaneousPushConfig(uint16_t * period_m_p)
{
    LOGD("Reading Instantaneous Push config");
    *period_m_p = m_server_image.attributes.instantaneousPushConfig;
}

void Server_Attribute_Manager_writeProfilePushConfig(uint16_t cfg)
{
    LOGI("Saving Profile Push config: 0x%04X", cfg);
    m_server_image.attributes.profilePushConfig = cfg;
    attr_commit();
}

void Server_Attribute_Manager_readProfilePushConfig(uint16_t * cfg_p)
{
    LOGD("Reading Profile Push config");
    *cfg_p = m_server_image.attributes.profilePushConfig;
}

void Server_Attribute_Manager_writeTransparentConfig(bool enabled)
{
    LOGI("Saving Transp mode conf: 0x%d", enabled);
    m_server_image.attributes.transparent_mode = enabled;
    attr_commit();
}

void Server_Attribute_Manager_readTransparentConfig(bool * enabled_p)
{
    LOGD("Reading Transp mode conf");
    *enabled_p = m_server_image.attributes.transparent_mode;
}

void Server_Attribute_Manager_writeEncryptInvocationCounterNoCommit(uint32_t ic)
{
    LOGD("Saving NIC server encryption InvocationCounter: %d", ic);
    m_server_image.attributes.encryption_invocation_counter = ic;
    // Commit has to be called explicitely
}

void Server_Attribute_Manager_readEncryptInvocationCounter(uint32_t * ic_p)
{
    *ic_p = m_server_image.attributes.encryption_invocation_counter;
    LOGD("Reading NIC server encryption InvocationCounter: %d", *ic_p);
}

void Server_Attribute_Manager_writeDecryptInvocationCounterNoCommit(uint32_t ic)
{
    LOGD("Saving NIC server decryption InvocationCounter: %d", ic);
    m_server_image.attributes.decryption_invocation_counter = ic;
    // Commit has to be called explicitely
}

void Server_Attribute_Manager_readDecryptInvocationCounter(uint32_t * ic_p)
{
    *ic_p = m_server_image.attributes.decryption_invocation_counter;
    LOGD("Reading NIC server decryption InvocationCounter: %d", *ic_p);
}

void Server_Attribute_Manager_resetInvocationCounters(void)
{
    reset_invocation_counters();
    attr_commit();
}

void Server_Attribute_Manager_writeMeterSerialNumber(const uint8_t * id_p, uint8_t size)
{
    meter_identification_t * meter_id_p = &m_server_image.attributes.meter_identification;

    meter_id_p->serial_number_len = MIN(size, METER_SERIAL_NUMBER_MAX_SIZE - 1);
    memcpy(meter_id_p->serial_number, id_p, meter_id_p->serial_number_len);
    if (size < METER_SERIAL_NUMBER_MAX_SIZE)
    {
        LOGI("Saving meter SN: %s", meter_id_p->serial_number);
    }
    else
    {
        LOGW("Truncating meter SN: %s vs %s", meter_id_p->serial_number, id_p);
    }
    attr_commit();
}

void Server_Attribute_Manager_readMeterSerialNumber(const uint8_t ** id_pp, uint8_t * size_p)
{
    *id_pp = m_server_image.attributes.meter_identification.serial_number;
    *size_p = m_server_image.attributes.meter_identification.serial_number_len;
}

void Server_Attribute_Manager_writeMeterDeviceId(const uint8_t * id_p, uint8_t size)
{
    meter_identification_t * meter_id_p = &m_server_image.attributes.meter_identification;

    meter_id_p->device_id_len = MIN(size, METER_DEVICE_ID_MAX_SIZE - 1);
    memcpy(meter_id_p->device_id, id_p, MIN(size, METER_DEVICE_ID_MAX_SIZE - 1));
    if (size < METER_DEVICE_ID_MAX_SIZE)
    {
        LOGI("Saving meter dev id: %s", meter_id_p->device_id);
    }
    else
    {
        LOGW("Truncating meter dev id: %s vs %s", meter_id_p->device_id, id_p);
    }

    attr_commit();
}

void Server_Attribute_Manager_readMeterDeviceId(const uint8_t ** id_pp, uint8_t * size_p)
{
    *id_pp = m_server_image.attributes.meter_identification.device_id;
    *size_p = m_server_image.attributes.meter_identification.device_id_len;
}

void Server_Attribute_Manager_writeNicRegistrationStatus(bool registered)
{
#ifdef METER_REGISTRATION_ENABLED
    LOGI("Saving Nic registration status: 0x%d", registered);
    if (m_server_image.attributes.nic_registration_status != registered)
    {
        m_server_image.attributes.nic_registration_status = registered;
        attr_commit();
    }
#endif
}

void Server_Attribute_Manager_readNicRegistrationStatus(bool * registered_p)
{
    *registered_p = m_server_image.attributes.nic_registration_status;
    LOGD("Reading Nic registration status: 0x%d", *registered_p);
}

void Server_Attribute_Manager_writeMeterFirmwareUpdateActivationOngoing(bool ongoing)
{
    LOGI("Saving Meter firmware update activation status: 0x%d", ongoing);
    m_server_image.attributes.meter_firmware_update_activation_ongoing = ongoing;
    attr_commit();
}

void Server_Attribute_Manager_readMeterFirmwareUpdateActivationOngoing(bool * ongoing_p)
{
    *ongoing_p = m_server_image.attributes.meter_firmware_update_activation_ongoing;
}

void Server_Attribute_writeFixedDayBillingInfo(uint8_t billing_day, uint32_t billing_date)
{
#ifdef FIXED_DAY_BILLING_ENABLED
    bool commit = false;
    LOGI("Saving Fixed Day billing day: %d", billing_day);
    if (m_server_image.attributes.billing_day != billing_day)
    {
        m_server_image.attributes.billing_day = billing_day;
        commit = true;
    }
    LOGI("Saving Fixed Day billing date: %u", billing_date);
    if (m_server_image.attributes.billing_date != billing_date)
    {
        m_server_image.attributes.billing_date = billing_date;
        commit = true;
    }
    if (commit)
    {
        attr_commit();
    }
#endif
}

void Server_Attribute_readFixedDayBillingInfo(uint8_t * billing_day_p, uint32_t * billing_date_p)
{
    *billing_day_p = m_server_image.attributes.billing_day;
    *billing_date_p = m_server_image.attributes.billing_date;
    LOGD("Reading Fixed Day billing info: day: %d, date: %u", *billing_day_p, *billing_date_p);
}
