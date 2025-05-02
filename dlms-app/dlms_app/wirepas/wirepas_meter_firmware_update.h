
/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */
#ifndef WIREPAS_METER_FIRMWARE_UPDATE_H_
#define WIREPAS_METER_FIRMWARE_UPDATE_H_

#include <stdint.h>
#include <stdbool.h>

#include "firmware_update.h"

void Wirepas_Meter_Firmware_Update_init(void);

void Wirepas_Meter_Firmware_Update_notify_status(firmware_update_status_t * status_p);

#endif /* WIREPAS_METER_FIRMWARE_UPDATE_H_ */
