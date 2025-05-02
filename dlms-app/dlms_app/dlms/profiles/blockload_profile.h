/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef BLOCKLOAD_PROFILE_H_
#define BLOCKLOAD_PROFILE_H_

#include <stdint.h>

/**
 * @brief   Verify that the block load start time saved in persistent storage
 *          is valid
 * @param   time_p
 *          pointer to the start time saved in persistent storage
 * @return  true: valid, false: invalid
 */
bool Blockload_isStartTimeValid(uint32_t * time_p);

/**
 * @brief   Start the periodic block load profile reading
 */
void Blockload_profile_start(void);

#endif /* BLOCKLOAD_PROFILE_H_ */
