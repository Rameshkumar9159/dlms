/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * @file    app.c
 * @brief   Tests of the external flash
 */

#include <stdlib.h>  // For NULL
#include <string.h>  //memcpy

#include "api.h"
#include "app_scheduler.h"
#include "node_configuration.h"

#include "extflash_tests.h"
#include "dut_protocol.h"

#define DEBUG_LOG_MODULE_NAME "EXTFLASH"
#define DEBUG_LOG_MAX_LEVEL   LVL_INFO
#include "debug_log.h"

#define NUMBER_OF_PAGES_ID_TO_TEST 3

/** Delays */
#define DELAY_BETWEEN_TEST_MS        100
#define DELAY_POLLING_AFTER_WRITE_MS 10
#define DELAY_POLLING_AFTER_ERASE_MS 100

#define BUFFER_SIZE 256

#define EXFT_FINAL_STATE      0x80
#define IS_FINAL_STATE(state) (((state) &EXFT_FINAL_STATE) == EXFT_FINAL_STATE)

typedef bool (*exft_func)(void);

typedef enum
{
    EXFT_STATE_LOOKING_FOR_EXFL_AREA,
    EXFT_STATE_SANITY_CHECKS,
    EXFT_STATE_ERASE_SETUP,
    EXFT_STATE_VERIFY_AFTER_SETUP,
    EXFT_STATE_WRITE_AND_VERIFY,
    EXFT_STATE_CLEANUP,
    EXFT_STATE_VERIFY_AFTER_CLEANUP,
    EXTF_STATE_TESTS_PASSED = EXFT_FINAL_STATE,
    EXTF_STATE_TESTS_FAILED
} exft_state_e;

typedef struct
{
    exft_state_e state;
    exft_func    func;
} extf_state_t;

// Prototypes
static void active_wait_ms(uint32_t period);
static bool passed_tests(void);
static bool failed_tests(void);
static bool find_extflash_area(void);
static bool sanity_checks(void);
static bool erase_tests(void);
static bool default_read_tests(void);
static bool write_read_tests(void);

static const extf_state_t exft_fsm[]
    = { { EXFT_STATE_LOOKING_FOR_EXFL_AREA, find_extflash_area },
        { EXFT_STATE_SANITY_CHECKS, sanity_checks },
        { EXFT_STATE_ERASE_SETUP, erase_tests },
        { EXFT_STATE_VERIFY_AFTER_SETUP, default_read_tests },
        { EXFT_STATE_WRITE_AND_VERIFY, write_read_tests },
        { EXFT_STATE_CLEANUP, erase_tests },
        { EXFT_STATE_VERIFY_AFTER_CLEANUP, default_read_tests },
        { EXTF_STATE_TESTS_PASSED, passed_tests },
        { EXTF_STATE_TESTS_FAILED, failed_tests } };

static app_lib_mem_area_info_t m_info;

// Buffers
static uint8_t data[BUFFER_SIZE];
static uint8_t ctrl[BUFFER_SIZE];

// FSM state
static exft_state_e m_state = EXFT_STATE_LOOKING_FOR_EXFL_AREA;

// Callback used when the flash testing is ended
static void (*m_end_cb)(test_status_e, app_lib_mem_area_info_t) = NULL;

/**
 * @brief       active wait
 * @param[in]   period
 *              duration in milliseconds of the active wait
 */
static void active_wait_ms(uint32_t period)
{
    app_lib_time_timestamp_hp_t end;
    end = lib_time->addUsToHpTimestamp(lib_time->getTimestampHp(),
                                       period * 1000);

    /* Active wait until period is elapsed */
    while (lib_time->isHpTimestampBefore(lib_time->getTimestampHp(), end))
        ;
}

/**
 * @brief   Function executed when all tests passed
 * @return  true
 */
static bool passed_tests(void)
{
    m_end_cb(TEST_STATUS_SUCCESS, m_info);

    return true;
}

/**
 * @brief   Function executed when one of the tests failed
 * @return  true
 */
static bool failed_tests(void)
{
    m_end_cb(TEST_STATUS_FAILED, m_info);

    return true;
}

/**
 * @brief   find an area in external flash to run the tests
 * @return  true if an area has been found, false otherwise
 */
static bool find_extflash_area(void)
{
    app_lib_mem_area_id_t areas[APP_LIB_MEM_AREA_MAX_AREAS];
    uint8_t               num_areas;

    LOG(LVL_INFO, "Looking for an area in external flash:");

    memset(&m_info, 0x00, sizeof(m_info));
    lib_memory_area->getAreaList(areas, &num_areas);

    for (uint8_t a = 0; a < num_areas; a++)
    {
        app_lib_mem_area_info_t info;
        if (APP_LIB_MEM_AREA_RES_OK
            != lib_memory_area->getAreaInfo(areas[a], &info))
        {
            LOG(LVL_ERROR, "Failed to get area info for id 0x%08X", areas[a]);
            return false;
        }
        if (info.external_flash && info.type == APP_LIB_MEM_AREA_TYPE_USER)
        {
            LOG(LVL_INFO,
                "Found area with id 0x%08X in external flash",
                areas[a]);
            memcpy(&m_info, &info, sizeof(app_lib_mem_area_info_t));
            return true;
        }
    }
    LOG(LVL_ERROR, "Failed to find any area in external flash");
    return false;
}

/**
 * @brief   sanity checks the flash info
 * @return  true if the flash info are as expected, false otherwise
 */
static bool sanity_checks(void)
{
    uint32_t block_size = m_info.flash.erase_sector_size;
    size_t   page_size  = m_info.flash.write_page_size;

    // Sanity check
    LOG(LVL_INFO, "Sanity checks:");
    if (m_info.area_size % block_size || page_size > sizeof(data))
    {
        LOG(LVL_ERROR, "Error in the sanity checks");
        return false;
    }
    LOG(LVL_INFO, "ok");
    return true;
}

/**
 * \brief Get the pages id to test on the flash
 *
 * \param[out] pages_id pointer to an array of size NUMBER_OF_PAGES_ID_TO_TEST,
 * storing the pages_id to test
 */
static void get_pages_id_to_test(uint32_t * pages_id)
{
    size_t page_size = m_info.flash.write_page_size;
    size_t page_nb   = m_info.area_size / page_size;

    // First Flash Page
    pages_id[0] = 0;
    // Page in the middle of the flash
    pages_id[1] = page_nb / 2;
    // Last Flash Page
    pages_id[2] = page_nb - 1;
}

/**
 * @brief   external flash test for erase command
 * @return  true if the erase test has been run successfully, false otherwise
 */
static bool erase_tests(void)
{
    uint32_t pages_id_to_sector_addr[NUMBER_OF_PAGES_ID_TO_TEST];
    uint32_t pages_id_to_test[NUMBER_OF_PAGES_ID_TO_TEST];
    size_t   page_size = m_info.flash.write_page_size;

    get_pages_id_to_test((uint32_t *) &pages_id_to_test);

    for (uint8_t i = 0; i < NUMBER_OF_PAGES_ID_TO_TEST; i++)
    {
        pages_id_to_sector_addr[i] = (pages_id_to_test[i] * page_size)
                                     & ~(m_info.flash.erase_sector_size - 1);
    }

    // Erase all blocks in the area
    LOG(LVL_INFO, "Erase tests:");
    for (uint8_t i = 0; i < NUMBER_OF_PAGES_ID_TO_TEST; i++)
    {
        size_t remaining_nb = 1;
        if (lib_memory_area->startErase(m_info.area_id,
                                        &pages_id_to_sector_addr[i],
                                        &remaining_nb)
            != APP_LIB_MEM_AREA_RES_OK)
        {
            LOG(LVL_ERROR,
                "Failed to erase block at address 0x%08X in area "
                "id 0x%08X",
                pages_id_to_sector_addr[i],
                m_info.area_id);
            return false;
        }
        do
        {
            // We use an active wait instead of constantly polling the flash
            // in order to not pollute the logic analyzer output
            active_wait_ms(DELAY_POLLING_AFTER_ERASE_MS);
        } while (lib_memory_area->isBusy(m_info.area_id));
    }

    LOG(LVL_INFO, "ok");

    return true;
}

/**
 * @brief   external flash test for default read
 * @return  true if the read test has been run successfully, false otherwise
 */
static bool default_read_tests(void)
{
    size_t   page_size = m_info.flash.write_page_size;
    uint32_t pages_id_to_test[NUMBER_OF_PAGES_ID_TO_TEST];

    get_pages_id_to_test((uint32_t *) &pages_id_to_test);

    // Default read tests (erased blocks => 0xFF)
    LOG(LVL_INFO, "Read tests:");
    for (uint32_t id = 0; id < NUMBER_OF_PAGES_ID_TO_TEST; id++)
    {
        if (lib_memory_area->startRead(m_info.area_id,
                                       data,
                                       pages_id_to_test[id] * page_size,
                                       page_size)
            != APP_LIB_MEM_AREA_RES_OK)
        {
            LOG(LVL_ERROR,
                "Failed to read address range 0x%08X--0x%08X "
                "in area id 0x%08X",
                pages_id_to_test[id] * page_size,
                (pages_id_to_test[id] + 1) * page_size - 1,
                m_info.area_id);
            return false;
        }
        // Check the data is 0xFF
        for (uint32_t b = 0; b < page_size; b++)
        {
            if (data[b] != 0xFF)
            {
                LOG(LVL_ERROR,
                    "Value read at address 0x%08X "
                    "in area id 0x%08X is not 0xFF",
                    (pages_id_to_test[id] * page_size) + b,
                    m_info.area_id);
                return false;
            }
        }
    }

    LOG(LVL_INFO, "ok");

    return true;
}

/**
 * @brief   external flash test for write then read
 * @return  true if the write test has been run successfully, false otherwise
 */
static bool write_read_tests(void)
{
    size_t   page_size = m_info.flash.write_page_size;
    uint32_t pages_id_to_test[NUMBER_OF_PAGES_ID_TO_TEST];

    get_pages_id_to_test((uint32_t *) &pages_id_to_test);

    // Write tests
    LOG(LVL_INFO, "Write and verify tests:");
    // data and ctrl both hold the data to write, ctrl buffer will be used to
    // verify the correctness of the written data
    // We simply write the value of the offset from the beginning of the page
    // at that offset e.g. data[0xAB] = 0xAB
    for (uint32_t i = 0; i < page_size; i++)
    {
        data[i] = ctrl[i] = (uint8_t) i;
    }

    // The test is repeated for each page of the area in the external flash to
    // test
    for (uint32_t id = 0; id < NUMBER_OF_PAGES_ID_TO_TEST; id++)
    {
        if (lib_memory_area->startWrite(m_info.area_id,
                                        pages_id_to_test[id] * page_size,
                                        data,
                                        page_size)
            != APP_LIB_MEM_AREA_RES_OK)
        {
            LOG(LVL_ERROR,
                "Failed to write address range 0x%08X--0x%08X "
                "in area id 0x%08X",
                pages_id_to_test[id] * page_size,
                (pages_id_to_test[id] + 1) * page_size - 1,
                m_info.area_id);
            return false;
        }

        do
        {
            // We use an active wait instead of constantly polling the flash
            // in order to not pollute the logic analyzer output
            active_wait_ms(DELAY_POLLING_AFTER_WRITE_MS);
        } while (lib_memory_area->isBusy(m_info.area_id));

        // Readback
        if (lib_memory_area->startRead(m_info.area_id,
                                       data,
                                       pages_id_to_test[id] * page_size,
                                       page_size)
            != APP_LIB_MEM_AREA_RES_OK)
        {
            LOG(LVL_ERROR,
                "Failed to read address range 0x%08X--0x%08X "
                "in area id 0x%08X",
                pages_id_to_test[id] * page_size,
                (pages_id_to_test[id] + 1) * page_size - 1,
                m_info.area_id);
            return false;
        }
        // Check the data
        for (uint32_t b = 0; b < page_size; b++)
        {
            if (data[b] != ctrl[b])
            {
                LOG(LVL_ERROR,
                    "Value read at address 0x%08X is different from "
                    "written value in area id 0x%08X : 0x%2X vs 0x%02X",
                    (pages_id_to_test[id] * page_size) + b,
                    m_info.area_id,
                    data[b],
                    ctrl[b]);
                return false;
            }
        }
    }

    LOG(LVL_INFO, "ok");

    return true;
}

void extflash_tests_reset_fsm_state(void)
{
    m_state = EXFT_STATE_LOOKING_FOR_EXFL_AREA;
}

void extflash_tests_set_end_cb(void (*end_cb)(test_status_e,
                                              app_lib_mem_area_info_t))
{
    m_end_cb = end_cb;
}

uint32_t extflash_tests_fsm_task(void)
{
    exft_func func   = NULL;
    uint32_t  idx    = 0;
    bool      failed = false;

    for (uint32_t i = 0; i < sizeof(exft_fsm) / sizeof(exft_fsm[0]); i++)
    {
        if (m_state == exft_fsm[i].state)
        {
            // The function related to this state has been found
            func = exft_fsm[i].func;
            idx  = i;
            break;
        }
    }
    if (!func)
    {
        LOG(LVL_ERROR, "No function for state %u", m_state);
        failed = true;
    }
    // Execute the function
    else if (false == func())
    {
        failed = true;
    }

    // If it is a final state, we stop this task
    if (IS_FINAL_STATE(m_state))
    {
        return APP_SCHEDULER_STOP_TASK;
    }

    // Update the state
    if (failed)
    {
        // The latest test failed
        m_state = EXTF_STATE_TESTS_FAILED;
    }
    else
    {
        // Set next state and mark the end of a sequence of tests
        m_state = exft_fsm[idx + 1].state;
        LOG(LVL_INFO, "Done");
    }
    // We delay next call a little bit in order to make it easier
    // to look at the logic analyzer output
    return DELAY_BETWEEN_TEST_MS;
}
