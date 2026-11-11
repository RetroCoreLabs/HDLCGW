/**************************************************************************
** USERS - the telnet users who may log in to XMSGGW                     **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "users.h"

#include <string.h>

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/pkcs5.h>

#include <libxmsg/auth/auth_hash.h>

#include "console_out.h"

static const char *NAMESPACE = "xmsgusr";
static const char *KEY = "users";
#define USERS_TEXT_SIZE (USERS_MAX * AUTH_LINE_MAX)

static uint32_t s_hash_ms = 0;

/* Work buffers, in PSRAM: internal RAM is short (about 28 KB free). */
static char *s_text = NULL;      /* USERS_TEXT_SIZE: the stored lines */
static AuthUser *s_all = NULL;   /* USERS_MAX records */

/* Two tasks use the list: the node task (the console's logins and USER commands) and
 * the telnet task (FTP logins, ftp_server.h). Both share s_text and s_all, so every
 * public call holds this lock. Recursive: users_put and users_lookup call users_read. */
static SemaphoreHandle_t s_lock = NULL;

static void lock(void)
{
    if (s_lock != NULL)
    {
        (void)xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL)
    {
        (void)xSemaphoreGiveRecursive(s_lock);
    }
}

/** @brief PBKDF2-HMAC-SHA256 through mbedTLS (auth_hash.h says how). Timed. */
static int pbkdf2_mbedtls(void *ctx, const uint8_t *pw, size_t pw_len, const uint8_t *salt,
                          size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len)
{
    uint32_t t0 = millis();
    int rc;
    (void)ctx;
    rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, pw, pw_len, salt, salt_len,
                                       iterations, (uint32_t)out_len, out);
    s_hash_ms = millis() - t0;
    return rc;
}

static int random_fill(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    /* The hardware RNG; with WiFi running its output is true random
     * (ESP-IDF esp_random docs; WiFi is on whenever telnet users exist). */
    esp_fill_random(buf, len);
    return 0;
}

static int store_lookup(void *ctx, const char *name, AuthUser *out)
{
    (void)ctx;
    return users_lookup(name, out);
}

static int store_save(void *ctx, const AuthUser *u)
{
    (void)ctx;
    return users_put(u);
}

static const AuthHashPort s_port = { NULL, pbkdf2_mbedtls };
static const AuthRandom s_rng = { NULL, random_fill };
static const AuthUserStore s_store = { NULL, store_lookup, store_save };

const AuthUserStore *users_store(void)
{
    return &s_store;
}

const AuthRandom *users_random(void)
{
    return &s_rng;
}

uint32_t users_last_hash_ms(void)
{
    return s_hash_ms;
}

static int users_read_locked(AuthUser *out, int cap)
{
    Preferences p;
    int n = 0;
    size_t len = 0;
    if (s_text == NULL)
    {
        return 0;
    }
    s_text[0] = '\0';
    if (p.begin(NAMESPACE, true))
    {
        if (p.isKey(KEY))
        {
            len = p.getString(KEY, s_text, USERS_TEXT_SIZE);
        }
        p.end();
    }
    if (len == 0)
    {
        return 0;
    }
    char *line = s_text;
    while (line != NULL && *line != '\0' && n < cap)
    {
        char *end = strchr(line, '\n');
        if (end != NULL)
        {
            *end = '\0';
        }
        AuthUser u;
        if (auth_user_parse_line(line, &u) == 0)
        {
            out[n++] = u;
        }
        auth_wipe(&u, sizeof u);
        line = (end != NULL) ? end + 1 : NULL;
    }
    auth_wipe(s_text, USERS_TEXT_SIZE);
    return n;
}

static int users_write_locked(const AuthUser *u, int n)
{
    Preferences p;
    size_t at = 0;
    size_t written = 0;
    bool opened;
    if (s_text == NULL || n < 0 || n > USERS_MAX)
    {
        return -1;
    }
    for (int i = 0; i < n; i++)
    {
        int len = auth_user_format_line(&u[i], &s_text[at], USERS_TEXT_SIZE - at - 1u);
        if (len <= 0)
        {
            return -1;
        }
        at += (size_t)len;
        s_text[at++] = '\n';
    }
    s_text[at] = '\0';
    opened = p.begin(NAMESPACE, false);
    if (opened)
    {
        written = p.putString(KEY, s_text);
        p.end();
    }
    auth_wipe(s_text, USERS_TEXT_SIZE);
    return (opened && written == at) ? 0 : -1;
}

static int users_lookup_locked(const char *name, AuthUser *out)
{
    int found = -1;
    if (s_all == NULL)
    {
        return -1;
    }
    int n = users_read(s_all, USERS_MAX);
    for (int i = 0; i < n; i++)
    {
        if (found < 0 && strcmp(s_all[i].name, name) == 0)
        {
            *out = s_all[i];
            found = 0;
        }
    }
    auth_wipe(s_all, USERS_MAX * sizeof(AuthUser));
    return found;
}

static int users_put_locked(const AuthUser *u)
{
    int at = -1;
    int rc;
    if (s_all == NULL)
    {
        return -1;
    }
    int n = users_read(s_all, USERS_MAX);
    for (int i = 0; i < n; i++)
    {
        if (strcmp(s_all[i].name, u->name) == 0)
        {
            at = i;
        }
    }
    if (at < 0)
    {
        if (n >= USERS_MAX)
        {
            auth_wipe(s_all, USERS_MAX * sizeof(AuthUser));
            return -1;
        }
        at = n++;
    }
    s_all[at] = *u;
    rc = users_write(s_all, n);
    auth_wipe(s_all, USERS_MAX * sizeof(AuthUser));
    return rc;
}

int users_init(void)
{
    AuthUser admin;
    auth_hash_set_port(&s_port);
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
    if (s_text == NULL)
    {
        s_text = (char *)heap_caps_calloc(1, USERS_TEXT_SIZE, MALLOC_CAP_SPIRAM);
    }
    if (s_all == NULL)
    {
        s_all = (AuthUser *)heap_caps_calloc(USERS_MAX, sizeof(AuthUser), MALLOC_CAP_SPIRAM);
    }
    if (s_text == NULL || s_all == NULL)
    {
        console_out_log(CONTROL_FRAME_LOG_ERROR, "Users: no memory for the user list");
        return -1;
    }
    int n = users_read(s_all, USERS_MAX);
    auth_wipe(s_all, USERS_MAX * sizeof(AuthUser));
    if (n > 0)
    {
        console_out_log(CONTROL_FRAME_LOG_INFO, "Users: %d stored", n);
        return 0;
    }
    if (auth_user_make_default_admin(&admin, USERS_ITERATIONS, &s_rng) != 0
        || users_write(&admin, 1) != 0)
    {
        auth_wipe(&admin, sizeof admin);
        console_out_log(CONTROL_FRAME_LOG_ERROR, "Users: cannot write the default admin");
        return -1;
    }
    auth_wipe(&admin, sizeof admin);
    console_out_log(CONTROL_FRAME_LOG_INFO,
                    "Users: none stored; wrote the default admin (PBKDF2 %u ms)",
                    (unsigned)s_hash_ms);
    return 0;
}

int users_read(AuthUser *out, int cap)
{
    lock();
    int rc = users_read_locked(out, cap);
    unlock();
    return rc;
}

int users_write(const AuthUser *u, int n)
{
    lock();
    int rc = users_write_locked(u, n);
    unlock();
    return rc;
}

int users_lookup(const char *name, AuthUser *out)
{
    lock();
    int rc = users_lookup_locked(name, out);
    unlock();
    return rc;
}

int users_put(const AuthUser *u)
{
    lock();
    int rc = users_put_locked(u);
    unlock();
    return rc;
}

