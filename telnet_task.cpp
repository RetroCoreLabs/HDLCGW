/**************************************************************************
** TELNET_TASK - WiFi, telnet login, the XMSGGW command line, and the    **
** users' terminal sessions to the host                                  **
**                                                                       **
** The WiFi station handling follows HDLCGW wifi_task.cpp (station mode, **
** no sleep, a new try every 20 s while not joined), without its TCP     **
** frame bridge. The login, command line and session steps follow        **
** libxmsg examples/host_tad_telnet/host_tad_telnet.c (its prompt mode). **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "telnet_task.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <libxmsg/auth/auth_hash.h>
#include <libxmsg/auth/auth_login.h>
#include <libxmsg/term/term_line.h>
#include <libxmsg/term/term_report.h>

#include "console_out.h"
#include "console_task.h"
#include "node_task.h"
#include "settings.h"
#include "term.h"
#include "route_list.h"
#include "users.h"

#define WIFI_RETRY_MS 20000u
#define HOSTNAME "xmsggw"
#define PROMPT "XMSGGW> "

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
static bool s_users_ready = false;

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

/* Where a user is. */
typedef enum
{
    PH_LOGIN = 0,   /* name and password (libxmsg auth_login) */
    PH_PROMPT,      /* at "XMSGGW> " */
    PH_PASSWD,      /* PASSWORD: changing one's own password (the login dialogue) */
    PH_SETPW,       /* ADDUSER / RESETPW: an admin types a password for another user */
    PH_SESSION      /* in a terminal session to a SINTRAN system */
} phase;

/* The plain-data part of a user, in PSRAM (internal RAM is short: the login
 * dialogue alone holds two user records per user). */
typedef struct
{
    phase ph;
    TermLine line;            /* the command line / login line editor */
    AuthLogin login;
    char user[AUTH_NAME_MAX + 1u];
    AuthLevel level;
    AuthUser setpw;           /* PH_SETPW: the record being made (hash only) */
    bool setpw_again;         /* PH_SETPW: the first entry is in, the second is due */
    bool closing;             /* close once the text is out */
    bool listing;             /* LIST running: its text comes, then the prompt */
    char system[16];          /* PH_SESSION: the system asked for */
    char remote[24];          /* "IP:port" */
    uint32_t connected_ms;
} user_state;

/** @brief One telnet user; user i always uses terminal session slot i. */
typedef struct
{
    WiFiClient client;
    bool on;
    bool session_seen_open;   /* the session for this user has opened */
    tn_state tn;
    uint8_t tn_verb;
    bool last_was_cr;
    TermEscape esc;           /* lone ESC (the escape key) or a function key (libxmsg) */
    user_state *s;            /* in PSRAM */
} telnet_user;

static telnet_user s_users[TELNET_TASK_USERS];
static telnet_task_stats s_stats;
static AuthUser *s_list = NULL;   /* USERS_MAX records for USERS / DELUSER, in PSRAM */
static char *s_report = NULL;     /* REPORT_SIZE for WHO, in PSRAM */
#define REPORT_SIZE (256 + (TELNET_TASK_USERS * 160))

void telnet_task_init(void)
{
    memset(&s_stats, 0, sizeof s_stats);
    s_stats.settings_source = settings_wifi(s_ssid, s_pass);
    for (int i = 0; i < TELNET_TASK_USERS; i++)
    {
        s_users[i].s = (user_state *)heap_caps_calloc(1, sizeof(user_state), MALLOC_CAP_SPIRAM);
    }
    s_list = (AuthUser *)heap_caps_calloc(USERS_MAX, sizeof(AuthUser), MALLOC_CAP_SPIRAM);
    s_report = (char *)heap_caps_calloc(1, REPORT_SIZE, MALLOC_CAP_SPIRAM);
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

/** @brief Write formatted text to a user. */
static void user_printf(telnet_user *u, const char *fmt, ...)
{
    char text[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (n > 0)
    {
        user_write(u, (const uint8_t *)text,
                   (size_t)((n < (int)sizeof text) ? n : (int)sizeof text - 1));
    }
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
    /* The login dialogue and the line may hold password hashes or typing. */
    if (u->s->listing)
    {
        route_list_cancel(i);
    }
    auth_wipe(u->s, sizeof *u->s);
    console_out_log(CONTROL_FRAME_LOG_INFO, "Telnet user %d gone: %s", i, why);
}

/** @brief The prompt for where the user is; sets the line's echo to match. */
static void show_prompt(telnet_user *u)
{
    user_state *s = u->s;
    const char *p;
    switch (s->ph)
    {
    case PH_LOGIN:
        p = auth_login_prompt(&s->login);
        if (p != NULL)
        {
            s->line.echo = auth_login_echo(&s->login);
            user_say(u, p);
        }
        break;
    case PH_PASSWD:
        s->line.echo = false;
        p = auth_login_prompt(&s->login);
        if (p != NULL)
        {
            user_say(u, p);
        }
        break;
    case PH_SETPW:
        s->line.echo = false;
        user_printf(u, "%s for %s: ", s->setpw_again ? "again" : "new password", s->setpw.name);
        break;
    case PH_PROMPT:
        s->line.echo = true;
        user_say(u, PROMPT);
        break;
    case PH_SESSION:
    default:
        break;
    }
}

/** @brief Back to the command line (after a session, or a refused CONNECT). */
static void to_prompt(telnet_user *u)
{
    u->s->ph = PH_PROMPT;
    term_line_init(&u->s->line, true);
    show_prompt(u);
}

/** @brief A new telnet user: negotiate, greet, ask for the name. */
static void user_start(telnet_user *u)
{
    static const uint8_t NEGOTIATE[] = { TN_IAC, TN_WILL, TN_OPT_ECHO, TN_IAC, TN_WILL, TN_OPT_SGA };
    user_state *s = u->s;
    int i = user_index(u);

    u->on = true;
    s_stats.users_on++;
    s_stats.users++;
    u->tn = TN_DATA;
    u->last_was_cr = false;
    u->session_seen_open = false;
    /* libxmsg's default 50 ms, as its own telnet bridge uses (host_tad_telnet.c). */
    term_escape_init(&u->esc, TERM_ESCAPE_DEFAULT_MS);
    u->client.setNoDelay(true);
    user_write(u, NEGOTIATE, sizeof NEGOTIATE);

    memset(s, 0, sizeof *s);
    s->connected_ms = millis();
    (void)snprintf(s->remote, sizeof s->remote, "%s:%u",
                   u->client.remoteIP().toString().c_str(), (unsigned)u->client.remotePort());
    s->ph = PH_LOGIN;
    term_line_init(&s->line, true);
    if (!s_users_ready
        || auth_login_init(&s->login, users_store(), users_random(), AUTH_LOGIN_TRIES,
                           AUTH_LOGIN_DELAY_MS) != 0)
    {
        user_say(u, "\r\nXMSGGW: the user list cannot be read. Try again later.\r\n");
        user_drop(u, "no user list");
        return;
    }
    user_printf(u, "\r\nXMSGGW %s, node %u\r\n", XMSGGW_VERSION, (unsigned)settings_node());
    console_out_log(CONTROL_FRAME_LOG_INFO, "Telnet user %d on from %s", i, s->remote);
    show_prompt(u);
}

/* ------------------------------------------------------------ commands */

static void cmd_help(telnet_user *u)
{
    user_say(u, "CONNECT <system>   connect to a SINTRAN system, e.g. CONNECT D100\r\n"
                "LIST               the systems that can be reached\r\n"
                "WHO                the users on this gateway\r\n"
                "INFO               the gateway and its link to the host\r\n"
                "PASSWORD           change your password\r\n"
                "HELP               this text\r\n"
                "EXIT, LOGOUT, QUIT close the connection\r\n");
    if (u->s->level == AUTH_LEVEL_ADMIN)
    {
        user_say(u, "USERS              the users (admin)\r\n"
                    "ADDUSER name <admin>  add a user (admin)\r\n"
                    "DELUSER name       delete a user (admin)\r\n"
                    "RESETPW name       set a new password, changed at login (admin)\r\n");
    }
    user_say(u, "In a session: ESC alone breaks the running program; log out of SINTRAN to\r\n"
                "come back here.\r\n");
}

static void cmd_who(telnet_user *self)
{
    TermWhoEntry e[TELNET_TASK_USERS];
    term_status ts[TELNET_TASK_USERS];
    uint32_t now = millis();
    size_t n = 0;
    if (s_report == NULL)
    {
        return;
    }
    for (int i = 0; i < TELNET_TASK_USERS; i++)
    {
        const telnet_user *u = &s_users[i];
        if (!u->on)
        {
            continue;
        }
        term_get_status(i, &ts[n]);
        e[n].terminal = i + 1;
        e[n].user = (u->s->ph == PH_LOGIN) ? NULL : u->s->user;
        e[n].remote = u->s->remote;
        e[n].connected_s = (now - u->s->connected_ms) / 1000u;
        e[n].system = (u->s->ph == PH_SESSION) ? u->s->system : NULL;
        e[n].session_state = ts[n].state;
        e[n].is_self = (u == self);
        n++;
    }
    if (term_report_who(e, n, s_report, REPORT_SIZE) > 0)
    {
        user_say(self, s_report);
    }
}

static void cmd_info(telnet_user *u)
{
    node_status st;
    node_task_get_status(&st);
    user_printf(u, "XMSGGW %s, built %s %s\r\n", XMSGGW_VERSION, __DATE__, __TIME__);
    user_printf(u, "node %u, host node %ld, link %s\r\n", (unsigned)st.own_node,
                (long)st.peer_node, st.ready ? "ready" : (st.up ? "up, not ready" : "down"));
    user_printf(u, "frames in %lu out %lu, datagrams in %lu out %lu, bad FCS %lu\r\n",
                (unsigned long)st.frames_in, (unsigned long)st.frames_out,
                (unsigned long)st.datagrams_in, (unsigned long)st.datagrams_out,
                (unsigned long)st.bad_crc);
    user_printf(u, "telnet users on %lu of %d, turned away %lu\r\n",
                (unsigned long)s_stats.users_on, TELNET_TASK_USERS,
                (unsigned long)s_stats.refused);
    user_printf(u, "last password check %lu ms, telnet stack free %lu, uptime %lu s\r\n",
                (unsigned long)users_last_hash_ms(),
                (unsigned long)uxTaskGetStackHighWaterMark(NULL),
                (unsigned long)(millis() / 1000u));
}

/** @brief CONNECT <system>: open the user's session slot. */
static void cmd_connect(telnet_user *u, const char *system)
{
    node_status st;
    int i = user_index(u);
    if (strlen(system) >= sizeof u->s->system)
    {
        user_say(u, "that system name is too long\r\n");
        show_prompt(u);
        return;
    }
    node_task_get_status(&st);
    if (!st.ready)
    {
        user_say(u, "the link to the host is not up; try again later\r\n");
        show_prompt(u);
        return;
    }
    if (term_open(i, system, u->s->user) != 0)
    {
        user_say(u, "this terminal session is still closing; try again\r\n");
        show_prompt(u);
        return;
    }
    /* The length was checked above: it fits with its NUL. */
    memcpy(u->s->system, system, strlen(system) + 1u);
    u->s->ph = PH_SESSION;
    u->session_seen_open = false;
    u->last_was_cr = false;
    term_escape_init(&u->esc, TERM_ESCAPE_DEFAULT_MS);
    user_printf(u, "connecting to %s...\r\n", system);
    console_out_log(CONTROL_FRAME_LOG_INFO, "Telnet user %d (%s): connect %s", i, u->s->user,
                    system);
}

static void cmd_users(telnet_user *u)
{
    int n = users_read(s_list, USERS_MAX);
    for (int i = 0; i < n; i++)
    {
        user_printf(u, "%-32s %-5s%s\r\n", s_list[i].name,
                    (s_list[i].level == AUTH_LEVEL_ADMIN) ? "admin" : "user",
                    s_list[i].must_change ? "  must change password" : "");
    }
    auth_wipe(s_list, USERS_MAX * sizeof(AuthUser));
}

static void cmd_deluser(telnet_user *u, const char *name)
{
    int n = users_read(s_list, USERS_MAX);
    int at = -1;
    for (int i = 0; i < n; i++)
    {
        if (strcmp(s_list[i].name, name) == 0)
        {
            at = i;
        }
    }
    if (at < 0)
    {
        user_printf(u, "no user %s\r\n", name);
    }
    else if (auth_users_is_last_admin(s_list, (size_t)n, name))
    {
        user_printf(u, "%s is the last admin and cannot be deleted\r\n", name);
    }
    else
    {
        s_list[at] = s_list[n - 1];
        n--;
        if (users_write(s_list, n) == 0)
        {
            console_out_log(CONTROL_FRAME_LOG_INFO, "Telnet user %d (%s): deleted user %s",
                            user_index(u), u->s->user, name);
            user_printf(u, "user %s deleted\r\n", name);
        }
        else
        {
            user_say(u, "cannot write the user list\r\n");
        }
    }
    auth_wipe(s_list, USERS_MAX * sizeof(AuthUser));
}

/** @brief ADDUSER and RESETPW: check the name, then ask the password twice. */
static void cmd_setpw_begin(telnet_user *u, const char *name, bool add, AuthLevel level)
{
    user_state *s = u->s;
    AuthUser found;
    bool exists;
    if (!auth_name_is_valid(name))
    {
        user_printf(u, "a user name is 1 to %u characters, no blank and no ':'\r\n",
                    AUTH_NAME_MAX);
        return;
    }
    exists = users_lookup(name, &found) == 0;
    auth_wipe(&found, sizeof found);
    if (add && exists)
    {
        user_printf(u, "user %s exists; RESETPW sets a new password\r\n", name);
        return;
    }
    if (!add && !exists)
    {
        user_printf(u, "no user %s\r\n", name);
        return;
    }
    memset(&s->setpw, 0, sizeof s->setpw);
    (void)snprintf(s->setpw.name, sizeof s->setpw.name, "%s", name);
    s->setpw.level = level;
    s->setpw.iterations = add ? 0u : 1u; /* 0: a new user, made at the first entry */
    s->setpw_again = false;
    s->ph = PH_SETPW;
}

/** @brief A line typed in PH_SETPW (wiped by the caller). */
static void setpw_line(telnet_user *u, const char *text)
{
    user_state *s = u->s;
    AuthUser rec;
    if (!s->setpw_again)
    {
        int rc;
        if (text[0] == '\0')
        {
            user_say(u, "an empty password is not allowed\r\n");
            s->ph = PH_PROMPT;
            return;
        }
        if (s->setpw.iterations == 0u)
        {
            AuthLevel level = s->setpw.level;
            rc = auth_user_make(&rec, s->setpw.name, text, USERS_ITERATIONS,
                                users_random());
            rec.level = level;
            if (rc == 0)
            {
                rec.must_change = true; /* the admin knows it; the user picks their own */
            }
        }
        else
        {
            rc = (users_lookup(s->setpw.name, &rec) == 0)
                     ? auth_user_reset_password(&rec, text, users_random())
                     : -1;
        }
        if (rc != 0)
        {
            user_say(u, "could not set the password\r\n");
            auth_wipe(&rec, sizeof rec);
            s->ph = PH_PROMPT;
            return;
        }
        s->setpw = rec;
        auth_wipe(&rec, sizeof rec);
        s->setpw_again = true;
        return;
    }
    if (!auth_user_check(&s->setpw, text))
    {
        user_say(u, "the two passwords differ; nothing changed\r\n");
    }
    else if (users_put(&s->setpw) != 0)
    {
        user_say(u, "cannot write the user list\r\n");
    }
    else
    {
        console_out_log(CONTROL_FRAME_LOG_INFO, "Telnet user %d (%s): set the password of %s",
                        user_index(u), s->user, s->setpw.name);
        user_printf(u, "password of %s set; it must be changed at the next login\r\n",
                    s->setpw.name);
    }
    auth_wipe(&s->setpw, sizeof s->setpw);
    s->ph = PH_PROMPT;
}

/** @brief A line typed in PH_PASSWD (the user's own change, through the login dialogue). */
static void passwd_line(telnet_user *u, const char *text, uint32_t now)
{
    user_state *s = u->s;
    AuthLoginState st = auth_login_line(&s->login, text, now);
    AuthLoginNote note = auth_login_take_note(&s->login);
    if (note != AUTH_NOTE_NONE)
    {
        user_printf(u, "%s\r\n", auth_login_note_text(note));
    }
    if (note == AUTH_NOTE_CHANGED)
    {
        console_out_log(CONTROL_FRAME_LOG_INFO, "Telnet user %d (%s): changed the password",
                        user_index(u), s->user);
    }
    if (st == AUTH_LOGIN_DONE)
    {
        s->ph = PH_PROMPT;
    }
}

/** @brief Whether an admin command may run; tells the user when not. */
static bool admin_only(telnet_user *u)
{
    if (u->s->level == AUTH_LEVEL_ADMIN)
    {
        return true;
    }
    user_say(u, "not allowed\r\n");
    return false;
}

/** @brief One line typed at the prompt. */
/* size exemption: command table */
static void prompt_line(telnet_user *u, const char *text)
{
    char arg[AUTH_NAME_MAX + 1u];
    TermCmd cmd = term_cmd_parse(text, arg, sizeof arg);
    if (strlen(arg) > AUTH_NAME_MAX)
    {
        cmd = TERM_CMD_UNKNOWN;
    }
    switch (cmd)
    {
    case TERM_CMD_EMPTY:
        break;
    case TERM_CMD_CONNECT:
        cmd_connect(u, arg);   /* no prompt after it: the host's banner comes */
        return;
    case TERM_CMD_LIST:
        /* Asks the host for its whole routing table (route_list.h); the
         * prompt comes back when the list has ended. */
        if (route_list_start(user_index(u)) == 0)
        {
            u->s->listing = true;
            return;
        }
        user_say(u, "LIST: another user's LIST is running; try again\r\n");
        break;
    case TERM_CMD_WHO:
        cmd_who(u);
        break;
    case TERM_CMD_INFO:
        cmd_info(u);
        break;
    case TERM_CMD_HELP:
        cmd_help(u);
        break;
    case TERM_CMD_EXIT:
        user_say(u, "bye\r\n");
        u->s->closing = true;
        return;
    case TERM_CMD_PASSWORD:
        if (auth_login_change_begin(&u->s->login) == 0)
        {
            u->s->ph = PH_PASSWD;
        }
        else
        {
            user_printf(u, "%s\r\n", auth_login_note_text(auth_login_take_note(&u->s->login)));
        }
        break;
    case TERM_CMD_USERS:
        if (admin_only(u))
        {
            cmd_users(u);
        }
        break;
    case TERM_CMD_ADDUSER:
        /* fall through */
    case TERM_CMD_ADDADMIN:
        if (admin_only(u))
        {
            cmd_setpw_begin(u, arg, true,
                            (cmd == TERM_CMD_ADDADMIN) ? AUTH_LEVEL_ADMIN : AUTH_LEVEL_USER);
        }
        break;
    case TERM_CMD_DELUSER:
        if (admin_only(u))
        {
            cmd_deluser(u, arg);
        }
        break;
    case TERM_CMD_RESETPW:
        if (admin_only(u))
        {
            cmd_setpw_begin(u, arg, false, AUTH_LEVEL_USER);
        }
        break;
    case TERM_CMD_UNKNOWN:
        /* fall through */
    default:
        user_say(u, "unknown command; HELP lists them\r\n");
        break;
    }
    show_prompt(u);
}

/** @brief One line typed at the login (it may be a password; wiped by the caller). */
static void login_line(telnet_user *u, const char *text, uint32_t now)
{
    user_state *s = u->s;
    int i = user_index(u);
    AuthLoginState before = s->login.state;
    AuthLoginState st = auth_login_line(&s->login, text, now);
    AuthLoginNote note = auth_login_take_note(&s->login);
    if (note != AUTH_NOTE_NONE)
    {
        user_printf(u, "%s\r\n", auth_login_note_text(note));
    }
    if (st == AUTH_LOGIN_NEW_PASSWORD && before == AUTH_LOGIN_PASSWORD)
    {
        user_say(u, "You must set a new password before going on.\r\n");
    }
    switch (st)
    {
    case AUTH_LOGIN_DONE:
        (void)snprintf(s->user, sizeof s->user, "%s", s->login.name);
        s->level = auth_login_level(&s->login);
        console_out_log(CONTROL_FRAME_LOG_INFO, "Telnet user %d: logged in as %s (%s)%s", i,
                        s->user, (s->level == AUTH_LEVEL_ADMIN) ? "admin" : "user",
                        (note == AUTH_NOTE_CHANGED) ? ", new password stored" : "");
        /* A record stored with another round count (the 100000 of the first
         * build) is stored again at USERS_ITERATIONS, from the password just
         * typed: a password change keeps the record's own count (libxmsg
         * auth_user.c), so it would otherwise never move. */
        if (before == AUTH_LOGIN_PASSWORD)
        {
            AuthUser rec;
            if (users_lookup(s->user, &rec) == 0 && rec.iterations != USERS_ITERATIONS)
            {
                AuthUser fresh;
                if (auth_user_make(&fresh, rec.name, text, USERS_ITERATIONS, users_random()) == 0)
                {
                    fresh.level = rec.level;
                    fresh.must_change = rec.must_change;
                    console_out_log(CONTROL_FRAME_LOG_INFO,
                                    "Telnet user %d: %s re-stored at %u rounds (was %lu): %s", i,
                                    s->user, USERS_ITERATIONS, (unsigned long)rec.iterations,
                                    (users_put(&fresh) == 0) ? "ok" : "write failed");
                }
                auth_wipe(&fresh, sizeof fresh);
            }
            auth_wipe(&rec, sizeof rec);
        }
        user_say(u, "Logged in. HELP lists the commands.\r\n");
        to_prompt(u);
        break;
    case AUTH_LOGIN_WAIT:
        console_out_log(CONTROL_FRAME_LOG_WARN, "Telnet user %d: login failed", i);
        user_say(u, "Login incorrect\r\n");
        break;
    case AUTH_LOGIN_REFUSED:
        console_out_log(CONTROL_FRAME_LOG_WARN, "Telnet user %d: login refused", i);
        if (note == AUTH_NOTE_NONE)
        {
            user_say(u, "Login incorrect\r\n");
        }
        s->closing = true;
        break;
    default:
        show_prompt(u);
        break;
    }
}

/** @brief A byte for the gateway's own command line or login. */
static void line_byte(telnet_user *u, uint8_t k)
{
    user_state *s = u->s;
    uint8_t echo[3];
    size_t el = 0;
    uint32_t now = millis();
    if (s->closing || s->listing || (s->ph == PH_LOGIN && s->login.state == AUTH_LOGIN_WAIT))
    {
        return; /* typed while a failed login waits: thrown away */
    }
    int done = term_line_byte(&s->line, k, echo, &el);
    user_write(u, echo, el);
    if (done != 1)
    {
        return;
    }
    char text[TERM_LINE_MAX + 1u];
    memcpy(text, s->line.text, sizeof text);
    term_line_clear(&s->line);
    if (s->ph == PH_LOGIN)
    {
        login_line(u, text, now);
    }
    else if (s->ph == PH_PASSWD)
    {
        passwd_line(u, text, now);
        show_prompt(u);
    }
    else if (s->ph == PH_SETPW)
    {
        setpw_line(u, text);
        show_prompt(u);
    }
    else
    {
        prompt_line(u, text);
    }
    auth_wipe(text, sizeof text);
}

/* ------------------------------------------------------------- telnet */

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
 * @brief One byte typed in a session: CR LF and CR NUL made into CR, data to
 *        the host (echoed if the host asks).
 */
static void session_byte(telnet_user *u, uint8_t c)
{
    int i = user_index(u);
    term_status ts;

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

    /* The lone-ESC rule (libxmsg term_escape.h, as host_tad_telnet.c does):
     * an ESC with nothing after it within 50 ms is the escape key and goes to
     * the host as an ESCA, not as a typed byte; an ESC followed at once by
     * more bytes is a function key and goes through as text, unchanged. */
    uint8_t out[2];
    bool escape = false;
    int n = term_escape_byte(&u->esc, c, millis(), out, &escape);
    if (escape)
    {
        term_escape(i);
    }
    term_get_status(i, &ts);
    for (int k = 0; k < n; k++)
    {
        uint8_t b = out[k];
        (void)term_type(i, &b, 1);
        /* No local echo for a password, a function key's ESC, or the Enter
         * key: the host sends its own CR LF for the CR (tad_asker.h: after
         * "sys" + CR the host sent only CR LF). Echoing CR here gave the user
         * two new lines per Enter (Ronny's log, 2026-11-09 23:37:39: our
         * CR LF at +9 ms, then the host's CR LF CR LF "R@" at +298 ms). */
        if (!ts.echo || b == TERM_ESC || b == '\r')
        {
            continue;
        }
        if (b == 8u || b == 127u)
        {
            user_say(u, "\b \b");
        }
        else
        {
            user_write(u, &b, 1);
        }
    }
}

/** @brief The session part of a user's turn: host text out, the end noticed. */
static void session_pass(telnet_user *u)
{
    uint8_t buf[256];
    term_status ts;
    int i = user_index(u);
    int n;

    /* A held ESC with no byte after it for 50 ms: the escape key. */
    if (term_escape_poll(&u->esc, millis()))
    {
        term_escape(i);
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
    else if (u->session_seen_open || strcmp(ts.state, "failed") == 0)
    {
        /* No [ ] { | } \ in text to a user: a TDV in Norwegian 7-bit shows
         * them as letters (Ronny, 2026-11-10: "[session" came out as
         * "Æsession"). The error number only when there is one. */
        if (ts.error != 0)
        {
            user_printf(u, "\r\n<session %s, error %d>\r\n", ts.state, ts.error);
        }
        else
        {
            user_printf(u, "\r\n<session %s>\r\n", ts.state);
        }
        console_out_log(CONTROL_FRAME_LOG_INFO, "Telnet user %d (%s): session to %s ended", i,
                        u->s->user, u->s->system);
        to_prompt(u);
    }
}

/** @brief Pass bytes both ways for one user. */
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
    if (u->s->closing)
    {
        user_drop(u, "logged out");
        return;
    }
    if (u->s->ph == PH_LOGIN && auth_login_tick(&u->s->login, millis()))
    {
        show_prompt(u);
    }
    n = u->client.available();
    if (n > (int)sizeof buf)
    {
        n = (int)sizeof buf;
    }
    if (n > 0 && u->s->ph == PH_SESSION)
    {
        /* Typing waits until the session is running, so no key is lost. */
        term_get_status(i, &ts);
        if (!(ts.open && strcmp(ts.state, "running") == 0))
        {
            n = 0;
        }
    }
    if (n > 0)
    {
        n = u->client.read(buf, (size_t)n);
        for (int k = 0; k < n; k++)
        {
            if (user_telnet(u, buf[k]))
            {
                continue;
            }
            s_stats.bytes_from_user++;
            if (u->s->ph == PH_SESSION)
            {
                session_byte(u, buf[k]);
            }
            else
            {
                line_byte(u, buf[k]);
            }
        }
    }
    if (u->s->ph == PH_SESSION)
    {
        session_pass(u);
    }
    if (u->s->listing)
    {
        char text[256];
        bool done = false;
        int got = route_list_read(i, text, (int)sizeof text, &done);
        user_write(u, (const uint8_t *)text, (size_t)got);
        if (done)
        {
            u->s->listing = false;
            show_prompt(u);
        }
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
        if (!s_users[i].on && !ts.open && s_users[i].s != NULL)
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
    /* Here, not in setup(): writing the default admin runs PBKDF2 once. */
    s_users_ready = (users_init() == 0) && s_list != NULL && s_report != NULL;
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
