/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef CLIENT_ATTR_MANAGER_H__
#define CLIENT_ATTR_MANAGER_H__

#include <stdint.h>
#include "wms_data.h"
#include "attribute_manager.h"
#include "billing_profile.h"
#include "event_log_profiles.h"

/**
 * @brief   Initialize the attribute manager for the client
 * @return  Result of the operation \ref attribute_result_e
 */
attribute_result_e Client_Attr_Manager_init(void);

/**
 * @brief   Reinitializes the attributes to their initial value
 * @return  Result of the operation \ref attribute_result_e
 */
attribute_result_e Client_Attr_Manager_reset(void);

/**
 * @brief   Store the status of the billing profile
 * @param   status_p
 *          Pointer to the new status of the billing data
 * @return  None
 */
void Client_Attribute_Manager_writeBillingData(const billing_status_t * status_p);

/**
 * @brief   Read back the status of the billing profile
 * @param   status_p
 *          Pointer to the new status of the billing data
 * @return  Result of operation
 */
void Client_Attribute_Manager_readBillingData(billing_status_t * status_p);

void Client_Attribute_Manager_writeBlockLoadData(uint32_t epoch);
void Client_Attribute_Manager_readBlockLoadData(uint32_t * epoch_p);

void Client_Attribute_Manager_writeDailyLoadData(uint32_t epoch);
void Client_Attribute_Manager_readDailyLoadData(uint32_t * epoch_p);

void Client_Attribute_Manager_writeEventLogData(evt_log_data_t * data_p);
void Client_Attribute_Manager_readEventLogData(evt_log_data_t * data_p);

void Client_Attribute_Manager_writeExportBillingData(const billing_status_t * status_p);
void Client_Attribute_Manager_readExportBillingData(billing_status_t * status_p);

#endif /* CLIENT_ATTR_MANAGER_H__ */
