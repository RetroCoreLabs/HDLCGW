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

/** @brief Print the DEBUG prompt, "# ". */
void console_task_show_prompt(void);

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
