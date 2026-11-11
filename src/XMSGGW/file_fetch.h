/**************************************************************************
** FILE_FETCH - SINTRAN file jobs for the FTP server                     **
**                                                                       **
** The FTP server (ftp_server.h) runs in the telnet task; the COSMOS     **
** file-access client (libcosmos fa/fa_client.h) must run in the node    **
** task, which owns the XMSG node. This module is the go-between: the    **
** FTP side asks for a job, the node task runs the client, and the       **
** results land in PSRAM that the FTP side reads from while they arrive. **
**                                                                       **
** Two kinds of job:                                                     **
**  - read one file (fa_client_read): its bytes, up to FILE_FETCH_MAX;   **
**    a longer file is refused when its length is known (the open        **
**    reply), and its bytes are dropped;                                 **
**  - list a user's files (fa_client_list, the walk SINTRAN's LIST-FILES **
**    and FILE-STATISTICS both use): one entry per file, up to           **
**    FILE_FETCH_LIST_MAX. With a filter "NAME:TYPE" SINTRAN matches it  **
**    itself (libxmsg, 2026-11-11).                                      **
**                                                                       **
** One job at a time: the FTP server serves one user at a time.          **
** PSRAM has about 1.9 MB free (INFO, 2026-11-11).                       **
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
#include <libcosmos/fa/fa_replies.h>

/** Longest file read, in bytes. */
#define FILE_FETCH_MAX (1024u * 1024u)

/** Most file entries one listing keeps. */
#define FILE_FETCH_LIST_MAX 1024u

/** Where a job stands. */
typedef enum
{
    FILE_FETCH_IDLE = 0,   /**< nothing asked */
    FILE_FETCH_ASKED,      /**< asked; the node task has not started it yet */
    FILE_FETCH_RUNNING,    /**< the client is working; bytes or entries may be arriving */
    FILE_FETCH_DONE,       /**< all in */
    FILE_FETCH_FAILED      /**< ended without it; see result and sintran_error */
} file_fetch_state;

/** A snapshot for the FTP side. */
typedef struct
{
    file_fetch_state state;
    uint32_t length;        /**< read: from the open reply; 0 until then */
    uint32_t have;          /**< read: bytes in the buffer so far */
    uint32_t entries;       /**< list: entries kept so far */
    bool list_full;         /**< list: more files than FILE_FETCH_LIST_MAX; the rest dropped */
    int result;             /**< the client's result: 0, FA_CLIENT_EREFUSED or XMSG_E* */
    uint16_t sintran_error; /**< with FA_CLIENT_EREFUSED: SINTRAN's error number */
    bool too_long;          /**< read refused: longer than FILE_FETCH_MAX */
} file_fetch_status;

/** @brief Buffers (PSRAM). Once, before the tasks run. @return 0 or -1. */
int file_fetch_init(void);

/**
 * @brief Ask for a file. FTP side.
 * @param system    The SINTRAN machine's system number.
 * @param user      SINTRAN user: whose directory, and who asks.
 * @param password  That user's password ("" for none).
 * @param spec      "NAME:TYPE".
 * @return 0, or -1 when a job is already under way or an argument does not fit.
 */
int file_fetch_start(uint16_t system, const char *user, const char *password, const char *spec);

/**
 * @brief Ask for a user's files. FTP side.
 * @param filter "" for every file, or "NAME:TYPE" (SINTRAN matches it).
 * @return As file_fetch_start().
 */
int file_fetch_list(uint16_t system, const char *user, const char *password, const char *filter);

/** @brief Read the state. FTP side. */
void file_fetch_get(file_fetch_status *out);

/**
 * @brief Copy file bytes from the buffer. FTP side. Only bytes below `have` are valid.
 * @return The number copied.
 */
size_t file_fetch_copy(uint32_t offset, uint8_t *out, size_t max);

/**
 * @brief Copy listing entries, from index `from`. FTP side. Only entries below `entries`
 *        are valid.
 * @return The number copied.
 */
size_t file_fetch_entries(uint32_t from, FaFileEntry *out, size_t max);

/** @brief The FTP side is finished with the job (sent, or the user left): back to IDLE
 *         once the client is over. */
void file_fetch_release(void);

/** @brief Work for the node task: start an asked job. */
void file_fetch_pass(XmsgNode *node, uint32_t now);

/** @brief A short plain-words reason for a failed job, for the FTP reply. */
void file_fetch_reason(const file_fetch_status *st, char *out, size_t cap);

#endif /* FILE_FETCH_H */
