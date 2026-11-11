/**************************************************************************
** FTP_SERVER - FTP into the SINTRAN machines                            **
**                                                                       **
** The commands follow RFC 959 (FTP) and RFC 2428 (EPSV). Replies are    **
** the RFC's codes; the texts are ours.                                  **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "ftp_server.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <fnmatch.h>

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <libxmsg/auth/auth_user.h>

#include "console_out.h"
#include "file_fetch.h"
#include "names.h"
#include "node_task.h"
#include "settings.h"
#include "sintran_accounts.h"
#include "users.h"

/** Longest command line kept; RFC 959 sets no limit, real clients stay far below. */
#define LINE_MAX_CHARS 256

/** Wrong passwords before the connection is closed (as the telnet login, auth_login.h). */
#define BAD_LOGINS_MAX 3

/** Room for one listing: every machine name in one line each. */
#define LISTING_SIZE (NAMES_MAX * 80)

/** Where the user stands. */
typedef enum
{
    LEVEL_ROOT = 0,  /**< "/": the machines */
    LEVEL_MACHINE,   /**< "/D100": the SINTRAN user's folder */
    LEVEL_USER       /**< "/D100/RONNY": the files */
} level;

/** A place in the folder tree. */
typedef struct
{
    level lv;
    char machine[sizeof(((NamesEntry *)0)->name)];
    uint16_t system;
    char user[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
} place;

/** How the next data connection is made. */
typedef enum
{
    MODE_NONE = 0,   /**< neither PASV/EPSV nor PORT/EPRT since the last transfer */
    MODE_PASSIVE,    /**< the client connects to FTP_SERVER_DATA_PORT */
    MODE_ACTIVE      /**< we connect to the address PORT/EPRT gave */
} data_mode;

/** What the data connection is doing. */
typedef enum
{
    DATA_IDLE = 0,
    DATA_WAIT_CLIENT, /**< a transfer is ready; waiting for the data connection */
    DATA_SENDING      /**< connected; sending */
} data_state;

/** What is sent on it. */
typedef enum
{
    XFER_LISTING = 0, /**< s_listing: the machine and user folders */
    XFER_FILE,        /**< a SINTRAN file, as file_fetch.h brings it in */
    XFER_DIR          /**< a SINTRAN user's files, as file_fetch.h lists them */
} xfer_kind;

/** A SIZE or MDTM waiting for SINTRAN's answer (the control connection is not read meanwhile). */
typedef enum
{
    STAT_NONE = 0,
    STAT_SIZE,
    STAT_MDTM
} stat_wait;

/** Listing entries formatted per pass while sending a user's files. */
#define DIR_BATCH 8u

/** Bytes handed to the socket per pass while sending a file. */
#define FILE_CHUNK 1024u

/** How long we wait to connect to an active-mode client. The telnet task waits with it
 *  (Ronny accepted that, 2026-11-11, when choosing to have active mode). */
#define ACTIVE_CONNECT_MS 3000

static WiFiServer s_ctrl_server(FTP_SERVER_PORT, 1);
static WiFiServer s_data_server(FTP_SERVER_DATA_PORT, 1);
static bool s_running = false;

static WiFiClient s_ctrl;
static bool s_on = false;             /* a user is connected */
static bool s_logged_in = false;
static int s_bad_logins = 0;
static char s_user_name[AUTH_NAME_MAX + 1u];
static char s_line[LINE_MAX_CHARS + 1];
static size_t s_line_len = 0;
static bool s_line_too_long = false;
static uint32_t s_last_ms = 0;
static place s_cwd;
static data_mode s_mode = MODE_NONE;
static IPAddress s_active_ip;         /* MODE_ACTIVE: where to connect */
static uint16_t s_active_port = 0;
static data_state s_data = DATA_IDLE;
static xfer_kind s_xfer = XFER_LISTING;
static WiFiClient s_dconn;            /* the data connection while DATA_SENDING */
static uint32_t s_data_deadline = 0;
static size_t s_listing_len = 0;
static uint32_t s_file_sent = 0;      /* XFER_FILE: bytes written to the data connection */
static char s_file_name[80];          /* XFER_FILE, SIZE, MDTM: "CHATLG2:SYMB" */
static uint32_t s_dir_sent = 0;       /* XFER_DIR: entries written to the data connection */
static bool s_dir_names_only = false; /* XFER_DIR: NLST (names) rather than LIST */
static char s_dir_owner[SINTRAN_ACCOUNT_FIELD_MAX + 1u]; /* XFER_DIR: the owner column */
static stat_wait s_stat = STAT_NONE;
static char s_dir_glob[64];           /* XFER_DIR: "CHAT*" from "NLST CHAT*"; "" = every file */
static FaFileEntry *s_dir = NULL;     /* DIR_BATCH, PSRAM */

static char *s_listing = NULL;        /* LISTING_SIZE, PSRAM */
static uint8_t *s_chunk = NULL;       /* FILE_CHUNK, PSRAM: keeps the telnet task's stack small */
static NamesEntry *s_names = NULL;    /* NAMES_MAX, PSRAM */
static ftp_server_stats s_stats;

int ftp_server_init(void)
{
    if (s_listing == NULL)
    {
        s_listing = (char *)heap_caps_calloc(1, LISTING_SIZE, MALLOC_CAP_SPIRAM);
    }
    if (s_names == NULL)
    {
        s_names = (NamesEntry *)heap_caps_calloc(NAMES_MAX, sizeof(NamesEntry), MALLOC_CAP_SPIRAM);
    }
    if (s_chunk == NULL)
    {
        s_chunk = (uint8_t *)heap_caps_malloc(FILE_CHUNK, MALLOC_CAP_SPIRAM);
    }
    if (s_dir == NULL)
    {
        s_dir = (FaFileEntry *)heap_caps_calloc(DIR_BATCH, sizeof(FaFileEntry), MALLOC_CAP_SPIRAM);
    }
    return (s_listing != NULL && s_names != NULL && s_chunk != NULL && s_dir != NULL
            && file_fetch_init() == 0) ? 0 : -1;
}

void ftp_server_get_stats(ftp_server_stats *out)
{
    *out = s_stats;
    out->user_on = s_on;
}

/** @brief Send one reply line, CR LF added. */
static void reply(const char *text)
{
    if (s_on)
    {
        (void)s_ctrl.print(text);
        (void)s_ctrl.print("\r\n");
    }
}

/** @brief Send a formatted reply line. */
static void replyf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void replyf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    reply(buf);
}

/**
 * @brief Close every data connection waiting on the data port. A client opens its data
 *        connection BEFORE it sends LIST; when the LIST is refused, or the session ends,
 *        that connection stays queued, and the next listing would pick up the dead one
 *        while the client's new connection waits behind it (seen 2026-11-11 with curl:
 *        every listing after a refused one timed out).
 */
static void data_flush(void)
{
    if (!s_running)
    {
        return;
    }
    for (int i = 0; i < 4; i++)
    {
        WiFiClient c = s_data_server.accept();
        if (!c)
        {
            break;
        }
        c.stop();
    }
}

/** @brief End any transfer: close the data connection, let go of a file, forget the mode. */
static void data_end(void)
{
    if (s_data == DATA_SENDING)
    {
        s_dconn.stop();
    }
    if ((s_xfer == XFER_FILE || s_xfer == XFER_DIR) && s_data != DATA_IDLE)
    {
        file_fetch_release();
    }
    s_data = DATA_IDLE;
    s_mode = MODE_NONE;
    s_file_sent = 0;
    s_dir_sent = 0;
    data_flush();
}

/** @brief End the session: close both connections, forget the login. */
static void session_end(const char *why)
{
    if (s_on)
    {
        console_out_log(CONTROL_FRAME_LOG_INFO, "FTP: session ends (%s)", why);
        s_ctrl.stop();
    }
    s_on = false;
    s_logged_in = false;
    s_bad_logins = 0;
    memset(s_user_name, 0, sizeof s_user_name);
    s_line_len = 0;
    s_line_too_long = false;
    data_end();
    if (s_stat != STAT_NONE)
    {
        s_stat = STAT_NONE;
        file_fetch_release();
    }
    memset(&s_cwd, 0, sizeof s_cwd);
}

void ftp_server_start(void)
{
    if (!s_running)
    {
        s_ctrl_server.begin();
        s_ctrl_server.setNoDelay(true);
        s_data_server.begin();
        s_running = true;
    }
}

void ftp_server_stop(void)
{
    session_end("wifi lost");
    if (s_running)
    {
        s_ctrl_server.end();
        s_data_server.end();
        s_running = false;
    }
}

/** @brief This gateway's own node number (0 before the link is started). */
static uint16_t own_node(void)
{
    node_status st;
    node_task_get_status(&st);
    return st.own_node;
}

/**
 * @brief Find a machine folder by name: the host's name table first, then a
 *        hand-set ALIAS (settings.h), the same order CONNECT uses (gw_console.cpp).
 * @param[out] out  The machine's name as listed, and its number.
 * @return 0, or -1 when no machine of that name is known, or it is this gateway.
 */
static int machine_find(const char *name, place *out)
{
    /* A folder name longer than any machine name cannot be one; refusing it here also
     * means the copies below never cut a name short. */
    if (strlen(name) >= sizeof out->machine)
    {
        return -1;
    }
    int n = names_copy(s_names, NAMES_MAX);
    uint16_t me = own_node();
    for (int i = 0; i < n; i++)
    {
        if (strcasecmp(s_names[i].name, name) == 0 && s_names[i].system != me)
        {
            (void)snprintf(out->machine, sizeof out->machine, "%s", s_names[i].name);
            out->system = s_names[i].system;
            return 0;
        }
    }
    uint16_t number = 0;
    if (settings_alias(name, &number) == 0 && number != me)
    {
        (void)snprintf(out->machine, sizeof out->machine, "%.*s", (int)(sizeof out->machine - 1u), name);
        out->system = number;
        return 0;
    }
    return -1;
}

/**
 * @brief Work out where a CWD or LIST path leads, from where the user stands.
 * @param[in]  path  Absolute ("/D100/RONNY") or relative ("D100", "..", "../D102").
 * @param[out] out   The place.
 * @param[out] why   Reply text when it fails.
 * @return 0, or -1 when the folder does not exist.
 */
static int resolve(const char *path, place *out, const char **why)
{
    char user[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
    char part[LINE_MAX_CHARS + 1];
    place p = (path[0] == '/') ? place{} : s_cwd;
    const char *at = path;
    *why = "550 No such folder.";
    if (path[0] == '/')
    {
        p.lv = LEVEL_ROOT;
    }
    while (*at != '\0')
    {
        size_t n = 0;
        while (*at == '/')
        {
            at++;
        }
        while (at[n] != '\0' && at[n] != '/' && n < sizeof part - 1u)
        {
            part[n] = at[n];
            n++;
        }
        part[n] = '\0';
        at += n;
        if (n == 0 || strcmp(part, ".") == 0)
        {
            continue;
        }
        if (strcmp(part, "..") == 0)
        {
            if (p.lv != LEVEL_ROOT)
            {
                p.lv = (level)(p.lv - 1);
            }
            continue;
        }
        if (p.lv == LEVEL_ROOT)
        {
            if (machine_find(part, &p) != 0)
            {
                return -1;
            }
            p.lv = LEVEL_MACHINE;
        }
        else if (p.lv == LEVEL_MACHINE)
        {
            if (sintran_account_get(s_user_name, user, NULL) != 0)
            {
                *why = "550 No SINTRAN user is set for you; an admin sets one with SINTRAN on the XMSGGW> command line.";
                return -1;
            }
            if (strcasecmp(part, user) != 0)
            {
                return -1;
            }
            (void)snprintf(p.user, sizeof p.user, "%s", user);
            p.lv = LEVEL_USER;
        }
        else
        {
            *why = "550 Folders inside a SINTRAN user are not built yet (needs the COSMOS file client).";
            return -1;
        }
    }
    *out = p;
    return 0;
}

/** @brief The place as a path: "/", "/D100", "/D100/RONNY". */
static void place_path(const place *p, char *out, size_t cap)
{
    if (p->lv == LEVEL_ROOT)
    {
        (void)snprintf(out, cap, "/");
    }
    else if (p->lv == LEVEL_MACHINE)
    {
        (void)snprintf(out, cap, "/%s", p->machine);
    }
    else
    {
        (void)snprintf(out, cap, "/%s/%s", p->machine, p->user);
    }
}

/** @brief Add one folder line to the listing: "ls -l" style for LIST, the bare name for NLST. */
static void listing_add(const char *name, bool names_only)
{
    int len;
    if (names_only)
    {
        len = snprintf(&s_listing[s_listing_len], LISTING_SIZE - s_listing_len, "%s\r\n", name);
    }
    else
    {
        /* The gateway has no clock source for folder dates; the epoch date says "unknown". */
        len = snprintf(&s_listing[s_listing_len], LISTING_SIZE - s_listing_len,
                       "drwxr-xr-x   1 xmsggw   xmsggw          0 Jan  1  1970 %s\r\n", name);
    }
    if (len > 0 && (size_t)len < LISTING_SIZE - s_listing_len)
    {
        s_listing_len += (size_t)len;
    }
}

/**
 * @brief Build the listing of a place.
 * @return 0, or -1 with *why set when it cannot be listed.
 */
static int listing_build(const place *p, bool names_only, const char **why)
{
    char user[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
    s_listing_len = 0;
    s_listing[0] = '\0';
    if (p->lv == LEVEL_ROOT)
    {
        int n = names_copy(s_names, NAMES_MAX);
        uint16_t me = own_node();
        for (int i = 0; i < n; i++)
        {
            bool seen = false;
            for (int j = 0; j < i; j++)
            {
                seen = seen || (s_names[j].system == s_names[i].system);
            }
            if (!seen && s_names[i].system != me)
            {
                listing_add(s_names[i].name, names_only);
            }
        }
        return 0;
    }
    if (p->lv == LEVEL_MACHINE)
    {
        if (sintran_account_get(s_user_name, user, NULL) == 0)
        {
            listing_add(user, names_only);
        }
        return 0;
    }
    return 1;   /* LEVEL_USER: the files come from SINTRAN (dir_start) */
}

/**
 * @brief The FTP name of a SINTRAN file: NAME.TYPE (Ronny, 2026-11-11). A file with no type
 *        is "NAME." so that RETR of that name asks SINTRAN for exactly "NAME:".
 */
static void dir_ftp_name(const FaFileEntry *e, char *out, size_t cap)
{
    (void)snprintf(out, cap, "%s.%s", e->name, e->type);
}

/** @brief Month abbreviations for "ls -l" dates. */
static const char *const MONTHS[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/**
 * @brief One "ls -l" line for a SINTRAN file. The size is the byte count SINTRAN keeps;
 *        an entry with none (a device file) shows 0. The date is the last write, else the
 *        creation, as SINTRAN's local time (it has no time zone); none shows 1970.
 *        Every version of a file is its own entry and is listed (the version number is
 *        not in the entry; libxmsg, 2026-11-11), so a name can appear more than once.
 */
static int dir_line(const FaFileEntry *e, char *out, size_t cap)
{
    char name[32];
    FaNdDate d;
    dir_ftp_name(e, name, sizeof name);
    if (s_dir_names_only)
    {
        return snprintf(out, cap, "%s\r\n", name);
    }
    bool dated = fa_nd_date_split(e->last_write, &d) || fa_nd_date_split(e->created, &d);
    if (!dated || d.month < 1u || d.month > 12u)
    {
        return snprintf(out, cap, "-rw-r--r--   1 %-8s %-8s %10lu Jan  1  1970 %s\r\n", s_dir_owner,
                        s_dir_owner, (unsigned long)(e->has_bytes ? e->bytes : 0u), name);
    }
    /* Always the year form: "ls -l" shows hh:mm only for the last six months, and SINTRAN
     * dates are decades old, so the year form is what a client expects to parse. */
    return snprintf(out, cap, "-rw-r--r--   1 %-8s %-8s %10lu %s %2u %5u %s\r\n", s_dir_owner,
                    s_dir_owner, (unsigned long)(e->has_bytes ? e->bytes : 0u), MONTHS[d.month - 1u],
                    (unsigned)d.day, (unsigned)d.year, name);
}

/**
 * @brief Work out which SINTRAN file a RETR / SIZE / MDTM argument names: its folder must be
 *        /<machine>/<user>; NAME.TYPE becomes NAME:TYPE in s_file_name.
 * @return NULL, or the reply to send when it names no file.
 */
static const char *file_target(const char *args, place *p)
{
    char dir[LINE_MAX_CHARS + 1];
    const char *why = NULL;
    const char *slash = strrchr(args, '/');
    const char *file = (slash != NULL) ? slash + 1 : args;
    size_t dir_len = (slash != NULL) ? (size_t)(slash - args) : 0u;

    if (*file == '\0' || dir_len >= sizeof dir)
    {
        return "501 Give a file name.";
    }
    if (slash == NULL)
    {
        *p = s_cwd;   /* a bare file name: in the current folder */
    }
    else
    {
        memcpy(dir, args, dir_len);
        dir[dir_len] = '\0';
        if (slash == args)
        {
            (void)snprintf(dir, sizeof dir, "/");
        }
        if (resolve(dir, p, &why) != 0)
        {
            return why;
        }
    }
    if (p->lv != LEVEL_USER)
    {
        return "550 Files are inside /<machine>/<SINTRAN user>.";
    }
    /* NAME.TYPE -> NAME:TYPE at the last dot; a name already holding ':' is kept. */
    (void)snprintf(s_file_name, sizeof s_file_name, "%s", file);
    if (strchr(s_file_name, ':') == NULL)
    {
        char *dot = strrchr(s_file_name, '.');
        if (dot != NULL)
        {
            *dot = ':';
        }
    }
    return NULL;
}

/**
 * @brief Ask SINTRAN for the files of the place, or one file's statistics (filter).
 * @return 0, or -1 when no job could start (no SINTRAN account, or one already running).
 */
static int dir_start(const place *p, const char *filter)
{
    char password[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
    int rc = -1;
    if (sintran_account_get(s_user_name, s_dir_owner, password) == 0)
    {
        rc = file_fetch_list(p->system, s_dir_owner, password, filter);
    }
    memset(password, 0, sizeof password);
    return rc;
}

/**
 * @brief Begin a transfer: tell the client, then wait for (or make) the data connection.
 * @return false when no data connection was set up (no PASV/EPSV/PORT/EPRT).
 */
static bool transfer_begin(xfer_kind kind, const char *what)
{
    if (s_mode == MODE_NONE)
    {
        reply("425 Use PASV, EPSV, PORT or EPRT first.");
        return false;
    }
    s_xfer = kind;
    s_file_sent = 0;
    replyf("150 Opening the data connection for %s.", what);
    s_data = DATA_WAIT_CLIENT;
    s_data_deadline = millis() + FTP_SERVER_DATA_WAIT_MS;
    return true;
}

/** @brief LIST / NLST: build the listing, then send it on the data connection. */
static void cmd_list(const char *args, bool names_only)
{
    place p = s_cwd;
    const char *why = NULL;
    while (*args == '-')   /* "LIST -la": options some clients send; nothing to do with them */
    {
        while (*args != '\0' && *args != ' ')
        {
            args++;
        }
        while (*args == ' ')
        {
            args++;
        }
    }
    /* "NLST CHAT*" (tnftp's nlist, mget): a name pattern, not a folder. It is matched
     * here against the FTP names of the current folder's files. */
    s_dir_glob[0] = '\0';
    if (strpbrk(args, "*?") != NULL && strchr(args, '/') == NULL && strlen(args) < sizeof s_dir_glob)
    {
        (void)snprintf(s_dir_glob, sizeof s_dir_glob, "%s", args);
        args = "";
    }
    int built = -1;
    if ((*args != '\0' && resolve(args, &p, &why) != 0) || (built = listing_build(&p, names_only, &why)) < 0)
    {
        data_end();
        reply(why);
        return;
    }
    if (built == 0)
    {
        (void)transfer_begin(XFER_LISTING, "the listing");
        return;
    }
    /* A user's folder: the files come from SINTRAN while they are sent. */
    if (dir_start(&p, "") != 0)
    {
        data_end();
        reply("450 The gateway cannot ask SINTRAN for the files now; try again.");
        return;
    }
    s_dir_names_only = names_only;
    if (!transfer_begin(XFER_DIR, "the file list"))
    {
        file_fetch_release();
    }
}

/** @brief SIZE / MDTM: ask SINTRAN for that one file (FILE-STATISTICS); stat_pass replies. */
static void cmd_stat(const char *args, stat_wait which)
{
    place p;
    const char *why = file_target(args, &p);
    if (why != NULL)
    {
        reply(why);
        return;
    }
    if (dir_start(&p, s_file_name) != 0)
    {
        reply("450 The gateway cannot ask SINTRAN now; try again.");
        return;
    }
    s_stat = which;
}

/** @brief A SIZE / MDTM under way: reply once SINTRAN has answered. */
static void stat_pass(void)
{
    file_fetch_status st;
    file_fetch_get(&st);
    if (st.state != FILE_FETCH_DONE && st.state != FILE_FETCH_FAILED)
    {
        return;
    }
    stat_wait which = s_stat;
    s_stat = STAT_NONE;
    if (st.state == FILE_FETCH_FAILED)
    {
        char why[96];
        file_fetch_reason(&st, why, sizeof why);
        file_fetch_release();
        replyf("550 %s: %s.", s_file_name, why);
        return;
    }
    if (st.entries == 0u || file_fetch_entries(0, s_dir, 1) != 1u)
    {
        file_fetch_release();
        replyf("550 %s: no such file.", s_file_name);
        return;
    }
    file_fetch_release();
    const FaFileEntry *e = &s_dir[0];
    if (which == STAT_SIZE)
    {
        if (!e->has_bytes)
        {
            replyf("550 %s: SINTRAN keeps no byte count for it.", s_file_name);
            return;
        }
        replyf("213 %lu", (unsigned long)e->bytes);
        return;
    }
    FaNdDate d;
    if (!fa_nd_date_split(e->last_write, &d) && !fa_nd_date_split(e->created, &d))
    {
        replyf("550 %s: SINTRAN has no date for it.", s_file_name);
        return;
    }
    /* RFC 3659 asks for UTC; SINTRAN keeps local time with no zone, so this is its local time. */
    replyf("213 %04u%02u%02u%02u%02u%02u", (unsigned)d.year, (unsigned)d.month, (unsigned)d.day,
           (unsigned)d.hour, (unsigned)d.minute, (unsigned)d.second);
}

/**
 * @brief RETR: fetch a SINTRAN file and send it. The FTP name NAME.TYPE is SINTRAN's
 *        NAME:TYPE (Ronny, 2026-11-11: Windows does not allow ':' in a file name).
 *        The bytes go out as SINTRAN holds them: no line-ending or character change in
 *        either TYPE A or TYPE I.
 */
static void cmd_retr(const char *args)
{
    char user[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
    char password[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
    place p;
    const char *why = file_target(args, &p);
    const char *slash = strrchr(args, '/');
    const char *file = (slash != NULL) ? slash + 1 : args;

    if (why != NULL)
    {
        data_end();
        reply(why);
        return;
    }
    if (sintran_account_get(s_user_name, user, password) != 0
        || file_fetch_start(p.system, user, password, s_file_name) != 0)
    {
        memset(password, 0, sizeof password);
        data_end();
        reply("450 The gateway cannot start a file transfer now; try again.");
        return;
    }
    memset(password, 0, sizeof password);
    if (!transfer_begin(XFER_FILE, file))
    {
        file_fetch_release();
    }
}

/**
 * @brief Write the next entries of a user's files; end the listing when SINTRAN has sent
 *        them all, or failed.
 */
static void dir_send(void)
{
    file_fetch_status st;
    file_fetch_get(&st);
    size_t n = file_fetch_entries(s_dir_sent, s_dir, DIR_BATCH);
    for (size_t i = 0; i < n; i++)
    {
        if (s_dir_glob[0] != '\0')
        {
            char name[32];
            dir_ftp_name(&s_dir[i], name, sizeof name);
            if (fnmatch(s_dir_glob, name, FNM_CASEFOLD) != 0)
            {
                s_dir_sent++;
                continue;
            }
        }
        int len = dir_line(&s_dir[i], (char *)s_chunk, FILE_CHUNK);
        if (len <= 0 || (size_t)len >= FILE_CHUNK)
        {
            continue;
        }
        if (s_dconn.write(s_chunk, (size_t)len) != (size_t)len)
        {
            data_end();
            reply("426 The data connection broke during the listing.");
            return;
        }
        s_dir_sent++;
    }
    if (n > 0u || s_dir_sent < st.entries)
    {
        return;
    }
    if (st.state == FILE_FETCH_DONE)
    {
        console_out_log(CONTROL_FRAME_LOG_INFO, "FTP: file list sent, %lu files%s",
                        (unsigned long)s_dir_sent, st.list_full ? " (more not kept)" : "");
        data_end();
        s_stats.listings++;
        reply(st.list_full ? "226 File list sent; it was cut at the gateway's limit."
                           : "226 File list sent.");
    }
    else if (st.state == FILE_FETCH_FAILED)
    {
        char why[96];
        file_fetch_reason(&st, why, sizeof why);
        data_end();
        replyf("451 The file list failed: %s.", why);
    }
}

/** @brief Write the next file bytes; end the transfer when the file is all sent or failed. */
static void file_send(void)
{
    file_fetch_status st;
    file_fetch_get(&st);
    if (s_file_sent < st.have)
    {
        size_t n = file_fetch_copy(s_file_sent, s_chunk, FILE_CHUNK);
        size_t w = (n > 0u) ? s_dconn.write(s_chunk, n) : 0u;
        s_file_sent += (uint32_t)w;
        if (n > 0u && w == 0u && !s_dconn.connected())
        {
            data_end();
            reply("426 The data connection broke during the transfer.");
        }
        return;
    }
    if (st.state == FILE_FETCH_DONE)
    {
        console_out_log(CONTROL_FRAME_LOG_INFO, "FTP: %s sent, %lu bytes", s_file_name,
                        (unsigned long)s_file_sent);
        data_end();
        s_stats.files++;
        reply("226 File sent.");
    }
    else if (st.state == FILE_FETCH_FAILED)
    {
        char why[96];
        file_fetch_reason(&st, why, sizeof why);
        data_end();
        replyf("550 %s: %s.", s_file_name, why);
    }
}

/** @brief Make the active-mode connection. The telnet task waits up to ACTIVE_CONNECT_MS. */
static bool active_connect(void)
{
    WiFiClient c;
    if (c.connect(s_active_ip, s_active_port, ACTIVE_CONNECT_MS) == 0)
    {
        return false;
    }
    s_dconn = c;
    return true;
}

/** @brief The data connection: get it, then send the listing or the file. */
static void data_pass(uint32_t now)
{
    if (s_data == DATA_WAIT_CLIENT)
    {
        bool up = false;
        if (s_mode == MODE_PASSIVE)
        {
            WiFiClient c = s_data_server.accept();
            if (c)
            {
                s_dconn = c;
                up = true;
            }
        }
        else if (!active_connect())
        {
            data_end();
            reply("425 Could not connect to the address PORT/EPRT gave.");
            return;
        }
        else
        {
            up = true;
        }
        if (up)
        {
            s_dconn.setNoDelay(true);
            s_data = DATA_SENDING;
        }
        else if ((int32_t)(now - s_data_deadline) >= 0)
        {
            data_end();
            reply("425 The data connection was not opened in time.");
            return;
        }
    }
    if (s_data != DATA_SENDING)
    {
        return;
    }
    if (s_xfer == XFER_FILE)
    {
        file_send();
        return;
    }
    if (s_xfer == XFER_DIR)
    {
        dir_send();
        return;
    }
    size_t sent = (s_listing_len > 0u) ? s_dconn.write((const uint8_t *)s_listing, s_listing_len) : 0u;
    data_end();
    s_stats.listings++;
    reply((sent == s_listing_len) ? "226 Listing sent." : "426 The data connection broke during the listing.");
}

/**
 * @brief PORT h1,h2,h3,h4,p1,p2 (RFC 959) and EPRT |1|address|port| (RFC 2428).
 *        Only the address the client itself came from is accepted: anything else
 *        would let a client make the gateway connect to a third machine ("FTP bounce").
 */
static void cmd_port(const char *args, bool extended)
{
    unsigned h1 = 0;
    unsigned h2 = 0;
    unsigned h3 = 0;
    unsigned h4 = 0;
    unsigned port = 0;
    bool ok;
    if (extended)
    {
        unsigned proto = 0;
        char d = args[0];
        char fmt[40];
        (void)snprintf(fmt, sizeof fmt, "%c%%u%c%%u.%%u.%%u.%%u%c%%u%c", d, d, d, d);
        ok = (d != '\0') && sscanf(args, fmt, &proto, &h1, &h2, &h3, &h4, &port) == 6 && proto == 1u;
    }
    else
    {
        unsigned p1 = 0;
        unsigned p2 = 0;
        ok = sscanf(args, "%u,%u,%u,%u,%u,%u", &h1, &h2, &h3, &h4, &p1, &p2) == 6 && p1 < 256u
             && p2 < 256u;
        port = (p1 << 8u) | p2;
    }
    if (!ok || h1 > 255u || h2 > 255u || h3 > 255u || h4 > 255u || port == 0u || port > 65535u)
    {
        reply(extended ? "522 Only IPv4 (protocol 1) in EPRT." : "501 Bad PORT address.");
        return;
    }
    IPAddress ip((uint8_t)h1, (uint8_t)h2, (uint8_t)h3, (uint8_t)h4);
    if (ip != s_ctrl.remoteIP())
    {
        reply("504 The data address must be the client's own address.");
        return;
    }
    data_flush();
    s_mode = MODE_ACTIVE;
    s_active_ip = ip;
    s_active_port = (uint16_t)port;
    reply("200 Active mode set.");
}

/** @brief USER and PASS. */
static void cmd_pass(const char *password)
{
    AuthUser u;
    if (s_user_name[0] == '\0')
    {
        reply("503 Send USER first.");
        return;
    }
    /* PBKDF2: about 0.46 s (users.h); the telnet task waits meanwhile. */
    bool ok = auth_user_verify_get(users_store(), s_user_name, password, &u);
    if (ok && u.must_change)
    {
        auth_wipe(&u, sizeof u);
        reply("530 Your password must be changed first: log in over telnet once.");
        session_end("password must be changed");
        return;
    }
    auth_wipe(&u, sizeof u);
    if (!ok)
    {
        s_stats.bad_logins++;
        console_out_log(CONTROL_FRAME_LOG_WARN, "FTP: bad login for '%s'", s_user_name);
        memset(s_user_name, 0, sizeof s_user_name);
        if (++s_bad_logins >= BAD_LOGINS_MAX)
        {
            reply("530 Login incorrect. Too many tries.");
            session_end("too many bad logins");
            return;
        }
        reply("530 Login incorrect.");
        return;
    }
    s_logged_in = true;
    s_stats.logins++;
    memset(&s_cwd, 0, sizeof s_cwd);
    console_out_log(CONTROL_FRAME_LOG_INFO, "FTP: '%s' logged in", s_user_name);
    reply("230 Logged in. The folders are the SINTRAN machines.");
}

/** @brief Whether the line starts with this command word; *args is what follows. */
static bool is_cmd(const char *line, const char *word, const char **args)
{
    size_t n = strlen(word);
    if (strncasecmp(line, word, n) != 0 || (line[n] != '\0' && line[n] != ' '))
    {
        return false;
    }
    line += n;
    while (*line == ' ')
    {
        line++;
    }
    *args = line;
    return true;
}

/** @brief One command line from the client. */
static void command(const char *line)
{
    const char *a = NULL;
    char path[80];
    place p;
    const char *why = NULL;

    if (is_cmd(line, "QUIT", &a))
    {
        reply("221 Goodbye.");
        session_end("QUIT");
        return;
    }
    if (is_cmd(line, "NOOP", &a))
    {
        reply("200 OK.");
        return;
    }
    if (is_cmd(line, "USER", &a))
    {
        s_logged_in = false;
        if (!auth_name_is_valid(a))
        {
            reply("530 Not a valid user name.");
            return;
        }
        (void)snprintf(s_user_name, sizeof s_user_name, "%s", a);
        reply("331 Password, please.");
        return;
    }
    if (is_cmd(line, "PASS", &a))
    {
        cmd_pass(a);
        return;
    }
    if (is_cmd(line, "FEAT", &a))
    {
        reply("211-Features:");
        reply(" EPSV");
        reply(" MDTM");
        reply(" PASV");
        reply(" SIZE");
        reply(" UTF8");
        reply("211 End");
        return;
    }
    if (is_cmd(line, "OPTS", &a))
    {
        reply("200 OK.");   /* "OPTS UTF8 ON": names are plain ASCII either way */
        return;
    }
    if (is_cmd(line, "SYST", &a))
    {
        reply("215 UNIX Type: L8");   /* the listing format clients know best */
        return;
    }
    if (!s_logged_in)
    {
        reply("530 Log in first.");
        return;
    }
    if (is_cmd(line, "PWD", &a) || is_cmd(line, "XPWD", &a))
    {
        place_path(&s_cwd, path, sizeof path);
        replyf("257 \"%s\" is the current folder.", path);
        return;
    }
    if (is_cmd(line, "CWD", &a) || is_cmd(line, "XCWD", &a) || is_cmd(line, "CDUP", &a)
        || is_cmd(line, "XCUP", &a))
    {
        const char *target = (strncasecmp(line, "CDUP", 4) == 0 || strncasecmp(line, "XCUP", 4) == 0) ? ".." : a;
        if (resolve(target, &p, &why) != 0)
        {
            reply(why);
            return;
        }
        s_cwd = p;
        place_path(&s_cwd, path, sizeof path);
        replyf("250 Now in \"%s\".", path);
        return;
    }
    if (is_cmd(line, "TYPE", &a))
    {
        reply("200 Type set.");   /* nothing is transferred yet that the type would change */
        return;
    }
    if (is_cmd(line, "MODE", &a) || is_cmd(line, "STRU", &a))
    {
        reply("200 OK.");
        return;
    }
    if (is_cmd(line, "PASV", &a))
    {
        IPAddress ip = WiFi.localIP();
        data_flush();
        s_mode = MODE_PASSIVE;
        replyf("227 Entering Passive Mode (%u,%u,%u,%u,%u,%u).", (unsigned)ip[0], (unsigned)ip[1],
               (unsigned)ip[2], (unsigned)ip[3], (unsigned)(FTP_SERVER_DATA_PORT >> 8u),
               (unsigned)(FTP_SERVER_DATA_PORT & 0xFFu));
        return;
    }
    if (is_cmd(line, "EPSV", &a))
    {
        data_flush();
        s_mode = MODE_PASSIVE;
        replyf("229 Entering Extended Passive Mode (|||%u|).", (unsigned)FTP_SERVER_DATA_PORT);
        return;
    }
    if (is_cmd(line, "PORT", &a))
    {
        cmd_port(a, false);
        return;
    }
    if (is_cmd(line, "EPRT", &a))
    {
        cmd_port(a, true);
        return;
    }
    if (is_cmd(line, "RETR", &a))
    {
        cmd_retr(a);
        return;
    }
    if (is_cmd(line, "LIST", &a))
    {
        cmd_list(a, false);
        return;
    }
    if (is_cmd(line, "NLST", &a))
    {
        cmd_list(a, true);
        return;
    }
    if (is_cmd(line, "SIZE", &a))
    {
        cmd_stat(a, STAT_SIZE);
        return;
    }
    if (is_cmd(line, "MDTM", &a))
    {
        cmd_stat(a, STAT_MDTM);
        return;
    }
    if (is_cmd(line, "STOR", &a) || is_cmd(line, "DELE", &a) || is_cmd(line, "RNFR", &a)
        || is_cmd(line, "RNTO", &a) || is_cmd(line, "APPE", &a))
    {
        reply("550 Writing, deleting and renaming SINTRAN files are not built yet.");
        return;
    }
    if (is_cmd(line, "MKD", &a) || is_cmd(line, "XMKD", &a) || is_cmd(line, "RMD", &a)
        || is_cmd(line, "XRMD", &a))
    {
        reply("550 The folders are the machines and their SINTRAN users; they cannot be made or removed.");
        return;
    }
    reply("502 Command not implemented.");
}

/** @brief Read what the client sent; run each complete line. */
static void ctrl_pass(uint32_t now)
{
    if (!s_ctrl.connected())
    {
        session_end("client left");
        return;
    }
    /* A SIZE / MDTM is waiting for SINTRAN: the next command stays unread until it is
     * answered (FTP replies come in order). */
    while (s_on && s_stat == STAT_NONE && s_ctrl.available() > 0)
    {
        int c = s_ctrl.read();
        if (c < 0)
        {
            break;
        }
        s_last_ms = now;
        if (c == '\n')
        {
            if (s_line_len > 0u && s_line[s_line_len - 1u] == '\r')
            {
                s_line_len--;
            }
            s_line[s_line_len] = '\0';
            if (s_line_too_long)
            {
                reply("500 Line too long.");
            }
            else if (s_data != DATA_IDLE && strncasecmp(s_line, "ABOR", 4) == 0)
            {
                bool sending = (s_data == DATA_SENDING);
                data_end();
                if (sending)
                {
                    reply("426 Transfer aborted.");   /* RFC 959: 426 for the transfer, then 226 */
                }
                reply("226 Aborted.");
            }
            else if (s_data != DATA_IDLE && strncasecmp(s_line, "QUIT", 4) != 0
                     && strncasecmp(s_line, "NOOP", 4) != 0)
            {
                reply("425 A transfer is still running; ABOR stops it.");
            }
            else
            {
                command(s_line);
            }
            memset(s_line, 0, sizeof s_line);   /* PASS lines carry a password */
            s_line_len = 0;
            s_line_too_long = false;
        }
        else if (s_line_len < LINE_MAX_CHARS)
        {
            s_line[s_line_len++] = (char)c;
        }
        else
        {
            s_line_too_long = true;
        }
    }
    if (s_on && (uint32_t)(now - s_last_ms) >= FTP_SERVER_IDLE_MS)
    {
        reply("421 Idle too long; closing.");
        session_end("idle");
    }
}

void ftp_server_pass(uint32_t now)
{
    if (!s_running || s_listing == NULL)
    {
        return;
    }
    WiFiClient incoming = s_ctrl_server.accept();
    if (incoming)
    {
        if (s_on)
        {
            /* One FTP user at a time (Ronny, 2026-11-11). */
            s_stats.refused++;
            (void)incoming.print("421 Another FTP user is on; one at a time. Try again later.\r\n");
            incoming.stop();
        }
        else
        {
            session_end("new session");
            s_ctrl = incoming;
            s_ctrl.setNoDelay(true);
            s_on = true;
            s_last_ms = now;
            s_stats.sessions++;
            console_out_log(CONTROL_FRAME_LOG_INFO, "FTP: connection from %s",
                            s_ctrl.remoteIP().toString().c_str());
            reply("220 XMSGGW FTP: the folders are the SINTRAN machines.");
        }
    }
    if (s_on)
    {
        if (s_stat != STAT_NONE)
        {
            stat_pass();
        }
        ctrl_pass(now);
        data_pass(now);
    }
}
