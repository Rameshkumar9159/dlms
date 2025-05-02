/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    security_service.h
 * \brief   interface for the update of the security material in the smart meters
 */
#ifndef SECURITY_SERVICE_H_
#define SECURITY_SERVICE_H_

#include <stdint.h>

#include "common.h"

/**
 * @brief   Perfom update of the secrets and keys in the meter
 * @param   cb
 *          Callback when updates have been performed done or a failure happened
 * @return  true if update operation will be performed, false otherwise
*/
bool Security_Service_updateSecurity(operation_result_cb cb);

#endif // SECURITY_SERVICE_H_
