/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    preamble_app.h
 * \brief   common interface for the DLMS application
 */

#ifndef PREAMBLE_APP_H_
#define PREAMBLE_APP_H_

#include <stdint.h>
#include <stdbool.h>

typedef void (*on_preamble_app_end_cb)(void);

/* \brief Function called to start the preamble app
   \param cb
          Callback to call when the preamble app has finish its execution
*/
bool Preamble_app_start(on_preamble_app_end_cb cb);

void Preamble_app_stop(void);

#endif // PREAMBLE_APP_H_
