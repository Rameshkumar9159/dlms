/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    opt_passthrough.h
 * \brief
 */

#ifndef OPT_PASSTHROUGH_H_
#define OPT_PASSTHROUGH_H_

#include <stdint.h>

#include "meter_connection_management.h"

bool Opt_Passthrough_handle_passthrough_message(const uint8_t * message,
                                                size_t size,
                                                mcm_aa_e aa_type);

#endif /* OPT_PASSTHROUGH_H_ */
