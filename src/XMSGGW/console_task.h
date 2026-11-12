/**************************************************************************
** CONSOLE_TASK - the USB serial port: control frames from a host        **
**                program, or a text console                             **
**                                                                       **
** The same structure as HDLCGW console_task: in CONSOLE_FORWARDING the  **
** port carries RFC 1662 byte-stuffed frames (byte_frame.h) and only     **
** control frames (control_frame.h) mean anything - commands, log        **
** level, STAT push, ping. XMSGGW runs the node itself, so a frame from  **
** the host for the line has nowhere to go: it is dropped and counted.   **
** Three ESC presses between frames switch to CONSOLE_DEBUG, a text      **
** console (commands.h); EXIT switches back.                             **
**                                                                       **
** Unlike HDLCGW this task does not touch the line: node_task owns it.   **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef CONSOLE_TASK_H
#define CONSOLE_TASK_H

#include <stdint.h>
#include <stdbool.h>

/** Firmware version, printed in the boot banner and sent in HELLO_REPLY. */
#define XMSGGW_VERSION "0.1.1"

/** @brief What the USB serial port is being used for. */
typedef enum
{
    CONSOLE_FORWARDING,   /**< default: control frames from a host program */
    CONSOLE_DEBUG         /**< text console for a person */
} console_mode;

extern console_mode g_console_mode;   /**< current port use */

/**
 * @brief FreeRTOS task entry point. Owns the USB serial port input. Never
 *        returns. Loops every 1 ms.
 * @param[in] params  Unused.
 */
void console_task_run(void *params);

/** @brief Print the prompt of the USB setup questions' end ("XMSG# "). */
void console_task_show_prompt(void);

/* ------------------------------------------------ the USB text console
 *
 * One command list for USB and telnet (Ronny, 2026-11-12): once ESC ESC ESC
 * has opened the text console and the setup questions (../common/board.h)
 * are done, the typed bytes go to the console core in gw_console.cpp (node
 * task) as one more console place, logged in as admin with no password. The
 * calls below hand bytes and state between the two tasks.
 */

/** @brief The text console is open and wants a console place (gw_console.cpp
 *         takes it once: the call clears the wish). */
bool console_task_usb_take_open(void);

/**
 * @brief Typed bytes for the console place, in order.
 * @param[out] buf  Where they go.
 * @param[in]  cap  Size of buf.
 * @return Bytes copied, 0 when none wait.
 */
int console_task_usb_read(uint8_t *buf, int cap);

/** @brief The console place has closed (EXIT): the port goes back to frames. */
void console_task_usb_closed(void);

/** @brief The USB host went away while the text console was open (cleared by the call). */
bool console_task_usb_take_lost(void);

/* ------------------------------------------------ commands run for a place
 *
 * A command of the old USB table (commands.h, commands_shared_check) asked for
 * at any console place runs HERE, in the console task, not in the node task:
 * some take seconds (WIFI SCAN, about 3 s) and the node task keeps the XMSG
 * link's timers. Its printed text is caught (console_out_capture_begin) and
 * handed back. One command at a time.
 */

/**
 * @brief Ask for a command to be run.
 * @param[in] place  The console place, given back with the text.
 * @param[in] line   The command line.
 * @return true when taken, false when another one is still running.
 */
bool console_task_job_post(int place, const char *line);

/**
 * @brief A finished command's text.
 * @param[out] place  Its place.
 * @return The text (zero-terminated), or NULL when none is finished. Give it
 *         back with console_task_job_release() once printed.
 */
const char *console_task_job_result(int *place);

/** @brief The text from console_task_job_result() has been used. */
void console_task_job_release(void);

/** @brief The user at place stopped the command or left: its text is thrown away. */
void console_task_job_cancel(int place);

/**
 * @brief Bytes read from the USB serial port since boot, in every mode.
 * @return Count; wraps at 2^32.
 */
uint32_t console_task_usb_rx_bytes(void);

/**
 * @brief Host frames dropped for being longer than BYTE_FRAME_MAX.
 * @return Count since boot.
 */
uint32_t console_task_host_overlong(void);

/**
 * @brief Host frames that were not control frames, dropped (XMSGGW has no
 *        host-to-line path).
 * @return Count since boot.
 */
uint32_t console_task_host_dropped(void);

/**
 * @brief Microseconds spent on host input since boot.
 * @return The count; wraps after 71 minutes, read it as a delta.
 */
uint32_t console_task_host_input_us(void);

/**
 * @brief Bytes the core dropped from USB input because its queue was full.
 * @return The count since boot.
 */
uint32_t console_task_usb_rx_dropped(void);

/**
 * @brief Longest gap between two host reads since boot.
 * @return Microseconds.
 */
uint32_t console_task_read_gap_max_us(void);

/**
 * @brief Host read gaps longer than 10 ms (the core's USB receive wait).
 * @return The count since boot.
 */
uint32_t console_task_read_gaps_over_limit(void);

#endif /* CONSOLE_TASK_H */
