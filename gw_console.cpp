/**************************************************************************
** GW_CONSOLE - the login and the XMSGGW> command line, libxmsg's console **
**                                                                       **
** Built after libxmsg examples/host_tad_telnet/host_tad_telnet.c, which **
** runs the same console on a PC.                                        **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "gw_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <libxmsg/console/console.h>
#include <libxmsg/console/console_tad.h>
#include <libxmsg/xrout/xrout_names_walk.h>
#include <libxmsg/xrout/xrout_walk.h>

#include "console_out.h"
#include "names.h"
#include "node_task.h"
#include "settings.h"
#include "telnet_task.h"
#include "term.h"
#include "users.h"

#ifndef XMSGGW_VERSION
#define XMSGGW_VERSION "0.1.0"
#endif

/** Console places: telnet users and SINTRAN callers together, one term.cpp
 *  slot each (place i = slot i). */
#define PLACES TERM_TELNET_SLOTS

/** @brief What the console side knows of one place's onward session. */
typedef struct
{
    bool asked;          /* onward_open was called; an end must be reported */
    bool running_told;   /* RUNNING reported */
    bool echo_known;
    bool echo;           /* the last echo wish reported */
} place_state;

/** @brief One telnet connection's line. */
typedef struct
{
    int conn;            /* telnet_task.h index */
    ConsoleSession *s;   /* NULL until opened, and after close / loss */
    bool closed;         /* the core closed it */
} telnet_line;

static ConsoleHost s_host;
static ConsoleSession *s_sessions = NULL;  /* PLACES, PSRAM */
static ConsoleTadLine *s_tad = NULL;       /* GW_CONSOLE_TAD_LINES, PSRAM */
static telnet_line s_tel[TELNET_TASK_USERS];
static place_state s_place[PLACES];
static AuthUser *s_users = NULL;           /* USERS_MAX, PSRAM: USERS and DELUSER */
static char s_banner[64];
static bool s_ok = false;

static XroutWalk *s_walk = NULL;           /* LIST, PSRAM */
static XroutNamesWalk *s_nwalk = NULL;     /* NAMES, PSRAM */
static int s_walk_place = -1;              /* who the running LIST is for */
static int s_nwalk_place = -1;             /* who the running NAMES is for */
static XmsgNode *s_node = NULL;            /* set on each pass; the services need it */
/* The node task's time for this pass. The walks must start on the same
 * clock the node ticks them with: a millis() read later than the pass's
 * own time made the timeout sum go below zero and LIST ended at once
 * with "the host did not answer" (2026-11-10, first live run). */
static uint32_t s_now = 0;

/* ------------------------------------------------------------- users */

static int users_get(void *ctx, size_t index, AuthUser *out)
{
    (void)ctx;
    int n = users_read(s_users, USERS_MAX);
    if ((int)index >= n)
    {
        return -1;
    }
    *out = s_users[index];
    return 0;
}

static int users_remove(void *ctx, const char *name)
{
    (void)ctx;
    int n = users_read(s_users, USERS_MAX);
    for (int i = 0; i < n; i++)
    {
        if (strcmp(s_users[i].name, name) == 0)
        {
            s_users[i] = s_users[n - 1];
            return users_write(s_users, n - 1);
        }
    }
    return -1;
}

/* ---------------------------------------------------------- services */

/** @brief A system name to its number: ALIAS by hand first, then the host's names. */
static int svc_resolve(void *ctx, const char *system, uint16_t *number)
{
    (void)ctx;
    if (settings_alias(system, number) == 0 || names_lookup(system, number) == 0)
    {
        return 0;
    }
    return -1;
}

static int svc_open(void *ctx, int place, const char *system, uint16_t number,
                    const char *tad_user)
{
    (void)ctx;
    if (place < 0 || place >= PLACES)
    {
        return XMSG_EINVAL;
    }
    if (term_open_to(place, system, number, tad_user) != 0)
    {
        return XMSG_ESTATE;
    }
    s_place[place].asked = true;
    s_place[place].running_told = false;
    s_place[place].echo_known = false;
    return XMSG_OK;
}

static int svc_write(void *ctx, int place, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    /* One call is at most one TAD data letter (console.h): take bytes only
     * while nothing typed earlier is still waiting, so the asker cuts this
     * call's bytes alone, at the host's break characters (tad_asker.h). */
    return term_type_one(place, bytes, (int)len);
}

static void svc_escape(void *ctx, int place)
{
    (void)ctx;
    term_escape(place);
}

static void svc_hangup(void *ctx, int place)
{
    (void)ctx;
    term_hangup(place);
}

static void walk_entry(void *ctx, const XroutRouteEntry *e)
{
    (void)ctx;
    if (s_walk_place >= 0)
    {
        console_list_entry(console_host_session(&s_host, s_walk_place), e);
    }
}

static void walk_done(void *ctx, int result, unsigned entries)
{
    (void)ctx;
    int p = s_walk_place;
    s_walk_place = -1;
    if (p >= 0)
    {
        console_list_done(console_host_session(&s_host, p), result, entries);
    }
}

/** @brief Whether any walk to the host is running (ours or the name-table refresh). */
static bool any_walk(void)
{
    return (s_walk != NULL && xrout_walk_is_busy(s_walk))
           || (s_nwalk != NULL && xrout_names_walk_is_busy(s_nwalk)) || names_busy();
}

static int svc_list(void *ctx, int place)
{
    (void)ctx;
    if (s_walk == NULL || s_node == NULL || any_walk())
    {
        return XMSG_EAGAIN;
    }
    int32_t peer = lapb_peer_node(&s_node->lapb);
    if (peer < 0)
    {
        return XMSG_ESTATE;
    }
    XroutWalkCallbacks cb = { NULL, walk_entry, walk_done };
    s_walk_place = place;
    /* The host's XMSG system number is taken to be its node number, as on
     * every captured machine (libxmsg xmsg_node.h, "Routing questions"). */
    int rc = xrout_walk_start(s_walk, s_node, (uint16_t)peer, (uint16_t)peer, 10000u, &cb,
                              s_now);
    if (rc != XMSG_OK)
    {
        s_walk_place = -1;
    }
    return rc;
}

static void svc_list_cancel(void *ctx, int place)
{
    (void)ctx;
    if (s_walk_place == place)
    {
        s_walk_place = -1;   /* no console_list_done for a user who left */
        (void)xrout_walk_cancel(s_walk);
    }
}

static void nwalk_row(void *ctx, const XroutNameRow *row)
{
    (void)ctx;
    if (s_nwalk_place >= 0)
    {
        console_names_row(console_host_session(&s_host, s_nwalk_place), row);
    }
}

static void nwalk_done(void *ctx, int result, unsigned rows)
{
    (void)ctx;
    int p = s_nwalk_place;
    s_nwalk_place = -1;
    if (p >= 0)
    {
        console_names_done(console_host_session(&s_host, p), result, rows);
    }
}

static int svc_names(void *ctx, int place)
{
    (void)ctx;
    if (s_nwalk == NULL || s_node == NULL || any_walk())
    {
        return XMSG_EAGAIN;
    }
    int32_t peer = lapb_peer_node(&s_node->lapb);
    if (peer < 0)
    {
        return XMSG_ESTATE;
    }
    XroutNamesWalkCallbacks cb = { NULL, nwalk_row, nwalk_done };
    s_nwalk_place = place;
    int rc = xrout_names_walk_start(s_nwalk, s_node, (uint16_t)peer, (uint16_t)peer, 10000u,
                                    &cb, s_now);
    if (rc != XMSG_OK)
    {
        s_nwalk_place = -1;
    }
    return rc;
}

static void svc_names_cancel(void *ctx, int place)
{
    (void)ctx;
    if (s_nwalk_place == place)
    {
        s_nwalk_place = -1;
        (void)xrout_names_walk_cancel(s_nwalk);
    }
}

static void svc_link_info(void *ctx, TermLinkInfo *li)
{
    node_status st;
    (void)ctx;
    node_task_get_status(&st);
    memset(li, 0, sizeof *li);
    li->own_node = st.own_node;
    li->own_system = st.own_node;
    li->peer_node_learned = st.peer_node;
    /* XMSGGW has no configured peer: it takes the node the link learns. */
    li->peer_node_cfg = (st.peer_node > 0) ? (uint16_t)st.peer_node : 0u;
    li->peer_system = NULL;
    li->link_up = st.up;
    li->link_ready = st.ready;
    li->transport = "HDLC line (ESP32-S2)";
    li->line_bps = settings_rate();
    li->line_is_tcp = false;
    li->frames_in = st.frames_in;
    li->frames_out = st.frames_out;
    li->datagrams_in = st.datagrams_in;
    li->datagrams_out = st.datagrams_out;
    li->uptime_s = millis() / 1000u;
}

static int svc_info_more(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    int n = snprintf(buf, cap, "XMSGGW %s, built %s %s\r\n", XMSGGW_VERSION, __DATE__, __TIME__);
    return (n < 0) ? 0 : ((size_t)n >= cap ? (int)cap - 1 : n);
}

/* --------------------------------------------- XMSGGW's own commands */

static bool s_restart = false;       /* RESTART asked; done once the text is out */
static uint32_t s_restart_at = 0;

/**
 * @brief Whether the command word of a line is `word` (case ignored).
 * @param[out] args  Set to the text after the word, spaces skipped.
 */
static bool word_is(const char *text, const char *word, const char **args)
{
    size_t n = strlen(word);
    while (*text == ' ')
    {
        text++;
    }
    if (strncasecmp(text, word, n) != 0 || (text[n] != '\0' && text[n] != ' '))
    {
        return false;
    }
    text += n;
    while (*text == ' ')
    {
        text++;
    }
    *args = text;
    return true;
}

/** @brief NODE [n] (admin): show or store our node number, used after RESTART. */
static void cmd_node(const char *args, char *out, size_t cap)
{
    node_status st;
    if (*args == '\0')
    {
        node_task_get_status(&st);
        (void)snprintf(out, cap, "stored node number %u, running as %u\r\n",
                       (unsigned)settings_node(), (unsigned)st.own_node);
        return;
    }
    char *end = NULL;
    unsigned long value = strtoul(args, &end, 10);
    while (*end == ' ')
    {
        end++;
    }
    if (end == args || *end != '\0' || value == 0 || value > 65535u)
    {
        (void)snprintf(out, cap, "NODE takes a number from 1 to 65535\r\n");
        return;
    }
    if (settings_set_node((uint16_t)value) != 0)
    {
        (void)snprintf(out, cap, "could not store the node number\r\n");
        return;
    }
    (void)snprintf(out, cap, "node number %lu stored; RESTART to use it\r\n", value);
}

/** @brief Lines the core does not know (console.h services.command). */
static int svc_command(void *ctx, int place, const char *line, AuthLevel level, char *out,
                       size_t cap)
{
    const char *args = NULL;
    (void)ctx;
    bool node = word_is(line, "NODE", &args);
    bool restart = !node && word_is(line, "RESTART", &args);
    if (!node && !restart)
    {
        return 0;
    }
    if (level != AUTH_LEVEL_ADMIN)
    {
        (void)snprintf(out, cap, "not allowed\r\n");
        return 1;
    }
    console_out_log(CONTROL_FRAME_LOG_INFO, "Console place %d: %s", place, line);
    if (node)
    {
        cmd_node(args, out, cap);
        return 1;
    }
    (void)snprintf(out, cap, "restarting; every session ends\r\n");
    s_restart = true;
    s_restart_at = millis() + 500u;   /* time for the text to reach the user */
    return 1;
}

/* ------------------------------------------------------- telnet lines */

static int tel_write(void *ctx, const uint8_t *bytes, size_t len)
{
    telnet_line *t = (telnet_line *)ctx;
    return telnet_conn_write(t->conn, bytes, (int)len);
}

static void tel_close(void *ctx, ConsoleClose why)
{
    telnet_line *t = (telnet_line *)ctx;
    (void)why;
    t->s = NULL;         /* the session is the core's again (console.h) */
    t->closed = true;
    telnet_conn_close(t->conn);
}

static void tel_describe(void *ctx, char *buf, size_t cap)
{
    telnet_line *t = (telnet_line *)ctx;
    char peer[32];
    telnet_conn_opened(t->conn, peer, sizeof peer);   /* only copies once OPEN */
    (void)snprintf(buf, cap, "%s", peer);
}

/** @brief New and lost telnet connections, and their typed bytes. */
static void telnet_pass(uint32_t now)
{
    for (int i = 0; i < TELNET_TASK_USERS; i++)
    {
        telnet_line *t = &s_tel[i];
        telnet_conn_state st = telnet_conn_get_state(i);
        if (st == TELNET_CONN_NEW)
        {
            ConsoleLine line;
            memset(&line, 0, sizeof line);
            t->conn = i;
            t->closed = false;
            telnet_conn_opened(i, NULL, 0);
            line.ctx = t;
            /* 7-bit text: users come in from TDV terminals in Norwegian
             * 7-bit too, where [ ] show as letters (Ronny, 2026-11-10). */
            line.caps = CONSOLE_LINE_7BIT;
            line.kind = "telnet";
            line.write = tel_write;
            line.close = tel_close;
            line.describe = tel_describe;
            t->s = console_host_open(&s_host, &line, now);
            if (t->s == NULL && !t->closed)
            {
                static const char full[] = "\r\nXMSGGW: every place is in use. Try again later.\r\n";
                (void)telnet_conn_write(i, (const uint8_t *)full, (int)sizeof full - 1);
                telnet_conn_close(i);
                t->closed = true;
            }
        }
        else if (st == TELNET_CONN_OPEN && t->s != NULL)
        {
            uint8_t buf[128];
            int n = telnet_conn_read(i, buf, (int)sizeof buf);
            if (n > 0)
            {
                console_input(t->s, buf, (size_t)n, now);
            }
        }
        else if (st == TELNET_CONN_GONE)
        {
            if (t->s != NULL)
            {
                console_line_lost(t->s);   /* hangs up any onward session */
                t->s = NULL;
            }
            telnet_conn_release(i);
        }
    }
}

/* ---------------------------------------------------------- TAD lines */

static int tad_incoming(void *ctx, const TadResponderCaller *who)
{
    uint16_t used = 0;
    uint16_t total = 0;
    (void)ctx;
    console_host_counts(&s_host, &used, &total);
    if (used >= total)
    {
        console_out_log(CONTROL_FRAME_LOG_WARN,
                        "SINTRAN caller from system %u refused: every place in use",
                        (unsigned)who->system);
        return 4; /* XRNSP: no free seat (tad_responder.h) */
    }
    console_out_log(CONTROL_FRAME_LOG_INFO, "SINTRAN caller from system %u port 0x%04X",
                    (unsigned)who->system, (unsigned)who->port);
    return 0;
}

static void tad_status(void *ctx, uint16_t *in_use, uint16_t *total)
{
    (void)ctx;
    console_host_counts(&s_host, in_use, total);
}

/** @brief The console side of each SINTRAN caller's line. */
static void tad_pass(uint32_t now)
{
    for (int k = 0; k < GW_CONSOLE_TAD_LINES; k++)
    {
        ConsoleTadLine *t = &s_tad[k];
        if (console_tad_needs_session(t))
        {
            ConsoleLine line;
            ConsoleSession *s;
            console_tad_line(t, &line);
            s = console_host_open(&s_host, &line, now);
            console_tad_attach(t, s);
            if (s == NULL && line.close != NULL && t->session == NULL && !t->close_wanted)
            {
                static const char full[] = "\r\nXMSGGW: every place is in use\r\n";
                (void)line.write(line.ctx, (const uint8_t *)full, sizeof full - 1);
                line.close(line.ctx, CONSOLE_CLOSE_EXIT);
            }
        }
        (void)console_tad_deliver(t, now);
        (void)console_tad_release(t);
        console_tad_pump(t, now);
    }
}

/* ---------------------------------------------------- onward sessions */

/** @brief Pass each place's onward session news to the console. */
static void onward_pass(void)
{
    for (int i = 0; i < PLACES; i++)
    {
        place_state *p = &s_place[i];
        if (!p->asked)
        {
            continue;
        }
        ConsoleSession *s = console_host_session(&s_host, i);
        term_status ts;
        uint8_t buf[256];
        int n;
        while ((n = term_read(i, buf, (int)sizeof buf)) > 0)
        {
            console_onward_text(s, buf, (size_t)n);
        }
        term_get_status(i, &ts);
        if (!p->running_told && ts.open && strcmp(ts.state, "running") == 0)
        {
            p->running_told = true;
            console_onward_state(s, CONSOLE_ONWARD_RUNNING, 0);
        }
        if (p->running_told && ts.open && (!p->echo_known || p->echo != ts.echo))
        {
            p->echo_known = true;
            p->echo = ts.echo;
            console_onward_echo(s, ts.echo);
        }
        if (!ts.open)   /* open covers "asked for, not started yet" too */
        {
            p->asked = false;
            console_onward_state(s, (ts.error != 0) ? CONSOLE_ONWARD_FAILED : CONSOLE_ONWARD_CLOSED,
                                 ts.error);
        }
    }
}

/* -------------------------------------------------------------- public */

int gw_console_init(void)
{
    ConsoleConfig cfg;
    node_status st;

    s_sessions = (ConsoleSession *)heap_caps_calloc(PLACES, sizeof(ConsoleSession),
                                                    MALLOC_CAP_SPIRAM);
    s_tad = (ConsoleTadLine *)heap_caps_calloc(GW_CONSOLE_TAD_LINES, sizeof(ConsoleTadLine),
                                               MALLOC_CAP_SPIRAM);
    s_users = (AuthUser *)heap_caps_calloc(USERS_MAX, sizeof(AuthUser), MALLOC_CAP_SPIRAM);
    s_walk = (XroutWalk *)heap_caps_calloc(1, sizeof(XroutWalk), MALLOC_CAP_SPIRAM);
    s_nwalk = (XroutNamesWalk *)heap_caps_calloc(1, sizeof(XroutNamesWalk), MALLOC_CAP_SPIRAM);
    if (s_sessions == NULL || s_tad == NULL || s_users == NULL || s_walk == NULL
        || s_nwalk == NULL)
    {
        console_out_log(CONTROL_FRAME_LOG_ERROR, "No PSRAM for the console");
        return -1;
    }
    if (users_init() != 0)
    {
        console_out_log(CONTROL_FRAME_LOG_ERROR, "The user list could not be read or written");
        return -1;
    }

    console_config_default(&cfg);
    cfg.mode = CONSOLE_MODE_PROMPT;
    cfg.prompt = "XMSGGW> ";
    node_task_get_status(&st);
    (void)snprintf(s_banner, sizeof s_banner, "\r\nXMSGGW %s, node %u\r\n", XMSGGW_VERSION,
                   (unsigned)settings_node());
    cfg.banner = s_banner;
    /* "<user>@xmsggw" when it fits 16 characters, else the name alone
     * (Ronny, 2026-11-10; not "@d101": D101 is shown as the system). */
    cfg.tad_user_suffix = "xmsggw";
    cfg.iterations = USERS_ITERATIONS;
    cfg.store = *users_store();
    cfg.rng = *users_random();
    cfg.users.get = users_get;
    cfg.users.remove = users_remove;
    cfg.services.resolve = svc_resolve;
    cfg.services.onward_open = svc_open;
    cfg.services.onward_write = svc_write;
    cfg.services.onward_escape = svc_escape;
    cfg.services.onward_hangup = svc_hangup;
    cfg.services.list_start = svc_list;
    cfg.services.list_cancel = svc_list_cancel;
    cfg.services.names_start = svc_names;
    cfg.services.names_cancel = svc_names_cancel;
    cfg.services.link_info = svc_link_info;
    cfg.services.info_more = svc_info_more;
    cfg.services.command = svc_command;
    cfg.services.help_admin =
        "NODE <n>           show or store our node number, used after RESTART (admin)\r\n"
        "RESTART            restart the gateway; every session ends (admin)\r\n";
    if (console_host_init(&s_host, &cfg, s_sessions, PLACES) != 0)
    {
        console_out_log(CONTROL_FRAME_LOG_ERROR, "console_host_init failed");
        return -1;
    }

    for (int k = 0; k < GW_CONSOLE_TAD_LINES; k++)
    {
        ConsoleTadConfig tc;
        console_tad_config_default(&tc);
        tc.incoming = tad_incoming;
        tc.status = tad_status;
        /* Each responder its own logical unit, 0x0300 + n as captured
         * hosts do (tad_responder.h: meaning inferred, not decoded). */
        tc.responder.logical_unit = (uint16_t)(0x0300u + (uint16_t)k);
        (void)console_tad_init(&s_tad[k], &tc);
    }
    for (int i = 0; i < TELNET_TASK_USERS; i++)
    {
        s_tel[i].conn = i;
    }
    s_ok = true;
    return 0;
}

void gw_console_letter(XmsgNode *node, const XmsgDatagram *dg, uint32_t now)
{
    if (!s_ok)
    {
        return;
    }
    for (int k = 0; k < GW_CONSOLE_TAD_LINES; k++)
    {
        int rc = console_tad_offer(&s_tad[k], node, dg, now);
        if (rc != XMSG_EAGAIN)
        {
            return; /* handled, not a TAD letter, or an error the line has logged */
        }
    }
    if (tad_responder_is_status_query(dg))
    {
        uint16_t used = 0;
        uint16_t total = 0;
        console_host_counts(&s_host, &used, &total);
        (void)tad_responder_status_reply(node, dg, used, (uint16_t)(total - used), now);
        return;
    }
    console_out_log(CONTROL_FRAME_LOG_WARN, "SINTRAN caller refused: all %d lines busy",
                    GW_CONSOLE_TAD_LINES);
    (void)tad_responder_refuse(node, dg, 4u, now);
}

void gw_console_pass(XmsgNode *node, uint32_t now)
{
    if (!s_ok)
    {
        return;
    }
    s_node = node;
    s_now = now;
    telnet_pass(now);
    tad_pass(now);
    onward_pass();
    console_host_poll(&s_host, now);
    if (s_restart && (int32_t)(now - s_restart_at) >= 0)
    {
        console_out_flush();
        ESP.restart();
    }
}

bool gw_console_walk_busy(void)
{
    return (s_walk != NULL && xrout_walk_is_busy(s_walk))
           || (s_nwalk != NULL && xrout_names_walk_is_busy(s_nwalk));
}
