/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef EXPORT_BILLING_PROFILE_H_
#define EXPORT_BILLING_PROFILE_H_

#include <stdint.h>

/**
 * \brief   Start the periodic export billing profile reading
 * \param   delay_ms
 *          Delay before the first reading in ms
 */
void Export_Billing_profile_start(uint32_t delay_ms);

#endif /* EXPORT_BILLING_PROFILE_H_ */
