/**************************************************************************
** SETTINGS - XMSGGW values kept in NVS across restarts                  **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <stdlib.h>
#include <strings.h>
#include <stdio.h>
#include <string.h>

static const char *NAMESPACE = "xmsggw";

uint16_t settings_node(void)
{
    Preferences p;
    uint16_t node = 0;
    if (p.begin(NAMESPACE, true))
    {
        node = p.getUShort("node", 0);
        p.end();
    }
    return node;
}

int settings_set_node(uint16_t node)
{
    Preferences p;
    size_t written = 0;
    if (p.begin(NAMESPACE, false))
    {
        written = p.putUShort("node", node);
        p.end();
    }
    return (written == sizeof node) ? 0 : -1;
}

uint32_t settings_rate(void)
{
    Preferences p;
    uint32_t rate = 0;
    if (p.begin(NAMESPACE, true))
    {
        rate = p.getULong("rate", 0);
        p.end();
    }
    return rate;
}

int settings_set_rate(uint32_t rate)
{
    Preferences p;
    size_t written = 0;
    if (p.begin(NAMESPACE, false))
    {
        written = p.putULong("rate", rate);
        p.end();
    }
    return (written == sizeof rate) ? 0 : -1;
}

/**
 * @brief NVS key for one peer's Flags1 counter: "f1_" and the node number.
 *        NVS keys are at most 15 characters; "f1_65535" is 8.
 * @param[out] key   At least 16 bytes.
 * @param[in]  peer  Peer node number.
 */
static void flags1_key(char *key, size_t size, uint16_t peer)
{
    (void)snprintf(key, size, "f1_%u", (unsigned)peer);
}

int settings_flags1_load(void *ctx, uint16_t peer_node, uint16_t *next)
{
    Preferences p;
    char key[16];
    int result = -1;
    (void)ctx;
    flags1_key(key, sizeof key, peer_node);
    if (p.begin(NAMESPACE, true))
    {
        if (p.isKey(key))
        {
            *next = p.getUShort(key, 0);
            result = 0;
        }
        p.end();
    }
    return result;
}

int settings_flags1_save(void *ctx, uint16_t peer_node, uint16_t next)
{
    Preferences p;
    char key[16];
    size_t written = 0;
    (void)ctx;
    flags1_key(key, sizeof key, peer_node);
    if (p.begin(NAMESPACE, false))
    {
        written = p.putUShort(key, next);
        p.end();
    }
    return (written == sizeof next) ? 0 : -1;
}

int settings_port_seed_load(void *ctx, uint16_t *seed)
{
    Preferences p;
    int result = -1;
    (void)ctx;
    if (p.begin(NAMESPACE, true))
    {
        if (p.isKey("pseed"))
        {
            *seed = p.getUShort("pseed", 0);
            result = 0;
        }
        p.end();
    }
    return result;
}

int settings_port_seed_save(void *ctx, uint16_t seed)
{
    Preferences p;
    size_t written = 0;
    (void)ctx;
    if (p.begin(NAMESPACE, false))
    {
        written = p.putUShort("pseed", seed);
        p.end();
    }
    return (written == sizeof seed) ? 0 : -1;
}

int settings_wifi(char *ssid, char *pass)
{
    Preferences p;
    ssid[0] = '\0';
    pass[0] = '\0';
    if (p.begin(NAMESPACE, true))
    {
        (void)p.getString("wssid", ssid, SETTINGS_SSID_SIZE);
        (void)p.getString("wpass", pass, SETTINGS_PASS_SIZE);
        p.end();
    }
    if (ssid[0] != '\0')
    {
        return 0;
    }
    /* None of our own: HDLCGW's, read only (settings.h). */
    pass[0] = '\0';
    if (p.begin("hdlcgw", true))
    {
        (void)p.getString("ssid", ssid, SETTINGS_SSID_SIZE);
        (void)p.getString("pass", pass, SETTINGS_PASS_SIZE);
        p.end();
    }
    return (ssid[0] != '\0') ? 1 : -1;
}

/**
 * @brief Store one string key in our namespace.
 * @param[in] key    Key.
 * @param[in] value  Text.
 * @param[in] max    Longest allowed length.
 * @return 0 or -1.
 */
static int put_text(const char *key, const char *value, size_t max)
{
    Preferences p;
    size_t len = strlen(value);
    size_t written = 0;
    bool opened;
    if (len > max)
    {
        return -1;
    }
    opened = p.begin(NAMESPACE, false);
    if (opened)
    {
        written = p.putString(key, value);
        p.end();
    }
    /* An empty password writes 0 bytes, so the open must be checked too. */
    return (opened && written == len) ? 0 : -1;
}

int settings_set_wifi_ssid(const char *ssid)
{
    if (ssid[0] == '\0')
    {
        return -1;
    }
    return put_text("wssid", ssid, SETTINGS_SSID_SIZE - 1);
}

uint16_t settings_ttyp(void)
{
    Preferences p;
    uint16_t v = 0;
    if (p.begin(NAMESPACE, true))
    {
        v = p.getUShort("ttyp", 0);
        p.end();
    }
    return v;
}

int settings_set_ttyp(uint16_t ttyp)
{
    Preferences p;
    size_t written = 0;
    if (p.begin(NAMESPACE, false))
    {
        written = p.putUShort("ttyp", ttyp);
        p.end();
    }
    return (written == sizeof ttyp) ? 0 : -1;
}

int settings_set_wifi_pass(const char *pass)
{
    return put_text("wpass", pass, SETTINGS_PASS_SIZE - 1);
}

/* ---------------------------------------------------------------- aliases */

/* The alias buffers below are fixed and shared, and two tasks use them
 * (telnet: CONNECT; console: ALIAS), so every public alias call holds this. */
static SemaphoreHandle_t s_alias_lock = NULL;

/** @brief Take the alias lock, making it on first use. @return true when held. */
static bool alias_lock(void)
{
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    if (s_alias_lock == NULL)
    {
        SemaphoreHandle_t made = xSemaphoreCreateMutex();
        portENTER_CRITICAL(&mux);
        if (s_alias_lock == NULL)
        {
            s_alias_lock = made;
            made = NULL;
        }
        portEXIT_CRITICAL(&mux);
        if (made != NULL)
        {
            vSemaphoreDelete(made);   /* the other task made it first */
        }
    }
    return s_alias_lock != NULL && xSemaphoreTake(s_alias_lock, pdMS_TO_TICKS(200)) == pdTRUE;
}

/** Longest stored list: every line "name=65535\n". */
#define ALIAS_TEXT_SIZE (SETTINGS_ALIAS_MAX * (SETTINGS_ALIAS_NAME_MAX + 8))

typedef struct
{
    char name[SETTINGS_ALIAS_NAME_MAX + 1];
    uint16_t system;
} alias_entry;

/** @brief Read the list. @return The count. */
static int alias_read(alias_entry *out)
{
    Preferences p;
    static char text[ALIAS_TEXT_SIZE];
    int n = 0;
    text[0] = '\0';
    if (p.begin(NAMESPACE, true))
    {
        if (p.isKey("alias"))
        {
            (void)p.getString("alias", text, sizeof text);
        }
        p.end();
    }
    char *line = text;
    while (*line != '\0' && n < SETTINGS_ALIAS_MAX)
    {
        char *end = strchr(line, '\n');
        if (end != NULL)
        {
            *end = '\0';
        }
        char *eq = strchr(line, '=');
        if (eq != NULL && eq != line && (size_t)(eq - line) <= SETTINGS_ALIAS_NAME_MAX)
        {
            memcpy(out[n].name, line, (size_t)(eq - line));
            out[n].name[eq - line] = '\0';
            out[n].system = (uint16_t)strtoul(eq + 1, NULL, 10);
            n++;
        }
        if (end == NULL)
        {
            break;
        }
        line = end + 1;
    }
    return n;
}

/** @brief Write the list. @return 0 or -1. */
static int alias_write(const alias_entry *a, int n)
{
    static char text[ALIAS_TEXT_SIZE];
    size_t at = 0;
    text[0] = '\0';
    for (int i = 0; i < n; i++)
    {
        int len = snprintf(&text[at], sizeof text - at, "%s=%u\n", a[i].name,
                           (unsigned)a[i].system);
        if (len <= 0 || (size_t)len >= sizeof text - at)
        {
            return -1;
        }
        at += (size_t)len;
    }
    Preferences p;
    size_t written = 0;
    bool opened = p.begin(NAMESPACE, false);
    if (opened)
    {
        written = (n == 0) ? (p.remove("alias") ? 0 : 0) : p.putString("alias", text);
        p.end();
    }
    return (opened && written == at) ? 0 : -1;
}

/** @brief The entry for a name (case ignored), or -1. */
static int alias_find(const alias_entry *a, int n, const char *name)
{
    for (int i = 0; i < n; i++)
    {
        if (strcasecmp(a[i].name, name) == 0)
        {
            return i;
        }
    }
    return -1;
}

static int alias_get_locked(const char *name, uint16_t *system)
{
    static alias_entry a[SETTINGS_ALIAS_MAX];
    int n = alias_read(a);
    int at = alias_find(a, n, name);
    if (at < 0)
    {
        return -1;
    }
    *system = a[at].system;
    return 0;
}

static int alias_set_locked(const char *name, uint16_t system)
{
    static alias_entry a[SETTINGS_ALIAS_MAX];
    size_t len = strlen(name);
    bool digits = true;
    if (len == 0 || len > SETTINGS_ALIAS_NAME_MAX || system == 0)
    {
        return -1;
    }
    for (size_t k = 0; k < len; k++)
    {
        if (name[k] == '=' || name[k] == ' ')
        {
            return -1;
        }
        if (name[k] < '0' || name[k] > '9')
        {
            digits = false;
        }
    }
    if (digits)
    {
        return -1;   /* a number already means that system */
    }
    int n = alias_read(a);
    int at = alias_find(a, n, name);
    if (at < 0)
    {
        if (n >= SETTINGS_ALIAS_MAX)
        {
            return -1;
        }
        at = n++;
    }
    memcpy(a[at].name, name, len + 1);
    a[at].system = system;
    return alias_write(a, n);
}

static int alias_delete_locked(const char *name)
{
    static alias_entry a[SETTINGS_ALIAS_MAX];
    int n = alias_read(a);
    int at = alias_find(a, n, name);
    if (at < 0)
    {
        return -1;
    }
    a[at] = a[n - 1];
    return alias_write(a, n - 1);
}

static int alias_list_locked(char *out, size_t size)
{
    static alias_entry a[SETTINGS_ALIAS_MAX];
    int n = alias_read(a);
    size_t at = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++)
    {
        int len = snprintf(&out[at], size - at, "%-16s %u\r\n", a[i].name, (unsigned)a[i].system);
        if (len <= 0 || (size_t)len >= size - at)
        {
            break;
        }
        at += (size_t)len;
    }
    return n;
}

int settings_alias(const char *name, uint16_t *system)
{
    int rc;
    if (!alias_lock())
    {
        return -1;
    }
    rc = alias_get_locked(name, system);
    xSemaphoreGive(s_alias_lock);
    return rc;
}

int settings_alias_set(const char *name, uint16_t system)
{
    int rc;
    if (!alias_lock())
    {
        return -1;
    }
    rc = alias_set_locked(name, system);
    xSemaphoreGive(s_alias_lock);
    return rc;
}

int settings_alias_delete(const char *name)
{
    int rc;
    if (!alias_lock())
    {
        return -1;
    }
    rc = alias_delete_locked(name);
    xSemaphoreGive(s_alias_lock);
    return rc;
}

int settings_alias_list(char *out, size_t size)
{
    int rc;
    out[0] = '\0';
    if (!alias_lock())
    {
        return 0;
    }
    rc = alias_list_locked(out, size);
    xSemaphoreGive(s_alias_lock);
    return rc;
}
