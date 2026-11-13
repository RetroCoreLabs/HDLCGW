/**************************************************************************
** COMMANDS - text commands of the debug console                         **
**                                                                       **
** One line in, one handler out. The table with the names and help text  **
** is in commands.cpp; HELP on the device prints it.                     **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#ifndef COMMANDS_H
#define COMMANDS_H

#include <stdbool.h>
#include <stddef.h>

#include <libxmsg.h>
#include <libxmsg/console/console.h>

/**
 * @brief Handler for one command.
 * @param[in] args  Rest of the line after the command name, or NULL.
 */
typedef void (*command_handler)(const char *args);

/** @brief One row of the command table. */
typedef struct
{
    const char *name;           /**< command word, matched case-insensitively */
    const char *help;           /**< one line for HELP */
    command_handler handler;    /**< what runs */
} command;

/**
 * @brief Parse and run one console line.
 *
 * The first word is the command, the rest is passed to its handler. Prints
 * a message for an unknown command.
 *
 * @param[in,out] line  NUL-terminated line; modified in place by strtok.
 */
void commands_process(char *line);

/**
 * @brief One command list for the USB and telnet consoles (Ronny, 2026-11-12).
 *        The telnet console's core (libxmsg console) owns HELP, INFO, EXIT,
 *        LIST, WHO, NAMES, CONNECT and the user commands; gw_console.cpp owns
 *        NODE, RESTART, FIRMWARE, SINTRAN and the file commands. Every other
 *        command of this file's table is offered to every console place here.
 *        Rights: an ordinary user may run STAT, INSPECT, TASK and
 *        WIFI [STATUS|SCAN]; everything else is for the admin. TERM (a terminal
 *        session printed on this USB port) and WIFI SHOW (the password) are
 *        for the USB console only.
 * @param[in]  line   The line as typed.
 * @param[in]  admin  Whether the user is the admin.
 * @param[in]  usb    Whether the user sits on the USB console.
 * @param[out] why    Set when the answer is -1: the refusal, CR LF ended.
 * @param[in]  cap    Size of why.
 * @return 1 run it (commands_process()), 0 not a command of this list,
 *         -1 refused (why holds the text).
 */
int commands_shared_check(const char *line, bool admin, bool usb, char *why, size_t cap);

/**
 * @brief The shared commands as rows for the console core's HELP, HELP
 *        <command> and TAB (libxmsg ConsoleCommandInfo). Each command's help
 *        text "USAGE - what it does" becomes the summary (the part after
 *        " - ") and the detail (the whole text). The detail lines are kept in
 *        text (PSRAM, owned by the caller); the rows point into it.
 * @param[out] rows      Where the rows go.
 * @param[in]  max       Room in rows.
 * @param[out] text      Storage for the detail lines.
 * @param[in]  text_cap  Size of text.
 * @return Rows written.
 */
int commands_shared_info(ConsoleCommandInfo *rows, int max, char *text, size_t text_cap);

#endif /* COMMANDS_H */
