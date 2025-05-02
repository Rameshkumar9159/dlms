/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef DLMS_LOCK_H_
#define DLMS_LOCK_H_

#include <stdint.h>
#include <stdbool.h>

typedef void (*on_lock_acquired_cb_t)(void);

typedef enum {
    DLMS_LOCK_RET_ERROR = -1,
    DLMS_LOCK_RET_OK = 0,
    DLMS_LOCK_RET_ACQUIRED,
    DLMS_LOCK_RET_NOT_ACQUIRED,
    DLMS_LOCK_RET_WAITING_FOR_LOCK,
    DLMS_LOCK_RET_ALREADY_OWNER
} Dlms_Lock_return_code_e;

/* Internal algorithm always give lock to the lowest id when multiple
   IDs are waiting for the lock.
   So declaration is in order of priority
 */
typedef enum {
    DLMS_LOCK_ID_INSTANTANEOUS = 0,
    DLMS_LOCK_ID_PASSTHROUGH,
    DLMS_LOCK_ID_NIC_SERVER,
    DLMS_LOCK_ID_ESW,
    DLMS_LOCK_ID_CUSTOM,
    DLMS_LOCK_ID_BLOCKLOAD,
    DLMS_LOCK_ID_DAILYLOAD,
    DLMS_LOCK_ID_FIXED_DAY_BILLING,
    DLMS_LOCK_ID_BILLING,
    DLMS_LOCK_ID_EXPORT_BILLING,
    DLMS_LOCK_ID_EVENT_LOG,
    DLMS_LOCK_ID_NAME_PLATE,
    DLMS_LOCK_ID_ID,
    DLMS_LOCK_ID_FIRMWARE_UPDATE,
    DLMS_LOCK_ID_CUSTOM_RECURRING,
    // Add the new log before this line
    DLMS_LOCK_ID_LAST,
    // Special value that is higher than LAST
    DLMS_LOCK_ID_FREE,
} Dlms_Lock_id_e;

typedef enum {
    DLMS_LOCK_TYPE_UNKNOWN,
    DLMS_LOCK_TYPE_WITHOUT_TRAFFIC, // No wirepas traffic expected by lock owner
    DLMS_LOCK_TYPE_WITH_TRAFFIC // Wirepas trafic is expected by lock owner
} Dlms_Lock_type_e;

/**
 * \brief    Initialize DLMS Lock module
 *           DLMS Lock is a very important concept. Communicating with the meter or starting NIC server or Client can only
 *           be done if the DLMS lock is held. In fact, Gurux DLMS library allocate lot of memory, so to have a predictable
 *           and consistent behavior of the app, some task execution MUST be explicit
 * \note     Any unfreed allocated memory will be automatically released when lock is released. In other words,memory can only be
 *           allocated by the owner of the lock.
 * \return   Return code of the operation
 */
Dlms_Lock_return_code_e Dlms_lock_init(void);

/**
 * \brief    Ask to take the lock
 * \param    id
 *           Id of the lock owner
 * \param    cb
 *           If set and Lock is not ready at the moment, this cb will be called when the lock becomes ready and this function
 *           will return DLMS_LOCK_RET_WAITING_FOR_LOCK.
 * \param    type
 *           Type of the lock
 * \return   Return code of the operation
*/
Dlms_Lock_return_code_e Dlms_lock_take(Dlms_Lock_id_e id,
                                       on_lock_acquired_cb_t cb,
                                       Dlms_Lock_type_e type);

/**
 * \brief    Release lock
 * \param    id
 *           Id of the lock to release
 * \return   Return code of the operation
 */
Dlms_Lock_return_code_e Dlms_lock_release(Dlms_Lock_id_e id);

/**
 * \brief    Ask to change the lock type
 * \param    id
 *           Id of the lock owner
 * \param    type
 *           New type of the lock
 * \return   Return DLMS_LOCK_RET_ACQUIRED if the caller is the lock owner,
 *           return DLMS_LOCK_RET_ERROR otherwise
*/
Dlms_Lock_return_code_e Dlms_lock_changeType(Dlms_Lock_id_e id,
                                             Dlms_Lock_type_e new_type);


#endif /* DLMS_LOCK_H */
