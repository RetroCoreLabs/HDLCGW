/**************************************************************************
** USERS - the telnet users who may log in to XMSGGW                     **
**                                                                       **
** Records are libxmsg AuthUser (PBKDF2-SHA256 hash and salt, never the  **
** password), kept as users-file lines (auth_user_format_line) joined by **
** LF in one NVS string, namespace "xmsgusr", key "users". With no users **
** stored, the store is started with libxmsg's default admin: name       **
** "admin", password "admin", which must be changed at the first login.  **
**                                                                       **
** Called from the telnet task only; no lock.                            **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef USERS_H
#define USERS_H

#include <stddef.h>
#include <stdint.h>

#include <libxmsg/auth/auth_login.h>
#include <libxmsg/auth/auth_user.h>

/** Most users kept. 16 lines of at most AUTH_LINE_MAX fit one NVS string (< 4000 bytes). */
#define USERS_MAX 16

/**
 * @brief Plug mbedTLS PBKDF2 into libxmsg, and write the default admin when
 *        no users are stored. The admin record costs one PBKDF2 run.
 * @return 0, or -1 when the default admin could not be written.
 */
int users_init(void);

/** @brief The store port for auth_login (lookup and save). */
const AuthUserStore *users_store(void);

/** @brief The random source (esp_fill_random) for salts. */
const AuthRandom *users_random(void);

/**
 * @brief Read every user.
 * @param[out] out  Room for cap records.
 * @param[in]  cap  Room.
 * @return The count (0 when none is stored).
 */
int users_read(AuthUser *out, int cap);

/**
 * @brief Write the whole list, replacing what is stored.
 * @return 0 or -1.
 */
int users_write(const AuthUser *u, int n);

/**
 * @brief Look one user up by exact name.
 * @return 0 and *out filled, or -1 when not known.
 */
int users_lookup(const char *name, AuthUser *out);

/**
 * @brief Store one record: replace the user of that name, or add it.
 * @return 0, or -1 when the list is full or the write failed.
 */
int users_put(const AuthUser *u);

/** @brief Milliseconds the last PBKDF2 run took (measured, for the INFO text). */
uint32_t users_last_hash_ms(void);

#endif /* USERS_H */
