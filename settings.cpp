/**************************************************************************
** SETTINGS - XMSGGW values kept in NVS across restarts                  **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "settings.h"

#include <Preferences.h>
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

int settings_set_wifi_pass(const char *pass)
{
    return put_text("wpass", pass, SETTINGS_PASS_SIZE - 1);
}
