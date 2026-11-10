/**************************************************************************
** TERM - terminal sessions (TAD CONNECT-TO) to systems on the peer      **
**                                                                       **
** A pool of TERM_SLOTS sessions, each a libxmsg TadAsker with a buffer  **
** for host text and one for typed text. Slots 0..TERM_TELNET_SLOTS-1    **
** belong to telnet users (one per user), the last one to the TERM       **
** command. Everything libxmsg does runs in the node task (term_pass);   **
** the telnet and console tasks only fill and empty the text buffers,    **
** under one lock. The slots live in PSRAM, allocated once at start-up:  **
** internal RAM is full once WiFi is linked in.                          **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef TERM_H
#define TERM_H

#include <stdbool.h>
#include <stdint.h>

#include <libxmsg.h>

/** Telnet users at once (Ronny, 2026-11-09: at least 8). */
#define TERM_TELNET_SLOTS 8
/** The slot the TERM command uses. */
#define TERM_CONSOLE_SLOT TERM_TELNET_SLOTS
/** All slots. XMSG_MAX_SESSIONS (build_opt.h) must be at least this. */
#define TERM_SLOTS (TERM_TELNET_SLOTS + 1)

#if XMSG_MAX_SESSIONS < TERM_SLOTS
#error "XMSG_MAX_SESSIONS is below TERM_SLOTS: set it in build_opt.h"
#endif

/** @brief What one session is doing. */
typedef struct
{
    const char *state;     /**< tad_asker_state_name(), or "none" */
    bool open;             /**< asked for or running; false once closed or failed */
    bool echo;             /**< the host wants typing echoed (ECKM 0x01); false for a password */
    int error;             /**< the asker's error on FAILED, else 0 */
    bool may_type;         /**< the host has granted input */
    uint32_t bytes_in;     /**< host text bytes received */
    uint32_t bytes_out;    /**< typed bytes sent */
    uint32_t lost_in;      /**< host text bytes dropped, buffer full */
    int queued_out;        /**< typed bytes waiting for an input grant */
} term_status;

/**
 * @brief Allocate the slots in PSRAM and create the lock. Once, from
 *        node_task_init().
 * @return 0, or -1 when PSRAM could not be had (then every open fails).
 */
int term_init(void);

/**
 * @brief Ask for a session in one slot. The node task sends the connect
 *        letter on its next pass once the link is ready.
 * @param[in] slot    0..TERM_SLOTS-1.
 * @param[in] system  A system number ("102") or an alias from XMSGGW's own
 *                    list ("D102"); at most 15 characters. Sent as typed in
 *                    the letter's name field.
 * @param[in] user    The user name SINTRAN shows for this session (TADADM),
 *                    or NULL for libxmsg's "REMOTE". Sent as "<user>@xmsggw"
 *                    when that fits the 16 characters TAD allows, else the
 *                    name alone, cut to 16 (Ronny, 2026-11-10; not "@d101":
 *                    D101 is shown as the system already).
 * @return 0 when queued; -1 for a bad slot, a name too long, or a slot in
 *         use; -2 when `system` is neither a system number nor an alias in
 *         XMSGGW's list (settings_alias).
 */
int term_open(int slot, const char *system, const char *user);

/**
 * @brief Ask for a session in one slot, to a system whose number is known
 *        already (the console resolves names itself, gw_console.cpp).
 * @param[in] slot      0..TERM_SLOTS-1.
 * @param[in] system    The name as the user typed it; sent in the letter.
 * @param[in] number    The target system's number: where the letter goes.
 * @param[in] tad_user  The user name SINTRAN shows (TADADM), used as it is
 *                      (cut to 16), or NULL for libxmsg's "REMOTE".
 * @return 0 when queued; -1 for a bad slot or name, number 0, or a slot in use.
 */
int term_open_to(int slot, const char *system, uint16_t number, const char *tad_user);

/**
 * @brief Queue typed text only when nothing typed earlier is still waiting,
 *        so each call's bytes go to the asker alone (the console's rule: one
 *        call is at most one data letter; the asker cuts it further at the
 *        host's break characters).
 * @return Bytes taken: all of them (up to the buffer), or 0 when earlier
 *         text still waits or there is no session.
 */
int term_type_one(int slot, const uint8_t *text, int len);

/**
 * @brief Queue typed text. CR is not added.
 * @return Bytes queued (fewer when the buffer is full), -1 with no session.
 */
int term_type(int slot, const uint8_t *text, int len);

/** @brief Ask for the session in a slot to end (DCON). */
void term_hangup(int slot);

/**
 * @brief Ask for an escape (TAD ESCA) in a slot: what the ESC key does on a
 *        real terminal, which breaks the running program on the host even
 *        when it has not granted input (libxmsg tad_asker_escape). The
 *        caller decides what is a lone ESC (libxmsg term_escape.h).
 */
void term_escape(int slot);

/**
 * @brief Take host text collected since the last call.
 * @return Bytes copied; 0 when there is none.
 */
int term_read(int slot, uint8_t *out, int max);

/**
 * @brief Read one slot's state.
 * @param[out] out  Filled; state "none" for a bad slot.
 */
void term_get_status(int slot, term_status *out);

/**
 * @brief Work for every slot: open, type, hang up, notice an end. Called
 *        by the node task on each pass, before xmsg_node_tick().
 * @param[in] node  The node.
 * @param[in] now   millis().
 */
void term_pass(XmsgNode *node, uint32_t now);

#endif /* TERM_H */
