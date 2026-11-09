/**************************************************************************
** SPI_TX_TASK - transmit half of the synchronous line                   **
**                                                                       **
** This end GENERATES the transmit clock: the SPI2 peripheral runs as a  **
** master, clock out on PIN_TX_CLOCK, data out on PIN_TX_DATA. Bytes to  **
** send are taken from g_tx_buffer; when it is empty the line is kept at **
** HDLC_IDLE (all ones, marking). The receive half, which follows the    **
** far end's clock, is spi_rx_task.                                      **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#ifndef SPI_TX_TASK_H
#define SPI_TX_TASK_H

#include <stdint.h>
#include "ring_buffer.h"

extern ring_buffer g_tx_buffer;   /**< bit-stuffed bytes waiting for the line */

/**
 * @brief Set up g_tx_buffer. Call once from setup(), before any task starts.
 */
void spi_tx_task_init(void);

/**
 * @brief FreeRTOS task entry point. Runs the transmitter forever.
 *
 * Opens SPI2 as master through the ESP-IDF spi_master driver with DMA, keeps
 * two 64-byte chunks queued, and sleeps on the driver until one finishes;
 * then refills it from g_tx_buffer (idle ones when empty) and queues it
 * again. The clock pauses for the driver's overhead between chunks; an HDLC
 * receiver does not mind, it samples on edges. Never returns.
 *
 * @param[in] params  Unused.
 */
void spi_tx_task_run(void *params);

/**
 * @brief Set the transmit bit rate.
 *
 * The SPI clock is the line clock, so bit/s equals SPI Hz. Takes effect at
 * the next chunk. Safe to call from another task.
 *
 * @param[in] bits_per_second  New rate.
 */
void spi_tx_task_set_bit_rate(uint32_t bits_per_second);

/**
 * @brief Current transmit bit rate.
 * @return bit/s.
 */
uint32_t spi_tx_task_bit_rate(void);

/**
 * @brief Run or stop the transmit clock.
 *
 * With the clock stopped the line is quiet: the clock pin sits at its idle
 * level, TD at 1, no DMA runs. Frames pushed meanwhile wait in g_tx_buffer
 * and go out when the clock starts again (the first thing out is whatever
 * was already queued). Takes effect within two chunk times. Safe to call
 * from another task. Added for bench work with a far end whose receiver is
 * sensitive to a continuous clock (the CLOCK console command).
 *
 * @param[in] enabled  true runs the clock (the default), false stops it.
 */
void spi_tx_task_set_clock_enabled(bool enabled);

/**
 * @brief Whether the transmit clock is running.
 * @return true when running.
 */
bool spi_tx_task_clock_enabled(void);

/**
 * @brief Queue raw bytes for the line, all of them or none.
 *
 * The block enters g_tx_buffer in one locked write, so a frame pushed here
 * can never be split by idle bytes on the line. Blocks (1 ms sleeps) until
 * the whole block fits, so it must be called from a task, never from an
 * interrupt.
 *
 * @param[in] bytes  Line bytes, already bit stuffed if they are a frame.
 * @param[in] n      Number of bytes.
 * @return 0 when queued, -1 when n can never fit (larger than the buffer).
 */
int spi_tx_task_push(const uint8_t *bytes, int n);

/**
 * @brief Is there room in g_tx_buffer for n more bytes right now?
 *
 * For a sender that must not sleep in spi_tx_task_push() (the WiFi task:
 * while it sleeps there it reads nothing from the TCP socket and sends
 * nothing to it, BUGS.md T1/T2). Only that task and the console write the
 * ring, so the room can only grow until the caller pushes.
 *
 * @param[in] n  Bytes the caller wants to push.
 * @return true when a push of n bytes will not wait.
 */
bool spi_tx_task_has_room(int n);

/**
 * @brief Bytes taken from g_tx_buffer and clocked onto the line since boot
 *        (frames and the idle byte after each closing flag).
 *
 * Counted when a chunk is filled, so up to two chunks (128 bytes) ahead of
 * the wire.
 *
 * @return Count since boot; wraps at 2^32.
 */
uint32_t spi_tx_task_data_bytes(void);

/**
 * @brief Idle 0xFF fill bytes clocked because g_tx_buffer was empty.
 *
 * With a full window of frames queued on the host, a growing count here
 * means the host-to-line path, not the line, set the rate.
 *
 * @return Count since boot; wraps at 2^32.
 */
uint32_t spi_tx_task_idle_bytes(void);

#endif /* SPI_TX_TASK_H */
