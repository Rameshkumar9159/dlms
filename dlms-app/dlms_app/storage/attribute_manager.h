/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef ATTR_MANAGER_H__
#define ATTR_MANAGER_H__

#include <stdint.h>
#include "wms_data.h"

/**
 * \file    attribute_manager.h
 *          Attribute Manager is used to store attributes.
 */

/** Result of writing/reading a parameter */
typedef enum
{
    ATTR_SUCCESS,
    ATTR_STORAGE_ERROR,
    ATTR_UNSUPPORTED_ATTRIBUTE,
    ATTR_INV_LENGTH,
    ATTR_INV_VALUE,
    ATTR_WRITE_ONLY,
    ATTR_ACCESS_DENIED
} attribute_result_e;

#endif /* ATTR_MANAGER_H__ */
