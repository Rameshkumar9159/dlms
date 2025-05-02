/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef EXTFLASH_TESTS_H_
#define EXTFLASH_TESTS_H_

#include "dut_protocol.h"

/**
 * \brief 100 ms is the maximum task execution time allowed by the scheduler
 * but it's not enough to run some of the tests. Thus, we are lying to
 * the scheduler but this is not an issue because we don't start the stack
 */
#define TEST_EXEC_TIME_US 10000

/**
 * \brief Sets the callback that will be used after the extflash tests are
 * performed
 *
 * \param end_cb End of External Flash testing callback
 */
void extflash_tests_set_end_cb(void (*end_cb)(test_status_e, app_lib_mem_area_info_t));

/**
 * \brief Resets the FSM State to perform a new test
 *
 */
void extflash_tests_reset_fsm_state(void);

/**
 * @brief   task implementing the FSM to run the tests
 * @return  the delay before the next execution of this callback
 */
uint32_t extflash_tests_fsm_task(void);

#endif  // EXTFLASH_TESTS_H_
