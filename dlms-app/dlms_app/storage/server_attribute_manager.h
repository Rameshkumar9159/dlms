/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef SERVER_ATTR_MANAGER_H__
#define SERVER_ATTR_MANAGER_H__

#include <stdint.h>
#include "wms_data.h"
#include "attribute_manager.h"

// Security material
#define SECRET_MAX_SIZE                 32
#define KEY_SIZE                        16

// Meter identification
#define METER_FLAG_ID_LEN               3
#define METER_SERIAL_NUMBER_MAX_SIZE    13
#define METER_DEVICE_ID_MAX_SIZE        (METER_SERIAL_NUMBER_MAX_SIZE + METER_FLAG_ID_LEN)
#define SYSTEM_TITLE_SIZE               8

typedef struct __attribute__ ((__packed__))
{
    uint8_t mr_secret[SECRET_MAX_SIZE];
    uint8_t us_secret[SECRET_MAX_SIZE];
    uint8_t fu_secret[SECRET_MAX_SIZE];
    uint8_t encryption_key[KEY_SIZE];
    uint8_t authentication_key[KEY_SIZE];
    uint8_t key_encryption_key[KEY_SIZE];
    uint8_t mr_secret_len;
    uint8_t us_secret_len;
    uint8_t fu_secret_len;
} security_material_t;

typedef enum {
    SERVER_ATTRIBUTE_MSM_TYPE_CURRENT,
    SERVER_ATTRIBUTE_MSM_TYPE_NEW,
} Server_Attr_Manager_meterSecurityMaterial_type_t;

typedef enum
{
    METER_ID_SERIAL_NUMBER,
    METER_ID_DEVICE_ID,
    METER_ID_SYTEM_TITLE
} meter_id_e;

typedef struct __attribute__ ((__packed__))
{
    uint8_t serial_number[METER_SERIAL_NUMBER_MAX_SIZE];
    uint8_t device_id[METER_DEVICE_ID_MAX_SIZE];
    uint8_t system_title[SYSTEM_TITLE_SIZE];
    uint8_t serial_number_len;
    uint8_t device_id_len;
    uint8_t system_title_len;
} meter_identification_t;

typedef enum
{
    PROF_PUSH_BIT_FIRST,
    PROF_PUSH_INSTANTANEOUS_BIT = PROF_PUSH_BIT_FIRST,
    PROF_PUSH_BLOCK_LOAD_BIT,
    PROF_PUSH_DAILY_LOAD_BIT,
    PROF_PUSH_BILLING_BIT,
    PROF_PUSH_FIRST_EVENT_LOG_BIT,
    PROF_PUSH_VOLTAGE_EVENT_LOG_BIT = PROF_PUSH_FIRST_EVENT_LOG_BIT,
    PROF_PUSH_CURRENT_EVENT_LOG_BIT,
    PROF_PUSH_POWER_EVENT_LOG_BIT,
    PROF_PUSH_TRANSACTION_EVENT_LOG_BIT,
    PROF_PUSH_OTHER_EVENT_LOG_BIT,
    PROF_PUSH_NON_ROLLOVER_EVENT_LOG_BIT,
    PROF_PUSH_CONTROL_EVENT_LOG_BIT,
    PROF_PUSH_LAST_EVENT_LOG_BIT = PROF_PUSH_CONTROL_EVENT_LOG_BIT,
    PROF_PUSH_EXPORT_BILLING_BIT,
    PROF_PUSH_BIT_LAST = PROF_PUSH_EXPORT_BILLING_BIT,
    PROF_PUSH_BIT_NB,
} prof_push_bit_e;

#define PROF_PUSH_CFG_BYTE_SIZE     ((PROF_PUSH_BIT_NB / 8) + \
                                     (PROF_PUSH_BIT_NB % 8 ? 1 : 0))

#define PUSH_PROF_IS_ENABLED(cfg, bit) ((cfg) & (1 << (15 - (bit))))
#define PUSH_PROF_SET_BIT(cfg, bit) \
do \
{ \
    if ((bit) <= 15) \
    { \
        (cfg) |= (1 << (15 - (bit))); \
    } \
} while (0)

#define PUSH_PROF_UNSET_BIT(cfg, bit) \
do \
{ \
    if ((bit) <= 15) \
    { \
        (cfg) &= ~(1 << (15 - (bit))); \
    } \
} while (0)

#define INSTANTANEOUS_PUSH_PROF_IS_ENABLED(cfg) \
            PUSH_PROF_IS_ENABLED(cfg, PROF_PUSH_INSTANTANEOUS_BIT)
#define BLOCKLOAD_PUSH_PROF_IS_ENABLED(cfg) \
            PUSH_PROF_IS_ENABLED(cfg, PROF_PUSH_BLOCK_LOAD_BIT)
#define DAILYLOAD_PUSH_PROF_IS_ENABLED(cfg) \
            PUSH_PROF_IS_ENABLED(cfg, PROF_PUSH_DAILY_LOAD_BIT)
#define BILLING_PUSH_PROF_IS_ENABLED(cfg) \
            PUSH_PROF_IS_ENABLED(cfg, PROF_PUSH_BILLING_BIT)
#define EXPORT_BILLING_PUSH_PROF_IS_ENABLED(cfg) \
            PUSH_PROF_IS_ENABLED(cfg, PROF_PUSH_EXPORT_BILLING_BIT)

#define FIXED_DAY_BILLING_DAY_DISABLED          0xFF

/**
 * @brief   Initialize the attribute manager for the server
 * @return  Result of the operation \ref attribute_result_e
 */
attribute_result_e Server_Attr_Manager_init(void);

/**
 * @brief   Reinitializes the attributes to their initial value
 * @return  Result of the operation \ref attribute_result_e
 */
attribute_result_e Server_Attr_Manager_reset(void);

/**
 * @brief   Commit the attributes to storage
 * @return  Result of the operation \ref attribute_result_e
 */
attribute_result_e Server_Attr_Manager_commit(void);

bool Server_Attribute_Manager_writeMeterSecurityMaterial(Server_Attr_Manager_meterSecurityMaterial_type_t type,
                                                         const security_material_t * msm_p);

bool Server_Attribute_Manager_readMeterSecurityMaterial(Server_Attr_Manager_meterSecurityMaterial_type_t type,
                                                        security_material_t ** msm_p);

void Server_Attribute_Manager_writeInstantaneousPushConfig(uint16_t period_m);
void Server_Attribute_Manager_readInstantaneousPushConfig(uint16_t * period_m_p);

void Server_Attribute_Manager_writeProfilePushConfig(uint16_t cfg);
void Server_Attribute_Manager_readProfilePushConfig(uint16_t * cfg_p);

void Server_Attribute_Manager_writeTransparentConfig(bool enabled);
void Server_Attribute_Manager_readTransparentConfig(bool * enabled_p);

void Server_Attribute_Manager_writeEncryptInvocationCounterNoCommit(uint32_t ic);
void Server_Attribute_Manager_readEncryptInvocationCounter(uint32_t * ic_p);

void Server_Attribute_Manager_writeDecryptInvocationCounterNoCommit(uint32_t ic);
void Server_Attribute_Manager_readDecryptInvocationCounter(uint32_t * ic_p);

void Server_Attribute_Manager_resetInvocationCounters(void);

void Server_Attribute_Manager_writeMeterSerialNumber(const uint8_t * id_p, uint8_t size);
void Server_Attribute_Manager_readMeterSerialNumber(const uint8_t ** id_pp, uint8_t * size_p);
void Server_Attribute_Manager_writeMeterDeviceId(const uint8_t * id_p, uint8_t size);
void Server_Attribute_Manager_readMeterDeviceId(const uint8_t ** id_pp, uint8_t * size_p);

void Server_Attribute_Manager_writeNicRegistrationStatus(bool registered);
void Server_Attribute_Manager_readNicRegistrationStatus(bool * registered_p);

void Server_Attribute_Manager_writeMeterFirmwareUpdateActivationOngoing(bool ongoing);
void Server_Attribute_Manager_readMeterFirmwareUpdateActivationOngoing(bool * ongoing_p);

void Server_Attribute_writeFixedDayBillingInfo(uint8_t billing_day, uint32_t billing_date);
void Server_Attribute_readFixedDayBillingInfo(uint8_t * billing_day_p, uint32_t * billing_date_p);

#endif /* SERVER_ATTR_MANAGER_H__ */
