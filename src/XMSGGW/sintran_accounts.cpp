/**************************************************************************
** SINTRAN_ACCOUNTS - the SINTRAN user each gateway user works as        **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "sintran_accounts.h"

#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <libxmsg/auth/auth_user.h>

static const char *NAMESPACE = "xmsgftp";
static const char *KEY = "acc";

typedef struct
{
    char gw[AUTH_NAME_MAX + 1u];
    char user[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
    char password[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
} account;

/* One stored line: three fields, two tabs, a newline. */
#define LINE_MAX_CHARS (AUTH_NAME_MAX + 2u * SINTRAN_ACCOUNT_FIELD_MAX + 3u)
#define TEXT_SIZE (SINTRAN_ACCOUNTS_MAX * LINE_MAX_CHARS + 1u)

static SemaphoreHandle_t s_lock = NULL;
static account *s_all = NULL;  /* SINTRAN_ACCOUNTS_MAX, PSRAM (internal RAM is short) */
static char *s_text = NULL;    /* TEXT_SIZE, PSRAM */

int sintran_accounts_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_all == NULL)
    {
        s_all = (account *)heap_caps_calloc(SINTRAN_ACCOUNTS_MAX, sizeof(account), MALLOC_CAP_SPIRAM);
    }
    if (s_text == NULL)
    {
        s_text = (char *)heap_caps_calloc(1, TEXT_SIZE, MALLOC_CAP_SPIRAM);
    }
    return (s_lock != NULL && s_all != NULL && s_text != NULL) ? 0 : -1;
}

bool sintran_account_field_ok(const char *text)
{
    size_t n = 0;
    if (text == NULL)
    {
        return false;
    }
    for (; text[n] != '\0'; n++)
    {
        unsigned char c = (unsigned char)text[n];
        if (c <= ' ' || c > '~' || c == '(' || c == ')' || c == '.' || c == ':')
        {
            return false;
        }
    }
    return n >= 1u && n <= SINTRAN_ACCOUNT_FIELD_MAX;
}

/** @brief Copy one field ended by `end` (or the string end); false when it does not fit. */
static bool take_field(char **at, char *out, size_t room, char end)
{
    char *p = *at;
    size_t n = 0;
    while (p[n] != '\0' && p[n] != end)
    {
        n++;
    }
    if (n >= room)
    {
        return false;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    *at = (p[n] == end && end != '\0') ? &p[n + 1u] : &p[n];
    return true;
}

/** @brief Read every account into s_all. Lock held. @return The count. */
static int read_all(void)
{
    Preferences p;
    size_t len = 0;
    int n = 0;
    s_text[0] = '\0';
    if (p.begin(NAMESPACE, true))
    {
        if (p.isKey(KEY))
        {
            len = p.getString(KEY, s_text, TEXT_SIZE);
        }
        p.end();
    }
    char *line = s_text;
    while (len > 0u && line != NULL && *line != '\0' && n < SINTRAN_ACCOUNTS_MAX)
    {
        char *end = strchr(line, '\n');
        if (end != NULL)
        {
            *end = '\0';
        }
        char *at = line;
        account *a = &s_all[n];
        if (take_field(&at, a->gw, sizeof a->gw, '\t') && take_field(&at, a->user, sizeof a->user, '\t')
            && take_field(&at, a->password, sizeof a->password, '\0') && a->gw[0] != '\0')
        {
            n++;
        }
        line = (end != NULL) ? end + 1 : NULL;
    }
    memset(s_text, 0, TEXT_SIZE);
    return n;
}

/** @brief Write s_all[0..n), or remove the key when n is 0. Lock held. @return 0 or -1. */
static int write_all(int n)
{
    Preferences p;
    size_t at = 0;
    bool ok = false;
    for (int i = 0; i < n; i++)
    {
        int len = snprintf(&s_text[at], TEXT_SIZE - at, "%s\t%s\t%s\n", s_all[i].gw, s_all[i].user,
                           s_all[i].password);
        if (len <= 0 || (size_t)len >= TEXT_SIZE - at)
        {
            memset(s_text, 0, TEXT_SIZE);
            return -1;
        }
        at += (size_t)len;
    }
    s_text[at] = '\0';
    if (p.begin(NAMESPACE, false))
    {
        if (at == 0u)
        {
            ok = !p.isKey(KEY) || p.remove(KEY);
        }
        else
        {
            ok = (p.putString(KEY, s_text) == at);
        }
        p.end();
    }
    memset(s_text, 0, TEXT_SIZE);
    return ok ? 0 : -1;
}

/** @brief Index of a gateway user in s_all[0..n), or -1. */
static int find(int n, const char *gw_user)
{
    for (int i = 0; i < n; i++)
    {
        if (strcmp(s_all[i].gw, gw_user) == 0)
        {
            return i;
        }
    }
    return -1;
}

int sintran_account_get(const char *gw_user, char *user, char *password)
{
    int rc = -1;
    if (s_lock == NULL || gw_user == NULL)
    {
        return -1;
    }
    (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = read_all();
    int i = find(n, gw_user);
    if (i >= 0)
    {
        if (user != NULL)
        {
            (void)snprintf(user, SINTRAN_ACCOUNT_FIELD_MAX + 1u, "%s", s_all[i].user);
        }
        if (password != NULL)
        {
            (void)snprintf(password, SINTRAN_ACCOUNT_FIELD_MAX + 1u, "%s", s_all[i].password);
        }
        rc = 0;
    }
    memset(s_all, 0, SINTRAN_ACCOUNTS_MAX * sizeof(account));
    xSemaphoreGive(s_lock);
    return rc;
}

int sintran_account_set(const char *gw_user, const char *user, const char *password)
{
    int rc = -1;
    if (s_lock == NULL || gw_user == NULL || !auth_name_is_valid(gw_user) || !sintran_account_field_ok(user)
        || password == NULL || (password[0] != '\0' && !sintran_account_field_ok(password)))
    {
        return -1;
    }
    (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = read_all();
    int i = find(n, gw_user);
    if (i < 0 && n < SINTRAN_ACCOUNTS_MAX)
    {
        i = n++;
    }
    if (i >= 0)
    {
        (void)snprintf(s_all[i].gw, sizeof s_all[i].gw, "%s", gw_user);
        (void)snprintf(s_all[i].user, sizeof s_all[i].user, "%s", user);
        (void)snprintf(s_all[i].password, sizeof s_all[i].password, "%s", password);
        rc = write_all(n);
    }
    memset(s_all, 0, SINTRAN_ACCOUNTS_MAX * sizeof(account));
    xSemaphoreGive(s_lock);
    return rc;
}

int sintran_account_remove(const char *gw_user)
{
    int rc = -1;
    if (s_lock == NULL || gw_user == NULL)
    {
        return -1;
    }
    (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = read_all();
    int i = find(n, gw_user);
    if (i >= 0)
    {
        for (int j = i; j < n - 1; j++)
        {
            s_all[j] = s_all[j + 1];
        }
        rc = write_all(n - 1);
    }
    memset(s_all, 0, SINTRAN_ACCOUNTS_MAX * sizeof(account));
    xSemaphoreGive(s_lock);
    return rc;
}

int sintran_accounts_list(char *out, size_t size)
{
    size_t at = 0;
    out[0] = '\0';
    if (s_lock == NULL)
    {
        return 0;
    }
    (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = read_all();
    for (int i = 0; i < n; i++)
    {
        int len = snprintf(&out[at], size - at, "  %-16s %-16s %s\r\n", s_all[i].gw, s_all[i].user,
                           (s_all[i].password[0] != '\0') ? "password set" : "no password");
        if (len <= 0 || (size_t)len >= size - at)
        {
            break;
        }
        at += (size_t)len;
    }
    memset(s_all, 0, SINTRAN_ACCOUNTS_MAX * sizeof(account));
    xSemaphoreGive(s_lock);
    return n;
}
