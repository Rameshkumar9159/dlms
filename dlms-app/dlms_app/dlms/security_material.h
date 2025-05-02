/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    security_material.h
 * \brief   This module centralize the mananagement of security materials
 *          (Keys and Secrets for different association level)
 */

#include <stdint.h>
#include <stdbool.h>

#ifndef SECURITY_MATERIAL_H_
#define SECURITY_MATERIAL_H_

#include "server_attribute_manager.h"

#define SECRET_MAX_SIZE 32
#define KEY_SIZE        16

void Security_Material_init(void);

bool Security_Material_get_mr_secret(uint8_t ** secret_pp, uint8_t * secret_len_p);

bool Security_Material_get_us_secret(uint8_t ** secret_pp, uint8_t * secret_len_p);

bool Security_Material_get_fu_secret(uint8_t ** secret_pp, uint8_t * secret_len_p);

bool Security_Material_get_keys(uint8_t ** enc_key_pp, uint8_t * enc_key_len_p,
                                uint8_t ** auth_key_pp, uint8_t * auth_key_len_p);

bool Security_Material_get_current_key_encryption_key(uint8_t ** kek_pp,
                                                      uint8_t * kek_len_p);

bool Security_Material_store_new_mr_secret(const uint8_t * secret_p, uint8_t secret_len);

bool Security_Material_store_new_us_secret(const uint8_t * secret_p, uint8_t secret_len);

bool Security_Material_store_new_fu_secret(const uint8_t * secret_p, uint8_t secret_len);

bool Security_Material_store_new_enc_key(const uint8_t * enc_key_p, uint8_t enc_key_len);

bool Security_Material_store_new_auth_key(const uint8_t * auth_key_p, uint8_t auth_key_len);

bool Security_Material_store_new_key_encryption_key(const uint8_t * kek_p, uint8_t kek_len);

bool Security_Material_is_new_material_available(void);

bool Security_Material_switch_to_new_material(void);

const security_material_t * Security_Material_get_new_material(void);

const security_material_t * Security_Material_get_current_material(void);

bool Security_Material_is_mr_secret_changed(void);
bool Security_Material_is_us_secret_changed(void);
bool Security_Material_is_fu_secret_changed(void);
bool Security_Material_is_enc_key_changed(void);
bool Security_Material_is_auth_key_changed(void);
bool Security_Material_is_key_encryption_key_changed(void);

#endif // SECURITY_MATERIAL_H_
