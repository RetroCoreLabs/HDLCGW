/**************************************************************************
** FILE_CONSOLE - LIST-FILES, FILE-STATISTICS and COPY-TERM on the       **
** gateway's console                                                     **
**                                                                       **
** Ronny's request (2026-11-11): the telnet console reads SINTRAN files  **
** on the machines behind the gateway, in SINTRAN's own syntax:          **
**                                                                       **
**   LIST-FILES D100(SYSTEM)                  every file of SYSTEM       **
**   LIST-FILES D100(SYSTEM(password)).:MODE  every MODE file            **
**   FILE-STAT  D100(SYSTEM).CHATVT:MODE      one file's statistics      **
**   COPY-TERM  D100(SYSTEM).CHATVT:MODE      the file shown on the      **
**                                            console, bit 7 cleared     **
**                                                                       **
** or with spaces: LIST-FILES D100 SYSTEM :MODE. Leave the user out and  **
** the gateway user's own SINTRAN account is used (the SINTRAN command,  **
** sintran_accounts.h), with its stored password.                        **
**                                                                       **
** The work is libcosmos' COSMOS file client (fa_client_list and         **
** fa_client_read); SINTRAN itself matches the filter (libxmsg checked   **
** "", "NAME:TYPE", ":SYMB" and ":MODE" live on D100, 2026-11-11). The   **
** answer arrives over XMSG, so the commands answer later (console.h     **
** command() answering 2, libxmsg 1cb5ec5).                              **
**                                                                       **
** Node task only. One command at a time for the whole gateway (one      **
** client); another user is told to try again.                           **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef FILE_CONSOLE_H
#define FILE_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

#include <libxmsg.h>
#include <libxmsg/console/console.h>

/** @brief A machine name or number to its system number: 0, or -1 when unknown. */
typedef int (*file_console_resolve)(const char *name, uint16_t *number);

/**
 * @brief Buffers (PSRAM). Once, before the tasks run.
 * @param resolve How a machine name is looked up (the same as CONNECT).
 * @param own     This gateway's own system number (refused as a target).
 * @return 0 or -1.
 */
int file_console_init(file_console_resolve resolve, uint16_t own);

/**
 * @brief A console line: one of ours? Called from ConsoleServices.command.
 * @param place The user's place.
 * @param gw_user The logged-in gateway user (for the stored SINTRAN account).
 * @param line  The line as typed.
 * @param out   Text to print now.
 * @return 0 not ours, 1 answered in `out`, 2 started (the answer comes later).
 */
int file_console_command(int place, const char *gw_user, const char *line, char *out, size_t cap);

/** @brief The user at `place` pressed escape or left. From ConsoleServices.command_cancel. */
void file_console_cancel(int place);

/** @brief Work for the node task: start an asked command, feed its output to the console. */
void file_console_pass(XmsgNode *node, ConsoleHost *host, uint32_t now);

/** How many commands FILE_CONSOLE_COMMANDS holds. */
#define FILE_CONSOLE_COMMAND_COUNT 3

/** @brief LIST-FILES, FILE-STAT and COPY-TERM for HELP, HELP <command> and TAB
 *         (ConsoleServices.commands). */
extern const ConsoleCommandInfo FILE_CONSOLE_COMMANDS[FILE_CONSOLE_COMMAND_COUNT];

#endif /* FILE_CONSOLE_H */
