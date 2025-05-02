/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    nic_provisioning.h
 * \brief   nic_provisioning interface for the provisioning process
 */

#ifndef NIC_PROVISIONING_H_
#define NIC_PROVISIONING_H_

/**
 * \brief   Function that will start the Provisioning Application
 *
 * \param timeout_cb Callback to call in case the NIC is already provisioned
 */
void Nic_Provisioning_start(void (*timeout_cb)(void) );

#endif  // NIC_PROVISIONING_H_
