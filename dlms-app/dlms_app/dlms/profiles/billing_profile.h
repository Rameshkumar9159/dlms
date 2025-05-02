/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef BILLING_PROFILE_H_
#define BILLING_PROFILE_H_

#include <stdint.h>

typedef struct
{
    // The number of entries in this profile
    uint16_t profile_entries;
    // The last entry that has been read
    uint16_t current_entry;
    // The CRC of the last entry that has been read
    uint16_t crc;
} billing_status_t;

/**
 * \brief   Start the periodic billing profile reading
 * \param   delay_ms
 *          Delay before the first reading in ms
 */
void Billing_profile_start(uint32_t delay_ms);

#endif /* BILLING_PROFILE_H_ */
