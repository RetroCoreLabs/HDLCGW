/**************************************************************************
** FTP_SERVER - FTP into the SINTRAN machines                            **
**                                                                       **
** Ronny's design (2026-11-11). An FTP client logs in with a gateway     **
** user (users.h, the same users as telnet). The folders are:            **
**                                                                       **
**   /                     the machines, by the names read from the host  **
**                         (names.h), without this gateway itself         **
**   /<machine>            one folder: the gateway user's SINTRAN user    **
**                         (sintran_accounts.h)                           **
**   /<machine>/<user>     that user's files: LIST, NLST, SIZE, MDTM and **
**                         RETR, through libcosmos' COSMOS file client   **
**                         (file_job.h)                                  **
**                                                                       **
** Passive mode (PASV, EPSV) and active mode (PORT, EPRT, only back to   **
** the client's own address).                                            **
**                                                                       **
** Not built yet either: showing only the machines that are online right **
** now (Ronny wants that, like CONNECT-TO's LIST-SYSTEMS). How to ask    **
** each machine is still to be settled from a capture by the libxmsg     **
** session; until then the top folder lists every machine the host      **
** knows by name.                                                        **
**                                                                       **
** One FTP user at a time (Ronny, 2026-11-11). Plain FTP: the password    **
** crosses the network in clear, as telnet's does.                       **
**                                                                       **
** Run by the telnet task (telnet_task.cpp), which owns WiFi: no task of  **
** its own, because a task's stack costs internal RAM, which is short.   **
** A login runs PBKDF2 (about 0.46 s, users.h) and holds the telnet      **
** task for that long.                                                   **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef FTP_SERVER_H
#define FTP_SERVER_H

#include <stdbool.h>
#include <stdint.h>

/** Control connection port. */
#define FTP_SERVER_PORT 21

/** Passive-mode data port. One FTP user at a time, so one fixed port is enough. */
#define FTP_SERVER_DATA_PORT 2121

/** A control connection with no command and no transfer for this long is closed (RFC 959
 *  leaves it open). It frees the one FTP place from a client left connected; 30 minutes so
 *  that a user who is only reading is not thrown out (Ronny, 2026-11-12; was 5). */
#define FTP_SERVER_IDLE_MS (30u * 60u * 1000u)

/** How long a LIST or RETR waits for the client to open the data connection. */
#define FTP_SERVER_DATA_WAIT_MS 10000u

/** @brief Work buffers (PSRAM). Once, from setup(). @return 0 or -1. */
int ftp_server_init(void);

/** @brief WiFi is up: start listening. Telnet task. */
void ftp_server_start(void);

/** @brief WiFi is gone: drop the user, stop listening. Telnet task. */
void ftp_server_stop(void);

/** @brief One pass: accept, read commands, move listing bytes. Telnet task. */
void ftp_server_pass(uint32_t now);

/** @brief Counters for INFO / STAT. */
typedef struct
{
    uint32_t sessions;      /**< control connections accepted */
    uint32_t refused;       /**< turned away: another FTP user was on */
    uint32_t logins;        /**< good logins */
    uint32_t bad_logins;    /**< wrong user or password */
    uint32_t listings;      /**< LIST/NLST sent */
    uint32_t files;         /**< files sent (RETR) or stored (STOR) */
    bool user_on;           /**< an FTP client is connected now */
    char user[32];          /**< with user_on: the gateway user logged in; "" before login */
    char remote[16];        /**< with user_on: the client's IP address */
    uint32_t on_ms;         /**< with user_on: milliseconds since it connected */
} ftp_server_stats;

/** @brief Read the counters and who is on. Any task (INFO calls it from the node task). */
void ftp_server_get_stats(ftp_server_stats *out);

#endif /* FTP_SERVER_H */
