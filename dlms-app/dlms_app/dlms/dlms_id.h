/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    dlms_id.h
 * \brief   common interface for the DLMS code that retrieve meter ids
 */

#ifndef DLMS_ID_H_
#define DLMS_ID_H_

#include "common.h"

typedef void (* dlms_id_read_meter_serial_number_cb) (bool read_ok);

void Dlms_Id_readSerialNumber(dlms_id_read_meter_serial_number_cb cb);

#endif // DLMS_ID_H_
