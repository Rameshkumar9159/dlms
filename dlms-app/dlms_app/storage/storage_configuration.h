/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */
#ifndef STORAGE_CONFIGURATION_H_
#define STORAGE_CONFIGURATION_H_

// ---------------------
// Storage configuration
// ---------------------
// App persistent memory (refer to the app_persistent area in dlms_app.ini)
// 64 KB (8 pages of 8 KB each in internal flash)
//   - 1st page is reserved for provisionning app
//   - 2 pages are used to store the server persistent data
//     sector size is defined as 256 B
//   - 5 pages are used to store the client persistent data
//     sector size is defined as 128 B
#define RESERVED_BLOCK_NUMBER               1

#define SERVER_STORAGE_BLOCK_OFFSET         RESERVED_BLOCK_NUMBER
#define SERVER_STORAGE_BLOCK_NB             2
#define SERVER_STORAGE_SECTOR_SIZE          512

#define CLIENT_STORAGE_BLOCK_OFFSET         (SERVER_STORAGE_BLOCK_OFFSET + \
                                             SERVER_STORAGE_BLOCK_NB)
#define CLIENT_STORAGE_BLOCK_NB             5
#define CLIENT_STORAGE_SECTOR_SIZE          128

#endif /* STORAGE_CONFIGURATION_H_ */
