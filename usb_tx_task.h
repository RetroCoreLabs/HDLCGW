/**************************************************************************
** USB_TX_TASK - binary output to the host, decoupled from everything   **
**               else                                                    **
**                                                                       **
** Why a task of its own: the core's USB CDC write (USBCDC::write) holds **
** the caller until the host has fetched the bytes, 64 at a time, and    **
** spins while the FIFO is full. When console_task did those writes      **
** itself it stopped draining the line receive ring for as long as one   **
** frame took to reach the host, and at 983 kbit/s with 1024-byte frames **
** the ring overflowed and frames were lost (bench, 2026-11-08). Now     **
** console_task only drops the byte-stuffed frame into g_usb_tx_buffer   **
** and carries on; this task feeds the USB at whatever pace the host     **
** takes. If the host is slower than the line for long enough the ring   **
** fills and whole frames are dropped and COUNTED (usb_tx_task_dropped), **
** never split.                                                          **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#ifndef USB_TX_TASK_H
#define USB_TX_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "ring_buffer.h"

extern ring_buffer g_usb_tx_buffer;   /**< byte-stuffed frames waiting for the host */

/**
 * @brief Set up g_usb_tx_buffer. Call once from setup(), before any task starts.
 */
void usb_tx_task_init(void);

/**
 * @brief FreeRTOS task entry point. Copies g_usb_tx_buffer to the USB port forever.
 *
 * Writes in pieces of at most 64 bytes (the CDC FIFO size), every byte of
 * a piece even when the core takes it in parts, and sleeps 1 ms
 * when the ring is empty. Never returns.
 *
 * @param[in] params  Unused.
 */
void usb_tx_task_run(void *params);

/**
 * @brief Queue a complete block of bytes for the host, all of it or nothing.
 *
 * Never blocks: when the block does not fit it is dropped and counted.
 *
 * @param[in] bytes  Bytes to send, normally one byte-stuffed frame.
 * @param[in] n      Number of bytes.
 * @return true when queued, false when dropped.
 */
bool usb_tx_task_push(const uint8_t *bytes, int n);

/**
 * @brief Blocks dropped by usb_tx_task_push() because the ring was full.
 * @return Count since boot.
 */
uint32_t usb_tx_task_dropped(void);

/**
 * @brief Pieces the core took only in part while the host had the port open.
 *
 * The rest of the piece is written again, so nothing is lost; a count above
 * zero only says the host stopped reading for longer than the core's 250 ms
 * write timeout. Counted once per piece.
 *
 * @return Count since boot; wraps at 2^32.
 */
uint32_t usb_tx_task_short_writes(void);

/**
 * @brief Times the queued output to the host was thrown away because no
 *        host had the port open (whole frames only).
 * @return Count since boot; wraps at 2^32.
 */
uint32_t usb_tx_task_host_closed_flushes(void);

/**
 * @brief Microseconds spent inside the USB CDC write since boot.
 * @return The count; wraps after 71 minutes, read it as a delta.
 */
uint32_t usb_tx_task_write_us(void);

/**
 * @brief Bytes the USB CDC write accepted since boot.
 * @return The count.
 */
uint32_t usb_tx_task_written_bytes(void);

/**
 * @brief Longest single USB CDC write call since boot.
 * @return Microseconds.
 */
uint32_t usb_tx_task_write_max_us(void);

/**
 * @brief USB CDC write calls longer than 20 ms since boot.
 * @return The count.
 */
uint32_t usb_tx_task_writes_slow(void);

/**
 * @brief 1 ms sleeps after a USB write that took nothing, since boot.
 * @return The count.
 */
uint32_t usb_tx_task_zero_write_sleeps(void);

/**
 * @brief 1 ms sleeps with the USB TX ring empty, since boot.
 * @return The count.
 */
uint32_t usb_tx_task_empty_sleeps(void);

/**
 * @brief Longest run of back-to-back USB writes over 1 ms each, since boot.
 * @return Microseconds.
 */
uint32_t usb_tx_task_slow_run_max_us(void);

#endif /* USB_TX_TASK_H */
