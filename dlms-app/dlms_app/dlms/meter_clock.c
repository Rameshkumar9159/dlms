/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#define DEBUG_LOG_MODULE_NAME "METER_CLK"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"


static uint32_t m_epoch;
static uint32_t m_timer_start;
static uint16_t m_deviation;
static uint32_t m_new_clock_epoch; // 0 if no new clock to set
static uint16_t m_new_deviation;
static uint32_t m_new_clock_local_ts_s; // When was the previous field set

void MeterClock_set(uint32_t epoch, int16_t deviation)
{
    m_epoch = epoch;
    m_deviation = deviation;
    // Start timer
    m_timer_start = lib_time->getTimestampS();
}

bool MeterClock_get(uint32_t * epoch_p, int16_t * deviation_p)
{
    // if setClock has not been called before a getClock is issued,
    // we just return as it means no timestamp when generating
    // DLMS DataNotification
    if (! m_epoch && ! m_timer_start)
    {
        return false;
    }

    if (epoch_p != NULL)
    {
        // The wrap cycle is long enough (136 years) to be of no concern.
        *epoch_p = m_epoch + lib_time->getTimestampS() - m_timer_start;
    }

    if (deviation_p != NULL)
    {
        *deviation_p = m_deviation;
    }
    return true;
}

void MeterClock_set_new_meter_clock(uint32_t epoch, int16_t deviation)
{
    m_new_clock_epoch = epoch;
    m_new_deviation = deviation;
    // Take the current time is second to compensate when asked later on
    m_new_clock_local_ts_s = lib_time->getTimestampS();
}

bool MeterClock_get_new_meter_clock(uint32_t * epoch_p, int16_t * deviation_p)
{
    if (m_new_clock_epoch == 0)
    {
        return false;
    }
    *epoch_p = m_new_clock_epoch + (lib_time->getTimestampS() - m_new_clock_local_ts_s);
    *deviation_p = m_new_deviation;

    return true;
}
