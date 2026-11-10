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

#endif /* COMMANDS_H */
