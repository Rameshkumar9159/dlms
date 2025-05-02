/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */
#ifndef WIREPAS_COM_H_
#define WIREPAS_COM_H_

#include <stdint.h>
#include <stdbool.h>

#include "api.h"

typedef void (* wirepas_com_first_valid_route_cb_f) (void);
typedef void (* wirepas_com_status_change_cb_f)(bool valid_route);

typedef enum {
    WC_TYPE_UNKNOWN = 1,
    WC_TYPE_PUSH_FIRST = 2,
    WC_TYPE_NIC_STATUS = WC_TYPE_PUSH_FIRST,
    WC_TYPE_NAMEPLATE = 3,
    WC_TYPE_INST_PROF = 4,
    WC_TYPE_BLOCK_LOAD_PROF = 5,
    WC_TYPE_DAILY_PROF = 6,
    WC_TYPE_BILLING_PROF = 7,
    WC_TYPE_EVENT_LOGS = 8,
    WC_TYPE_LAST_GASP = 9,
    WC_TYPE_ESW = 10,
    WC_TYPE_EXPORT_BILLING_PROF = 11,
    WC_TYPE_PUSH_LAST = WC_TYPE_EXPORT_BILLING_PROF,
    WC_TYPE_ON_DEMAND = 32,
} Wirepas_com_traffic_type_e;

void Wirepas_com_init(wirepas_com_first_valid_route_cb_f cb, bool limited_mode);

bool Wirepas_com_send_message(const uint8_t * data, uint16_t len,
                              app_lib_data_data_sent_cb_f sent_cb,
                              Wirepas_com_traffic_type_e type);

void Wirepas_com_subscribeStatusChangeCb(wirepas_com_status_change_cb_f cb);

#endif /* WIREPAS_COM_H_ */
