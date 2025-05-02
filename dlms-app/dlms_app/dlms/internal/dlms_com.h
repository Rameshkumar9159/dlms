/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef DLMS_COM_H_
#define DLMS_COM_H_

#include <stdint.h>

#include "usart.h"

/* ************************************ */
/* PUBLIC API                           */
#include "include/message.h"
#include "include/replydata.h"
#include "common.h"

// Parameter used during connection
#ifndef DLMS_COM_PDU_SIZE
#define DLMS_COM_PDU_SIZE  512
#endif

void Dlms_Com_init();
void Dlms_Com_call_async(message * messages, gxReplyData * reply,
                         operation_result_cb cb);
void Dlms_Com_call_async_with_timeout(message * messages, gxReplyData * reply,
                                      operation_result_cb cb, uint16_t timeout_s);

#endif /* DLMS_COM_H_ */
