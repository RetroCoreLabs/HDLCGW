/**************************************************************************
** NAMES - the system names (aliases) the host knows, read from its XROUT **
**                                                                       **
** What LIST-NAMES does on a SINTRAN machine: ask the host's XROUT        **
** (port 0) for its name table, one row per question (libxmsg            **
** xrout_names_walk.h). The rows whose port word is 0x0001 are system     **
** names, such as "D102" for system 102, set up on the host with          **
** DEF-REMOTE. CONNECT takes them in place of a system number, so the     **
** names do not have to be typed into XMSGGW by hand (Ronny, 2026-11-10:  **
** "d100 is an alias that has been set up ... it could be anything").     **
**                                                                       **
** The table is read when the link to the host becomes ready, and again   **
** every NAMES_REFRESH_MS. A name set by hand (ALIAS, settings.h) wins    **
** over a name read from the host.                                        **
**                                                                       **
** The node task runs the walk (names_pass); other tasks only look names  **
** up (names_lookup) or list them (names_list), under a spinlock.         **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef NAMES_H
#define NAMES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libxmsg.h>

/** System names kept from one walk; more are dropped and counted. */
#define NAMES_MAX 64

/** How often the host's table is read again while the link stays up. */
#define NAMES_REFRESH_MS (15u * 60u * 1000u)

/** @brief Tables in PSRAM. Once, before the node task runs. @return 0 or -1. */
int names_init(void);

/**
 * @brief Work for the node task: start a walk when one is due, take its end.
 * @param[in] node  The node.
 * @param[in] now   millis().
 */
void names_pass(XmsgNode *node, uint32_t now);

/** @brief Whether a walk is running (LIST waits for it: one walk to the host at a time). */
bool names_busy(void);

/** @brief Ask for a new walk on the next pass (ALIAS READ). */
void names_refresh(void);

/**
 * @brief Look a system name up in the table read from the host (case ignored).
 * @return 0 and *system filled, or -1 when the host has no such name.
 */
int names_lookup(const char *name, uint16_t *system);

/**
 * @brief The table as text, one "name  number" line each, CR LF ended,
 *        then a line saying when it was read and how the last walk ended.
 * @return The number of names.
 */
int names_list(char *out, size_t size);

/** One system name from the host's table. */
typedef struct
{
    char name[20];    /**< "D102"; XROUT names are at most 16 (XROUT_NAME_LEN_MAX) */
    uint16_t system;  /**< its system number */
} NamesEntry;

/**
 * @brief Copy the table read from the host, for other tasks (the FTP server's top folder).
 * @param[out] out  Room for cap entries.
 * @param[in]  cap  Room.
 * @return The number copied (0 before the first walk has ended).
 */
int names_copy(NamesEntry *out, int cap);

#endif /* NAMES_H */
