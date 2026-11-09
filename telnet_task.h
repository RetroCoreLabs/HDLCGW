/**************************************************************************
** TELNET_TASK - WiFi, and a telnet user's terminal session to the host  **
**                                                                       **
** Joins the stored WiFi network (settings_wifi) and listens on TCP port **
** 23, for up to TELNET_TASK_USERS users at once. A user first logs in  **
** to XMSGGW (users.h; libxmsg auth_login), then gets the "XMSGGW> "     **
** prompt: CONNECT <system>, LIST, WHO, INFO, PASSWORD, HELP, EXIT, and  **
** for admins USERS, ADDUSER, DELUSER, RESETPW. CONNECT opens a terminal **
** session (term_open, the user's own slot) and passes bytes both ways;  **
** when the session ends (LOGOUT, or a refusal) the user is back at the  **
** prompt. The user hanging up ends the session (DCON).                  **
**                                                                       **
** Telnet: the task says WILL ECHO and WILL SUPPRESS-GO-AHEAD so the     **
** client sends each key as typed and does not echo; the task echoes     **
** itself (not passwords; in a session as the host asks, ECKM). Other    **
** telnet options are refused or ignored.                                **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef TELNET_TASK_H
#define TELNET_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "term.h"

/** TCP port the telnet server listens on. */
#define TELNET_TASK_PORT 23

/** Telnet users at once, one terminal session each (term.h). */
#define TELNET_TASK_USERS TERM_TELNET_SLOTS

/** @brief Counters and state for STAT / WIFI STATUS. */
typedef struct
{
    bool wifi_up;              /**< joined, with an IP address */
    uint32_t ip;               /**< IPv4, first octet in the low byte (as IPAddress) */
    int rssi;                  /**< signal, dBm; 0 when not joined */
    int settings_source;       /**< settings_wifi(): 0 own, 1 HDLCGW's, -1 none */
    uint32_t wifi_connects;    /**< times the network was joined */
    uint32_t users;            /**< telnet users accepted */
    uint32_t refused;          /**< telnet users turned away (every session in use) */
    uint32_t users_on;         /**< users connected now */
    uint32_t bytes_from_user;  /**< bytes typed (telnet commands not counted) */
    uint32_t bytes_to_user;    /**< bytes written to the user */
} telnet_task_stats;

/** @brief Read the WiFi settings. Call once from setup() before the task starts. */
void telnet_task_init(void);

/** @brief The task body, for xTaskCreate. Never returns. */
void telnet_task_run(void *params);

/** @brief Ask the task to read the WiFi settings again and join anew. */
void telnet_task_reconnect(void);

/**
 * @brief Read the counters.
 * @param[out] out  Filled with a copy.
 */
void telnet_task_get_stats(telnet_task_stats *out);

#endif /* TELNET_TASK_H */
