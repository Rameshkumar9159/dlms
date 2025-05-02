/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#define DEBUG_LOG_MODULE_NAME "RNG"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#include "rng.h"
#include "random.h"

#define RANDOM_RAND_MAX                     INT_MAX

void Rng_init(void)
{
    app_addr_t node_addr;
    uint32_t seed;

    if (APP_RES_OK == lib_settings->getNodeAddress(&node_addr))
    {
        seed = node_addr;
    }
    else
    {
        seed = lib_time->getTimestampHp();
    }
    Random_init(seed);
}

uint32_t Rng_number(uint32_t limit)
{
    uint32_t nb;

    // Remove the modulo bias, refer to
    // https://stackoverflow.com/questions/10984974/why-do-people-say-there-is-modulo-bias-when-using-a-random-number-generator
    do
    {
        nb = Random_get32();
    }
    while (nb >= (RANDOM_RAND_MAX - (RANDOM_RAND_MAX % limit)));

    nb %= limit;

    return nb;
}