/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef RNG_H_
#define RNG_H_

#include <stdint.h>

void Rng_init(void);

/**
 * @brief   Get random value within [0:limit]
 */
uint32_t Rng_number(uint32_t limit);

#endif /* READ_ESW_H_ */
