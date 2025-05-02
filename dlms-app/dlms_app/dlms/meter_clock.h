/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef METER_CLOCK_H_
#define METER_CLOCK_H_

#include <stdint.h>

/**
 * @brief   Set meter clock and deviation
 * @Note    It doesn't update meter clock, but set it inside the nic
 */
void MeterClock_set(uint32_t epoch, int16_t deviation);

/**
 * @brief   Get meter clock and deviation
 */
bool MeterClock_get(uint32_t * epoch_p, int16_t * deviation_p);

/**
 * @brief   Store the new clock to be updated to the meter
 * @param   epoch Time to set (0 mean nothing to update)
*/
void MeterClock_set_new_meter_clock(uint32_t epoch, int16_t deviation);

/**
 * @brief   Get the stored new clock to be updated to the meter
 * @return  epoch Time to set (0 mean nothing to update)
 * @note    Time elapsed between MeterClock_set_new_meter_clock and this call is compensated internally
*/
bool MeterClock_get_new_meter_clock(uint32_t * epoch_p, int16_t * deviation_p);



#endif /* METER_CLOCK_H_ */
