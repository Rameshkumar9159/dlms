/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h> // For memset

#include "common.h" //LOGx
#include "board.h"
#include "crc.h"
#include "util.h"
#include "storage_driver.h"

#include "wms_memory_area.h"

#define DEBUG_LOG_MODULE_NAME       "STR_DRV "
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL         LVL_INFO
#include "debug_log.h"

/**
 * This value in storage_control_t.validity means intentionally written data.
 */
#define STORAGE_IS_VALID            0xA55Au

/**
 * Skip first bytes in CRC calculation.
 * (crc field and validity field are not included to crc).
 */
#define STORAGE_CRC_IGNORE_BYTES    offsetof(storage_control_t, seq)

typedef struct
{
    uint32_t sector_read_time_us;
    uint32_t sector_write_time_us;
    uint32_t block_erase_time_us;
} storage_timing_t;

static uint32_t m_offset;

/**
 * Banks are used cyclically, starting from bank 0.
 * Each bank describes physical location for storing attribute_image_t.
 * Page erase is needed when we need to write page's 1st bank.
 * (which has page_addr == bank_addr).
 */

/**
 * Local function declarations
 */
static uint16_t calculate_crc(uint8_t * img_p, uint32_t len);
static uint32_t nextSectorIndex(const storage_context_t * ctx_p, uint32_t idx);
static uint32_t previousSectorIndex(const storage_context_t * ctx_p, uint32_t idx);
static bool drv_erase(uint32_t block_addr);
static bool drv_write(uint32_t addr, uint8_t * from_p, uint32_t len);
static bool drv_read(uint8_t * to_p, uint32_t addr, uint32_t len);

/**
 * CRC is calculated over the image minus some bytes from the start
 * because CRC field and validity field are excluded.
 */
static uint16_t calculate_crc(uint8_t * img_p, uint32_t len)
{
    uint16_t crc;
    crc = Crc_fromBuffer(img_p + STORAGE_CRC_IGNORE_BYTES,
                         len - STORAGE_CRC_IGNORE_BYTES);
    return crc;
}
static uint32_t nextSectorIndex(const storage_context_t * ctx_p, uint32_t idx)
{
    uint32_t snb = (ctx_p->block_size * ctx_p->block_nb) / ctx_p->sector_size;

    if (++idx >= snb)
    {
        idx = 0;
    }
    return idx;
}

static uint32_t previousSectorIndex(const storage_context_t * ctx_p, uint32_t idx)
{
    uint32_t snb = (ctx_p->block_size * ctx_p->block_nb) / ctx_p->sector_size;

    if (idx == 0)
    {
        idx = snb;
    }
    return --idx;
}

static void get_storage_timing_info(storage_timing_t * timing_p)
{
    app_lib_mem_area_info_t info;

    if (lib_memory_area->getAreaInfo(STORAGE_AREA_ID, &info) !=
                                                        APP_LIB_MEM_AREA_RES_OK)
    {
        memset(timing_p, 0x00, sizeof(storage_timing_t));
    }
    else
    {
        const app_lib_mem_area_flash_info_t * fi_p = &info.flash;
        // There is no value for reading
        timing_p->sector_read_time_us = 10;
        timing_p->sector_write_time_us = fi_p->page_write_time;
        timing_p->block_erase_time_us = fi_p->sector_erase_time;
    }
}

static bool drv_erase(uint32_t block_addr)
{
    storage_timing_t timing;
    uint32_t addr = block_addr;
    size_t nb = 1;
    app_lib_time_timestamp_hp_t end;
    bool busy;

    get_storage_timing_info(&timing);

    if (lib_memory_area->startErase(STORAGE_AREA_ID, &addr, &nb) !=
                                                        APP_LIB_MEM_AREA_RES_OK)
    {
        LOGE("Failed to erase block at 0x%08X", addr);
        return false;
    }

    end = lib_time->addUsToHpTimestamp(lib_time->getTimestampHp(),
                                       timing.block_erase_time_us);

    while ((busy = lib_memory_area->isBusy(STORAGE_AREA_ID)) &&
           lib_time->isHpTimestampBefore(lib_time->getTimestampHp(), end));

    if (busy)
    {
        LOGE("Flash still busy after erasing block at 0x%08X", addr);
        return false;
    }
    return true;
}

static bool drv_write(uint32_t addr, uint8_t * from_p, uint32_t len)
{
    storage_timing_t timing;
    app_lib_time_timestamp_hp_t end;
    uint32_t aligned_len = (len / sizeof(uint32_t)) * sizeof(uint32_t);
    bool busy;

    get_storage_timing_info(&timing);

    if ((addr & (sizeof(uint32_t) - 1)) != 0)
    {
        // This should not happen so no need to handle it currently
        return false;
    }

    if (lib_memory_area->startWrite(STORAGE_AREA_ID, addr, from_p, aligned_len)
                                                    != APP_LIB_MEM_AREA_RES_OK)
    {
        LOGE("Failed to write %u bytes at address 0x%08X",
            aligned_len, addr);
        return false;
    }

    end = lib_time->addUsToHpTimestamp(lib_time->getTimestampHp(),
                                       timing.sector_write_time_us);

    while ((busy = lib_memory_area->isBusy(STORAGE_AREA_ID)) &&
           lib_time->isHpTimestampBefore(lib_time->getTimestampHp(), end));

    if (busy)
    {
        LOGE("Flash still busy after writing %u bytes at 0x%08X",
            aligned_len, addr);
        return false;
    }

    if (aligned_len != len)
    {
        uint8_t rem[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        for (uint8_t i  = 0; i < len - aligned_len; i++)
        {
            rem[i] = from_p[aligned_len + i];
        }
        if (lib_memory_area->startWrite(STORAGE_AREA_ID, addr + aligned_len,
                            rem, sizeof(uint32_t)) != APP_LIB_MEM_AREA_RES_OK)
        {
            LOGE("Failed to write %u bytes at 0x%08X",
                len - aligned_len, addr + aligned_len);
            return false;
        }

        end = lib_time->addUsToHpTimestamp(lib_time->getTimestampHp(),
                                           timing.sector_write_time_us);

        while ((busy = lib_memory_area->isBusy(STORAGE_AREA_ID)) &&
               lib_time->isHpTimestampBefore(lib_time->getTimestampHp(), end));

        if (busy)
        {
            LOGE("Flash still busy after writing %u bytes at 0x%08X",
                len - aligned_len, addr + aligned_len);
            return false;
        }
    }

    return true;
}

static bool drv_read(uint8_t * to_p, uint32_t addr, uint32_t len)
{
    storage_timing_t timing;
    app_lib_time_timestamp_hp_t end;
    bool busy;

    get_storage_timing_info(&timing);

    if (lib_memory_area->startRead(STORAGE_AREA_ID, to_p, addr, len) !=
                                                        APP_LIB_MEM_AREA_RES_OK)
    {
        LOGE("Failed to read %u bytes at address 0x%08X", len, addr);
        return false;
    }

    end = lib_time->addUsToHpTimestamp(lib_time->getTimestampHp(),
                                       timing.sector_read_time_us);

    while ((busy = lib_memory_area->isBusy(STORAGE_AREA_ID)) &&
           lib_time->isHpTimestampBefore(lib_time->getTimestampHp(), end));

    if (busy)
    {
        LOGE("Flash still busy after reading %u bytes at 0x%08X",
            len, addr);
        return false;
    }
    return true;
}

storage_driver_result_e Storage_Driver_contextInit(storage_context_t * ctx_p,
                                                   uint16_t block_offset,
                                                   uint16_t block_nb,
                                                   uint16_t sector_size,
                                                   uint16_t image_version)
{
    app_lib_mem_area_info_t info;

    if (lib_memory_area->getAreaInfo(STORAGE_AREA_ID, &info) !=
                                                        APP_LIB_MEM_AREA_RES_OK)
    {
        return STORAGE_DRIVER_RES_INV_AREA;
    }

    ctx_p->base_address = block_offset * info.flash.erase_sector_size;
    ctx_p->block_size = info.flash.erase_sector_size;
    ctx_p->sector_size = sector_size;
    ctx_p->block_nb = block_nb;
    ctx_p->cur_sector_idx = 0xFFFF;
    ctx_p->image_version = image_version;
    m_offset += ctx_p->block_size * ctx_p->block_nb;

    // Sanity checks
    if (ctx_p->sector_size > info.flash.write_page_size)
    {
        LOGE("Defined sector size is larger that media sector size"
            "(%u > %u bytes)", ctx_p->sector_size, info.flash.write_page_size);
        return STORAGE_DRIVER_RES_INV_PARAM;
    }

    if (ctx_p->block_size % ctx_p->sector_size)
    {
        LOGE("Invalid sector size %u bytes, not a divider of "
            "block size (%u bytes)", ctx_p->sector_size, ctx_p->block_size);
        return STORAGE_DRIVER_RES_INV_PARAM;
    }

    if (m_offset > info.area_size)
    {
        LOGE("Reserved size for storage is larger than memory area"
            "(%u > %u bytes)", m_offset, info.area_size);
        return STORAGE_DRIVER_RES_INV_PARAM;
    }
    LOGI("Initialized ctx 0x%08X: @ 0x%08X, %u blocks of %u KB, "
        "%u B sector size", ctx_p, ctx_p->base_address, ctx_p->block_nb,
        ctx_p->block_size / 1024, ctx_p->sector_size);

    return STORAGE_DRIVER_RES_OK;
}

/**
 * Initialize storage system and load newest intact bank if possible.
 */
storage_driver_result_e Storage_Driver_init(storage_context_t * ctx_p,
                                            uint8_t * img_p, uint32_t len)
{
    uint32_t sector_nb;
    uint32_t idx = (uint32_t)(~0);
    uint32_t seq = (uint32_t)(~0);
    uint32_t i;

    memset((void *)img_p, 0xFF, len);

    // Try to find the newest possible storage bank
    // (i.e.bank with lowest sequence number).
    // For this we loop thru all bank headers and check sequence number.
    sector_nb = (ctx_p->block_size * ctx_p->block_nb) / ctx_p->sector_size;

    for (i = 0; i < sector_nb; i++)
    {
        uint32_t address = ctx_p->base_address + (i * ctx_p->sector_size);
        storage_control_t ctrl_header;
        // load header only
        if (! drv_read((uint8_t *)&ctrl_header, address,
                       sizeof(storage_control_t)))
        {
            // TODO should we ignore the error and continue?
            return STORAGE_DRIVER_RES_MEDIA_ERROR;
        }

        // check & compare seq
        if (ctrl_header.seq < seq && ctrl_header.validity == STORAGE_IS_VALID)
        {
            seq = ctrl_header.seq;
            idx = i;
        }
    }
    if (idx == ~0u)
    {
        // There are no valid banks. Local storage not modified.
        LOGI("Ctx 0x%08X: no data in persistent storage", ctx_p);
        return STORAGE_DRIVER_RES_OK;
    }

    // We now have idx to newest bank. We load that, and in case of
    // failure we can try older banks one by one:
    for (i = 0; i < sector_nb; i++, idx = previousSectorIndex(ctx_p, idx))
    {
        // load full bank
        uint32_t address = ctx_p->base_address + (idx * ctx_p->sector_size);
        if (! drv_read(img_p, address, len))
        {
            return STORAGE_DRIVER_RES_MEDIA_ERROR;
        }

        // check validity and crc
        // return if success
        const storage_control_t * ctrl_p = (storage_control_t *) img_p;
        if (ctrl_p->validity == STORAGE_IS_VALID &&
            ctrl_p->image_size < ctx_p->sector_size &&
            ctrl_p->crc == calculate_crc(img_p, ctrl_p->image_size))
        {
            // Valid storage bank loaded:
            LOGI("Ctx 0x%08X: valid data found at 0x%08X (sector #%u)",
                ctx_p, address, idx);
            ctx_p->cur_sector_idx = idx;
            return STORAGE_DRIVER_RES_OK;
        }
        // otherwise try older bank
    }
    // There are no intact banks, we have loaded trash to local storage.
    // Restore local storage to 0xFF.
    LOGI("Ctx 0x%08X: no valid data in persistent storage", ctx_p);
    return STORAGE_DRIVER_RES_OK;
}

/**
 * Write to the next bank. Banks are used cyclically.
 */
storage_driver_result_e Storage_Driver_write(storage_context_t * ctx_p,
                                             uint8_t * img_p, uint32_t len)
{
    uint32_t addr;
    uint32_t sector_per_block;
    uint16_t crc;

    // Sanity check
    if (! ctx_p || ! img_p || len >= ctx_p->sector_size)
    {
        return STORAGE_DRIVER_RES_INV_PARAM;
    }

    // Initialize local variables
    addr = ctx_p->base_address;
    sector_per_block = ctx_p->block_size / ctx_p->sector_size;

    // Switch to use next sector:
    ctx_p->cur_sector_idx = nextSectorIndex(ctx_p, ctx_p->cur_sector_idx);
    addr += ctx_p->cur_sector_idx * ctx_p->sector_size;

    // If new sector is block's first, then we must erase the block:
    if (! (ctx_p->cur_sector_idx % sector_per_block))
    {
        if (! drv_erase(addr))
        {
            LOGE("Ctx 0x%08X: failed to erase block at 0%08X",
                ctx_p, addr);
            return STORAGE_DRIVER_RES_MEDIA_ERROR;
        }
    }
    storage_control_t * ctrl_p = (storage_control_t *) img_p;
    ctrl_p->image_version = ctx_p->image_version;
    ctrl_p->image_size = len;
    ctrl_p->seq--; // lower is newer. Needs no wrap control.
    ctrl_p->validity = STORAGE_IS_VALID;
    // calculate crc just before bank write:
    crc = ctrl_p->crc = calculate_crc(img_p, len);

    // Write data and check written data using the previously computed CRC
    if (! drv_write(addr, img_p, len))
    {
        LOGE("Ctx 0x%08X: failed to write %u bytes at 0x%08X",
            ctx_p, len, addr);
        return STORAGE_DRIVER_RES_MEDIA_ERROR;
    }
    if (! drv_read(img_p, addr, len))
    {
        LOGE("Ctx 0x%08X: failed to read back %u bytes at 0x%08X",
            ctx_p, len, addr);
        return STORAGE_DRIVER_RES_MEDIA_ERROR;
    }
    if (crc != calculate_crc(img_p, len))
    {
        LOGE("Ctx 0x%08X: checksum mismatch", ctx_p);
        return STORAGE_DRIVER_RES_MEDIA_ERROR;
    }
    LOGI("Ctx 0x%08X: data written successfully at 0x%08X",
        ctx_p, addr);
    return STORAGE_DRIVER_RES_OK;
}

/**
 * Return information about the image
 */
bool Storage_Driver_getImageInfo(void * image_p, uint16_t * version_p,
                                 uint16_t * size_p)
{
    const storage_control_t * ctrl_p = (storage_control_t *) image_p;

    if (ctrl_p->validity == STORAGE_IS_VALID)
    {
        *version_p = ctrl_p->image_version;
        *size_p = ctrl_p->image_size;
        return true;
    }
    return false;
}
