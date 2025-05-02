/* Copyright 2024 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef TRANSPARENT_MODE_H_
#define TRANSPARENT_MODE_H_

#include <stdint.h>

bool Transparent_mode_init(void);

bool Transparent_mode_enable(void);

bool Transparent_mode_disable(void);

bool Transparent_mode_is_enabled(void);

#endif /* TRANSPARENT_MODE_H_ */
