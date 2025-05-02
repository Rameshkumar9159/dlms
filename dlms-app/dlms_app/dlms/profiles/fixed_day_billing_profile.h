/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef FIXED_DAY_BILLING_PROFILE_H_
#define FIXED_DAY_BILLING_PROFILE_H_

#include <stdint.h>

/**
 * \brief   Start the fixed date billing profile reading
 * \param   delay_ms
 *          Delay before the first reading in ms
 */
void Fixed_Day_Billing_Profile_start(void);

/**
 * \brief   Check if the fixed day for billing is valid
 * \param   billing_day
 *          day to validate
 * \return  true if day is valid, false otherwise
 */
bool Fixed_Day_Billing_Day_isValid(uint8_t billing_day);

/**
 * \brief   update the fixed day of the billing day
 * \param   new_day
 *          New day for for the fixed day billing, disable if = 0xFF
 */
void Fixed_Day_Billing_Profile_updateBillingDay(uint8_t new_day);

#endif /* FIXED_DAY_BILLING_PROFILE_H_ */
