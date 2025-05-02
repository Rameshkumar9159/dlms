/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    common.h
 * \brief   common interface for the DLMS application
 */

#ifndef COMMON_H_
#define COMMON_H_

#include <stdint.h>
#include <stdbool.h>

#include "include/dlmssettings.h"

#include "server_attribute_manager.h"
#include "sfmalloc.h"
#include "mcu.h"
#include "app_scheduler.h"

#define OFFICIAL_TAG    "v1.4"

/** DLMS NIC Server address */
#define NIC_SERVER_ADDRESS      100

/** DLMS Meter address (passthrough) */
#define METER_SERVER_ADDRESS    1

// Public Client
#define PC_CLIENT_ADDRESS               0x10
// Meter Reader
#define MR_CLIENT_ADDRESS               0x20
// Utility Setting
#define US_CLIENT_ADDRESS               0x30
// Push association
#define PUSH_CLIENT_ADDRESS             0x40
// Firmware Upgrade
#define FU_CLIENT_ADDRESS               0x50

#define WRAPPER_VERSION 0x0001

/** DLMS wrapper mode header*/
typedef struct
{
    uint16_t    version;
    uint16_t    source;
    uint16_t    destination;
    uint16_t    length;
} wrapper_header_t;

#define OBIS_CODE_SIZE                  6
typedef const uint8_t obis_code_t[OBIS_CODE_SIZE];

static inline bool isInterrupt(void)
{
    return __get_IPSR() != 0;
}

#define SFSM_EXECUTION_TIME_US (99 * 1000) // max

#define SFSM_DELAY_PAUSED       APP_SCHEDULER_STOP_TASK
// 100 ms
#define SFSM_DELAY_SHORT        100
// Delay when something wrong happen to retry (30 sec)
#define SFSM_DELAY_RETRY        (30 * 1000)

#define SFSM_ENTRY(state)       LOGD("-> %u", state); CONTEXT_PUSH();

#define SFSM_EXIT()             LOGD("<-"); CONTEXT_POP();

// Macro to determine the size of an array
#define ARRAY_SIZE(a)               (sizeof((a)) / sizeof((a)[0]))

#define MIN(a, b)                       ((a) < (b) ? (a) : (b))
#define MAX(a, b)                       ((a) > (b) ? (a) : (b))

#define PRINTABLE_SYS_TITLE_MAX_SIZE    17

#if defined HEAP_MEM_SIZE || defined HEAP_FROM_LINKER
#define SFSM_CHECK()            \
    do \
    { \
        struct mallinfo info = mallinfo(); \
        /* Fix compilation warning when logs are disabled */ \
        (void)info.arena; \
        LOGI("Malloc :  heap %u / allocated %u / free %u", \
            info.arena, info.uordblks, info.fordblks); \
        sf_free_all(); \
    } \
    while (0)
#else // HEAP_MEM_SIZE
#define SFSM_CHECK()
#endif // HEAP_MEM_SIZE

// Generic type of calback to resume the caller
typedef void (* operation_result_cb) (int32_t result);

dlmsSettings * Common_getMeterSettings(void);
dlmsSettings * Common_getPushSettings(void);
void Common_formatSystemTitle(const uint8_t * sys_title_p, char * str_p, uint8_t str_size);
void Common_reboot(uint16_t delay_s);

#ifdef USE_SAFE_MALLOC

#define CONTEXT_PUSH()  Common_pushContextName(__func__)
#define CONTEXT_POP()   Common_popContextName()

void Common_pushContextName(const char * name);
void Common_popContextName();

#else

#define CONTEXT_PUSH()
#define CONTEXT_POP()

#endif

DLMS_INTERFACE_TYPE Common_getNicInterfaceType(void);

uint32_t Common_getNicBaudrate(void);

const char * Common_getContextName(void);

void Common_logBuffer(const char * msg, uint8_t level, const uint8_t * buffer, uint16_t size);

void Common_wrapperFrameGetHeader(const uint8_t * bytes, wrapper_header_t * hdr_p);

#endif // COMMON_H_
