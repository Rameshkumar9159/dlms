/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    name_plate_profile.h
 * \brief   common interface for the DLMS code that retrieve meter ids
 */

#ifndef NAME_PLATE_PROFILE_H_
#define NAME_PLATE_PROFILE_H_

#include "common.h"

typedef void (* dlms_np_read_name_plate_cb) (bool read_ok);

/**
 * @brief   Read the name plate profile
 * @param   cb
 *          Callback to be called once name plate is successfuly read
 */
void Name_Plate_Profile_read(dlms_np_read_name_plate_cb cb);

#endif // NAME_PLATE_PROFILE_H_
