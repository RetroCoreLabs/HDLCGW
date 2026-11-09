/**************************************************************************
** TELNET_TASK - WiFi, and telnet users' terminal sessions to the host   **
**                                                                       **
** The WiFi station handling follows HDLCGW wifi_task.cpp (station mode, **
** no sleep, a new try every 20 s while not joined), without its TCP     **
** frame bridge.                                                         **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "telnet_task.h"

#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <WiFi.h>

#include "console_out.h"
#include "console_task.h"
#include "node_task.h"
#include "settings.h"
#include "term.h"

#define WIFI_RETRY_MS 20000u
#define HOSTNAME "xmsggw"

/* Telnet bytes (RFC 854, 857, 858). */
#define TN_IAC  255u
#define TN_DONT 254u
#define TN_DO   253u
#define TN_WONT 252u
#define TN_WILL 251u
#define TN_SB   250u
#define TN_SE   240u
#define TN_OPT_ECHO 1u
#define TN_OPT_SGA  3u

static char s_ssid[SETTINGS_SSID_SIZE];
static char s_pass[SETTINGS_PASS_SIZE];
static volatile bool s_reconnect = false;
static bool s_wifi_begun = false;
static bool s_wifi_was_up = false;
static uint32_t s_next_try = 0;

static WiFiServer s_server(TELNET_TASK_PORT, TELNET_TASK_USERS);
static bool s_server_running = false;

/* Telnet command parser state. */
typedef enum
{
    TN_DATA,
    TN_GOT_IAC,
    TN_GOT_OPTION_VERB,   /* WILL/WONT/DO/DONT: the option byte follows */
    TN_IN_SB,
    TN_IN_SB_IAC
} tn_state;

/** @brief One telnet user; user i always uses terminal session slot i. */
typedef struct
{
    WiFiClient client;
    bool on;
    bool session_seen_open;   /* the session for this user has opened */
    tn_state tn;
    uint8_t tn_verb;
    bool last_was_cr;
} telnet_user;

static telnet_user s_users[TELNET_TASK_USERS];
static telnet_task_stats s_stats;

void telnet_task_init(void)
{
    memset(&s_stats, 0, sizeof s_stats);
    s_stats.settings_source = settings_wifi(s_ssid, s_pass);
}

void telnet_task_reconnect(void)
{
    s_reconnect = true;
}

void telnet_task_get_stats(telnet_task_stats *out)
{
    *out = s_stats;
}

/** @brief The user's index, which is also its session slot. */
static int user_index(const telnet_user *u)
{
    return (int)(u - s_users);
}

/** @brief Write to a user, counted. */
static void user_write(telnet_user *u, const uint8_t *bytes, size_t len)
{
    if (!u->on || len == 0)
    {
        return;
    }
    s_stats.bytes_to_user += (uint32_t)u->client.write(bytes, len);
}

/** @brief Write text to a user. */
static void user_say(telnet_user *u, const char *text)
{
    user_write(u, (const uint8_t *)text, strlen(text));
}

/** @brief End a user's connection and the session behind it. */
static void user_drop(telnet_user *u, const char *why)
{
    term_status ts;
    int i = user_index(u);
    term_get_status(i, &ts);
    if (ts.open)
    {
        term_hangup(i);
    }
    if (u->on)
    {
        u->client.stop();
    }
    u->on = false;
    s_stats.users_on--;
    console_out_log(CONTROL_FRAME_LOG_INFO, "Telnet user %d gone: %s", i, why);
}

/** @brief A new telnet user: negotiate, greet, ask for the session. */
static void user_start(telnet_user *u)
{
    static const uint8_t NEGOTIATE[] = { TN_IAC, TN_WILL, TN_OPT_ECHO, TN_IAC, TN_WILL, TN_OPT_SGA };
    node_status st;
    char system[16];
    char text[120];
    int i = user_index(u);

    u->on = true;
    s_stats.users_on++;
    s_stats.users++;
    u->tn = TN_DATA;
    u->last_was_cr = false;
    u->session_seen_open = false;
    u->client.setNoDelay(true);
    user_write(u, NEGOTIATE, sizeof NEGOTIATE);

    node_task_get_status(&st);
    if (!st.ready || st.peer_node < 0)
    {
        user_say(u, "\r\nXMSGGW: the link to the host is not up. Try again later.\r\n");
        user_drop(u, "link down");
        return;
    }
    /* The target is "D" + the peer's node number: the bench host D100 is node
     * 100 and that name worked (TERM CONNECT D100, 2026-11-09). That SINTRAN
     * always names a system so is an ASSUMPTION; a setting for the target
     * name is still to come. */
    (void)snprintf(system, sizeof system, "D%ld", (long)st.peer_node);
    if (term_open(i, system) != 0)
    {
        user_say(u, "\r\nXMSGGW: this terminal session is still closing. Try again.\r\n");
        user_drop(u, "session busy");
        return;
    }
    (void)snprintf(text, sizeof text, "\r\nXMSGGW %s - connecting to %s...\r\n", XMSGGW_VERSION,
                   system);
    user_say(u, text);
    console_out_log(CONTROL_FRAME_LOG_INFO, "Telnet user %d on, session to %s", i, system);
}

/**
 * @brief Handle one telnet command byte.
 * @return true when the byte was part of a telnet command (not data).
 */
static bool user_telnet(telnet_user *u, uint8_t c)
{
    switch (u->tn)
    {
    case TN_GOT_IAC:
        if (c == TN_WILL || c == TN_WONT || c == TN_DO || c == TN_DONT)
        {
            u->tn_verb = c;
            u->tn = TN_GOT_OPTION_VERB;
        }
        else if (c == TN_SB)
        {
            u->tn = TN_IN_SB;
        }
        else
        {
            /* IAC IAC (data 255: SINTRAN text is 7-bit, dropped), NOP, GA,
             * AYT and the rest: ignored. */
            u->tn = TN_DATA;
        }
        return true;

    case TN_GOT_OPTION_VERB:
        u->tn = TN_DATA;
        /* We offered ECHO and SGA; a DO for those is the agreement. Refuse
         * anything else the client asks us to do, and say DONT to anything
         * it offers. */
        if (u->tn_verb == TN_DO && c != TN_OPT_ECHO && c != TN_OPT_SGA)
        {
            const uint8_t r[] = { TN_IAC, TN_WONT, c };
            user_write(u, r, sizeof r);
        }
        else if (u->tn_verb == TN_WILL && c != TN_OPT_SGA)
        {
            const uint8_t r[] = { TN_IAC, TN_DONT, c };
            user_write(u, r, sizeof r);
        }
        return true;

    case TN_IN_SB:
        if (c == TN_IAC)
        {
            u->tn = TN_IN_SB_IAC;
        }
        return true;

    case TN_IN_SB_IAC:
        u->tn = (c == TN_SE) ? TN_DATA : TN_IN_SB;
        return true;

    case TN_DATA:
    default:
        if (c == TN_IAC)
        {
            u->tn = TN_GOT_IAC;
            return true;
        }
        return false;
    }
}

/**
 * @brief One byte from a user: telnet commands taken out, CR LF and CR NUL
 *        made into CR, data to the host (echoed if the host asks).
 */
static void user_byte(telnet_user *u, uint8_t c)
{
    int i = user_index(u);
    term_status ts;

    if (user_telnet(u, c))
    {
        return;
    }
    if (u->last_was_cr && (c == '\n' || c == 0))
    {
        u->last_was_cr = false;
        return;             /* the second half of CR LF or CR NUL */
    }
    u->last_was_cr = (c == '\r');
    if (c == '\n')
    {
        c = '\r';           /* a client that sends a bare LF for Enter */
    }
    s_stats.bytes_from_user++;
    (void)term_type(i, &c, 1);

    term_get_status(i, &ts);
    if (ts.echo)
    {
        if (c == '\r')
        {
            user_say(u, "\r\n");
        }
        else if (c == 8u || c == 127u)
        {
            user_say(u, "\b \b");
        }
        else
        {
            user_write(u, &c, 1);
        }
    }
}

/** @brief Pass bytes both ways for one user and notice the session ending. */
static void user_pass(telnet_user *u)
{
    uint8_t buf[256];
    term_status ts;
    int i = user_index(u);
    int n;

    if (!u->client.connected())
    {
        user_drop(u, "client closed");
        return;
    }
    term_get_status(i, &ts);
    n = u->client.available();
    if (n > (int)sizeof buf)
    {
        n = (int)sizeof buf;
    }
    /* Typing waits until the session is running, so no key is lost. */
    if (n > 0 && ts.open && strcmp(ts.state, "running") == 0)
    {
        n = u->client.read(buf, (size_t)n);
        for (int k = 0; k < n; k++)
        {
            user_byte(u, buf[k]);
        }
    }
    while ((n = term_read(i, buf, (int)sizeof buf)) > 0)
    {
        /* Bit 7 cleared: the host may send parity (tad_asker.h passes the
         * bytes raw), and a telnet client takes 255 as IAC. */
        for (int k = 0; k < n; k++)
        {
            buf[k] = (uint8_t)(buf[k] & 0x7Fu);
        }
        user_write(u, buf, (size_t)n);
    }

    term_get_status(i, &ts);
    if (ts.open)
    {
        u->session_seen_open = true;
    }
    else if (u->session_seen_open)
    {
        char text[80];
        (void)snprintf(text, sizeof text, "\r\nXMSGGW: session %s (error %d). Bye.\r\n", ts.state,
                       ts.error);
        user_say(u, text);
        user_drop(u, "session ended");
    }
}

/** @brief A new connection: give it a free user, or turn it away. */
static void accept_user(WiFiClient &incoming)
{
    for (int i = 0; i < TELNET_TASK_USERS; i++)
    {
        term_status ts;
        term_get_status(i, &ts);
        /* A slot whose session has not finished closing is not free yet. */
        if (!s_users[i].on && !ts.open)
        {
            s_users[i].client = incoming;
            user_start(&s_users[i]);
            return;
        }
    }
    incoming.print("\r\nXMSGGW: all terminal sessions are in use. Try again later.\r\n");
    incoming.stop();
    s_stats.refused++;
}

/** @brief Keep the station joined, and the server running while it is. */
static void run_wifi(uint32_t now)
{
    bool up = (WiFi.status() == WL_CONNECTED);

    if (s_reconnect)
    {
        s_reconnect = false;
        s_stats.settings_source = settings_wifi(s_ssid, s_pass);
        WiFi.disconnect(false, false);
        s_wifi_begun = false;
        up = false;
    }
    if (s_ssid[0] == '\0')
    {
        return;
    }
    if (!up && (!s_wifi_begun || (int32_t)(now - s_next_try) >= 0))
    {
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.setHostname(HOSTNAME);
        WiFi.begin(s_ssid, (s_pass[0] != '\0') ? s_pass : NULL);
        s_wifi_begun = true;
        s_next_try = now + WIFI_RETRY_MS;
        console_out_log(CONTROL_FRAME_LOG_INFO, "WiFi: joining %s", s_ssid);
        return;
    }
    if (up && !s_wifi_was_up)
    {
        char text[80];
        s_wifi_was_up = true;
        s_stats.wifi_connects++;
        s_stats.wifi_up = true;
        s_stats.ip = (uint32_t)WiFi.localIP();
        s_stats.rssi = (int)WiFi.RSSI();
        (void)snprintf(text, sizeof text, "ip=%s rssi=%d", WiFi.localIP().toString().c_str(),
                       s_stats.rssi);
        console_out_event(CONTROL_FRAME_EVENT_WIFI_UP, text);
        s_server.begin();
        s_server.setNoDelay(true);
        s_server_running = true;
    }
    else if (!up && s_wifi_was_up)
    {
        s_wifi_was_up = false;
        s_stats.wifi_up = false;
        s_stats.rssi = 0;
        console_out_event(CONTROL_FRAME_EVENT_WIFI_DOWN, "wifi lost");
        for (int i = 0; i < TELNET_TASK_USERS; i++)
        {
            if (s_users[i].on)
            {
                user_drop(&s_users[i], "wifi lost");
            }
        }
        if (s_server_running)
        {
            s_server.end();
            s_server_running = false;
        }
    }
}

void telnet_task_run(void *params)
{
    (void)params;
    while (true)
    {
        uint32_t now = millis();
        run_wifi(now);
        if (s_server_running)
        {
            WiFiClient incoming = s_server.accept();
            if (incoming)
            {
                accept_user(incoming);
            }
        }
        for (int i = 0; i < TELNET_TASK_USERS; i++)
        {
            if (s_users[i].on)
            {
                user_pass(&s_users[i]);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
