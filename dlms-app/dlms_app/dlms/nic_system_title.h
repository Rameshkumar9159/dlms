/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef NIC_SYSTEM_TITLE_H_
#define NIC_SYSTEM_TITLE_H_

#include <stdint.h>

void Nic_ST_init(void);

/**
 * @brief   Get Nic System Title
 */
bool Nic_ST_get(const uint8_t ** title_pp, uint8_t * title_len_p);

#endif /* NIC_SYSTEM_TITLE_H_ */
