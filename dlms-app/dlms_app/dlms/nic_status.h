/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef NIC_STATUS_H_
#define NIC_STATUS_H_

#include <stdint.h>
#include "application_association.h"
#include "firmware_update.h"

// Bitfield
typedef enum
{
    STATUS_REASON_NO_REASON                     = 0x00,
    STATUS_REASON_COMMUNICATION_PROBLEM         = 0x01,
    STATUS_REASON_NIC_REBOOT                    = 0x02,
    STATUS_REASON_NIC_REGISTRATION              = 0x04,
    STATUS_REASON_METER_ASSOCIATION_ISSUES      = 0x08,
    STATUS_REASON_SINK_CHANGE                   = 0x10
} status_reason_e;

/* Send NIC status as per the */
void Nic_status_generate_and_send_notification(status_reason_e status_reason);

void Nic_status_set_aa_state(aa_type_e type, bool can_connect);

/* Expose meter connection status & NIC status to other modules. */
void Nic_status_compute_statuses(uint8_t * const connection_status,
                                    uint8_t * const status_reason);

#endif /* NIC_STATUS_H_ */
