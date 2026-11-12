/**************************************************************************
** FILE_CONSOLE - LIST-FILES, FILE-STATISTICS and COPY-TERM on the       **
** gateway's console                                                     **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "file_console.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <libcosmos/fa/fa_client.h>

#include "console_out.h"
#include "sintran_accounts.h"

/** What the running command does. */
typedef enum
{
    CMD_LIST = 0,   /**< LIST-FILES */
    CMD_STAT,       /**< FILE-STATISTICS */
    CMD_COPY        /**< COPY-TERM */
} cmd_kind;

/** Where the command stands. */
typedef enum
{
    ST_IDLE = 0,
    ST_ASKED,       /**< command() took it; the pass starts the client */
    ST_RUNNING,     /**< the client works; output flows */
    ST_ENDED        /**< the client is over; the last output is being fed */
} cmd_state;

/** Entries kept between the client and the console (the console drains far faster than
 *  SINTRAN walks: about 0.24 s per file, measured 2026-11-11). */
#define ENTRY_RING 64u

/** File bytes kept between the client and the console (COPY-TERM). */
#define DATA_RING (64u * 1024u)

/** One piece of console text. */
#define PEND_MAX 640u

const char FILE_CONSOLE_HELP[] =
    "LIST-FILES         list files on a SINTRAN machine\r\n"
    "FILE-STAT          size, dates and use count of files\r\n"
    "COPY-TERM          show a file here (bit 7 cleared)\r\n"
    "  Write the file as SINTRAN does:  machine(user(password)).NAME:TYPE\r\n"
    "  Leave out (password): the stored one is used for your SINTRAN user.\r\n"
    "  Leave out (user):     your SINTRAN account is used (admin: SINTRAN).\r\n"
    "  :TYPE alone selects every file of that type.\r\n"
    "  Examples:\r\n"
    "    LIST-FILES D100(SYSTEM)              every file of SYSTEM on D100\r\n"
    "    LIST-FILES D100(SYSTEM).:MODE        every MODE file\r\n"
    "    LIST-FILES D102(RONNY(secret))       with a password\r\n"
    "    LIST-FILES D100                      your own SINTRAN user's files\r\n"
    "    LIST-FILES D100 SYSTEM :SYMB         the same with spaces\r\n"
    "    FILE-STAT D100(SYSTEM).CHATKT:MODE   one file\r\n"
    "    FILE-STAT D100(SYSTEM).:MODE         every MODE file\r\n"
    "    FILE-STAT D100(SYSTEM)               every file\r\n"
    "    COPY-TERM D100(SYSTEM).LOAD-MODE:MODE\r\n"
    "  The escape key stops a command that is running.\r\n";

static file_console_resolve s_resolve = NULL;
static uint16_t s_own = 0;
static FaClient *s_client = NULL;      /* PSRAM */
static FaFileEntry *s_ring = NULL;     /* ENTRY_RING, PSRAM */
static uint8_t *s_data = NULL;         /* DATA_RING, PSRAM */
static char *s_pend = NULL;            /* PEND_MAX, PSRAM: text not yet taken by the console */

/* The command. Node task only: command(), cancel and the client callbacks all run there. */
static cmd_state s_state = ST_IDLE;
static cmd_kind s_kind = CMD_LIST;
static int s_place = -1;
static bool s_cancelled = false;       /* the user left or pressed escape; output is dropped */
static bool s_closing = false;         /* the closing lines are written */
static bool s_cancel_sent = false;     /* fa_client_cancel called for this command */
static bool s_pend_raw = false;        /* s_pend holds file bytes: console_command_raw */
static uint16_t s_system = 0;
static char s_machine[24];
static char s_user[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
static char s_password[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
static char s_filter[FA_CLIENT_SPEC];
static char s_dir[24];                 /* "PACK-ONE", from the directory callback */
static char s_dir_user[24];            /* "SYSTEM", from the directory callback */
static uint32_t s_ring_in = 0;
static uint32_t s_ring_out = 0;
static uint32_t s_ring_lost = 0;
static uint32_t s_data_in = 0;
static uint32_t s_data_out = 0;
static bool s_data_lost = false;
static uint32_t s_files = 0;
static int s_result = 0;
static uint16_t s_sintran_error = 0;
static size_t s_pend_len = 0;
static size_t s_pend_off = 0;

int file_console_init(file_console_resolve resolve, uint16_t own)
{
    s_resolve = resolve;
    s_own = own;
    if (s_client == NULL)
    {
        s_client = (FaClient *)heap_caps_calloc(1, sizeof(FaClient), MALLOC_CAP_SPIRAM);
    }
    if (s_ring == NULL)
    {
        s_ring = (FaFileEntry *)heap_caps_calloc(ENTRY_RING, sizeof(FaFileEntry), MALLOC_CAP_SPIRAM);
    }
    if (s_data == NULL)
    {
        s_data = (uint8_t *)heap_caps_malloc(DATA_RING, MALLOC_CAP_SPIRAM);
    }
    if (s_pend == NULL)
    {
        s_pend = (char *)heap_caps_malloc(PEND_MAX, MALLOC_CAP_SPIRAM);
    }
    return (s_client != NULL && s_ring != NULL && s_data != NULL && s_pend != NULL) ? 0 : -1;
}

/* ------------------------------------------------------------ parsing */

/** @brief Copy at most cap-1 characters of [from, to) into out, upper case. */
static bool take(const char *from, const char *to, char *out, size_t cap)
{
    size_t n = (size_t)(to - from);
    if (n == 0u || n >= cap)
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        out[i] = (char)toupper((unsigned char)from[i]);
    }
    out[n] = '\0';
    return true;
}

/** @brief Same, keeping case (a password). */
static bool take_exact(const char *from, const char *to, char *out, size_t cap)
{
    size_t n = (size_t)(to - from);
    if (n >= cap)
    {
        return false;
    }
    memcpy(out, from, n);
    out[n] = '\0';
    return true;
}

/**
 * @brief Read "<machine>(<user>(<password>)).<filter>" or "<machine> [<user>] [<filter>]".
 *        Empty user / password / filter are left "". A token holding ':' is the filter.
 * @return NULL, or what is wrong.
 */
static const char *parse_target(const char *a, char *machine, char *user, char *password, bool *pw_given,
                                char *filter)
{
    *pw_given = false;
    user[0] = '\0';
    password[0] = '\0';
    filter[0] = '\0';
    while (*a == ' ')
    {
        a++;
    }
    const char *paren = strchr(a, '(');
    const char *space = strchr(a, ' ');
    if (paren != NULL && (space == NULL || paren < space))
    {
        if (!take(a, paren, machine, sizeof s_machine))
        {
            return "give a machine: LIST-FILES D100(SYSTEM)";
        }
        const char *u = paren + 1;
        const char *u_end = u;
        while (*u_end != '\0' && *u_end != '(' && *u_end != ')')
        {
            u_end++;
        }
        if (u_end > u && !take(u, u_end, user, SINTRAN_ACCOUNT_FIELD_MAX + 1u))
        {
            return "the user name is too long";
        }
        const char *p = u_end;
        if (*p == '(')
        {
            const char *pw = p + 1;
            const char *pw_end = strchr(pw, ')');
            if (pw_end == NULL || !take_exact(pw, pw_end, password, SINTRAN_ACCOUNT_FIELD_MAX + 1u))
            {
                return "the password is not closed with ')', or is too long";
            }
            *pw_given = true;
            p = pw_end + 1;
        }
        if (*p != ')')
        {
            return "close the user with ')': D100(SYSTEM)";
        }
        p++;
        if (*p == '.')
        {
            p++;
        }
        while (*p == ' ')
        {
            p++;
        }
        const char *f_end = p + strlen(p);
        while (f_end > p && f_end[-1] == ' ')
        {
            f_end--;
        }
        if (f_end > p && !take(p, f_end, filter, FA_CLIENT_SPEC))
        {
            return "the file name is too long";
        }
        return NULL;
    }
    /* Spaces: machine, then user and/or filter. */
    const char *m_end = (space != NULL) ? space : a + strlen(a);
    if (!take(a, m_end, machine, sizeof s_machine))
    {
        return "give a machine: LIST-FILES D100(SYSTEM)";
    }
    const char *t = m_end;
    for (int i = 0; i < 2; i++)
    {
        while (*t == ' ')
        {
            t++;
        }
        if (*t == '\0')
        {
            break;
        }
        const char *t_end = strchr(t, ' ');
        if (t_end == NULL)
        {
            t_end = t + strlen(t);
        }
        bool is_filter = (memchr(t, ':', (size_t)(t_end - t)) != NULL) || user[0] != '\0';
        if (is_filter)
        {
            if (!take(t, t_end, filter, FA_CLIENT_SPEC))
            {
                return "the file name is too long";
            }
        }
        else if (!take(t, t_end, user, SINTRAN_ACCOUNT_FIELD_MAX + 1u))
        {
            return "the user name is too long";
        }
        t = t_end;
    }
    while (*t == ' ')
    {
        t++;
    }
    return (*t == '\0') ? NULL : "too many words";
}

/** @brief Does the line start with this command word (any case), followed by a space or end? */
static bool word_is(const char *line, const char *word, const char **args)
{
    size_t n = strlen(word);
    if (strncasecmp(line, word, n) != 0 || (line[n] != '\0' && line[n] != ' '))
    {
        return false;
    }
    *args = line + n;
    return true;
}

int file_console_command(int place, const char *gw_user, const char *line, char *out, size_t cap)
{
    const char *args = NULL;
    cmd_kind kind;
    while (*line == ' ')
    {
        line++;
    }
    if (word_is(line, "LIST-FILES", &args))
    {
        kind = CMD_LIST;
    }
    else if (word_is(line, "FILE-STATISTICS", &args) || word_is(line, "FILE-STAT", &args))
    {
        kind = CMD_STAT;
    }
    else if (word_is(line, "COPY-TERM", &args))
    {
        kind = CMD_COPY;
    }
    else
    {
        return 0;
    }
    if (s_client == NULL)
    {
        (void)snprintf(out, cap, "No memory for file commands.\r\n");
        return 1;
    }
    if (s_state != ST_IDLE)
    {
        (void)snprintf(out, cap, "Another file command is running on the gateway; try again.\r\n");
        return 1;
    }
    char machine[sizeof s_machine];
    char user[sizeof s_user];
    char password[sizeof s_password];
    char filter[sizeof s_filter];
    bool pw_given = false;
    const char *why = parse_target(args, machine, user, password, &pw_given, filter);
    /* FILE-STAT with no file: every file, as LIST-FILES (Ronny, 2026-11-11). */
    if (why == NULL && kind == CMD_COPY && filter[0] == '\0')
    {
        why = "give the file: D100(SYSTEM).NAME:TYPE";
    }
    if (why == NULL && kind == CMD_COPY && strchr(filter, ':') == NULL)
    {
        why = "give the file with its type: NAME:TYPE";
    }
    uint16_t system = 0;
    if (why == NULL && (s_resolve == NULL || s_resolve(machine, &system) != 0))
    {
        why = "unknown machine (NAMES lists them)";
    }
    if (why == NULL && system == s_own)
    {
        why = "that is this gateway; it has no SINTRAN files";
    }
    /* The user's own SINTRAN account fills what was not typed: the user when none was
     * given, and the stored password when the user is that account's. */
    char acc_user[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
    char acc_pw[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
    bool has_acc = (sintran_account_get(gw_user, acc_user, acc_pw) == 0);
    if (why == NULL && user[0] == '\0')
    {
        if (!has_acc)
        {
            why = "give the SINTRAN user, D100(SYSTEM), or ask an admin for a SINTRAN account";
        }
        else
        {
            (void)snprintf(user, sizeof user, "%s", acc_user);
        }
    }
    if (why == NULL && !pw_given && has_acc && strcasecmp(user, acc_user) == 0)
    {
        (void)snprintf(password, sizeof password, "%s", acc_pw);
    }
    memset(acc_pw, 0, sizeof acc_pw);
    if (why != NULL)
    {
        memset(password, 0, sizeof password);
        (void)snprintf(out, cap, "%s.\r\n", why);
        return 1;
    }
    s_kind = kind;
    s_place = place;
    s_system = system;
    (void)snprintf(s_machine, sizeof s_machine, "%s", machine);
    (void)snprintf(s_user, sizeof s_user, "%s", user);
    (void)snprintf(s_password, sizeof s_password, "%s", password);
    (void)snprintf(s_filter, sizeof s_filter, "%s", filter);
    memset(password, 0, sizeof password);
    s_state = ST_ASKED;
    out[0] = '\0';
    return 2;
}

void file_console_cancel(int place)
{
    if (s_state != ST_IDLE && place == s_place)
    {
        /* Ronny (2026-11-11): escape MUST stop the listing. The pass stops the client
         * (fa_client_cancel); its output is dropped meanwhile. */
        s_cancelled = true;
    }
}

/* ------------------------------------------------- client callbacks (node task) */

static void cb_opened(void *ctx, uint32_t length)
{
    (void)ctx;
    (void)length;
}

static void cb_data(void *ctx, uint32_t offset, const uint8_t *bytes, size_t n)
{
    (void)ctx;
    (void)offset;   /* the pieces come in order */
    for (size_t i = 0; i < n; i++)
    {
        if (s_data_in - s_data_out >= DATA_RING)
        {
            s_data_lost = true;   /* the console fell behind: the rest is dropped */
            return;
        }
        s_data[s_data_in % DATA_RING] = bytes[i];
        s_data_in++;
    }
}

static void cb_directory(void *ctx, const char *directory, const char *user)
{
    (void)ctx;
    (void)snprintf(s_dir, sizeof s_dir, "%s", directory);
    /* SINTRAN's full user name: "SYST" typed is "SYSTEM" here, as SINTRAN's own
     * LIST-FILES prints it (D100 to D102, 2026-11-11). */
    (void)snprintf(s_dir_user, sizeof s_dir_user, "%s", user);
}

static void cb_entry(void *ctx, const FaFileEntry *e)
{
    (void)ctx;
    if (s_ring_in - s_ring_out >= ENTRY_RING)
    {
        s_ring_lost++;
        return;
    }
    s_ring[s_ring_in % ENTRY_RING] = *e;
    s_ring_in++;
}

static void cb_done(void *ctx, int result, uint16_t sintran_error)
{
    (void)ctx;
    s_result = result;
    s_sintran_error = sintran_error;
    s_state = ST_ENDED;
}

/* --------------------------------------------------------------- output */

static const char *const MONTHS[12] = { "JANUARY", "FEBRUARY", "MARCH",     "APRIL",   "MAY",      "JUNE",
                                        "JULY",    "AUGUST",   "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER" };

/** @brief " 07.49.55  JULY 28, 1998" as SINTRAN prints a date (Ronny's sample, 2026-11-11). */
static int date_text(uint32_t raw, char *out, size_t cap)
{
    FaNdDate d;
    if (!fa_nd_date_split(raw, &d) || d.month < 1u || d.month > 12u)
    {
        return -1;
    }
    return snprintf(out, cap, "%02u.%02u.%02u  %s %u, %u", (unsigned)d.hour, (unsigned)d.minute,
                    (unsigned)d.second, MONTHS[d.month - 1u], (unsigned)d.day, (unsigned)d.year);
}

/** @brief Append to s_pend. */
static void pend_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void pend_add(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(&s_pend[s_pend_len], PEND_MAX - s_pend_len, fmt, ap);
    va_end(ap);
    if (n > 0)
    {
        s_pend_len += ((size_t)n < PEND_MAX - s_pend_len) ? (size_t)n : PEND_MAX - s_pend_len - 1u;
    }
}

/**
 * @brief One access group as SINTRAN's FILE-STATISTICS prints it, for example
 *        "FRIEND ACCESS : READ, WRITE, APPEND, COMMON, DIRECTORY", or NONE when the group
 *        grants nothing. Inside a group R is bit 0, W bit 1, A bit 2, C bit 3, D bit 4.
 *        The words were read off D100's own FILE-STAT of STARTX3:MODE (access 003777),
 *        2026-11-11; NONE is from a SINTRAN printout quoted in RetroIO's tests.
 */
static void access_line(const char *group, unsigned bits)
{
    static const char *const names[5] = { "READ", "WRITE", "APPEND", "COMMON", "DIRECTORY" };
    char text[64];
    size_t len = 0;
    text[0] = '\0';
    for (unsigned i = 0; i < 5u; i++)
    {
        if ((bits & (1u << i)) != 0u)
        {
            int n = snprintf(&text[len], sizeof text - len, "%s%s", (len > 0u) ? ", " : "", names[i]);
            if (n > 0)
            {
                len += (size_t)n;
            }
        }
    }
    pend_add("           %s ACCESS : %s\r\n", group, (len > 0u) ? text : "NONE");
}

/**
 * @brief One file as SINTRAN shows it. The version (";1") and the file number SINTRAN
 *        prints are not in the entry libcosmos hands over (libxmsg, 2026-11-11), so they are
 *        left out.
 */
static void entry_text(const FaFileEntry *e)
{
    char date[48];
    pend_add("FILE : %s.(%s:%s)%s:%s\r\n", s_machine, (s_dir[0] != '\0') ? s_dir : "?",
             (s_dir_user[0] != '\0') ? s_dir_user : s_user, e->name,
             e->type);
    if (s_kind != CMD_STAT)
    {
        return;
    }
    if ((e->attributes & 0x0008u) != 0u)
    {
        pend_add("           (INDEXED FILE)\r\n");
    }
    if ((e->attributes & 0x0020u) != 0u)
    {
        pend_add("           (CONTIGUOUS FILE)\r\n");
    }
    /* The access word holds three groups of five bits: PUBLIC in bits 10-14, FRIEND in
     * 5-9, OWN in 0-4 (RetroIO SintranAccessForm, from the SINTRAN reference manual
     * @SET-FILE-ACCESS). SINTRAN prints them in that order. */
    access_line("PUBLIC", (unsigned)(e->access >> 10));
    access_line("FRIEND", (unsigned)(e->access >> 5));
    access_line("OWN", (unsigned)e->access);
    pend_add("           OPENED %u TIME%s\r\n", (unsigned)e->open_total, (e->open_total == 1u) ? "" : "S");
    if (date_text(e->created, date, sizeof date) > 0)
    {
        pend_add("           CREATED %s\r\n", date);
    }
    if (date_text(e->last_read, date, sizeof date) > 0)
    {
        pend_add("           OPENED FOR READ %s\r\n", date);
    }
    if (date_text(e->last_write, date, sizeof date) > 0)
    {
        pend_add("           OPENED FOR WRITE %s\r\n", date);
    }
    if (e->has_bytes)
    {
        pend_add("           %lu PAGE%s , %lu BYTES IN FILE\r\n", (unsigned long)e->pages,
                 (e->pages == 1u) ? "" : "S", (unsigned long)e->bytes);
    }
    else
    {
        pend_add("           %lu PAGE%s\r\n", (unsigned long)e->pages, (e->pages == 1u) ? "" : "S");
    }
}

/** @brief COPY-TERM: the next file bytes, bit 7 cleared, zero bytes dropped (the console
 *         text call stops at a zero). Line ends pass as they are: a MODE file read
 *         2026-11-11 ends its lines CR LF. */
static void data_text(void)
{
    s_pend_raw = true;   /* only called with s_pend empty: the piece is all file bytes */
    while (s_data_out != s_data_in && s_pend_len + 1u < PEND_MAX)
    {
        uint8_t b = (uint8_t)(s_data[s_data_out % DATA_RING] & 0x7Fu);
        s_data_out++;
        if (b != 0u)
        {
            s_pend[s_pend_len++] = (char)b;
        }
    }
    s_pend[s_pend_len] = '\0';
}

/** @brief The closing line(s), once the client is over and everything else is out. */
static void end_text(void)
{
    if (s_result == FA_CLIENT_EREFUSED)
    {
        pend_add("SINTRAN refused it: error %u (octal %o).\r\n", (unsigned)s_sintran_error,
                 (unsigned)s_sintran_error);
    }
    else if (s_result != 0)
    {
        pend_add("It failed: XMSG error %d.\r\n", s_result);
    }
    else if (s_kind != CMD_COPY && s_files == 0u)
    {
        pend_add("No such file.\r\n");
    }
    if (s_kind == CMD_COPY && s_data_lost)
    {
        pend_add("\r\n<the console fell behind; the rest of the file was dropped>\r\n");
    }
    if (s_kind == CMD_COPY && s_result == 0)
    {
        pend_add("\r\n");   /* the prompt on its own line even when the file has no last line end */
    }
    if (s_ring_lost > 0u)
    {
        pend_add("<%lu files dropped: the console fell behind>\r\n", (unsigned long)s_ring_lost);
    }
}

/** @brief Give the waiting text to the console. @return true when all of it was taken. */
static bool pend_flush(ConsoleSession *s)
{
    while (s_pend_off < s_pend_len)
    {
        /* File bytes (COPY-TERM) pass unchanged (Ronny, 2026-11-11: the file as SINTRAN holds
         * it; [ ] are Norwegian letters on a 7-bit SINTRAN terminal); the gateway's own lines
         * keep the console's 7-bit rule. */
        int took = (s == NULL) ? -1
                   : s_pend_raw ? console_command_raw(s, &s_pend[s_pend_off])
                                : console_command_text(s, &s_pend[s_pend_off]);
        if (took < 0)
        {
            s_cancelled = true;   /* the command is no longer the console's: drop the rest */
            s_pend_len = 0;
            s_pend_off = 0;
            return true;
        }
        if (took == 0)
        {
            return false;
        }
        s_pend_off += (size_t)took;
    }
    s_pend_len = 0;
    s_pend_off = 0;
    s_pend[0] = '\0';
    s_pend_raw = false;
    return true;
}

static void finish(void)
{
    memset(s_password, 0, sizeof s_password);
    s_state = ST_IDLE;
    s_place = -1;
    s_cancelled = false;
    s_cancel_sent = false;
    s_closing = false;
}

/** @brief Start the client for an asked command. */
static void start(XmsgNode *node, ConsoleSession *s, uint32_t now)
{
    FaClientConfig cfg;
    FaClientCallbacks cb = { NULL, cb_opened, cb_data, cb_done, cb_directory, cb_entry };
    s_dir[0] = '\0';
    s_dir_user[0] = '\0';
    s_ring_in = s_ring_out = s_ring_lost = 0;
    s_data_in = s_data_out = 0;
    s_data_lost = false;
    s_files = 0;
    s_result = 0;
    s_sintran_error = 0;
    s_pend_len = s_pend_off = 0;
    fa_client_config_default(&cfg);
    int rc = fa_client_init(s_client, &cfg, &cb);
    /* The machine's own number as both node and system: the host relays it (file_fetch.cpp,
     * verified for D100 and for D102 through D100, 2026-11-11). */
    if (rc == XMSG_OK)
    {
        rc = (s_kind == CMD_COPY)
                 ? fa_client_read(s_client, node, s_system, s_system, s_user, s_user, s_password, s_filter, now)
                 : fa_client_list(s_client, node, s_system, s_system, s_user, s_user, s_password, s_filter, now);
    }
    memset(s_password, 0, sizeof s_password);
    console_out_log(CONTROL_FRAME_LOG_INFO, "File command %d: %s(%s) \"%s\": start %d", (int)s_kind, s_machine,
                    s_user, s_filter, rc);
    if (rc == XMSG_OK)
    {
        s_state = ST_RUNNING;
        return;
    }
    s_result = rc;
    s_state = ST_ENDED;
    (void)s;
}

void file_console_pass(XmsgNode *node, ConsoleHost *host, uint32_t now)
{
    if (s_state == ST_IDLE || node == NULL || host == NULL)
    {
        return;
    }
    ConsoleSession *s = console_host_session(host, s_place);
    if (s_state == ST_ASKED)
    {
        if (s_cancelled)
        {
            finish();
            return;
        }
        start(node, s, now);
    }
    if (s_cancelled)
    {
        /* Stop SINTRAN: the client tears the conversation down and ends with
         * done(XMSG_ECANCELED); output is dropped until then. */
        if (s_state == ST_RUNNING && !s_cancel_sent)
        {
            s_cancel_sent = true;
            int rc = fa_client_cancel(s_client, now);
            console_out_log(CONTROL_FRAME_LOG_INFO, "File command stopped by the user: cancel %d", rc);
        }
        s_ring_out = s_ring_in;
        s_data_out = s_data_in;
        if (s_state == ST_ENDED)
        {
            finish();
        }
        return;
    }
    /* Feed: the waiting text first, then more from the entries or the file bytes. */
    for (int round = 0; round < 8; round++)
    {
        if (!pend_flush(s))
        {
            return;   /* the line is full; try on the next pass */
        }
        if (s_cancelled)
        {
            return;
        }
        /* The directory name comes AFTER the first file (fa_client.h): hold the first line
         * until it is in, or the walk is over (seen 2026-11-11: "(?:SYSTEM)" otherwise). */
        if (s_ring_out != s_ring_in && (s_files > 0u || s_dir[0] != '\0' || s_state == ST_ENDED))
        {
            entry_text(&s_ring[s_ring_out % ENTRY_RING]);
            s_ring_out++;
            s_files++;
            continue;
        }
        if (s_data_out != s_data_in)
        {
            data_text();
            continue;
        }
        break;
    }
    if (s_state != ST_ENDED || s_ring_out != s_ring_in || s_data_out != s_data_in || !pend_flush(s))
    {
        return;
    }
    if (!s_closing)
    {
        end_text();
        s_closing = true;
        if (!pend_flush(s))
        {
            return;
        }
    }
    if (s != NULL && !s_cancelled)
    {
        console_command_done(s);
    }
    finish();
}
