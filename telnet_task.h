/**************************************************************************
** TELNET_TASK - WiFi, and a telnet user's terminal session to the host  **
**                                                                       **
** Joins the stored WiFi network (settings_wifi) and listens on TCP port **
** 23, for up to TELNET_TASK_USERS users at once. When a user connects,  **
** the task opens a terminal session for it (term_open, the user's own   **
** slot) to the peer's system and passes bytes both ways: what the user  **
** types                                                                 **
** goes to the host as keystrokes, the host's text goes to the user.     **
** The user hanging up ends the session (DCON); the session ending       **
** (LOGOUT, or a refusal) closes the user's connection.                  **
**                                                                       **
** Telnet: the task says WILL ECHO and WILL SUPPRESS-GO-AHEAD so the     **
** client sends each key as typed and does not echo; the task echoes     **
** itself as the host asks (ECKM: not for a password). Other telnet      **
** options are refused or ignored. No login on the gateway yet: the      **
** SINTRAN login is the only one (users and levels come later).          **
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
