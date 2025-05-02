/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include "dlms_lock.h"
#include <string.h> // for memset

#include "api.h"
#include "common.h"

#define DEBUG_LOG_MODULE_NAME "M_LOCK  "
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"


static Dlms_Lock_id_e m_lock;

typedef struct {
    on_lock_acquired_cb_t cb;
    Dlms_Lock_type_e type;
    bool waiting;
} waiting_lock_t;

// DLMS_LOCK_ID_LAST is number of lock owner (rename it for clarity)
#define LOCK_REQUESTER_NB (DLMS_LOCK_ID_LAST)

// Create a table for waiter of the lock (1 entry per lock)
static waiting_lock_t m_waiting_queue[LOCK_REQUESTER_NB];


Dlms_Lock_return_code_e Dlms_lock_init(void)
{
    memset(m_waiting_queue, 0, sizeof(m_waiting_queue));
    m_lock = DLMS_LOCK_ID_FREE;

    return DLMS_LOCK_RET_OK;
}

Dlms_Lock_return_code_e Dlms_lock_take(Dlms_Lock_id_e id,
                                       on_lock_acquired_cb_t cb,
                                       Dlms_Lock_type_e type)
{
    // Check id is valid
    if (id >= DLMS_LOCK_ID_LAST)
    {
        LOGE("%d is not a valid lock", id);
        return DLMS_LOCK_RET_ERROR;
    }

    // Lock must always be taken from task context
    if (isInterrupt())
    {
        // not a good practice to print log under IRQ but it should not happen
        LOGE("No Lock in IRQ");
        return DLMS_LOCK_RET_ERROR;
    }

    if (m_lock == DLMS_LOCK_ID_FREE)
    {
        // Lock is free, take it
        m_lock = id;
        LOGD("Lock aquired by %d", id);
        // Check if lock owner needs traffic otherwise stop it
        // as we don't want/can't queue request for later
        if (type == DLMS_LOCK_TYPE_WITHOUT_TRAFFIC)
        {
            lib_data->allowReception(false);
        }
        else
        {
            lib_data->allowReception(true);
        }
        return DLMS_LOCK_RET_ACQUIRED;
    }

    if (m_lock == id)
    {
        LOGE("%d already has the lock", id);
        return DLMS_LOCK_RET_ALREADY_OWNER;
    }

    LOGD("Lock already held by %d ", m_lock);

    if (cb == NULL)
    {
        LOGI("No cb specified by %d, return directly", id);
        /* Caller do not want to be queued */
        return DLMS_LOCK_RET_NOT_ACQUIRED;
    }

    /* Check if lock is not in the queue yet */
    if (m_waiting_queue[id].waiting)
    {
        LOGE("Client %d already in the queue", id);
        return DLMS_LOCK_RET_ERROR;
    }

    /* Add lock to the table */
    m_waiting_queue[id].cb = cb;
    m_waiting_queue[id].type = type;
    m_waiting_queue[id].waiting = true;

    LOGI("%d added to the queue", id);

    return DLMS_LOCK_RET_WAITING_FOR_LOCK;
}

static uint32_t _lock_release()
{
    LOGE("Lock release in IRQ handler is not allowed");
    LOGI( "Unlock is done in a deferred task");
    Dlms_lock_release(m_lock);

    return APP_SCHEDULER_STOP_TASK;
}

Dlms_Lock_return_code_e Dlms_lock_release(Dlms_Lock_id_e id)
{
    // Check id is valid
    if (id >= DLMS_LOCK_ID_LAST)
    {
        LOGE("%d is not a valid lock", id);
        return DLMS_LOCK_RET_ERROR;
    }

    if (isInterrupt())
    {
        // This must not happen, but in order to avoid a deadlock
        // we will schedule a task to do the release

        // The lock check is racy but it should not lead to a wrong result.
        if (id == m_lock)
        {
            App_Scheduler_addTask_execTime(_lock_release,
                    APP_SCHEDULER_SCHEDULE_ASAP,
                    50);
            /* It will be released, but still notify the caller that it is an error */
            return DLMS_LOCK_RET_ERROR;
        }
        else
        {
            // Double issue! Unlock with a wrong lock id in IRQ context...
            // Let's try to print the error log message of the standard path
        }
    }

    if (id != m_lock)
    {
        LOGE("Cannot release lock %d, held by %d", id, m_lock);
        return DLMS_LOCK_RET_ERROR;
    }

    // Lock is held by us, release it
    m_lock = DLMS_LOCK_ID_FREE;

    // Check if someone is waiting for the lock
    // Read table in order so lowest id has highest priority
    for (uint8_t i=0; i < LOCK_REQUESTER_NB; i++)
    {
        if (m_waiting_queue[i].waiting)
        {
            // Someone waiting for the lock, give it
            m_lock = i;
            // Requester is not waiting anymore
            m_waiting_queue[i].waiting = false;
            LOGI("Lock owner has changed to %d", m_lock);
            break;
        }
    }

    // Call new owner
    if (m_lock != DLMS_LOCK_ID_FREE)
    {
        m_waiting_queue[m_lock].cb();
        if (m_waiting_queue[m_lock].type == DLMS_LOCK_TYPE_WITHOUT_TRAFFIC)
        {
            lib_data->allowReception(false);
        }
        else
        {
            lib_data->allowReception(true);
        }
    }
    else
    {
        LOGD("Lock is free, no one is waiting for it");
        /* Enable data reception globally (without using SharedLib api as we don't have explicitly a filter here) */
        lib_data->allowReception(true);
    }

    return true;
}

/**
 * \brief    Ask to change the lock type
 * \param    id
 *           Id of the lock owner
 * \param    type
 *           Type of the lock
 * \return   Return DLMS_LOCK_RET_ACQUIRED if the caller is the lock owner,
 *           return DLMS_LOCK_RET_ERROR otherwise
 */
Dlms_Lock_return_code_e Dlms_lock_changeType(Dlms_Lock_id_e id,
                                             Dlms_Lock_type_e new_type)
{
    // Check id is valid
    if (id >= DLMS_LOCK_ID_LAST)
    {
        LOGE("%d is not a valid lock", id);
        return DLMS_LOCK_RET_ERROR;
    }

    // Lock must always be taken from task context
    if (isInterrupt())
    {
        // not a good practice to print log under IRQ but it should not happen
        LOGE("No Lock in IRQ");
        return DLMS_LOCK_RET_ERROR;
    }

    if (id != m_lock)
    {
        LOGE("Cannot change type of lock %d, held by %d", id, m_lock);
        return DLMS_LOCK_RET_ERROR;
    }

    if (new_type == DLMS_LOCK_TYPE_WITHOUT_TRAFFIC)
    {
        lib_data->allowReception(false);
    }
    else
    {
        lib_data->allowReception(true);
    }
    LOGI("Lock type changed");

    return DLMS_LOCK_RET_ACQUIRED;
}
