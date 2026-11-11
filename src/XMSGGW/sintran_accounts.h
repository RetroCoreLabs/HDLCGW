/**************************************************************************
** SINTRAN_ACCOUNTS - the SINTRAN user each gateway user works as        **
**                                                                       **
** Ronny's FTP design (2026-11-11): an FTP client logs in with a gateway **
** user; each gateway user has ONE SINTRAN user name and password, used  **
** on every machine. That SINTRAN user is the one folder shown under     **
** each machine, and its name and password go with every file request    **
** (D100(USER(PASSWORD)). in SINTRAN's file name syntax).                **
**                                                                       **
** Kept in NVS, namespace "xmsgftp", key "acc", one line per gateway     **
** user: "gateway-user TAB sintran-user TAB sintran-password". The       **
** SINTRAN password has to be sent to SINTRAN, so it is stored as typed, **
** NOT hashed: anyone who can read this board's flash can read it.       **
** Ronny accepted that when choosing the design.                         **
**                                                                       **
** Used by the node task (the SINTRAN command) and the telnet task (the  **
** FTP server); every call holds one lock.                               **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef SINTRAN_ACCOUNTS_H
#define SINTRAN_ACCOUNTS_H

#include <stdbool.h>
#include <stddef.h>

/** Longest SINTRAN user name or password kept, without the terminator. SINTRAN's own
 *  limit has not been checked; 32 is room enough for the names in use. */
#define SINTRAN_ACCOUNT_FIELD_MAX 32u

/** Accounts kept: one per gateway user (users.h USERS_MAX). */
#define SINTRAN_ACCOUNTS_MAX 16

/** @brief Make the lock and the work buffers. Once, before any task runs. @return 0 or -1. */
int sintran_accounts_init(void);

/**
 * @brief Whether a SINTRAN user name or password can be stored and sent: 1 to
 *        SINTRAN_ACCOUNT_FIELD_MAX characters, printable ASCII, and none of space,
 *        '(' ')' '.' ':' (they are part of SINTRAN's file name syntax).
 */
bool sintran_account_field_ok(const char *text);

/**
 * @brief The SINTRAN account of a gateway user.
 * @param[in]  gw_user   Gateway user name (exact).
 * @param[out] user      SINTRAN user name; room for SINTRAN_ACCOUNT_FIELD_MAX + 1. May be NULL.
 * @param[out] password  SINTRAN password; same room; "" when none is stored. May be NULL.
 * @return 0, or -1 when the gateway user has no SINTRAN account.
 */
int sintran_account_get(const char *gw_user, char *user, char *password);

/**
 * @brief Store (replace) a gateway user's SINTRAN account.
 * @param[in] password  May be "" (a SINTRAN user with no password).
 * @return 0; -1 on a bad field, a full list or a failed write.
 */
int sintran_account_set(const char *gw_user, const char *user, const char *password);

/** @brief Remove a gateway user's SINTRAN account. @return 0, or -1 when there was none. */
int sintran_account_remove(const char *gw_user);

/**
 * @brief Every account as text, "gateway-user  SINTRAN-user  password set|no password",
 *        CR LF per line. The password itself is never shown.
 * @return The number of accounts.
 */
int sintran_accounts_list(char *out, size_t size);

#endif /* SINTRAN_ACCOUNTS_H */
