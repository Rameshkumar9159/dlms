/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef READ_ESW_H_
#define READ_ESW_H_

#include <stdint.h>

/**
 * @brief   Start the polling of the ESW at regular interval and
 *          enable the push notifications through GPIOs if available
 */
void Read_Esw_start(void);

/**
 * @brief   Reschedule ASAP the ESW polling FSM if it is not currently running
 */
void Read_Esw_reschedule(void);

#endif /* READ_ESW_H_ */
