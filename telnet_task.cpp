/**************************************************************************
** TELNET_TASK - WiFi, and the telnet connections' bytes                 **
**                                                                       **
** The WiFi station handling follows HDLCGW wifi_task.cpp (station mode, **
** no sleep, a new try every 20 s while not joined). The telnet option   **
** talk is libxmsg term/term_telnet.h. The login and the command line    **
** are libxmsg's console, run by the node task (gw_console.cpp).         **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "telnet_task.h"

#include <stdio.h>
#include <string.h>

#include <new>

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <libxmsg/term/term_telnet.h>

#include "console_out.h"
#include "settings.h"

#define WIFI_RETRY_MS 20000u
#define HOSTNAME "xmsggw"

/** Typed bytes waiting for the console. */
#define CONN_IN_SIZE 512
/** Bytes for the user waiting for the socket (a WHO of 20 users is about 1.7 KB). */
#define CONN_OUT_SIZE 4096

/** @brief One connection. The queues are shared with the node task. */
typedef struct
{
    WiFiClient client;                 /* telnet task only */
    TermTelnet tn;                     /* telnet task only */
    volatile telnet_conn_state state;
    volatile bool want_close;          /* node task: close once the output is out */
    char peer[32];                     /* set before NEW */
    uint8_t in[CONN_IN_SIZE];
    int in_len;
    uint8_t out[CONN_OUT_SIZE];
    int out_len;
    portMUX_TYPE mux;
} conn;

static conn *s_conn = NULL;            /* TELNET_TASK_USERS of them, in PSRAM */

static char s_ssid[SETTINGS_SSID_SIZE];
static char s_pass[SETTINGS_PASS_SIZE];
static volatile bool s_reconnect = false;
static bool s_wifi_begun = false;
static bool s_wifi_was_up = false;
static uint32_t s_next_try = 0;

static WiFiServer s_server(TELNET_TASK_PORT, TELNET_TASK_USERS);
static bool s_server_running = false;

static telnet_task_stats s_stats;

void telnet_task_init(void)
{
    memset(&s_stats, 0, sizeof s_stats);
    s_stats.settings_source = settings_wifi(s_ssid, s_pass);
    /* PSRAM: internal RAM is full once WiFi is linked in. calloc gives
     * zeroed memory, so every state starts FREE; the C++ members are made
     * in place below. */
    s_conn = (conn *)heap_caps_calloc(TELNET_TASK_USERS, sizeof(conn), MALLOC_CAP_SPIRAM);
    if (s_conn == NULL)
    {
        console_out_log(CONTROL_FRAME_LOG_ERROR, "No PSRAM for the telnet connections");
        return;
    }
    for (int i = 0; i < TELNET_TASK_USERS; i++)
    {
        new (&s_conn[i].client) WiFiClient();
        portMUX_INITIALIZE(&s_conn[i].mux);
    }
}

void telnet_task_reconnect(void)
{
    s_reconnect = true;
}

void telnet_task_get_stats(telnet_task_stats *out)
{
    *out = s_stats;
}

/** @brief The connection for an index, or NULL. */
static conn *conn_at(int i)
{
    if (s_conn == NULL || i < 0 || i >= TELNET_TASK_USERS)
    {
        return NULL;
    }
    return &s_conn[i];
}

/* ------------------------------------------------------ node task side */

telnet_conn_state telnet_conn_get_state(int i)
{
    conn *c = conn_at(i);
    return (c == NULL) ? TELNET_CONN_FREE : c->state;
}

void telnet_conn_opened(int i, char *peer, size_t cap)
{
    conn *c = conn_at(i);
    if (c == NULL)
    {
        return;
    }
    if (peer != NULL && cap > 0)
    {
        (void)snprintf(peer, cap, "%s", c->peer);
    }
    portENTER_CRITICAL(&c->mux);
    if (c->state == TELNET_CONN_NEW)
    {
        c->state = TELNET_CONN_OPEN;
    }
    portEXIT_CRITICAL(&c->mux);
}

int telnet_conn_read(int i, uint8_t *out, int max)
{
    conn *c = conn_at(i);
    int n;
    if (c == NULL || max <= 0)
    {
        return 0;
    }
    portENTER_CRITICAL(&c->mux);
    n = (c->in_len < max) ? c->in_len : max;
    if (n > 0)
    {
        memcpy(out, c->in, (size_t)n);
        memmove(c->in, &c->in[n], (size_t)(c->in_len - n));
        c->in_len -= n;
    }
    portEXIT_CRITICAL(&c->mux);
    return n;
}

int telnet_conn_write(int i, const uint8_t *bytes, int len)
{
    conn *c = conn_at(i);
    int n;
    if (c == NULL || len <= 0)
    {
        return 0;
    }
    portENTER_CRITICAL(&c->mux);
    n = CONN_OUT_SIZE - c->out_len;
    if (n > len)
    {
        n = len;
    }
    if (c->state != TELNET_CONN_OPEN)
    {
        n = 0;   /* the client is gone: nobody to read it */
    }
    if (n > 0)
    {
        memcpy(&c->out[c->out_len], bytes, (size_t)n);
        c->out_len += n;
    }
    portEXIT_CRITICAL(&c->mux);
    return n;
}

void telnet_conn_close(int i)
{
    conn *c = conn_at(i);
    if (c != NULL)
    {
        c->want_close = true;
    }
}

void telnet_conn_release(int i)
{
    conn *c = conn_at(i);
    if (c == NULL)
    {
        return;
    }
    portENTER_CRITICAL(&c->mux);
    if (c->state == TELNET_CONN_GONE)
    {
        c->in_len = 0;
        c->out_len = 0;
        c->want_close = false;
        c->state = TELNET_CONN_FREE;
    }
    portEXIT_CRITICAL(&c->mux);
}

/* ---------------------------------------------------- telnet task side */

/** @brief Stop the socket and mark the connection GONE for the node task. */
static void conn_drop(conn *c, const char *why)
{
    int i = (int)(c - s_conn);
    c->client.stop();
    portENTER_CRITICAL(&c->mux);
    c->state = TELNET_CONN_GONE;
    c->out_len = 0;
    portEXIT_CRITICAL(&c->mux);
    if (s_stats.users_on > 0)
    {
        s_stats.users_on--;
    }
    console_out_log(CONTROL_FRAME_LOG_INFO, "Telnet connection %d (%s): %s", i, c->peer, why);
}

/** @brief A new client: a free connection, or turned away. */
static void conn_accept(WiFiClient &incoming)
{
    for (int i = 0; i < TELNET_TASK_USERS; i++)
    {
        conn *c = &s_conn[i];
        if (c->state != TELNET_CONN_FREE)
        {
            continue;
        }
        uint8_t open[TERM_TELNET_OPEN_MAX];
        c->client = incoming;
        c->client.setNoDelay(true);
        /* No BINARY: XMSGGW's line is 7-bit text (term_telnet.h). */
        term_telnet_init(&c->tn, false);
        int n = term_telnet_open(&c->tn, open, sizeof open);
        if (n > 0)
        {
            (void)c->client.write(open, (size_t)n);
        }
        (void)snprintf(c->peer, sizeof c->peer, "%s:%u",
                       c->client.remoteIP().toString().c_str(), (unsigned)c->client.remotePort());
        c->in_len = 0;
        c->out_len = 0;
        c->want_close = false;
        s_stats.users++;
        s_stats.users_on++;
        console_out_log(CONTROL_FRAME_LOG_INFO, "Telnet connection %d from %s", i, c->peer);
        c->state = TELNET_CONN_NEW;   /* last: the node task may take it now */
        return;
    }
    incoming.print("\r\nXMSGGW: all telnet connections are in use. Try again later.\r\n");
    incoming.stop();
    s_stats.refused++;
}

/** @brief One connection's turn: bytes in, bytes out, the end. */
static void conn_pass(conn *c)
{
    uint8_t buf[256];
    if (c->state == TELNET_CONN_FREE || c->state == TELNET_CONN_GONE)
    {
        return;
    }
    if (!c->client.connected())
    {
        conn_drop(c, "client closed");
        return;
    }
    /* In: take telnet commands out, answer them, queue the data. */
    int room;
    portENTER_CRITICAL(&c->mux);
    room = CONN_IN_SIZE - c->in_len;
    portEXIT_CRITICAL(&c->mux);
    int avail = c->client.available();
    int n = (avail < room) ? avail : room;
    if (n > (int)sizeof buf)
    {
        n = (int)sizeof buf;
    }
    if (n > 0)
    {
        n = c->client.read(buf, (size_t)n);
        uint8_t data[256];
        int dn = 0;
        for (int k = 0; k < n; k++)
        {
            uint8_t reply[TERM_TELNET_REPLY_MAX];
            size_t rl = 0;
            int d = term_telnet_rx(&c->tn, buf[k], reply, &rl);
            if (rl > 0)
            {
                (void)c->client.write(reply, rl);
            }
            if (d >= 0)
            {
                data[dn++] = (uint8_t)d;
            }
        }
        if (dn > 0)
        {
            s_stats.bytes_from_user += (uint32_t)dn;
            portENTER_CRITICAL(&c->mux);
            memcpy(&c->in[c->in_len], data, (size_t)dn);   /* fits: dn <= room */
            c->in_len += dn;
            portEXIT_CRITICAL(&c->mux);
        }
    }
    /* Out: what the console wrote, 0xFF doubled. */
    while (true)
    {
        uint8_t raw[128];
        int rn;
        portENTER_CRITICAL(&c->mux);
        rn = (c->out_len < (int)sizeof raw) ? c->out_len : (int)sizeof raw;
        memcpy(raw, c->out, (size_t)rn);
        portEXIT_CRITICAL(&c->mux);
        if (rn == 0)
        {
            break;
        }
        size_t taken = 0;
        int en = term_telnet_escape(raw, (size_t)rn, buf, sizeof buf, &taken);
        if (en > 0)
        {
            (void)c->client.write(buf, (size_t)en);
            s_stats.bytes_to_user += (uint32_t)en;
        }
        portENTER_CRITICAL(&c->mux);
        memmove(c->out, &c->out[taken], (size_t)(c->out_len - (int)taken));
        c->out_len -= (int)taken;
        portEXIT_CRITICAL(&c->mux);
    }
    if (c->want_close)
    {
        c->client.flush();
        conn_drop(c, "closed by the console");
    }
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
            if (s_conn[i].state == TELNET_CONN_NEW || s_conn[i].state == TELNET_CONN_OPEN)
            {
                conn_drop(&s_conn[i], "wifi lost");
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
        if (s_conn != NULL)
        {
            if (s_server_running)
            {
                WiFiClient incoming = s_server.accept();
                if (incoming)
                {
                    conn_accept(incoming);
                }
            }
            for (int i = 0; i < TELNET_TASK_USERS; i++)
            {
                conn_pass(&s_conn[i]);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
