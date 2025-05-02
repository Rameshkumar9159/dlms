/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    data_notification.c
 * \brief   service to generate data notification
 */
#if defined METER_RETROFIT & defined METER_RETROFIT_FLAG_ID
#include <stdio.h>
#endif

#include "data_notification.h"
#include "security_material.h"

//Gurux DLMS includes.
#include "include/cosem.h" // cosem_setStructure
#include "include/notify.h" // notify_generateDataNotificationMessages2
#include "meter_clock.h"
#include "nic_system_title.h"

#define DEBUG_LOG_MODULE_NAME "DATANOTIF"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

static void setup_secured_client(dlmsSettings * dlms_settings)
{
    ciphering * cipher_p = &dlms_settings->cipher;
    uint8_t * secret_p = NULL;
    uint8_t * enc_key_p = NULL;
    uint8_t * auth_key_p = NULL;
    uint8_t secret_len = 0;
    uint8_t enc_key_len = 0;
    uint8_t auth_key_len = 0;
    const uint8_t * system_title_p = NULL;
    uint8_t system_title_len = 0;

    Security_Material_get_us_secret(&secret_p, &secret_len);

    // retrieve NIC system title
    Nic_ST_get(&system_title_p, &system_title_len);

    cl_init(dlms_settings, 1, NIC_SERVER_ADDRESS, PUSH_CLIENT_ADDRESS,
            DLMS_AUTHENTICATION_HIGH, NULL, DLMS_INTERFACE_TYPE_WRAPPER);

    // Set the US password
    bb_clear(&dlms_settings->password);
    bb_set(&dlms_settings->password, secret_p, secret_len);

    // Ciphering configuration
    cipher_p->security = DLMS_SECURITY_AUTHENTICATION_ENCRYPTION;
    cipher_p->suite = DLMS_SECURITY_SUITE_V0;

    // No need to allocate challenges as no association done
    // TODO: invocation counter needed?
    //cipher_p->invocationCounter = ???

    // Keys
    Security_Material_get_keys(&enc_key_p, &enc_key_len, &auth_key_p, &auth_key_len);
    bb_clear(&cipher_p->authenticationKey);
    bb_set(&cipher_p->authenticationKey, auth_key_p, auth_key_len);
    bb_clear(&cipher_p->blockCipherKey);
    bb_set(&cipher_p->blockCipherKey, enc_key_p, enc_key_len);

    // System title
    bb_clear(&cipher_p->systemTitle);
    bb_attach(&cipher_p->systemTitle, (uint8_t *) system_title_p, system_title_len, system_title_len);
}

static int add_date_time(gxByteBuffer * reply_p)
{
    // Data is send in octet string. Remove data type.
    dlmsVARIANT tmp;
    gxtime t;
    uint32_t epoch;
    int16_t deviation;

    MeterClock_get(&epoch, &deviation);
    tmp.dateTime = &t;
    tmp.vt = DLMS_DATA_TYPE_DATETIME;
    time_initUnix(&t, epoch);
    t.deviation = deviation;
    return dlms_setData(reply_p, DLMS_DATA_TYPE_OCTET_STRING, &tmp);
}

/**
 * @brief       Generate a DataNotification message
 * @param[in]   aa_type
 *              type of AA: use to decide if notification has to be encrypted or not
 * @param[in]   push_obis
 *              OBIS code of the generated push
 * @param[in]   pull_data
 *              pointer on the byte buffer containing the pulled data
 * @param[out]  push_msg_p
 *              pointer on the generated DataNotification
 * @return      true if message has been generated, false otherwise
 */
bool Data_Notification_generatePushFromPull(mcm_aa_e aa_type,
                                            obis_code_t push_obis,
                                            gxByteBuffer * pulled_data_p,
                                            message * push_msg_p)
{
    dlmsSettings dlms_settings;
    gxByteBuffer push_body;
    const uint8_t * dev_id_p;
    uint8_t dev_id_len;
    int32_t res;
    uint32_t meter_epoch;

#ifdef METER_RETROFIT
    // Device id does not exist, so serial number is used instead
    Server_Attribute_Manager_readMeterSerialNumber(&dev_id_p, &dev_id_len);
#ifdef METER_RETROFIT_FLAG_ID
    char device_id[METER_DEVICE_ID_MAX_SIZE];
    snprintf(device_id, sizeof(device_id), "%.3s%s", METER_RETROFIT_FLAG_ID, (const char *)dev_id_p);
    dev_id_p = (const uint8_t *)device_id;
    dev_id_len = strlen(device_id);
#endif
#else
    Server_Attribute_Manager_readMeterDeviceId(&dev_id_p, &dev_id_len);
#endif

    memset(&dlms_settings, 0, sizeof(dlms_settings));
    bb_init(&push_body);

    switch (aa_type)
    {
        case MCM_AA_PC:
            // Create client to generate the push
            cl_init(&dlms_settings, 1, NIC_SERVER_ADDRESS, PUSH_CLIENT_ADDRESS,
                    DLMS_AUTHENTICATION_NONE, NULL, DLMS_INTERFACE_TYPE_WRAPPER);
            break;
        case MCM_AA_US:
            setup_secured_client(&dlms_settings);
            break;
        default:
            return false;
    }

    // The pull body is a structure containing:
    // 1- the logical device name
    // 2- the OBIS of the push
    // 3- the real time clock
    // 4- the pulled data
    // Create a structure with thre correct number of entries
    if ((res = cosem_setStructure(&push_body, 4)) == DLMS_ERROR_CODE_OK &&
        // First entry is Device id
        (res = cosem_setOctetString2(&push_body, dev_id_p,
                                     dev_id_len)) == DLMS_ERROR_CODE_OK &&
        // Second entry is the push obis
        (res = cosem_setOctetString2(&push_body, push_obis, OBIS_CODE_SIZE)) ==
                                                        DLMS_ERROR_CODE_OK &&
        // Third entry is the real time clock
        (res = add_date_time(&push_body)) == DLMS_ERROR_CODE_OK &&
        // Last entry is the content of the profile (that is an array)
        (res = bb_set2(&push_body, pulled_data_p, pulled_data_p->position,
                       bb_available(pulled_data_p))) == DLMS_ERROR_CODE_OK)
    {
        MeterClock_get(&meter_epoch, NULL);
        res = notify_generateDataNotificationMessages2(&dlms_settings,
                                                       0,
                                                       &push_body, push_msg_p);
    }

    // Clear our temporary buffer
    cl_clear(&dlms_settings);
    bb_clear(&push_body);

    if (DLMS_ERROR_CODE_OK != res)
    {
        // Something went wrong
        // TODO: make multiple checks to know where the problem is
        LOGE("generatePushFromPull error : %d", res);
        return false;
    }
    return true;
}
