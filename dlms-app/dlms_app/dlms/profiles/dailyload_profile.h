/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef DAILYLOAD_PROFILE_H_
#define DAILYLOAD_PROFILE_H_

#include <stdint.h>

/**
 * @brief   Verify that the daily load start time saved in persistent storage
 *          is valid
 * @param   time_p
 *          pointer to the start time saved in persistent storage
 * @return  true: valid, false: invalid
 */
bool Dailyload_isStartTimeValid(uint32_t * time_p);

/**
 * \brief   Start the periodic daily load profile reading
 * \param   delay_ms
 *          Delay before the first reading in ms
 */
void Dailyload_profile_start(uint32_t delay_ms);

#endif /* DAILYLOAD_PROFILE_H_ */
