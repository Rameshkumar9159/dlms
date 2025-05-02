/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include <stddef.h>
#include <string.h>

#include "security_material.h"
#include "server_attribute_manager.h"

#define DEBUG_LOG_MODULE_NAME "SECU_MAT"
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"


void Security_Material_init(void)
{
    // Nothing todo for now
}

bool Security_Material_get_mr_secret(uint8_t ** secret_pp, uint8_t * secret_len_p)
{
    security_material_t * msm;
    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_CURRENT, &msm);

    *secret_pp = msm->mr_secret;
    *secret_len_p = msm->mr_secret_len;

    return true;
}

bool Security_Material_get_us_secret(uint8_t ** secret_pp, uint8_t * secret_len_p)
{
    security_material_t * msm;
    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_CURRENT, &msm);

    *secret_pp = msm->us_secret;
    *secret_len_p = msm->us_secret_len;

    return true;
}

bool Security_Material_get_fu_secret(uint8_t ** secret_pp, uint8_t * secret_len_p)
{
    security_material_t * msm;
    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_CURRENT, &msm);

    *secret_pp = msm->fu_secret;
    *secret_len_p = msm->fu_secret_len;

    return true;
}

bool Security_Material_get_keys(uint8_t ** enc_key_pp, uint8_t * enc_key_len_p,
                                uint8_t ** auth_key_pp, uint8_t * auth_key_len_p)
{
    security_material_t * msm;
    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_CURRENT, &msm);

    *enc_key_pp = msm->encryption_key;
    *enc_key_len_p = KEY_SIZE;

    *auth_key_pp = msm->authentication_key;
    *auth_key_len_p = KEY_SIZE;

    return true;
}

bool Security_Material_get_current_key_encryption_key(uint8_t ** kek_pp,
                                                      uint8_t * kek_len_p)
{
    security_material_t * msm;

    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_CURRENT, &msm);

    *kek_pp = msm->key_encryption_key;
    *kek_len_p = KEY_SIZE;

    return true;
}

static bool update_secret(const uint8_t * secret_p, uint8_t secret_len, size_t offset_secret, size_t offset_secret_len)
{
    security_material_t * sm_p;
    if (secret_len > SECRET_MAX_SIZE)
    {
        return false;
    }
    // Read current new config
    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_NEW, &sm_p);
    // Update secret
    memset(((uint8_t *) sm_p) + offset_secret, 0x00, SECRET_MAX_SIZE);
    memcpy(((uint8_t *) sm_p) + offset_secret, secret_p, secret_len);
    *(((uint8_t *) sm_p) + offset_secret_len) = secret_len;

    // Write back result
    Server_Attribute_Manager_writeMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_NEW, sm_p);

    return true;
}

static bool update_key(const uint8_t * key_p, size_t offset_key)
{
    security_material_t * sm_p;

    // Read current new config
    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_NEW, &sm_p);
    // Update secret
    memcpy(((uint8_t *) sm_p) + offset_key, key_p, KEY_SIZE);

    // Write back result
    Server_Attribute_Manager_writeMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_NEW, sm_p);

    return true;
}

bool Security_Material_store_new_mr_secret(const uint8_t * secret_p, uint8_t secret_len)
{
    return update_secret(secret_p,
                         secret_len,
                         offsetof(security_material_t, mr_secret),
                         offsetof(security_material_t, mr_secret_len));
}

bool Security_Material_store_new_us_secret(const uint8_t * secret_p, uint8_t secret_len)
{
    return update_secret(secret_p,
                         secret_len,
                         offsetof(security_material_t, us_secret),
                         offsetof(security_material_t, us_secret_len));
}

bool Security_Material_store_new_fu_secret(const uint8_t * secret_p, uint8_t secret_len)
{
    return update_secret(secret_p,
                         secret_len,
                         offsetof(security_material_t, fu_secret),
                         offsetof(security_material_t, fu_secret_len));
}

bool Security_Material_store_new_enc_key(const uint8_t * enc_key_p, uint8_t enc_key_len)
{
    if (enc_key_len != KEY_SIZE)
    {
        return false;
    }

    return update_key(enc_key_p,
                      offsetof(security_material_t, encryption_key));
}


bool Security_Material_store_new_auth_key(const uint8_t * auth_key_p, uint8_t auth_key_len)
{
    if (auth_key_len != KEY_SIZE)
    {
        return false;
    }

    return update_key(auth_key_p,
                      offsetof(security_material_t, authentication_key));
}

bool Security_Material_store_new_key_encryption_key(const uint8_t * kek_p,
                                                    uint8_t kek_len)
{
    if (kek_len != KEY_SIZE)
    {
        return false;
    }

    return update_key(kek_p, offsetof(security_material_t, key_encryption_key));
}

bool Security_Material_is_new_material_available(void)
{
    security_material_t * sm_new_p = NULL;
    security_material_t * sm_cur_p = NULL;
    // Compare new vs current
    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_NEW, &sm_new_p);
    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_CURRENT, &sm_cur_p);

    return (memcmp(sm_new_p, sm_cur_p, sizeof(security_material_t)) != 0);
}

bool Security_Material_switch_to_new_material(void)
{
    security_material_t * sm_new_p = NULL;

    // Update the config
    // From this point, new config will be used everywhere, ie when communicating with meter or HES

    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_NEW, &sm_new_p);
    Server_Attribute_Manager_writeMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_CURRENT, sm_new_p);

    return true;
}

const security_material_t * Security_Material_get_new_material(void)
{
    security_material_t * sm_new_p = NULL;

    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_NEW, &sm_new_p);

    return sm_new_p;
}

const security_material_t * Security_Material_get_current_material(void)
{
    security_material_t * sm_cur_p = NULL;

    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_CURRENT, &sm_cur_p);

    return sm_cur_p;
}

bool Security_Material_is_mr_secret_changed(void)
{
    security_material_t * sm_cur_p = NULL;
    security_material_t * sm_new_p = NULL;

    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_NEW, &sm_new_p);
    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_CURRENT, &sm_cur_p);

    return sm_new_p->mr_secret_len != sm_cur_p->mr_secret_len ||
           memcmp(sm_new_p->mr_secret, sm_cur_p->mr_secret, sm_new_p->mr_secret_len);
}

bool Security_Material_is_us_secret_changed(void)
{
    security_material_t * sm_cur_p = NULL;
    security_material_t * sm_new_p = NULL;

    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_NEW, &sm_new_p);
    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_CURRENT, &sm_cur_p);

    return sm_new_p->us_secret_len!= sm_cur_p->us_secret_len ||
           memcmp(sm_new_p->us_secret, sm_cur_p->us_secret, sm_new_p->us_secret_len);
}

bool Security_Material_is_fu_secret_changed(void)
{
    security_material_t * sm_cur_p = NULL;
    security_material_t * sm_new_p = NULL;

    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_NEW, &sm_new_p);
    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_CURRENT, &sm_cur_p);

    return sm_new_p->fu_secret_len!= sm_cur_p->fu_secret_len ||
           memcmp(sm_new_p->fu_secret, sm_cur_p->fu_secret, sm_new_p->fu_secret_len);
}

bool Security_Material_is_enc_key_changed(void)
{
    security_material_t * sm_cur_p = NULL;
    security_material_t * sm_new_p = NULL;

    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_NEW, &sm_new_p);
    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_CURRENT, &sm_cur_p);

    return memcmp(sm_new_p->encryption_key, sm_cur_p->encryption_key, KEY_SIZE);
}

bool Security_Material_is_auth_key_changed(void)
{
    security_material_t * sm_cur_p = NULL;
    security_material_t * sm_new_p = NULL;

    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_NEW, &sm_new_p);
    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_CURRENT, &sm_cur_p);

    return memcmp(sm_new_p->authentication_key, sm_cur_p->authentication_key, KEY_SIZE);
}

bool Security_Material_is_key_encryption_key_changed(void){
    security_material_t * sm_cur_p = NULL;
    security_material_t * sm_new_p = NULL;

    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_NEW, &sm_new_p);
    Server_Attribute_Manager_readMeterSecurityMaterial(SERVER_ATTRIBUTE_MSM_TYPE_CURRENT, &sm_cur_p);

    return memcmp(sm_new_p->key_encryption_key, sm_cur_p->key_encryption_key, KEY_SIZE);
}
