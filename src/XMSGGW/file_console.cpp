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
#include <libcosmos/fa/fa_text.h>

#include "console_out.h"
#include "file_job.h"
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
    ST_RUNNING      /**< the job is the command's: output flows, or the job is being stopped */
} cmd_state;

/** Entries kept between the client and the console (the console drains far faster than
 *  SINTRAN walks: about 0.24 s per file, measured 2026-11-11). */
#define ENTRY_RING 64u

/** File bytes kept between the client and the console (COPY-TERM). */
#define DATA_RING (64u * 1024u)

/** One piece of console text. */
#define PEND_MAX 640u

/* How a file is written, shared by the three commands' HELP <command> text. */
#define FILE_NAME_RULES \
    "  Write the file as SINTRAN does:  machine(user(password)).NAME:TYPE\r\n" \
    "  Leave out (password): the stored one is used for your SINTRAN user.\r\n" \
    "  Leave out (user):     your SINTRAN account is used (admin: SINTRAN).\r\n" \
    "  :TYPE alone selects every file of that type.\r\n" \
    "  The escape key stops a command that is running.\r\n"

const ConsoleCommandInfo FILE_CONSOLE_COMMANDS[FILE_CONSOLE_COMMAND_COUNT] = {
    { "LIST-FILES", AUTH_LEVEL_USER, "list files on a SINTRAN machine",
      "LIST-FILES machine(user(password)).NAME:TYPE\r\n" FILE_NAME_RULES
      "  Examples:\r\n"
      "    LIST-FILES D100(SYSTEM)              every file of SYSTEM on D100\r\n"
      "    LIST-FILES D100(SYSTEM).:MODE        every MODE file\r\n"
      "    LIST-FILES D102(RONNY(secret))       with a password\r\n"
      "    LIST-FILES D100                      your own SINTRAN user's files\r\n"
      "    LIST-FILES D100 SYSTEM :SYMB         the same with spaces\r\n" },
    { "FILE-STAT", AUTH_LEVEL_USER, "size, dates and use count of files",
      "FILE-STAT machine(user(password)).NAME:TYPE\r\n" FILE_NAME_RULES
      "  Examples:\r\n"
      "    FILE-STAT D100(SYSTEM).CHATKT:MODE   one file\r\n"
      "    FILE-STAT D100(SYSTEM).:MODE         every MODE file\r\n"
      "    FILE-STAT D100(SYSTEM)               every file\r\n" },
    { "COPY-TERM", AUTH_LEVEL_USER, "show a file here (bit 7 cleared)",
      "COPY-TERM machine(user(password)).NAME:TYPE\r\n" FILE_NAME_RULES
      "  Example:\r\n"
      "    COPY-TERM D100(SYSTEM).LOAD-MODE:MODE\r\n" },
};

static file_console_resolve s_resolve = NULL;
static uint16_t s_own = 0;
static FileJob *s_job = NULL;          /* the console's SINTRAN file job (file_job.h) */
static char *s_pend = NULL;            /* PEND_MAX, PSRAM: text not yet taken by the console */

/* The command. Node task only: command(), cancel and the pass all run there. */
static cmd_state s_state = ST_IDLE;
static cmd_kind s_kind = CMD_LIST;
static int s_place = -1;
static bool s_cancelled = false;       /* the user left or pressed escape; output is dropped */
static bool s_closing = false;         /* the closing lines are written */
static bool s_pend_raw = false;        /* s_pend holds file bytes: console_command_raw */
static char s_machine[24];
static char s_user[SINTRAN_ACCOUNT_FIELD_MAX + 1u];   /* the user as typed or filled in */
static file_job_status s_st;           /* the job, as the pass last saw it */
static FaFileEntry s_next;             /* an entry taken from the job, not yet shown */
static bool s_next_ok = false;
static uint32_t s_files = 0;
static size_t s_pend_len = 0;
static size_t s_pend_off = 0;

int file_console_init(file_console_resolve resolve, uint16_t own)
{
    s_resolve = resolve;
    s_own = own;
    if (s_job == NULL)
    {
        s_job = file_job_create("console", DATA_RING, ENTRY_RING);
    }
    if (s_pend == NULL)
    {
        s_pend = (char *)heap_caps_malloc(PEND_MAX, MALLOC_CAP_SPIRAM);
    }
    return (s_job != NULL && s_pend != NULL) ? 0 : -1;
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
    if (s_job == NULL)
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
    char password[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
    char filter[FA_CLIENT_SPEC];
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
    /* A job still stopping from the last command (escape) is not free yet. */
    int rc = file_job_start(s_job, kind != CMD_COPY, system, user, password, filter);
    memset(password, 0, sizeof password);
    console_out_log(CONTROL_FRAME_LOG_INFO, "File command %d: %s(%s) \"%s\": %s", (int)kind, machine, user,
                    filter, (rc == 0) ? "asked" : "busy");
    if (rc != 0)
    {
        (void)snprintf(out, cap, "The last file command is still stopping; try again.\r\n");
        return 1;
    }
    s_kind = kind;
    s_place = place;
    (void)snprintf(s_machine, sizeof s_machine, "%s", machine);
    (void)snprintf(s_user, sizeof s_user, "%s", user);
    s_next_ok = false;
    s_files = 0;
    s_pend_len = s_pend_off = 0;
    s_closing = false;
    s_cancelled = false;
    memset(&s_st, 0, sizeof s_st);
    s_state = ST_RUNNING;
    out[0] = '\0';
    return 2;
}

void file_console_cancel(int place)
{
    if (s_state != ST_IDLE && place == s_place)
    {
        /* Ronny (2026-11-11): escape MUST stop the listing. The pass lets go of the job,
         * which stops SINTRAN (fa_client_cancel); output is dropped meanwhile. */
        s_cancelled = true;
    }
}

/* --------------------------------------------------------------- output */

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

/** @brief One access group as SINTRAN's FILE-STATISTICS prints it (libcosmos fa_text). */
static void access_line(const char *group, uint16_t access, unsigned shift)
{
    char words[FA_TEXT_ACCESS_WORDS];
    if (fa_text_access_words(access, shift, words, sizeof words) > 0)
    {
        pend_add("           %s ACCESS : %s\r\n", group, words);
    }
}

/**
 * @brief One file as SINTRAN shows it: "FILE 215 : D100.(PACK-ONE:SYSTEM)STARTX3:MODE;1".
 *        The number is the file's slot in the user's directory (libcosmos object_index);
 *        the version is left out when libcosmos cannot tell it (a file with more versions).
 */
static void entry_text(const FaFileEntry *e)
{
    char date[48];
    char name[FA_TEXT_NAME];
    if (fa_text_name(e, FA_NAME_SINTRAN_VERSION, name, sizeof name) < 0)
    {
        (void)fa_text_name(e, FA_NAME_SINTRAN, name, sizeof name);
    }
    pend_add("FILE %u : %s.(%s:%s)%s\r\n", (unsigned)e->object_index, s_machine,
             (s_st.directory[0] != '\0') ? s_st.directory : "?", (s_st.user[0] != '\0') ? s_st.user : s_user,
             name);
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
    access_line("PUBLIC", e->access, FA_ACCESS_PUBLIC_SHIFT);
    access_line("FRIEND", e->access, FA_ACCESS_FRIEND_SHIFT);
    access_line("OWN", e->access, FA_ACCESS_OWN_SHIFT);
    pend_add("           OPENED %u TIME%s\r\n", (unsigned)e->open_total, (e->open_total == 1u) ? "" : "S");
    if (fa_text_date_stat(e->created, date, sizeof date) > 0)
    {
        pend_add("           CREATED %s\r\n", date);
    }
    if (fa_text_date_stat(e->last_read, date, sizeof date) > 0)
    {
        pend_add("           OPENED FOR READ %s\r\n", date);
    }
    if (fa_text_date_stat(e->last_write, date, sizeof date) > 0)
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
static bool data_text(void)
{
    /* Only called with s_pend empty: the piece is all file bytes. */
    size_t n = file_job_take_bytes(s_job, (uint8_t *)s_pend, PEND_MAX - 1u);
    size_t kept = 0;
    for (size_t i = 0; i < n; i++)
    {
        uint8_t b = (uint8_t)((uint8_t)s_pend[i] & 0x7Fu);
        if (b != 0u)
        {
            s_pend[kept++] = (char)b;
        }
    }
    s_pend[kept] = '\0';
    s_pend_len = kept;
    s_pend_raw = true;
    return n > 0u;
}

/** @brief The closing line(s), once the client is over and everything else is out. */
static void end_text(void)
{
    if (s_st.result != 0)
    {
        char why[96];
        file_job_reason(&s_st, why, sizeof why);
        why[0] = (char)toupper((unsigned char)why[0]);
        pend_add("%s.\r\n", why);
    }
    else if (s_kind != CMD_COPY && s_files == 0u)
    {
        pend_add("No such file.\r\n");
    }
    if (s_kind == CMD_COPY && s_st.data_lost)
    {
        pend_add("\r\n<the console fell behind; the rest of the file was dropped>\r\n");
    }
    if (s_kind == CMD_COPY && s_st.result == 0)
    {
        pend_add("\r\n");   /* the prompt on its own line even when the file has no last line end */
    }
    if (s_st.entries_lost > 0u)
    {
        pend_add("<%lu files dropped: the console fell behind>\r\n", (unsigned long)s_st.entries_lost);
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
    s_state = ST_IDLE;
    s_place = -1;
    s_cancelled = false;
    s_closing = false;
    s_next_ok = false;
}

void file_console_pass(XmsgNode *node, ConsoleHost *host, uint32_t now)
{
    (void)node;   /* the job runs in file_job_pass_all */
    (void)now;
    if (s_state == ST_IDLE || host == NULL)
    {
        return;
    }
    ConsoleSession *s = console_host_session(host, s_place);
    if (s_cancelled)
    {
        /* Stop SINTRAN: letting go of the job stops the client, which ends with
         * done(XMSG_ECANCELED); output is dropped until the job is free again. */
        file_job_release(s_job);
        s_pend_len = s_pend_off = 0;
        file_job_get(s_job, &s_st);
        if (s_st.state == FILE_JOB_IDLE)
        {
            console_out_log(CONTROL_FRAME_LOG_INFO, "File command stopped by the user");
            finish();
        }
        return;
    }
    /* The state first: when it already says ENDED, every entry and byte is in the job, so
     * nothing more to take means the output is complete. */
    file_job_get(s_job, &s_st);
    bool ended = (s_st.state == FILE_JOB_ENDED);
    bool drained = false;
    for (int round = 0; round < 8 && !drained; round++)
    {
        if (!pend_flush(s))
        {
            return;   /* the line is full; try on the next pass */
        }
        if (s_cancelled)
        {
            return;
        }
        if (!s_next_ok)
        {
            s_next_ok = file_job_take_entry(s_job, &s_next);
        }
        /* The directory name comes AFTER the first file (fa_client.h): hold the first line
         * until it is in, or the walk is over (seen 2026-11-11: "(?:SYSTEM)" otherwise). */
        if (s_next_ok && (s_files > 0u || s_st.directory[0] != '\0' || ended))
        {
            entry_text(&s_next);
            s_next_ok = false;
            s_files++;
            continue;
        }
        if (!s_next_ok && data_text())
        {
            continue;
        }
        drained = true;
    }
    if (!drained || !ended || s_next_ok || !pend_flush(s))
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
    file_job_release(s_job);   /* ENDED and all shown: the job is free */
    finish();
}
