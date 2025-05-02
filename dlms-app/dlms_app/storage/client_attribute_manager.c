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

#include "storage_driver.h"
#include "storage_configuration.h"

#include "client_attribute_manager.h"

#include "blockload_profile.h"
#include "dailyload_profile.h"

#define DEBUG_LOG_MODULE_NAME "CLT_ATTR"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

// V1 (dlms_app v1.2) and V2 (dlms_app v1.3) differ
// due to the addition of the export billing profile data
#define V1                      1
#define V2                      2
#define V1_SIZE                 (sizeof(storage_control_t) + offsetof(client_attribute_table_t, export_billing_data))
#define V2_SIZE                 (sizeof(storage_control_t) + sizeof(client_attribute_table_t))

/**
 * Versionning of the image
 */
#define CLIENT_IMAGE_VERSION    V2
#define CLIENT_IMAGE_SIZE       sizeof(m_client_image)

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
    // ------------- Blockload profile -------------
    uint32_t            bl_start_time;
    // ------------- Dailyload profile -------------
    uint32_t            dl_start_time;
    // -------------- Billing profile --------------
    billing_status_t    billing_data;
    // ------------- Event log profile -------------
    evt_log_data_t      evt_log_data;
    // ----------- Export billing profile ----------
    billing_status_t    export_billing_data;

     // Add new stuff to the end (just above this line).
} client_attribute_table_t;

/**
 * The actual permanent storage bank image.
 */
typedef __PACKED_STRUCT client_attribute_image_t
{
    // Opaque type for
    storage_control_t   _ctrl; // Special field for storage driver.
    client_attribute_table_t   attributes;
} client_attribute_image_t;

/**
 * If this test fails then the variables do not fit to the bank any more.
 */
_Static_assert(sizeof(client_attribute_image_t) <= CLIENT_STORAGE_SECTOR_SIZE,
               "sizeof(client_attribute_image_t) > CLIENT_STORAGE_SECTOR_SIZE");

// Storage descriptor
static storage_context_t m_client_context;

// RAM table
static client_attribute_image_t __attribute__((aligned(4)))  m_client_image;

static bool m_client_storage_available;

// Prototypes
/**
 * @brief       Check if the version of the last valid image read from
 *              persistent storage needs to be upgraded to a new version
 *              and perform the upgrade
 * @param[in]   version : version of the last valid image
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
attribute_result_e Client_Attr_Manager_init(void)
{
    bool modified = false;

    if (STORAGE_DRIVER_RES_OK != Storage_Driver_contextInit(&m_client_context,
                                                    CLIENT_STORAGE_BLOCK_OFFSET,
                                                    CLIENT_STORAGE_BLOCK_NB,
                                                    CLIENT_STORAGE_SECTOR_SIZE,
                                                    CLIENT_IMAGE_VERSION))
    {
        return ATTR_INV_VALUE;
    }

    if (STORAGE_DRIVER_RES_OK != Storage_Driver_init(&m_client_context,
                                                (uint8_t *) &m_client_image,
                                                CLIENT_IMAGE_SIZE))
    {
        return ATTR_STORAGE_ERROR;
    }

    m_client_storage_available = true;

    // First, check if the image needs to be upgraded to a new version
    modified = attr_upgrade_needed();

    // Check current values and reset to default ones if needed
    modified |= attr_set_defaults();

    if (modified)
    {
        // Commit to persistent storage
        if (! attr_commit())
        {
            return ATTR_STORAGE_ERROR;
        }
    }
    return ATTR_SUCCESS;
}

attribute_result_e Client_Attr_Manager_reset(void)
{
   // Reset everything
    memset(&m_client_image.attributes, 0xFF, sizeof(client_attribute_table_t));

    // Set default values
    attr_set_defaults();

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

    if (V1_SIZE != size)
    {
        // PVIS: Previous Version Image Size mismatch
        LOGI("Resetting client image (PVIS)");
        (void)Client_Attr_Manager_reset();
    }
    else
    {
        LOGI("Upgrading client image from v1 to v2");
        // We just need to commit as default values are all 0xFFFF
        to_upgrade = true;
    }
    return to_upgrade;
}

/** storage variables upgrade */
static bool attr_upgrade_needed(void)
{
    uint16_t version, size;
    bool valid = Storage_Driver_getImageInfo(&m_client_image, &version, &size);
    bool to_upgrade = false;

    // If not valid, reset everything
    // if something is wrong
    if (! valid)
    {
        // This function already commits to persistent storage so no need
        // to return true
        // II: Invalid Image
        LOGI("Resetting client image (II)");
        (void)Client_Attr_Manager_reset();
    }
    else
    {
        if ((version != EMPTY_IMAGE_VERSION && version != CLIENT_IMAGE_VERSION))
        {
            switch (version)
            {
                case V1:
                    to_upgrade = v1_to_v2_upgrade_handler(size);
                    break;

                default:
                    // IVU: Invalid Version Upgrade
                    LOGI("Resetting client image (IVU)");
                    (void)Client_Attr_Manager_reset();
                    break;
            }
        }
        else if (size != CLIENT_IMAGE_SIZE)
        {
            // This error should not happen outside of a development cycle
            LOGE("Persistent storage and app client image size differ: %u vs %u, "
                 "did you forget to bump the version?", size, CLIENT_IMAGE_SIZE);
            // IV: Inconsistent Version
            LOGI("Resetting client image (IV)");
            (void)Client_Attr_Manager_reset();
        }
    }
    return to_upgrade;
}

static bool attr_commit(void)
{
    if (! m_client_storage_available)
    {
        LOGW("client attributes are not saved in persistent memory");
        return false;
    }

    if (STORAGE_DRIVER_RES_OK != Storage_Driver_write(&m_client_context,
                                                    (uint8_t *) &m_client_image,
                                                    CLIENT_IMAGE_SIZE))
    {
        LOGW("disabling client attributes saving in persistent"
            "memory");
        m_client_storage_available = false;
        return false;
    }
    return true;
}

static void updateEventLogData(evt_log_data_t * data_p)
{
    client_attribute_table_t * attr_p = &m_client_image.attributes;

    for (uint8_t i = 0; i < EVENT_LOG_TYPE_NB; i++)
    {
        const evt_log_status_t * st = &data_p->log_status[i];
        attr_p->evt_log_data.log_status[i].profile_entries = st->profile_entries;
        attr_p->evt_log_data.log_status[i].current_entry = st->current_entry;
        attr_p->evt_log_data.log_status[i].entries_in_use = st->entries_in_use;
        attr_p->evt_log_data.log_status[i].crc = st->crc;
    }

    attr_p->evt_log_data.first_log = data_p->first_log;
    attr_p->evt_log_data.state = data_p->state;
}

static bool attr_set_defaults(void)
{
    client_attribute_table_t * attr_p = &m_client_image.attributes;
    bool changed = false;
    uint32_t start_time;
    evt_log_data_t evt_log_data;

    start_time = attr_p->bl_start_time;
    if (! Blockload_isStartTimeValid(&start_time))
    {
        LOGI("Changing blockload profile start time value from 0x%08X"
            " to 0x%08X", attr_p->bl_start_time, start_time);
        attr_p->bl_start_time = start_time;
        changed = true;
    }

    start_time = attr_p->dl_start_time;
    if (! Dailyload_isStartTimeValid(&start_time))
    {
        LOGI("Changing dailyload profile start time value from 0x%08X"
            " to 0x%08X", attr_p->dl_start_time, start_time);
        attr_p->dl_start_time = start_time;
        changed = true;
    }

    // Event log data check
    Client_Attribute_Manager_readEventLogData(&evt_log_data);
    if (! Event_Log_Profiles_checkDataValidity(&evt_log_data))
    {
        updateEventLogData(&evt_log_data);
        changed = true;
    }

    return changed;
}

void Client_Attribute_Manager_writeBillingData(const billing_status_t * status_p)
{
    client_attribute_table_t * attr_p = &m_client_image.attributes;

    LOGI("Saving billing profile status");
    attr_p->billing_data.profile_entries = status_p->profile_entries;
    attr_p->billing_data.current_entry = status_p->current_entry;
    attr_p->billing_data.crc = status_p->crc;

    attr_commit();
}

void Client_Attribute_Manager_readBillingData(billing_status_t * status_p)
{
    const client_attribute_table_t * attr_p = &m_client_image.attributes;

    status_p->profile_entries = attr_p->billing_data.profile_entries;
    status_p->current_entry = attr_p->billing_data.current_entry;
    status_p->crc = attr_p->billing_data.crc;
}

void Client_Attribute_Manager_writeBlockLoadData(uint32_t epoch)
{
    LOGI("Saving block load profile start time");
    m_client_image.attributes.bl_start_time = epoch;
    attr_commit();
}

void Client_Attribute_Manager_readBlockLoadData(uint32_t * epoch_p)
{
    *epoch_p = m_client_image.attributes.bl_start_time;
}

void Client_Attribute_Manager_writeDailyLoadData(uint32_t epoch)
{
    LOGI("Saving daily load profile start time");
    m_client_image.attributes.dl_start_time = epoch;
    attr_commit();
}

void Client_Attribute_Manager_readDailyLoadData(uint32_t * epoch_p)
{
    *epoch_p = m_client_image.attributes.dl_start_time;
}

void Client_Attribute_Manager_writeEventLogData(evt_log_data_t * data_p)
{
    LOGI("Saving event log status");
    updateEventLogData(data_p);

    attr_commit();
}

void Client_Attribute_Manager_readEventLogData(evt_log_data_t * data_p)
{
    client_attribute_table_t * attr_p = &m_client_image.attributes;

    LOGI("Reading stored event log status");
    for (uint8_t i = 0; i < EVENT_LOG_TYPE_NB; i++)
    {
        evt_log_status_t * st = &data_p->log_status[i];
        st->profile_entries = attr_p->evt_log_data.log_status[i].profile_entries;
        st->current_entry = attr_p->evt_log_data.log_status[i].current_entry;
        st->entries_in_use = attr_p->evt_log_data.log_status[i].entries_in_use;
        st->crc = attr_p->evt_log_data.log_status[i].crc;
    }
    data_p->first_log = attr_p->evt_log_data.first_log;
    data_p->state = attr_p->evt_log_data.state;
}

void Client_Attribute_Manager_writeExportBillingData(const billing_status_t * status_p)
{
    client_attribute_table_t * attr_p = &m_client_image.attributes;

    LOGI("Saving billing profile status");
    attr_p->export_billing_data.profile_entries = status_p->profile_entries;
    attr_p->export_billing_data.current_entry = status_p->current_entry;
    attr_p->export_billing_data.crc = status_p->crc;

    attr_commit();
}

void Client_Attribute_Manager_readExportBillingData(billing_status_t * status_p)
{
    client_attribute_table_t * attr_p = &m_client_image.attributes;

    status_p->profile_entries = attr_p->export_billing_data.profile_entries;
    status_p->current_entry = attr_p->export_billing_data.current_entry;
    status_p->crc = attr_p->export_billing_data.crc;
}
