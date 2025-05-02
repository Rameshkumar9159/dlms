/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    preamble_app.c
 * \brief   This app is the first one starting and is executed after boot.
 *          Once execution is finished, the code cannot be called anymore as
 *          its reserved ram is erased (static) to be reused by the main app
 *          for its heap.
 */
#include <string.h>

#include "preamble_app.h"
#include "nic_provisioning.h"
#include "common.h"
#include "app_scheduler.h"
#include "usart.h"

#define DEBUG_LOG_MODULE_NAME "PRE_APP "
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

extern unsigned int __heap_start__;
extern unsigned int __preamble_app_data_start__;
extern unsigned int __preamble_app_data_end__;
extern unsigned int __preamble_app_bss_start__;
extern unsigned int __preamble_app_bss_end__;
extern unsigned int __preamble_app_data_src_start__;
extern unsigned int __heap_size__;

static on_preamble_app_end_cb m_end_cb = NULL;

bool Preamble_app_start(on_preamble_app_end_cb cb)
{
    const unsigned int *src;
    unsigned int *dst;
    uint32_t start, end;

    // Prepare the RAM (data and bss for preamble app)
    start = (uint32_t)&__preamble_app_data_start__;
    end = (uint32_t)&__preamble_app_data_end__;
    LOGI("Copy %u bytes FLASH -> RAM at 0x%x", end - start,
         &__preamble_app_data_start__);
    (void) start, (void)end;
    /* Copy data from flash to RAM */
    for (src = &__preamble_app_data_src_start__,
         dst  = &__preamble_app_data_start__;
         dst != &__preamble_app_data_end__;)
    {
        *dst++ = *src++;
    }

    start = (uint32_t)&__preamble_app_bss_start__;
    end = (uint32_t)&__preamble_app_bss_end__;
    LOGI("Initialize bss from 0x%x (%u bytes)",
         &__preamble_app_bss_start__, end - start);
    (void) start, (void)end;
    /* Initialize the .bss section */
    for (dst = &__preamble_app_bss_start__; dst != &__preamble_app_bss_end__;)
    {
        *dst++ = 0;
    }

    // RAM is ready
    m_end_cb = cb;

    // Starting custom app
    Nic_Provisioning_start(Preamble_app_stop);

    return true;
}

void Preamble_app_stop(void)
{
    uint8_t * heap_start = (uint8_t *) &__heap_start__;
    uint32_t  heap_size  = (uint32_t) &__heap_size__;

    // Keep a reference to callback on stack as we are cleaning
    // our static area
    on_preamble_app_end_cb end_cb = m_end_cb;

    LOGI("End of preamble app, restore heap");

    // Clean the RAM (heap for the malloc)
    memset(heap_start, 0, heap_size);

    LOGI("Heap size = %u bytes", heap_size);

    if (end_cb == NULL)
    {
        LOGE("Cb is NULL");
    }
    else
    {
        end_cb();
    }
}
