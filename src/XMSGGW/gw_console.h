/**************************************************************************
** GW_CONSOLE - the login and the XMSG> command line, libxmsg's console  **
**                                                                       **
** libxmsg console/console.h is the one login and command line for every **
** way a user arrives; this file plugs XMSGGW into it:                   **
**  - lines: telnet connections (telnet_task.h queues) and SINTRAN       **
**    callers (CONNECT-TO D101, libxmsg console_tad.h);                   **
**  - services: onward terminal sessions on term.cpp's slots (place i =  **
**    slot i), LIST and NAMES walks to the host, INFO's link facts, and  **
**    the system names CONNECT takes (ALIAS by hand, then the host's own  **
**    names, names.h);                                                   **
**  - users: users.h (NVS, PBKDF2 at USERS_ITERATIONS).                  **
**                                                                       **
** All of it runs in the node task, the task that owns the XMSG node:    **
** the console must never be called from two tasks (console.h), and its **
** services call the node, so one task does both, as libxmsg's           **
** host_tad_telnet does.                                                  **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef GW_CONSOLE_H
#define GW_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

#include <libxmsg.h>

/** SINTRAN callers at once (each needs a free console place too). */
#define GW_CONSOLE_TAD_LINES 4

/**
 * @brief Make the console: sessions and lines in PSRAM, the user list
 *        (users_init: writes the default admin when none is stored, one
 *        PBKDF2 run). Once, from the node task before its loop.
 * @return 0, or -1 when memory or the user list could not be had.
 */
int gw_console_init(void);

/**
 * @brief A port-0 letter from the node (its on_letter): a SINTRAN caller's
 *        CONNECT-TO, or LI-SYS's status question.
 */
void gw_console_letter(XmsgNode *node, const XmsgDatagram *dg, uint32_t now);

/**
 * @brief Everything for one node-task pass: new and lost telnet
 *        connections, typed bytes, SINTRAN callers, onward sessions, walks,
 *        the console's own timers. Before xmsg_node_tick().
 */
void gw_console_pass(XmsgNode *node, uint32_t now);

/**
 * @brief Whether a LIST or NAMES walk of a console user is running. The
 *        name-table refresh (names.h) waits for it: one walk to the host at
 *        a time, since a reply is matched by being the only question out.
 */
bool gw_console_walk_busy(void);

#endif /* GW_CONSOLE_H */
