/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    data_notification.h
 * \brief   interface to generate data notification
 */

#ifndef DATA_NOTIFICATION_H_
#define DATA_NOTIFICATION_H_

#include <stdint.h>

#include "common.h"
#include "meter_connection_management.h"
#include "include/bytebuffer.h"

/**
 * @brief       Generate a DataNotification message
 * @param[in]   aa_type
 *              type of AA: use to decide if notification has to be encrypted or not
 * @param[in]   push_obis
 *              OBIS code of the generated push
 * @param[in]   pull_data
 *              pointer on the byte buffer containing the pulled data
 * @param[in]   pull_data
 *              pointer on the byte buffer containing the pulled data
 * @param[out]  push_msg_p
 *              pointer on the generated DataNotification
 * @return      true if message has been generated, false otherwise
 */
bool Data_Notification_generatePushFromPull(mcm_aa_e aa_type,
                                            obis_code_t push_obis,
                                            gxByteBuffer * pulled_data_p,
                                            message * push_msg_p);

#endif // DATA_NOTIFICATION_H_
