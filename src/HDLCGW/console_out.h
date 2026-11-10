/**************************************************************************
** CONSOLE_OUT - the one place text for the host leaves the firmware     **
**                                                                       **
** WHY: every command used to print straight to Serial, which is only    **
** right in the DEBUG text console. When a command arrives inside a      **
** control frame (control_frame.h) the port is in FORWARDING mode and    **
** carries binary frames, so its output must be wrapped in REPLY frames  **
** and go through usb_tx_task_push like every other binary byte (the     **
** rule from usb_tx_task.h: console_task never writes to Serial in       **
** FORWARDING). Rather than rewrite twenty command handlers, they call   **
** console_out_* instead of Serial.* and this module decides where the   **
** text goes:                                                            **
**                                                                       **
**   SERIAL sink  - DEBUG mode, a person with a terminal: straight to    **
**                  Serial, exactly as before.                           **
**   FRAME sink   - between console_out_reply_begin() and _end(): text   **
**                  is collected into chunks of CONSOLE_OUT_CHUNK bytes  **
**                  and each chunk leaves as one REPLY frame; the last   **
**                  one carries the LAST flag so the host knows the      **
**                  command is done without waiting for a prompt.        **
**                                                                       **
** Log lines are separate from replies: console_out_log() prints them   **
** in DEBUG mode and sends them as LOG frames in FORWARDING, but only    **
** when the host has turned logging on (default OFF, Ronny 2026-11-08). **
** EVENT and STAT frames follow the same gate for push, except events    **
** which are always sent (they are rare and the host asked for frames    **
** by sending HELLO). Nothing here blocks: a frame that does not fit in  **
** the USB ring is dropped and counted, like a data frame.               **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#ifndef CONSOLE_OUT_H
#define CONSOLE_OUT_H

#include <stdint.h>
#include <stdbool.h>

#include "libhdlc/control_frame.h"

/** Longest REPLY payload per frame. Two of these plus line traffic fit in
 *  the 16 KB USB ring with room to spare; the full 1018 would be legal. */
#define CONSOLE_OUT_CHUNK 512

/** Log level the host set with LOG_LEVEL (or the LOG text command).
 *  CONTROL_FRAME_LOG_OFF at boot and after HELLO or a lost USB host. */
extern uint8_t g_console_out_log_level;

/** STAT push period in ms, 0 = off. Same reset rules as the log level. */
extern uint16_t g_console_out_stat_period_ms;

/**
 * @brief Reset the module: SERIAL sink, logging off, counters zero.
 *        Call once from setup() before any task starts.
 */
void console_out_init(void);

/**
 * @brief Print text with no line ending.
 * @param[in] s  NUL-terminated text.
 */
void console_out_print(const char *s);

/**
 * @brief Print text followed by CR LF. With no argument prints a blank line.
 * @param[in] s  NUL-terminated text, or "".
 */
void console_out_println(const char *s = "");

/**
 * @brief printf-style print. Output longer than CONSOLE_OUT_CHUNK is split
 *        across chunks; a single call is limited to 256 characters.
 * @param[in] fmt  printf format.
 */
void console_out_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * @brief Select the FRAME sink: what follows is the reply to request id.
 * @param[in] id  Request id from the COMMAND frame.
 */
void console_out_reply_begin(uint8_t id);

/**
 * @brief Send the final chunk (even if empty) with LAST, and ERROR when
 *        asked, then return to the SERIAL sink.
 * @param[in] error  True when the command failed or was refused.
 */
void console_out_reply_end(bool error);

/**
 * @brief Whether a reply is being collected (FRAME sink active).
 * @return true between reply_begin and reply_end.
 */
bool console_out_in_reply(void);

/**
 * @brief One log line.
 *
 * In DEBUG mode it is printed as text when level is at most DEBUG (or at
 * most the level the host set, whichever is higher), so the console shows
 * what it always showed. Otherwise it becomes a LOG frame when level is at
 * most g_console_out_log_level, and is dropped when logging is off. Never
 * blocks. Safe to call from any task: it builds the frame on the stack and
 * usb_tx_task_push() is locked.
 *
 * @param[in] level  CONTROL_FRAME_LOG_ERROR .. CONTROL_FRAME_LOG_TRACE.
 * @param[in] fmt    printf format, at most 200 characters of output.
 */
void console_out_log(uint8_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/**
 * @brief One event: a text line in DEBUG mode, an EVENT frame otherwise.
 *        Events are sent even when logging is off.
 * @param[in] code  CONTROL_FRAME_EVENT_*.
 * @param[in] text  Short text, or NULL.
 */
void console_out_event(uint8_t code, const char *text);

/**
 * @brief Send one STAT frame with every counter (see control_frame.h for
 *        the field order). Called by console_task when the period elapses;
 *        does nothing in DEBUG mode.
 */
void console_out_stat_push(void);

/**
 * @brief Send one control frame of any type to the host, counted as
 *        dropped when the USB ring is full.
 * @param[in] type         CONTROL_FRAME_* type.
 * @param[in] id           Request id or 0.
 * @param[in] flags        Flags.
 * @param[in] payload      Payload bytes or NULL.
 * @param[in] payload_len  0..CONSOLE_OUT_CHUNK (longer is dropped and counted).
 * @return true when queued.
 */
bool console_out_send_frame(uint8_t type, uint8_t id, uint8_t flags, const uint8_t *payload,
                            int payload_len);

/**
 * @brief Close a pending reply, then wait up to 200 ms for the USB ring and
 *        the CDC to drain. For RESET, so the host sees the reply.
 */
void console_out_flush(void);

/**
 * @brief Forget host-specific state: logging off, stat push off. Called
 *        when the USB host goes away or sends HELLO.
 */
void console_out_host_reset(void);

/** @brief Counters for STAT. */
typedef struct
{
    uint32_t replies;   /**< REPLY frames sent */
    uint32_t logs;      /**< LOG frames sent */
    uint32_t dropped;   /**< control frames dropped, USB ring full */
} console_out_stats;

/**
 * @brief Read the counters.
 * @param[out] out  Receives a copy.
 */
void console_out_get_stats(console_out_stats *out);

#endif /* CONSOLE_OUT_H */
