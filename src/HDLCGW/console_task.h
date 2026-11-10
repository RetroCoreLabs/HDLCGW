/**************************************************************************
** CONSOLE_TASK - the USB serial port: frame channel to the host, or a   **
**                text console                                           **
**                                                                       **
** In CONSOLE_FORWARDING the port carries byte-stuffed frames to and     **
** from the host (byte_frame.h). Three ESC presses between frames switch **
** to CONSOLE_DEBUG, a text console (commands.h); EXIT switches back.    **
** The task also drains what the SPI link received: decode, echo,        **
** loopback.                                                             **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#ifndef CONSOLE_TASK_H
#define CONSOLE_TASK_H

#include <stdint.h>
#include <stdbool.h>

/** Firmware version, printed in the boot banner and sent in HELLO_REPLY. */
#define HDLCGW_VERSION "0.2.0"

/** @brief What the USB serial port is being used for. Same three modes as RC422Dev. */
typedef enum
{
    CONSOLE_FORWARDING,   /**< default: frames flow host <-> line, no prompt */
    CONSOLE_DEBUG,        /**< text console; frames are printed, not forwarded */
    CONSOLE_PACKET_GEN    /**< every host keystroke becomes a byte-stuffed frame
                               back to the host, to exercise a host receiver;
                               ESC ESC ESC returns to CONSOLE_FORWARDING */
} console_mode;

/** @brief Frame counters shown by the STAT command. */
typedef struct
{
    int total_frames;   /**< every frame seen, good or bad */
    int ok_frames;      /**< frames with a good FCS (received) or sent */
    int bad_frames;     /**< bad FCS, aborted or too long */
    int bytes;          /**< bytes in the good frames, FCS included */
} console_frame_stats;

extern console_mode g_console_mode;             /**< current port use */
extern console_frame_stats g_sent_stats;        /**< frames host -> line */
extern console_frame_stats g_received_stats;    /**< frames line -> host */

/* Flags the DEBUG commands toggle. */
extern bool g_loopback_enabled;   /**< bytes from the line go straight back out */
extern bool g_echo_enabled;       /**< print what comes from the line */
extern bool g_hdlc_mode;          /**< in DEBUG: decode frames instead of raw bytes */

/**
 * @brief FreeRTOS task entry point. Owns the USB serial port. Never returns.
 *
 * Loops every 1 ms: reads host bytes (frames or console keystrokes depending
 * on g_console_mode), then drains up to 256 bytes from g_rx_buffer.
 *
 * @param[in] params  Unused.
 */
void console_task_run(void *params);

/**
 * @brief Create the mutex that serialises the two senders to the line
 *        (console_task for USB, wifi_task for TCP). Call once from setup(),
 *        before any task starts.
 */
void console_task_init(void);

/**
 * @brief Print the DEBUG prompt.
 *
 * '#' plain, '@' HDLC decoding on, '>' echo on, and [rx:tx] byte counts in
 * front when either buffer holds data.
 */
void console_task_show_prompt(void);

/**
 * @brief Bit-stuff a complete frame onto the line and count it as sent.
 *
 * The stuffed frame is built in full, followed by one byte of idle ones so
 * that two flags are never separated by fewer than 7 ones (ISO 13239 fill is
 * flags or idle, nothing in between), and enters g_tx_buffer in one piece,
 * so the line never sees idle bytes inside it. Safe to call from console_task
 * and wifi_task at once (mutex). Blocks while g_tx_buffer has
 * no room (see spi_tx_task_push()). A frame longer than BYTE_FRAME_MAX is
 * counted as bad and dropped.
 *
 * @param[in] frame  Address, control, data and the two FCS bytes.
 * @param[in] len    Number of bytes in frame, at most BYTE_FRAME_MAX.
 */
void console_task_send_line_frame(const uint8_t *frame, int len);

/**
 * @brief Byte-stuff a complete frame and write it to the host over USB.
 *
 * Used for every good frame from the line in FORWARDING and for the frames
 * PACKET_GEN makes. Writes nothing when no host has the port open.
 *
 * @param[in] frame  Address, control, data and the two FCS bytes.
 * @param[in] len    Number of bytes in frame, at most BYTE_FRAME_MAX.
 */
void console_task_send_host_frame(const uint8_t *frame, int len);

/**
 * @brief The last frame that came off the line, for the INSPECT command.
 *
 * Kept whether its FCS was good or bad; an aborted frame is not kept.
 *
 * @param[out] len       Number of bytes, FCS included; 0 when none yet.
 * @param[out] crc_good  FCS verdict of that frame.
 * @return Pointer to the frame bytes, valid until the next frame ends.
 */
const uint8_t *console_task_last_frame(int *len, bool *crc_good);

/**
 * @brief One of the last four frames off the line whose FCS was bad, for
 *        INSPECT BAD [n].
 *
 * Bad frames are never forwarded to the host, so this is the only way to see
 * one. Aborted frames are not kept. Four are kept because a run that loses
 * three frames in two minutes must show all three (bench 2026-11-08: the
 * first one seen was a single bit flipped with the length intact).
 *
 * @param[in]  index  0 = the newest, 1 = the one before, up to 3.
 * @param[out] len    Number of bytes, FCS included; 0 when there is no such
 *                    frame yet.
 * @return Pointer to the frame bytes, valid until that slot is reused.
 */
const uint8_t *console_task_last_bad_frame(int index, int *len);

/**
 * @brief Flag-to-flag runs on the line too short to be a frame, ignored by
 *        the decoder (hdlc.h explains where they come from).
 * @return Count since boot.
 */
uint32_t console_task_short_frames(void);

/**
 * @brief Bytes read from the USB serial port since boot, in every mode
 *        (frames, control frames and keystrokes alike).
 *
 * Compared with what the host wrote, it shows whether the host-to-board
 * USB path kept up.
 *
 * @return Count since boot; wraps at 2^32.
 */
uint32_t console_task_usb_rx_bytes(void);

/**
 * @brief Frames from the host over USB dropped for being longer than
 *        BYTE_FRAME_MAX (1024 bytes after un-stuffing, FCS included).
 * @return Count since boot.
 */
uint32_t console_task_host_overlong(void);

/**
 * @brief Microseconds the console loop spent on host input since boot.
 * @return The count; wraps after 71 minutes, read it as a delta.
 */
uint32_t console_task_host_input_us(void);

/**
 * @brief Microseconds the console loop spent draining the line RX ring.
 * @return The count; wraps after 71 minutes, read it as a delta.
 */
uint32_t console_task_line_drain_us(void);

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
