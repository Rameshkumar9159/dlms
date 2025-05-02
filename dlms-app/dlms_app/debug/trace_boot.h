/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    trace_boot.h
 * \brief   Print boot info
 */

#ifndef TRACE_BOOT_H_
#define TRACE_BOOT_H_

#include <stdint.h>

#ifdef APP_PRINTING
void trace_boot_print(void);
#else
#define trace_boot_print()
#endif

#endif // TRACE_BOOT_H_
