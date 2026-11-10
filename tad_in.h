/**************************************************************************
** TAD_IN - terminal sessions FROM a SINTRAN machine (CONNECT-TO D101)   **
**                                                                       **
** A SINTRAN user types CONNECT-TO D101; libxmsg's tad_responder answers **
** it, and the user then gets XMSGGW's login and command line exactly as **
** a telnet user does (Ronny, 2026-11-10: "an allowed incoming           **
** connection to XMSGGW as it came from a telnet port"). The same module **
** answers LI-SYS's status question with the counts of the user places.  **
**                                                                       **
** A caller takes one of the TELNET_TASK_USERS user places (shared with  **
** telnet), so "8 sessions" stays true for LI-SYS. The node task runs    **
** the responders (only it touches the node); the telnet task runs the   **
** user and moves bytes through the functions below. Output and the two  **
** control messages that must keep their place in it (echo on/off, the  **
** login state) go through one queue, so a password prompt is never sent **
** before the echo-off that belongs to it.                               **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef TAD_IN_H
#define TAD_IN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libxmsg.h>

/** Responders: calls that can be in progress at once. Each takes 2 node sessions. */
#define TAD_IN_RESPONDERS 4

/** Where a user place's incoming session is, for the telnet task. */
typedef enum
{
    TAD_IN_NONE = 0,   /**< no incoming session on this place */
    TAD_IN_COMING,     /**< accepted, the bring-up is running */
    TAD_IN_RUNNING,    /**< the user may be greeted */
    TAD_IN_ENDED       /**< closed or failed; the telnet task drops the user */
} tad_in_state;

/** @brief Buffers in PSRAM. Call before the node task starts. @return 0 or -1. */
int tad_in_init(void);

/* ---------------------------------------------------- node task only */

/** @brief The node's on_letter callback: offer a port-0 letter to the responders. */
void tad_in_letter(XmsgNode *node, const XmsgDatagram *dg, uint32_t now);

/** @brief Each node pass: move output, grant input, hang up, notice the end. */
void tad_in_pass(XmsgNode *node, uint32_t now);

/* --------------------------------------------------- telnet task only */

/** @brief State of the incoming session on a user place. */
tad_in_state tad_in_get_state(int place);

/** @brief The caller's system and terminal, for WHO, e.g. "D100 port 0x08F0". */
void tad_in_describe(int place, char *out, size_t size);

/** @brief Take what the remote user typed. @return Bytes copied. */
int tad_in_read(int place, uint8_t *out, int max);

/** @brief Whether the remote user pressed the escape key since the last call. */
bool tad_in_take_escape(int place);

/** @brief Text to the remote user. @return Bytes taken (fewer when the queue is full). */
int tad_in_write(int place, const uint8_t *bytes, int len);

/** @brief Echo on (the caller's machine echoes typed keys) or off (a password). */
void tad_in_echo(int place, bool on);

/** @brief Login state (TAD_SYCN_*), in order with the text. */
void tad_in_login_state(int place, uint16_t sycn);

/** @brief End the session (the user typed EXIT, or the user place is closing). */
void tad_in_hangup(int place);

/** @brief The telnet task has dropped the user: the place is free again. */
void tad_in_release(int place);

#endif /* TAD_IN_H */
