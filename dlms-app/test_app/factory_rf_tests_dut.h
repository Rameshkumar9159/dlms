/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef FACTORY_RF_TESTS_DUT_H_
#define FACTORY_RF_TESTS_DUT_H_

#include "dut_protocol.h"
#include "wms_settings.h"

/**
 * \brief Initializes the DUT RF Testing
 *
 * \param rf_test_params RSSI Measurements Thresholds to define RF Test success
 * or failure
 * \param router_addr Router Address to communicate to during the RF testing
 * \param end_cb Callback executed once the test is done
 */
void factory_rf_tests_dut_init(rf_testing_parameters_t rf_test_params,
                               app_addr_t              router_addr,
                               void (*end_cb)(test_status_e, int8_t, int8_t));

#endif  // FACTORY_RF_TESTS_DUT_H_
