/* Copyright 2019 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */
#ifndef _PROVISIONING_DATA_H_
#define _PROVISIONING_DATA_H_

#include <stdint.h>
#include "api.h"
#include "cbor.h"

/** \brief Return codes of provisioning functions */
typedef enum
{
    PROV_RET_OK                = 0, /**< Operation is a success. */
    PROV_RET_INVALID_STATE     = 1, /**< Provisioning not in a valid state. */
    PROV_RET_INVALID_PARAM     = 2, /**< Invalid parameters. */
    PROV_RET_INVALID_DATA      = 3, /**< Invalid data. */
    PROV_RET_JOINING_LIB_ERROR = 4, /**< Joining library error. */
    PROV_RET_INTERNAL_ERROR    = 5, /**< Internal error (no more task, ...). */
} provisioning_ret_e;

/** \brief Provisioning result */
typedef enum
{
    PROV_RES_SUCCESS        = 0, /**< Provisioning is a success. */
    PROV_RES_TIMEOUT        = 1, /**< Timeout during provisioning. */
    PROV_RES_NACK           = 2, /**< Provisioning server returned a NACK. */
    PROV_RES_INVALID_DATA   = 3, /**< Received provisioning data is invalid. */
    PROV_RES_INVALID_PACKET = 4, /**< Received packet badly formated. */
    PROV_RES_ERROR_SENDING_DATA     = 5, /**< Problem when sending packet. */
    PROV_RES_ERROR_SCANNING_BEACONS = 6, /**< Error while scanning for joining
                                                                    beacons. */
    PROV_RES_ERROR_JOINING  = 7,         /**< Error during joining process. */
    PROV_RES_ERROR_NO_ROUTE = 8, /**< No route to host after joining network. */
    PROV_RES_STOPPED        = 9, /**< Application called stop function. */
    PROV_RES_ERROR_INTERNAL = 10 /**< Internal error (no more task, ...). */
} provisioning_res_e;

/**
 * \brief   The end provisioning callback. This function is called at the end
 *          of the provisioning process.
 * \param   result
 *          Result of the provisioning process.
 * \return  True: Apply received network parameters and reboot; False: discard
 *          data and end provisioning process.
 */
typedef bool (*provisioning_end_cb_f)(provisioning_res_e result);

/**
 * \brief   Received User provisioning data callback.
 *          Provisioning data is received as a map of id:data. This function
 *          is callback for each id that are not reserved by Wirepas.
 * \param   id
 *          Id of the received item.
 * \param   data
 *          Received data.
 * \param   len
 *          Length of the data.
 */
typedef void (*provisioning_user_data_cb_f)(uint32_t  id,
                                            CborType  type,
                                            uint8_t * data,
                                            uint8_t   len);


/**
 * \brief This structure holds the provisioning data parameters.
 */
typedef struct
{
    /** End provisioning callback. */
    provisioning_end_cb_f end_cb;
    /** Data provisioning callback. */
    provisioning_user_data_cb_f user_data_cb;
    /** buffer containing provisioning data. */
    uint8_t * buffer;
    /** length of provisioning buffer. */
    uint8_t length;
} provisioning_data_conf_t;

/**
 * \brief   Decode (and apply if valid) the received provisioning data.
 * \param   conf
 *          Configuration for the provisioning data decoder.
 * \param   dry_run
 *          If true, only check data validity and don't apply it.
 * \return  Result code, \ref PROV_RET_OK if config is valid.
 *          See \ref provisioning_ret_e for other return codes.
 */
provisioning_ret_e Provisioning_Data_decode(provisioning_data_conf_t * conf,
                                            bool                       dry_run);

/**
 * \brief Initializes the provisioning data
 *
 */
void Provisioning_Data_init(void);

/**
 * \brief Tells if the NIC contains all the required parameters to launch the
 * DLMS App
 *
 * \return true All parameters are provisioned in the NIC
 * \return false Missing parameters in the NIC
 */
bool Provisioning_Data_is_nic_provisioned(void);

/**
 * \brief Returns
 *
 * \param[out] buffer Buffer holding the parameters
 * \param[inout] buffer_length in: Buffer's max length in bytes
 * out: Actual buffer length used to store the parameters
 */
void Provisioning_Data_read_parameters(uint8_t *  buffer,
                                       uint16_t * buffer_length);

#endif  //_PROVISIONING_DATA_H_
