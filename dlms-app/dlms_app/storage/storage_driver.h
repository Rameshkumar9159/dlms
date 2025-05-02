/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */
#ifndef STORAGE_DRIVER_H_
#define STORAGE_DRIVER_H_

#include <stdint.h>

#include "cmsis_compiler.h" //__PACKED_STRUCT

/**
 * Id of the app persistent area
 */
#define STORAGE_AREA_ID             0x8AE573BA

/**
 * Versionning of the images
 */
#define EMPTY_IMAGE_VERSION         0xFFFF

/**
 *  All possible return codes from the storage driver
 */
typedef enum
{
    STORAGE_DRIVER_RES_OK,
    STORAGE_DRIVER_RES_INV_AREA,
    STORAGE_DRIVER_RES_INV_PARAM,
    STORAGE_DRIVER_RES_MEDIA_ERROR
} storage_driver_result_e;

/**
 *  Describes physical location of the memory bank.
 */
typedef struct
{
    uint32_t base_address;
    uint32_t block_size;
    uint16_t block_nb;
    uint16_t sector_size;
    uint16_t cur_sector_idx;
    uint16_t image_version;
} storage_context_t;

/**
 * Field that is used by storage driver for permanent storage control.
 */
typedef __PACKED_STRUCT
{
    uint16_t validity;      // see STORAGE_IS_VALID
    uint16_t crc;           // CRC-CCITT_16
    // CRC is calculated starting from this point:
    uint32_t seq;           // decreasing from 0xFFFFFFFEu, lower is newer.
    uint16_t image_version; // version of the image
    uint16_t image_size;    // size of the image
    uint32_t reserved2;     // for future use
} storage_control_t;

/**
 * @brief   Initialize a storage descriptor
 * @param   storage_p: pointer on the storage context
 *          block_nb: number of blocks reserved for this storage
 *          sector_size: sector size to store a whole data structure
 *          image_version: image version, used for backward compatibility
 * @return  true is successful, false otherwise
 */
storage_driver_result_e Storage_Driver_contextInit(storage_context_t * ctx_p,
                                                   uint16_t block_offset,
                                                   uint16_t block_nb,
                                                   uint16_t sector_size,
                                                   uint16_t image_version);
/**
 * @brief   Initialize storage hardware block, and get stored values
 * @param   ctx_p: pointer on the storage context
 *          img_p: pointer on the buffer to store the data readback from flash
 *          len: length of the data to read
 * @return  true is successful, false otherwise
 */
storage_driver_result_e Storage_Driver_init(storage_context_t * ctx_p,
                                            uint8_t * img_p, uint32_t len);
/**
 * @brief   Write a storage block to hardware
 * @param   ctx_p: pointer on the storage context
 *          img_p: pointer on the image to write in flash
 *          len: length of the data to write
 * @return  true is successful, false otherwise
 */
storage_driver_result_e Storage_Driver_write(storage_context_t * ctx_p,
                                             uint8_t * img_p, uint32_t len);

/**
 * @brief       Provide information about an image
 * @param[in]   image_p: pointer on the image
 * @param[out]  version_p: pointer on memory to store the image version
 * @param[out]  size_p: pointer on memory to store the image size
 * @return  true if image is valid, false otherwise
 */
bool Storage_Driver_getImageInfo(void * image_p, uint16_t * version_p,
                                 uint16_t * size_p);

#endif /* STORAGE_DRIVER_H_ */
