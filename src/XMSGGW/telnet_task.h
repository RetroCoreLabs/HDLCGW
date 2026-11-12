/**************************************************************************
** TELNET_TASK - WiFi, and the telnet connections' bytes                 **
**                                                                       **
** Joins the shared WiFi network (board_wifi) and listens on TCP port    **
** 23. For each connection it does the telnet part (libxmsg              **
** term/term_telnet.h: option talk, IAC doubling) and moves bytes: what  **
** the user typed into the connection's input queue, what the console    **
** wrote out of its output queue to the socket.                          **
**                                                                       **
** Everything else - the login, the XMSG> command line, the onward       **
** terminal sessions - is libxmsg's console (console/console.h), run by  **
** the node task (gw_console.h), so that the console and the node are in **
** one task, as the console asks. The node task reaches a connection only **
** through the telnet_conn_* calls below; the queues are guarded by a    **
** spinlock each.                                                        **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef TELNET_TASK_H
#define TELNET_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "term.h"

/** TCP port the telnet server listens on. */
#define TELNET_TASK_PORT 23

/** Telnet connections at once. Telnet users and SINTRAN callers share the
 *  console's places (gw_console.h); a connection without a free place is
 *  told so and closed. */
#define TELNET_TASK_USERS TERM_TELNET_SLOTS

/** @brief Counters and state for STAT / WIFI STATUS. */
typedef struct
{
    bool wifi_up;              /**< joined, with an IP address */
    uint32_t ip;               /**< IPv4, first octet in the low byte (as IPAddress) */
    int rssi;                  /**< signal, dBm; 0 when not joined */
    int settings_source;       /**< board_wifi(): 0 a network is stored, -1 none */
    uint32_t wifi_connects;    /**< times the network was joined */
    uint32_t users;            /**< telnet connections accepted */
    uint32_t refused;          /**< telnet connections turned away (no connection free) */
    uint32_t users_on;         /**< connections open now */
    uint32_t bytes_from_user;  /**< bytes typed (telnet commands not counted) */
    uint32_t bytes_to_user;    /**< bytes written to the user */
} telnet_task_stats;

/** @brief Read the WiFi settings, make the queues. Once, from setup(). */
void telnet_task_init(void);

/** @brief The task body, for xTaskCreate. Never returns. */
void telnet_task_run(void *params);

/** @brief Ask the task to read the WiFi settings again and join anew. */
void telnet_task_reconnect(void);

/** @brief Read the counters. @param[out] out  Filled with a copy. */
void telnet_task_get_stats(telnet_task_stats *out);

/* ---------------------------------------------------- node task side */

/** Where one connection stands, as the node task sees it. */
typedef enum
{
    TELNET_CONN_FREE = 0, /**< nothing */
    TELNET_CONN_NEW,      /**< accepted; the console has not taken it yet */
    TELNET_CONN_OPEN,     /**< the console runs on it */
    TELNET_CONN_GONE      /**< the client left, or the close is done; telnet_conn_release() */
} telnet_conn_state;

/** @brief A connection's state (read without the lock: at most one pass old). */
telnet_conn_state telnet_conn_get_state(int i);

/**
 * @brief The console has taken a NEW connection: it is OPEN from now on.
 * @param[out] peer  The client's address, "192.168.1.20:50211", for WHO.
 */
void telnet_conn_opened(int i, char *peer, size_t cap);

/** @brief Bytes the user typed (telnet commands taken out). @return The count copied. */
int telnet_conn_read(int i, uint8_t *out, int max);

/**
 * @brief Bytes for the user (0xFF is doubled by the telnet task).
 * @return The count taken; fewer when the output queue is full (the rest
 *         is dropped, as a client that does not read loses text).
 */
int telnet_conn_write(int i, const uint8_t *bytes, int len);

/** @brief Close the connection once what was written is out. */
void telnet_conn_close(int i);

/** @brief The node task is done with a GONE connection: it is FREE again. */
void telnet_conn_release(int i);

#endif /* TELNET_TASK_H */
