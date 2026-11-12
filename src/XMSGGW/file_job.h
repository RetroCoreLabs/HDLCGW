/**************************************************************************
** FILE_JOB - SINTRAN file jobs, shared by the FTP server and the        **
**            console's file commands                                    **
**                                                                       **
** The COSMOS file-access client (libcosmos fa/fa_client.h) must run in  **
** the node task, which owns the XMSG node. The FTP server runs in the   **
** telnet task and the console commands in the node task. A job is the   **
** go-between for both: the front end asks, the node task runs the       **
** client (file_job_pass_all), and what SINTRAN sends lands in two rings  **
** the front end takes from while it arrives:                            **
**  - read one file (fa_client_read): its bytes;                         **
**  - list a user's files (fa_client_list, the walk SINTRAN's LIST-FILES **
**    and FILE-STATISTICS both use): one entry per file. With a filter   **
**    "NAME:TYPE" SINTRAN matches it itself (libxmsg, 2026-11-11).       **
**                                                                       **
** A ring that fills because the front end falls behind drops the rest   **
** and says so (lost flags); nothing waits, as the client cannot be       **
** paused. Each front end owns one job, so a console command and an FTP  **
** transfer can run at the same time; one job runs one thing at a time.  **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef FILE_JOB_H
#define FILE_JOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libxmsg.h>
#include <libcosmos/fa/fa_replies.h>

/** Jobs the gateway can have: the FTP server's and the console's. */
#define FILE_JOB_MAX 2u

/** One job; its insides are file_job.cpp's. */
typedef struct FileJob FileJob;

/** Where a job stands. */
typedef enum
{
    FILE_JOB_IDLE = 0,   /**< free */
    FILE_JOB_ASKED,      /**< asked; the node task has not started it yet */
    FILE_JOB_RUNNING,    /**< the client works; bytes or entries may be arriving */
    FILE_JOB_ENDED       /**< the client is over (see result); the rings may still hold some */
} file_job_state;

/** A snapshot for the front end. */
typedef struct
{
    file_job_state state;
    int result;             /**< when ENDED: 0, FA_CLIENT_EREFUSED or XMSG_E* */
    uint16_t sintran_error; /**< with FA_CLIENT_EREFUSED: SINTRAN's error number */
    uint32_t length;        /**< read: from the open reply; 0 until then */
    uint32_t bytes;         /**< read: bytes SINTRAN has sent so far */
    uint32_t entries;       /**< list: entries SINTRAN has sent so far */
    bool data_lost;         /**< the byte ring was full: bytes were dropped */
    uint32_t entries_lost;  /**< entries dropped because the entry ring was full */
    char directory[24];     /**< "PACK-ONE"; "" until the client names it */
    char user[24];          /**< SINTRAN's full user name, "SYSTEM"; "" until named */
} file_job_status;

/**
 * @brief Make a job, with its rings in PSRAM. Once per front end, before the tasks run.
 * @param name       For the log: "FTP", "console".
 * @param data_cap   Bytes the byte ring holds.
 * @param entry_cap  Entries the entry ring holds.
 * @return The job, or NULL when memory is short or FILE_JOB_MAX jobs exist.
 */
FileJob *file_job_create(const char *name, size_t data_cap, size_t entry_cap);

/**
 * @brief Ask for a file (read) or a user's files (list). Any task.
 * @param job       The job.
 * @param list      true to list, false to read.
 * @param system    The SINTRAN machine's system number.
 * @param user      SINTRAN user: whose directory, and who asks.
 * @param password  That user's password ("" for none). Wiped once the client has it.
 * @param spec      read: "NAME:TYPE"; list: "" for every file, or a SINTRAN filter.
 * @return 0, or -1 when the job is not IDLE or an argument does not fit.
 */
int file_job_start(FileJob *job, bool list, uint16_t system, const char *user, const char *password,
                   const char *spec);

/** @brief Read the state. Any task. */
void file_job_get(FileJob *job, file_job_status *out);

/**
 * @brief Take file bytes from the ring, in order. Any task, the front end only.
 * @return The number taken (0 when none are waiting).
 */
size_t file_job_take_bytes(FileJob *job, uint8_t *out, size_t max);

/**
 * @brief Take the next entry. Any task, the front end only.
 * @return true when one was taken.
 */
bool file_job_take_entry(FileJob *job, FaFileEntry *out);

/**
 * @brief The front end is finished with the job (all taken, or the user stopped or left).
 *        A job still asked or running is stopped: the node task calls fa_client_cancel,
 *        SINTRAN's work ends, and the job is IDLE once the client is over. Any task.
 */
void file_job_release(FileJob *job);

/** @brief Work for the node task: start asked jobs and stop released ones. */
void file_job_pass_all(XmsgNode *node, uint32_t now);

/** @brief A short plain-words reason for a job that ended with an error. */
void file_job_reason(const file_job_status *st, char *out, size_t cap);

#endif /* FILE_JOB_H */
