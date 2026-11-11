/**************************************************************************
** FILE_FETCH - read one SINTRAN file for the FTP server                 **
**                                                                       **
** The FTP server (ftp_server.h) runs in the telnet task; the COSMOS     **
** file-access client (libcosmos fa/fa_client.h) must run in the node    **
** task, which owns the XMSG node. This module is the go-between: the    **
** FTP side asks for a file, the node task runs the client, and the      **
** file's bytes land in one buffer in PSRAM that the FTP side reads from **
** while they arrive.                                                    **
**                                                                       **
** One file at a time: the FTP server serves one user at a time.         **
**                                                                       **
** The whole file is kept, up to FILE_FETCH_MAX bytes; a longer file is   **
** refused when its length is known (the open reply), before any data is **
** read. PSRAM has about 1.9 MB free (INFO, 2026-11-11).                 **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef FILE_FETCH_H
#define FILE_FETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libxmsg.h>

/** Longest file read, in bytes. */
#define FILE_FETCH_MAX (1024u * 1024u)

/** Where a fetch stands. */
typedef enum
{
    FILE_FETCH_IDLE = 0,   /**< nothing asked */
    FILE_FETCH_ASKED,      /**< asked; the node task has not started it yet */
    FILE_FETCH_RUNNING,    /**< the client is working; bytes may be arriving */
    FILE_FETCH_DONE,       /**< every byte is in */
    FILE_FETCH_FAILED      /**< ended without the file; see result and sintran_error */
} file_fetch_state;

/** A snapshot for the FTP side. */
typedef struct
{
    file_fetch_state state;
    uint32_t length;        /**< from the open reply; 0 until then */
    uint32_t have;          /**< bytes in the buffer so far */
    int result;             /**< the client's result: 0, FA_CLIENT_EREFUSED or XMSG_E* */
    uint16_t sintran_error; /**< with FA_CLIENT_EREFUSED: SINTRAN's error number */
    bool too_long;          /**< refused: longer than FILE_FETCH_MAX */
} file_fetch_status;

/** @brief Buffers (PSRAM). Once, before the tasks run. @return 0 or -1. */
int file_fetch_init(void);

/**
 * @brief Ask for a file. FTP side.
 * @param system    The SINTRAN machine's system number.
 * @param user      SINTRAN user: whose directory, and who asks.
 * @param password  That user's password ("" for none).
 * @param spec      "NAME:TYPE".
 * @return 0, or -1 when a fetch is already under way or an argument does not fit.
 */
int file_fetch_start(uint16_t system, const char *user, const char *password, const char *spec);

/** @brief Read the state. FTP side. */
void file_fetch_get(file_fetch_status *out);

/**
 * @brief Copy file bytes from the buffer. FTP side. Only bytes below `have` are valid.
 * @return The number copied.
 */
size_t file_fetch_copy(uint32_t offset, uint8_t *out, size_t max);

/** @brief The FTP side is finished with the file (sent, or the user left): back to IDLE
 *         once the client is over. */
void file_fetch_release(void);

/** @brief Work for the node task: start an asked fetch. */
void file_fetch_pass(XmsgNode *node, uint32_t now);

/** @brief A short plain-words reason for a failed fetch, for the FTP reply. */
void file_fetch_reason(const file_fetch_status *st, char *out, size_t cap);

#endif /* FILE_FETCH_H */
