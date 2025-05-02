/* Copyright 2019 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include <ctype.h>

#include "app_persistent.h"
#include "include/enums.h"
#include "node_configuration.h"
#include "provisioning_data.h"
#include "public_provisioning.h"
#include "server_attribute_manager.h"

#define DEBUG_LOG_MODULE_NAME "PROV DAT"
#define DEBUG_LOG_MAX_LEVEL   LVL_INFO
#include "debug_log.h"


/** Size of the buffer to store strings from CBOR buffer. */
#define MAX_STRING_BUFFER_SIZE 94

/** Invalid values for the network parameters. */
#define INVALID_KEY       0xFF
#define INVALID_NET_ADDR  0
#define INVALID_NET_CHAN  0
#define INVALID_NODE_ADDR 0
#define INVALID_NODE_ROLE 0xFF

/** Invalid values for NIC parameters **/
#define UNDEFINED_DLMS_INTERFACE_TYPE 255

// We want to hide all the bytes except the last 2 when reading secrets
#define VISIBLE_KEY_LENGTH      2
#define HIDDEN_KEY_LENGTH       (KEY_SIZE - VISIBLE_KEY_LENGTH)
#define EMPTY_KEY_LENGTH        0
#define VISIBLE_PASSWORD_LENGTH 2

/** Minimum NIC Specific Id in Cbor provisioning buffer (range [128:255]). */
#define PROV_DATA_MIN_NIC_ID 128

/** Minimum User Specific Id in Cbor provisioning buffer (range [128:255]). */
#define PROV_DATA_MIN_USER_ID 192

/** Maximum User Specific Id in Cbor provisioning buffer (range [128:255]). */
#define PROV_DATA_MAX_USER_ID 255

/** \brief List of Wirepas Ids for CBOR encoded provisioning data. */
typedef enum
{
    PROV_DATA_ID_ENC_KEY   = 0,
    PROV_DATA_ID_AUTH_KEY  = 1,
    PROV_DATA_ID_NET_ADDR  = 2,
    PROV_DATA_ID_NET_CHAN  = 3,
    PROV_DATA_ID_NODE_ADDR = 4,
    PROV_DATA_ID_NODE_ROLE = 5
} provisioning_data_ids_e;

/**
 * \brief Stores the custom IDs used to provision the NIC
 * \note  Based on User Specific Id in Cbor provisioning buffer
 *        (range [128;255]).
 */
typedef enum
{
    NIC_PROV_ID_KEY_ENC_KEY = PROV_DATA_MIN_NIC_ID,
    NIC_PROV_ID_AUTH_KEY,
    NIC_PROV_ID_ENC_KEY,
    NIC_PROV_ID_MR_PASSWORD,
    NIC_PROV_ID_US_PASSWORD,
    NIC_PROV_ID_FU_PASSWORD,
    NIC_PROV_ID_FLAG_ID,
    NIC_PROV_ID_BAUDRATE,
    NIC_PROV_ID_INTERFACE_TYPE
} nic_prov_data_ids_e;

/** \brief Structure containing the Wirepas network parameters. */
static struct
{
    uint8_t                        enc_key[APP_LIB_SETTINGS_AES_KEY_NUM_BYTES];
    bool                           is_enc_key_set;
    uint8_t                        auth_key[APP_LIB_SETTINGS_AES_KEY_NUM_BYTES];
    bool                           is_auth_key_set;
    app_lib_settings_net_addr_t    net_addr;
    app_lib_settings_net_channel_t net_chan;
    app_addr_t                     node_addr;
    app_lib_settings_role_t        node_role;
} m_provisioning_data;

static nic_provisioning_parameters_t m_nic_provisioning_data;

/**
 * \brief Hides secret except for its last 2 characters
 *
 * \param[inout] secret Pointer to the secret to hide
 * \param secret_length Length of the secret
 */
void hide_secret(uint8_t * secret, uint8_t secret_length)
{
    uint8_t to_preserve = 0;

    if (secret == NULL || secret_length == 0)
    {
        return;
    }

    if (secret_length > VISIBLE_PASSWORD_LENGTH)
    {
        to_preserve = VISIBLE_PASSWORD_LENGTH;
    }
    memset(secret, '*', secret_length - to_preserve);
}

/**
 * \brief Tells if a key is set or not
 *
 * \param[in] key Pointer to the key
 * \return true Key is set
 * \return false Key is not set
 */
static bool is_key_set(const uint8_t* key)
{
    if (key == NULL)
    {
        return false;
    }

    const uint8_t uninit_key[KEY_SIZE] = { 0 };

    return !!memcmp(key, uninit_key, KEY_SIZE);
}

/**
 * \brief   Extract a byte array from a CBOR Value.
 * \param   value
 *          Pointer to a CBOR Value.
 * \param   buffer
 *          Pointer to where to store the data.
 * \param   buflen
 *          [In] Size of the buffer. [Out] Size of the extracted byte string.
 * \return  A CborError error code.
 */
static CborError extract_byte_string(const CborValue * value,
                                     uint8_t *   buffer,
                                     size_t *    buflen)
{
    if (!cbor_value_is_byte_string(value))
    {
        return CborErrorIllegalType;
    }

    return cbor_value_copy_byte_string(value, buffer, buflen, NULL);
}

/**
 * \brief   Extract a char array from a CBOR Value.
 * \param   value
 *          Pointer to a CBOR Value.
 * \param   buffer
 *          Pointer to where to store the data.
 * \param   buflen
 *          [In] Size of the buffer. [Out] Size of the extracted byte string.
 * \return  A CborError error code.
 */
static CborError extract_text_string(const CborValue * value,
                                     uint8_t *   buffer,
                                     size_t *    buflen)
{
    if (!cbor_value_is_text_string(value))
    {
        return CborErrorIllegalType;
    }

    return cbor_value_copy_text_string(value, (char *) buffer, buflen, NULL);
}

/**
 * \brief   Extract an unsigned int a CBOR Value.
 * \param   value
 *          Pointer to a CBOR Value.
 * \param   data
 *          Pointer to store the unsigned int.
 * \param   size
 *          Size in bytes of value pointed by data.
 * \note    This function does not support unsigned int bigger than 64 bits.
 * \return  A CborError error code.
 */
static CborError extract_unsigned_int(const CborValue * value,
                                      void *      data,
                                      size_t      size)
{
    uint64_t  val;

    if (!cbor_value_is_unsigned_integer(value))
    {
        return CborErrorIllegalType;
    }

    cbor_value_get_uint64(value, &val);

    if (size == 8)
    {
    }
    else if (size == 0)
    {
        return CborErrorDataTooLarge;
    }

    else if (size > 8 || val > (uint64_t) ((2ULL << (size * 8)) - 1))
    {
        return CborErrorDataTooLarge;
    }

    memcpy(data, &val, size);

    return CborNoError;
}

/**
 * \brief   Parse encryption key from CBOR buffer.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \return  A CborError error code.
 */
static CborError parse_enc_key(const CborValue * value)
{
    CborError err;
    size_t    len = APP_LIB_SETTINGS_AES_KEY_NUM_BYTES;
    err = extract_byte_string(value, m_provisioning_data.enc_key, &len);
    if (err != CborNoError)
    {
        return err;
    }

    if (len != APP_LIB_SETTINGS_AES_KEY_NUM_BYTES)
    {
        return CborErrorImproperValue;
    }

    m_provisioning_data.is_enc_key_set = true;

    return err;
}

/**
 * \brief   Parse authentication key from CBOR buffer.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \return  A CborError error code.
 */
static CborError parse_auth_key(const CborValue * value)
{
    CborError err;
    size_t    len = APP_LIB_SETTINGS_AES_KEY_NUM_BYTES;
    err = extract_byte_string(value, m_provisioning_data.auth_key, &len);
    if (err != CborNoError)
    {
        return err;
    }

    if (len != APP_LIB_SETTINGS_AES_KEY_NUM_BYTES)
    {
        return CborErrorImproperValue;
    }

    m_provisioning_data.is_auth_key_set = true;

    return err;
}

/**
 * \brief   Parse network address from CBOR buffer.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \return  A CborError error code.
 */
static CborError parse_net_address(const CborValue * value)
{
    CborError err;

    err = extract_unsigned_int(value,
                               &m_provisioning_data.net_addr,
                               sizeof(app_lib_settings_net_addr_t));

    if (err != CborNoError)
    {
        return err;
    }

    if (!lib_settings->isValidNetworkAddress(m_provisioning_data.net_addr))
    {
        m_provisioning_data.net_addr = INVALID_NET_ADDR;
        return CborErrorImproperValue;
    }

    return err;
}

/**
 * \brief   Parse network channel from CBOR buffer.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \return  A CborError error code.
 */
static CborError parse_net_channel(const CborValue * value)
{
    CborError err;

    err = extract_unsigned_int(value,
                               &m_provisioning_data.net_chan,
                               sizeof(app_lib_settings_net_channel_t));

    if (err != CborNoError)
    {
        return err;
    }

    if (!lib_settings->isValidNetworkChannel(m_provisioning_data.net_chan))
    {
        m_provisioning_data.net_chan = INVALID_NET_CHAN;
        return CborErrorImproperValue;
    }

    return err;
}

/**
 * \brief   Parse node address from CBOR buffer.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \return  A CborError error code.
 */
static CborError parse_node_address(const CborValue * value)
{
    CborError err;

    err = extract_unsigned_int(value,
                               &m_provisioning_data.node_addr,
                               sizeof(app_addr_t));

    if (err != CborNoError)
    {
        return err;
    }

    if (!lib_settings->isValidNodeAddress(m_provisioning_data.node_addr))
    {
        m_provisioning_data.node_addr = INVALID_NODE_ADDR;
        return CborErrorImproperValue;
    }

    return err;
}

/**
 * \brief   Parse node role from CBOR buffer.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \return  A CborError error code.
 */
static CborError parse_node_role(const CborValue * value)
{
    CborError err;
    size_t    len = sizeof(app_lib_settings_role_t);

    err = extract_byte_string(value, &m_provisioning_data.node_role, &len);

    if (err != CborNoError)
    {
        return err;
    }

    if (len != sizeof(app_lib_settings_role_t)
        || !lib_settings->isValidNodeRole(m_provisioning_data.node_role))
    {
        m_provisioning_data.node_role = INVALID_NODE_ROLE;
        return CborErrorImproperValue;
    }

    return err;
}

/**
 * \brief   Parse NIC key encryption key from CBOR buffer.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \return  A CborError error code.
 */
static CborError parse_nic_key_enc_key(const CborValue * value)
{
    CborError err;
    size_t    len = KEY_SIZE;

    err = extract_byte_string(
        value,
        m_nic_provisioning_data.sec_mat.key_encryption_key,
        &len);
    if (err != CborNoError)
    {
        return err;
    }

    if (len != KEY_SIZE)
    {
        return CborErrorImproperValue;
    }

    return err;
}

/**
 * \brief   Parse NIC authentication key from CBOR buffer.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \return  A CborError error code.
 */
static CborError parse_nic_auth_key(const CborValue * value)
{
    CborError err;
    size_t    len = KEY_SIZE;

    err = extract_byte_string(
        value,
        m_nic_provisioning_data.sec_mat.authentication_key,
        &len);

    if (err != CborNoError)
    {
        return err;
    }

    if (len != KEY_SIZE)
    {
        return CborErrorImproperValue;
    }

    return err;
}

/**
 * \brief   Parse NIC encryption key from CBOR buffer.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \return  A CborError error code.
 */
static CborError parse_nic_enc_key(const CborValue * value)
{
    CborError err;
    size_t    len = KEY_SIZE;

    err = extract_byte_string(value,
                              m_nic_provisioning_data.sec_mat.encryption_key,
                              &len);
    if (err != CborNoError)
    {
        return err;
    }

    if (len != KEY_SIZE)
    {
        return CborErrorImproperValue;
    }

    return err;
}

/**
 * \brief   Parse NIC MR password from CBOR buffer.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \return  A CborError error code.
 */
static CborError parse_nic_mr_password(const CborValue * value)
{
    CborError err;
    size_t    len = SECRET_MAX_SIZE;

    err = extract_text_string(value,
                              m_nic_provisioning_data.sec_mat.mr_secret,
                              &len);

    if (err != CborNoError)
    {
        return err;
    }

    if (len > SECRET_MAX_SIZE || len == 0)
    {
        return CborErrorImproperValue;
    }

    m_nic_provisioning_data.sec_mat.mr_secret_len = len;

    return err;
}

/**
 * \brief   Parse NIC US password from CBOR buffer.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \return  A CborError error code.
 */
static CborError parse_nic_us_password(const CborValue * value)
{
    CborError err;
    size_t    len = SECRET_MAX_SIZE;

    err = extract_text_string(value,
                              m_nic_provisioning_data.sec_mat.us_secret,
                              &len);

    if (err != CborNoError)
    {
        return err;
    }

    if (len > SECRET_MAX_SIZE || len == 0)
    {
        return CborErrorImproperValue;
    }

    m_nic_provisioning_data.sec_mat.us_secret_len = len;

    return err;
}

/**
 * \brief   Parse NIC FU password from CBOR buffer.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \return  A CborError error code.
 */
static CborError parse_nic_fu_password(const CborValue * value)
{
    CborError err;
    size_t    len = SECRET_MAX_SIZE;

    err = extract_text_string(value,
                              m_nic_provisioning_data.sec_mat.fu_secret,
                              &len);

    if (err != CborNoError)
    {
        return err;
    }

    if (len > SECRET_MAX_SIZE || len == 0)
    {
        return CborErrorImproperValue;
    }

    m_nic_provisioning_data.sec_mat.fu_secret_len = len;

    return err;
}

/**
 * \brief   Parse NIC Flag ID from CBOR buffer.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \return  A CborError error code.
 */
static CborError parse_nic_flag_id(const CborValue * value)
{
    CborError err;
    size_t    len = NIC_FLAG_ID_LENGTH;

    err = extract_text_string(value, m_nic_provisioning_data.nic_flag_id, &len);

    if (err != CborNoError)
    {
        return err;
    }

    if (len != NIC_FLAG_ID_LENGTH)
    {
        return CborErrorImproperValue;
    }

    for (uint8_t i = 0; i < NIC_FLAG_ID_LENGTH; i++)
    {
        if (!isalpha(m_nic_provisioning_data.nic_flag_id[i]))
        {
            return CborErrorImproperValue;
        }
    }

    return err;
}

/**
 * \brief   Parse NIC Baudrate from CBOR buffer.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \return  A CborError error code.
 */
static CborError parse_nic_baudrate(const CborValue * value)
{
    CborError err;

    err = extract_unsigned_int(value,
                               &m_nic_provisioning_data.nic_baudrate,
                               sizeof(uint32_t));

    if (err != CborNoError)
    {
        return err;
    }

    return err;
}

/**
 * \brief   Parse NIC interface type from CBOR buffer.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \return  A CborError error code.
 */
static CborError parse_nic_interface_type(const CborValue * value)
{
    CborError err;

    err = extract_unsigned_int(value,
                               &m_nic_provisioning_data.nic_interface_type,
                               sizeof(uint8_t));

    if (err != CborNoError)
    {
        return err;
    }

    if (m_nic_provisioning_data.nic_interface_type != DLMS_INTERFACE_TYPE_HDLC
        && m_nic_provisioning_data.nic_interface_type
               != DLMS_INTERFACE_TYPE_WRAPPER)
    {
        return CborErrorImproperValue;
    }

    return err;
}

/**
 * \brief   Parse one CBOR Id of wirepas data.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \param   id
 *          Id corresponding to the data
 * \return  A CborError error code.
 */
static CborError parse_wirepas_data(const CborValue * value, int id)
{
    CborError err;

    switch (id)
    {
        case PROV_DATA_ID_ENC_KEY:
        {
            err = parse_enc_key(value);
            break;
        }
        case PROV_DATA_ID_AUTH_KEY:
        {
            err = parse_auth_key(value);
            break;
        }
        case PROV_DATA_ID_NET_ADDR:
        {
            err = parse_net_address(value);
            break;
        }
        case PROV_DATA_ID_NET_CHAN:
        {
            err = parse_net_channel(value);
            break;
        }
        case PROV_DATA_ID_NODE_ADDR:
        {
            err = parse_node_address(value);
            break;
        }
        case PROV_DATA_ID_NODE_ROLE:
        {
            err = parse_node_role(value);
            break;
        }
        default:
        {
            /* This should not happen as id is tested before calling this
             * function.
             */
            err = CborErrorImproperValue;
        }
    }

    return err;
}

/**
 * \brief   Parse one CBOR Id of NIC data.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \param   id
 *          Id corresponding to the data
 * \return  A CborError error code.
 */
static CborError parse_nic_data(const CborValue * value, int id)
{
    CborError err;

    switch (id)
    {
        case NIC_PROV_ID_KEY_ENC_KEY:
        {
            err = parse_nic_key_enc_key(value);
            break;
        }
        case NIC_PROV_ID_AUTH_KEY:
        {
            err = parse_nic_auth_key(value);
            break;
        }
        case NIC_PROV_ID_ENC_KEY:
        {
            err = parse_nic_enc_key(value);
            break;
        }
        case NIC_PROV_ID_MR_PASSWORD:
        {
            err = parse_nic_mr_password(value);
            break;
        }
        case NIC_PROV_ID_US_PASSWORD:
        {
            err = parse_nic_us_password(value);
            break;
        }
        case NIC_PROV_ID_FU_PASSWORD:
        {
            err = parse_nic_fu_password(value);
            break;
        }
        case NIC_PROV_ID_FLAG_ID:
        {
            err = parse_nic_flag_id(value);
            break;
        }
        case NIC_PROV_ID_BAUDRATE:
        {
            err = parse_nic_baudrate(value);
            break;
        }
        case NIC_PROV_ID_INTERFACE_TYPE:
        {
            err = parse_nic_interface_type(value);
            break;
        }
        default:
        {
            /* This should not happen as id is tested before calling this
             * function.
             */
            err = CborErrorImproperValue;
        }
    }

    return err;
}

/**
 * \brief   Parse one CBOR Id of User data.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor(Data of one Id:Data map entry).
 * \param   id
 *          Id corresponding to the data
 * \param   cb
 *          Callback to call for each User specific Id found.
 *          It is called only if the whole provisioning data is valid.
 * \return  A CborError error code.
 */
static CborError parse_user_data(const CborValue *           value,
                                 int                         id,
                                 provisioning_user_data_cb_f cb)
{
    CborError err;

    size_t len;
    /* Force alignement as buffer can contain int64 values. */
    uint8_t  val[MAX_STRING_BUFFER_SIZE] __attribute__((aligned(8)));
    void *   data = &val;
    CborType type = cbor_value_get_type(value);

    switch (type)
    {
        case CborIntegerType:
        {
            len = sizeof(int64_t);
            err = cbor_value_get_int64_checked(value, (int64_t *) data);
            break;
        }

        case CborByteStringType:
        {
            len = MAX_STRING_BUFFER_SIZE;
            err = cbor_value_copy_byte_string(value,
                                              (uint8_t *) data,
                                              &len,
                                              NULL);
            break;
        }

        case CborTextStringType:
        {
            len = MAX_STRING_BUFFER_SIZE;
            err = cbor_value_copy_text_string(value, (char *) data, &len, NULL);
            break;
        }

        case CborSimpleType:
        {
            len = sizeof(uint8_t);
            err = cbor_value_get_simple_type(value, (uint8_t *) data);
            break;
        }

        case CborBooleanType:
        {
            len = sizeof(bool);
            err = cbor_value_get_boolean(value, (bool *) data);
            break;
        }

        case CborDoubleType:
        {
            len = sizeof(double);
            err = cbor_value_get_double(value, (double *) data);
            break;
        }

        case CborFloatType:
        {
            len = sizeof(float);
            err = cbor_value_get_float(value, (float *) data);
            break;
        }
        case CborHalfFloatType:
        {
            len = sizeof(uint16_t);
            err = cbor_value_get_half_float(value, (uint16_t *) data);
            break;
        }

        default:
            return CborErrorUnknownType;
    }

    if (err != CborNoError)
    {
        return err;
    }

    if (cb != NULL)
    {
        LOGD("User data (id : %d, type : %d, len : %d).", id, type, len);
        cb(id, type, data, len);
    }

    return err;
}

/**
 * \brief   Parse the CBOR encoded provisioning data.
 * \param   value
 *          Pointer to a CBOR Value. Points to provisioning data encoded
 *          in a Cbor map (Id:Data).
 * \param   cb
 *          Callback to call for each User specific Id found.
 *          It is called only if the whole provisioning data is valid.
 * \return  A CborError error code.
 */
static CborError parse_map(CborValue * value, provisioning_user_data_cb_f cb)
{
    int       id;

    /* Sets provisioning data structure to invalid values. */
    m_provisioning_data.is_auth_key_set = false;
    m_provisioning_data.is_enc_key_set  = false;
    m_provisioning_data.net_addr        = INVALID_NET_ADDR;
    m_provisioning_data.net_chan        = INVALID_NET_CHAN;
    m_provisioning_data.node_addr       = INVALID_NODE_ADDR;
    m_provisioning_data.node_role       = INVALID_NODE_ROLE;


    while (!cbor_value_at_end(value))
    {
        CborError err;
        /* Get the Id. */
        if (!cbor_value_is_unsigned_integer(value))
        {
            return CborErrorIllegalType;
        }

        err = cbor_value_get_int_checked(value, &id);
        if (err != CborNoError)
        {
            return err;
        }

        /* Get the data. */
        err = cbor_value_advance_fixed(value);
        if (err != CborNoError)
        {
            return err;
        }

        /* Match Wirepas Ids. Id > 0 already checked above. */
        if (id <= PROV_DATA_ID_NODE_ROLE)
        {
            err = parse_wirepas_data(value, id);
        }
        else if (id >= PROV_DATA_MIN_NIC_ID && id <= NIC_PROV_ID_INTERFACE_TYPE)
        {
            err = parse_nic_data(value, id);
        }
        /* Match User Ids. */
        else if (id >= PROV_DATA_MIN_USER_ID && id <= PROV_DATA_MAX_USER_ID)
        {
            err = parse_user_data(value, id, cb);
        }
        else
        {
            return CborErrorImproperValue;
        }

        if (err != CborNoError)
        {
            return err;
        }

        err = cbor_value_advance(value);
        if (err != CborNoError)
        {
            return err;
        }
    }

    return CborNoError;
}

/**
 * \brief   Apply received network and nic parameters.
 */
static void apply_wirepas_and_nic_parameters(void)
{
    const security_material_t * sm_p = &m_nic_provisioning_data.sec_mat;
    uint8_t pwd[SECRET_MAX_SIZE + 1];

    LOGD("Network parameters :");

    if (m_provisioning_data.is_auth_key_set)
    {
        LOGD(" - Authentication key : * * * * * * * * * * * *"
             " %02X %02X %02X %02X",
             m_provisioning_data.auth_key[12],
             m_provisioning_data.auth_key[13],
             m_provisioning_data.auth_key[14],
             m_provisioning_data.auth_key[15]);
        lib_settings->setAuthenticationKey(m_provisioning_data.auth_key);
    }

    if (m_provisioning_data.is_enc_key_set)
    {
        LOGD(" - Encryption key : * * * * * * * * * * * *"
             " %02X %02X %02X %02X",
             m_provisioning_data.enc_key[12],
             m_provisioning_data.enc_key[13],
             m_provisioning_data.enc_key[14],
             m_provisioning_data.enc_key[15]);
        lib_settings->setEncryptionKey(m_provisioning_data.enc_key);
    }

    if (m_provisioning_data.net_addr != INVALID_NET_ADDR)
    {
        LOGD(" - Network address : 0x%06X", m_provisioning_data.net_addr);
        lib_settings->setNetworkAddress(m_provisioning_data.net_addr);
    }

    if (m_provisioning_data.net_chan != INVALID_NET_CHAN)
    {
        LOGD(" - Network channel : %d", m_provisioning_data.net_chan);
        lib_settings->setNetworkChannel(m_provisioning_data.net_chan);
    }

    if (m_provisioning_data.node_addr != INVALID_NODE_ADDR)
    {
        LOGD(" - Node address : 0x%08X", m_provisioning_data.node_addr);
        lib_settings->setNodeAddress(m_provisioning_data.node_addr);
    }

    if (m_provisioning_data.node_role != INVALID_NODE_ROLE)
    {
        LOGD(" - Node role : 0x%02X", m_provisioning_data.node_role);
        lib_settings->setNodeRole(m_provisioning_data.node_role);
    }

    LOGD("NIC parameters :");

    if (is_key_set(sm_p->key_encryption_key))
    {
        LOGD(" - NIC Key Encryption key : * * * * * * * * * * * *"
             " %02X %02X %02X %02X",
             sm_p->key_encryption_key[12],
             sm_p->key_encryption_key[13],
             sm_p->key_encryption_key[14],
             sm_p->key_encryption_key[15]);
    }

    if (is_key_set(sm_p->authentication_key))
    {
        LOGD(" - NIC Authentication key : * * * * * * * * * * * *"
             " %02X %02X %02X %02X",
             sm_p->authentication_key[12],
             sm_p->authentication_key[13],
             sm_p->authentication_key[14],
             sm_p->authentication_key[15]);
    }

    if (is_key_set(sm_p->encryption_key))
    {
        LOGD(" - NIC Encryption key : * * * * * * * * * * * *"
             " %02X %02X %02X %02X",
             sm_p->encryption_key[12],
             sm_p->encryption_key[13],
             sm_p->encryption_key[14],
             sm_p->encryption_key[15]);
    }

    memcpy(pwd, sm_p->mr_secret, sm_p->mr_secret_len);
    pwd[sm_p->mr_secret_len] = '\0';
    hide_secret(pwd, sm_p->mr_secret_len);
    LOGD(" - NIC MR Password : %s", pwd);

    memcpy(pwd, sm_p->us_secret, sm_p->us_secret_len);
    pwd[sm_p->us_secret_len] = '\0';
    hide_secret(pwd, sm_p->us_secret_len);
    LOGD(" - NIC US Password : %s", pwd);

    memcpy(pwd, sm_p->fu_secret, sm_p->fu_secret_len);
    hide_secret(pwd, sm_p->fu_secret_len);
    LOGD(" - NIC FU Password : %s", pwd);

    LOGD(" - NIC Flag ID : %c%c%c",
         m_nic_provisioning_data.nic_flag_id[0],
         m_nic_provisioning_data.nic_flag_id[1],
         m_nic_provisioning_data.nic_flag_id[2]);

    LOGD(" - NIC Baudrate : %u", m_nic_provisioning_data.nic_baudrate);

    LOGD(" - NIC Interface Type : %s",
         m_nic_provisioning_data.nic_interface_type == DLMS_INTERFACE_TYPE_HDLC      ? "HDLC"
         : m_nic_provisioning_data.nic_interface_type == DLMS_INTERFACE_TYPE_WRAPPER ? "WRAPPER"
                                                    : "UNDEFINED");

    if (APP_PERSISTENT_RES_OK
        != App_Persistent_write((uint8_t *) &m_nic_provisioning_data,
                                sizeof(nic_provisioning_parameters_t)))
    {
        LOGE("Unable to write parameters to App_Persistent");
    }

    LOGI("Reboot.");

    /* Wait some time to print logs before rebooting. */
    LOG_FLUSH(LVL_INFO);
}

provisioning_ret_e Provisioning_Data_decode(provisioning_data_conf_t * conf,
                                            bool                       dry_run)
{
    CborParser parser;
    CborValue  value;
    CborError  err;

    if (conf == NULL || conf->buffer == NULL || conf->length == 0)
    {
        LOGE("%s : PROV_RET_INVALID_PARAM.", __func__);
        return PROV_RET_INVALID_PARAM;
    }

    // Enhancing the currently stored parameters
    if (APP_PERSISTENT_RES_OK
        != App_Persistent_read((uint8_t *) &m_nic_provisioning_data,
                               sizeof(nic_provisioning_parameters_t)))
    {
        LOGE("%s: Unable to read currently stored parameters in App_Persistent",
             __func__);
    }

    err = cbor_parser_init(conf->buffer, conf->length, 0, &parser, &value);

    if (err == CborNoError)
    {
        CborValue map;

        /* Begin parsing the received Cbor buffer. */
        if (!cbor_value_is_map(&value))
        {
            /* Data buffer must be organised as a map. */
            LOGE("%s : PROV_RET_INVALID_DATA.", __func__);
            return PROV_RET_INVALID_DATA;
        }

        err = cbor_value_enter_container(&value, &map);
        if (err != CborNoError)
        {
            /* Error entering the map. */
            LOGE("%s : PROV_RET_INVALID_DATA.", __func__);
            return PROV_RET_INVALID_DATA;
        }

        if (dry_run)
        {
            /* First to verify the data is valid but don't call the User
             * callback.
             */
            err = parse_map(&map, NULL);

            if (err != CborNoError)
            {
                /* Error when parsing the buffer. */
                LOGE("%s : PROV_RET_INVALID_DATA (cBorError %d).",
                     __func__,
                     err);
                return PROV_RET_INVALID_DATA;
            }

            LOGI("Provisioning data is valid.");
            return PROV_RET_OK;
        }
        else
        {
            /* Parse the buffer. call user callback for customer data */
            err = parse_map(&map, conf->user_data_cb);

            /* This should not happen as the buffer is tested during dryrun. */
            if (err != CborNoError)
            {
                /* Error when parsing the buffer. */
                LOGE("%s : PROV_RET_INVALID_DATA (cBorError %d).",
                     __func__,
                     err);
                return PROV_RET_INVALID_DATA;
            }

            if (conf->end_cb != NULL)
            {
                if (conf->end_cb(PROV_RES_SUCCESS))
                {
                    /* Stop the stack and apply new network parameters.
                     * This will trigger a reboot.
                     */
                    LOGI("Applying network parameters.");
                    lib_system->setShutdownCb(apply_wirepas_and_nic_parameters);
                    lib_state->stopStack(); /* Does not return. */
                }
            }
            return PROV_RET_OK;
        }
    }

    LOGE("%s : PROV_RET_INVALID_DATA.", __func__);
    return PROV_RET_INVALID_DATA;
}

void Provisioning_Data_init(void)
{
    app_lib_settings_net_addr_t    temp_net_add;
    app_lib_settings_net_channel_t temp_net_ch;

    LOGI("Starting provisioning process!");

    if (APP_PERSISTENT_RES_OK != App_Persistent_init())
    {
        LOGE("Unable to initialize App_Persistent");
        return;
    }

    // If read fails, it means we never wrote parameters to the persistent
    // memory area
    if (APP_PERSISTENT_RES_OK
        != App_Persistent_read((uint8_t *) &m_nic_provisioning_data,
                               sizeof(nic_provisioning_parameters_t)))
    {
        LOGI("Provisioning parameters from config.mk!");
#if defined(AUTOMATIC_NODE_ADDRESSING)
        LOGI("Automatic Node Addressing is enabled!");
        app_addr_t addr = getUniqueAddress();
        if (APP_RES_OK != lib_settings->setNodeAddress(addr))
        {
            LOGE("Unable to set Automatic Node Address");
        }
#endif /* AUTOMATIC_NODE_ADDRESSING */

        // Set keys if it's define in config.mk and currently not applied
        if (authen_key_p != NULL
            && lib_settings->getAuthenticationKey(NULL)
                   == APP_RES_INVALID_CONFIGURATION)
        {
            lib_settings->setAuthenticationKey(authen_key_p);
        }

        if (cipher_key_p != NULL
            && lib_settings->getEncryptionKey(NULL)
                   == APP_RES_INVALID_CONFIGURATION)
        {
            lib_settings->setEncryptionKey(cipher_key_p);
        }

#if defined(NETWORK_ADDRESS)
        // Check network address
        if (lib_settings->getNetworkAddress(&temp_net_add) != APP_RES_OK)
        {
            // Do not check return code, if it fails, it will not be set
            lib_settings->setNetworkAddress(NETWORK_ADDRESS);
        }
#endif

#if defined(NETWORK_CHANNEL)
        // Check network channel
        if (lib_settings->getNetworkChannel(&temp_net_ch) != APP_RES_OK)
        {
            lib_settings->setNetworkChannel(NETWORK_CHANNEL);
        }
#endif

#if defined(NIC_KEY_ENCRYPTION_KEY)
        const uint8_t nic_key_encryption_key[] = { NIC_KEY_ENCRYPTION_KEY };
        _Static_assert(sizeof(nic_key_encryption_key) == KEY_SIZE,
                       "NIC Key Encryption Key must be 16 bytes");
        memcpy(m_nic_provisioning_data.sec_mat.key_encryption_key,
               nic_key_encryption_key,
               KEY_SIZE);
#endif /* NIC_KEY_ENCRYPTION_KEY */

#if defined(NIC_AUTHENTICATION_KEY)
        const uint8_t nic_authentication_key[] = { NIC_AUTHENTICATION_KEY };
        _Static_assert(sizeof(nic_authentication_key) == KEY_SIZE,
                       "NIC Authentication Key must be 16 bytes");
        memcpy(m_nic_provisioning_data.sec_mat.authentication_key,
               nic_authentication_key,
               KEY_SIZE);
#endif /* NIC_AUTHENTICATION_KEY */
#if defined(NIC_ENCRYPTION_KEY)
        const uint8_t nic_encryption_key[] = { NIC_ENCRYPTION_KEY };
        _Static_assert(sizeof(nic_encryption_key) == KEY_SIZE,
                       "NIC Encryption Key must be 16 bytes");
        memcpy(m_nic_provisioning_data.sec_mat.encryption_key,
               nic_encryption_key,
               KEY_SIZE);
#endif /* NIC_ENCRYPTION_KEY */

#if defined(NIC_MR_PASSWORD)
        const uint8_t mr_secret[] = NIC_MR_PASSWORD;
        _Static_assert(strlen(NIC_MR_PASSWORD) <= SECRET_MAX_SIZE,
                       "NIC MR Password must be 32 bytes long max");
        m_nic_provisioning_data.sec_mat.mr_secret_len = strlen(NIC_MR_PASSWORD);

        memcpy(m_nic_provisioning_data.sec_mat.mr_secret,
               mr_secret,
               m_nic_provisioning_data.sec_mat.mr_secret_len);
#endif /* NIC_MR_PASSWORD */

#if defined(NIC_US_PASSWORD)
        const uint8_t us_secret[] = NIC_US_PASSWORD;
        _Static_assert(strlen(NIC_US_PASSWORD) <= SECRET_MAX_SIZE,
                       "NIC US Password must be 32 bytes long max");
        m_nic_provisioning_data.sec_mat.us_secret_len = strlen(NIC_US_PASSWORD);

        memcpy(m_nic_provisioning_data.sec_mat.us_secret,
               us_secret,
               m_nic_provisioning_data.sec_mat.us_secret_len);
#endif /* NIC_US_PASSWORD */

#if defined(NIC_FU_PASSWORD)
        const uint8_t fu_secret[] = NIC_FU_PASSWORD;
        _Static_assert(strlen(NIC_FU_PASSWORD) <= SECRET_MAX_SIZE,
                       "NIC FU Password must be 32 bytes long max");
        m_nic_provisioning_data.sec_mat.fu_secret_len = strlen(NIC_FU_PASSWORD);

        memcpy(m_nic_provisioning_data.sec_mat.fu_secret,
               fu_secret,
               m_nic_provisioning_data.sec_mat.fu_secret_len);
#endif /* NIC_FU_PASSWORD */

#if defined(NIC_FLAG_ID)
        _Static_assert(strlen(NIC_FLAG_ID) == NIC_FLAG_ID_LENGTH,
                       "NIC Flag ID Password must be 3 characters long");

        const uint8_t nic_flag_id[] = NIC_FLAG_ID;
        memcpy(m_nic_provisioning_data.nic_flag_id,
               nic_flag_id,
               NIC_FLAG_ID_LENGTH);
#endif /* NIC_FLAG_ID */

#if defined(NIC_BAUDRATE)
        _Static_assert(NIC_BAUDRATE > 0, "NIC Baudrate must be positive.");
        m_nic_provisioning_data.nic_baudrate = NIC_BAUDRATE;
#endif /* NIC_BAUDRATE */

#if defined(NIC_INTERFACE_TYPE)
        // If NIC_INTERFACE_TYPE is not HDLC nor WRAPPER, it is defaulted to
        // UNDEFINED_DLMS_INTERFACE_TYPE in the dlms_app makefile
        m_nic_provisioning_data.nic_interface_type = NIC_INTERFACE_TYPE;
#endif /* NIC_INTERFACE_TYPE */

        if (APP_PERSISTENT_RES_OK
            != App_Persistent_write((uint8_t *) &m_nic_provisioning_data,
                                    sizeof(nic_provisioning_parameters_t)))
        {
            LOGE("Unable to write to App Persistent");
            return;
        }
    }
}

bool Provisioning_Data_is_nic_provisioned(void)
{
    app_addr_t                     temp_node_addr;
    app_lib_settings_net_addr_t    temp_net_add;
    app_lib_settings_net_channel_t temp_net_ch;
    nic_provisioning_parameters_t  nic_parameters         = { 0 };
    const uint8_t uninitialized_nic_flag_id[NIC_FLAG_ID_LENGTH] = { 0 };

    // Check if keys are set
    if (lib_settings->getAuthenticationKey(NULL)
        == APP_RES_INVALID_CONFIGURATION)
    {
        // Not set
        return false;
    }

    if (lib_settings->getEncryptionKey(NULL) == APP_RES_INVALID_CONFIGURATION)
    {
        // Not set
        return false;
    }

    // Check Node address
    if (lib_settings->getNodeAddress(&temp_node_addr) != APP_RES_OK)
    {
        // Not set
        return false;
    }

    // Check Network address
    if (lib_settings->getNetworkAddress(&temp_net_add) != APP_RES_OK)
    {
        // Not set
        return false;
    }

    // Check Network channel
    if (lib_settings->getNetworkChannel(&temp_net_ch) != APP_RES_OK)
    {
        // Not set
        return false;
    }

    if (APP_PERSISTENT_RES_OK
        != App_Persistent_read((uint8_t *) &nic_parameters,
                               sizeof(nic_provisioning_parameters_t)))
    {
        LOGE("%s: Unable to read App Persistent", __func__);
        return false;
    }

    if (!is_key_set(nic_parameters.sec_mat.key_encryption_key))
    {
        // Not set
        return false;
    }

    if (!is_key_set(nic_parameters.sec_mat.authentication_key))
    {
        // Not set
        return false;
    }

    if (!is_key_set(nic_parameters.sec_mat.encryption_key))
    {
        // Not set
        return false;
    }

    if (nic_parameters.sec_mat.mr_secret_len == 0)
    {
        // Not set
        return false;
    }

    if (nic_parameters.sec_mat.us_secret_len == 0)
    {
        // Not set
        return false;
    }

    if (nic_parameters.sec_mat.fu_secret_len == 0)
    {
        // Not set
        return false;
    }

    if (!memcmp(nic_parameters.nic_flag_id,
                uninitialized_nic_flag_id,
                NIC_FLAG_ID_LENGTH))
    {
        // Not set
        return false;
    }

    if (nic_parameters.nic_baudrate == 0)
    {
        // Not set
        return false;
    }

    if (nic_parameters.nic_interface_type == UNDEFINED_DLMS_INTERFACE_TYPE)
    {
        // Not set
        return false;
    }

    return true;
}

void Provisioning_Data_read_parameters(uint8_t *  buffer,
                                       uint16_t * buffer_length)
{
    CborEncoder encoder, map_encoder;

    uint8_t                        auth_key[KEY_SIZE] = { 0 };
    uint8_t                        enc_key[KEY_SIZE]  = { 0 };
    app_addr_t                     nic_addr           = 0;
    app_lib_settings_net_addr_t    net_addr           = 0;
    app_lib_settings_net_channel_t net_chan           = 0;

    nic_provisioning_parameters_t nic_params = { 0 };

    cbor_encoder_init(&encoder, buffer, *buffer_length, 0);

    cbor_encoder_create_map(&encoder, &map_encoder, CborIndefiniteLength);

    // Wirepas Parameters
    if (lib_settings->getNetworkAddress(&net_addr) != APP_RES_OK)
    {
        net_addr = 0;
    }

    if (lib_settings->getNetworkChannel(&net_chan) != APP_RES_OK)
    {
        net_chan = 0;
    }

    if (lib_settings->getNodeAddress(&nic_addr) != APP_RES_OK)
    {
        nic_addr = 0;
    }

    cbor_encode_uint(&map_encoder, PROV_DATA_ID_ENC_KEY);
    if (lib_settings->getEncryptionKey((uint8_t *) &enc_key)
        == APP_RES_INVALID_CONFIGURATION)
    {
        // Not set
        cbor_encode_byte_string(&map_encoder, enc_key, EMPTY_KEY_LENGTH);
    }
    else
    {
        cbor_encode_byte_string(&map_encoder,
                                enc_key + HIDDEN_KEY_LENGTH,
                                VISIBLE_KEY_LENGTH);
    }

    cbor_encode_uint(&map_encoder, PROV_DATA_ID_AUTH_KEY);
    if (lib_settings->getAuthenticationKey((uint8_t *) &auth_key)
        == APP_RES_INVALID_CONFIGURATION)
    {
        // Not set
        cbor_encode_byte_string(&map_encoder, auth_key, EMPTY_KEY_LENGTH);
    }
    else
    {
        cbor_encode_byte_string(&map_encoder,
                                auth_key + HIDDEN_KEY_LENGTH,
                                VISIBLE_KEY_LENGTH);
    }

    cbor_encode_uint(&map_encoder, PROV_DATA_ID_NET_ADDR);
    cbor_encode_uint(&map_encoder, net_addr);

    cbor_encode_uint(&map_encoder, PROV_DATA_ID_NET_CHAN);
    cbor_encode_uint(&map_encoder, net_chan);

    cbor_encode_uint(&map_encoder, PROV_DATA_ID_NODE_ADDR);
    cbor_encode_uint(&map_encoder, nic_addr);

    // NIC Parameters
    if (APP_PERSISTENT_RES_OK
        != App_Persistent_read((uint8_t *) &nic_params,
                               sizeof(nic_provisioning_parameters_t)))
    {
        LOGE("%s: Unable to read current NIC Parameters.", __func__);
        memset((uint8_t *) &nic_params,
               0,
               sizeof(nic_provisioning_parameters_t));
    }

    cbor_encode_uint(&map_encoder, NIC_PROV_ID_KEY_ENC_KEY);
    if (is_key_set(nic_params.sec_mat.key_encryption_key))
    {
        cbor_encode_byte_string(&map_encoder,
                                nic_params.sec_mat.key_encryption_key
                                    + HIDDEN_KEY_LENGTH,
                                VISIBLE_KEY_LENGTH);
    }
    else
    {
        // Key not set
        cbor_encode_byte_string(&map_encoder,
                                nic_params.sec_mat.key_encryption_key,
                                EMPTY_KEY_LENGTH);
    }

    cbor_encode_uint(&map_encoder, NIC_PROV_ID_AUTH_KEY);
    if (is_key_set(nic_params.sec_mat.authentication_key))
    {
        cbor_encode_byte_string(&map_encoder,
                                nic_params.sec_mat.authentication_key
                                    + HIDDEN_KEY_LENGTH,
                                VISIBLE_KEY_LENGTH);
    }
    else
    {
        // Key not set
        cbor_encode_byte_string(&map_encoder,
                                nic_params.sec_mat.authentication_key,
                                EMPTY_KEY_LENGTH);
    }

    cbor_encode_uint(&map_encoder, NIC_PROV_ID_ENC_KEY);
    if (is_key_set(nic_params.sec_mat.encryption_key))
    {
        cbor_encode_byte_string(&map_encoder,
                                nic_params.sec_mat.encryption_key
                                    + HIDDEN_KEY_LENGTH,
                                VISIBLE_KEY_LENGTH);
    }
    else
    {
        // Key not set
        cbor_encode_byte_string(&map_encoder,
                                nic_params.sec_mat.encryption_key,
                                EMPTY_KEY_LENGTH);
    }

    hide_secret(nic_params.sec_mat.mr_secret, nic_params.sec_mat.mr_secret_len);

    cbor_encode_uint(&map_encoder, NIC_PROV_ID_MR_PASSWORD);
    cbor_encode_byte_string(&map_encoder,
                            nic_params.sec_mat.mr_secret,
                            nic_params.sec_mat.mr_secret_len);

    hide_secret(nic_params.sec_mat.us_secret, nic_params.sec_mat.us_secret_len);

    cbor_encode_uint(&map_encoder, NIC_PROV_ID_US_PASSWORD);
    cbor_encode_byte_string(&map_encoder,
                            nic_params.sec_mat.us_secret,
                            nic_params.sec_mat.us_secret_len);

    hide_secret(nic_params.sec_mat.fu_secret, nic_params.sec_mat.fu_secret_len);

    cbor_encode_uint(&map_encoder, NIC_PROV_ID_FU_PASSWORD);
    cbor_encode_byte_string(&map_encoder,
                            nic_params.sec_mat.fu_secret,
                            nic_params.sec_mat.fu_secret_len);

    cbor_encode_uint(&map_encoder, NIC_PROV_ID_FLAG_ID);
    cbor_encode_text_string(&map_encoder,
                            (char *) nic_params.nic_flag_id,
                            NIC_FLAG_ID_LENGTH);

    cbor_encode_uint(&map_encoder, NIC_PROV_ID_BAUDRATE);
    cbor_encode_uint(&map_encoder, nic_params.nic_baudrate);

    cbor_encode_uint(&map_encoder, NIC_PROV_ID_INTERFACE_TYPE);
    cbor_encode_uint(&map_encoder, nic_params.nic_interface_type);

    cbor_encoder_close_container(&encoder, &map_encoder);

    *buffer_length = cbor_encoder_get_buffer_size(&encoder, buffer);
}
