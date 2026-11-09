/**************************************************************************
** SPI_RX_TASK - receive half of the synchronous line                    **
**                                                                       **
** The FAR END supplies the receive clock. The SPI3 peripheral runs as a **
** slave following that clock, with DMA filling a ring of buffers; this  **
** task copies the filled buffers into g_rx_buffer for the HDLC decoder. **
**                                                                       **
** The stock ESP-IDF slave driver cannot do this: it works in            **
** transactions that start when software queues one and end when chip   **
** select rises, and an HDLC line has no chip select. So this file       **
** programs the peripheral through the low-level register layer          **
** (hal/spi_ll.h) instead. See spi_rx_task.cpp for the register facts    **
** and the reference manual sections they come from.                     **
**                                                                       **
** STATUS: verified on the bench 2026-11-08 with GPIO 7 wired to GPIO 12 **
** and GPIO 11 to GPIO 9 on the same board: 24 of 24 frames received     **
** with a good FCS at 9600, 38400 and 983000 bit/s, no FIFO overruns,    **
** no DMA errors. Repeat: open the debug console, type HDLC then F, and  **
** expect "HDLC frame, 24 bytes, FCS good".                              **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#ifndef SPI_RX_TASK_H
#define SPI_RX_TASK_H

#include <stdint.h>
#include "libhdlc/ring_buffer.h"

extern ring_buffer g_rx_buffer;   /**< raw bytes read from the line */

/** @brief Counters for the bench and the STAT command. */
typedef struct
{
    uint32_t bytes;         /**< bytes copied out of the DMA ring */
    uint32_t idle_dropped;  /**< repeated 0xFF idle bytes not copied */
    uint32_t fifo_full;     /**< DMA input FIFO overruns (data lost) */
    uint32_t dma_errors;    /**< DMA in_err_eof events */
    uint32_t overflow;      /**< bytes dropped because g_rx_buffer was full */
    uint32_t peeked;        /**< words taken from memory before the DMA closed
                                 their descriptor (far end stopped its clock) */
    uint32_t pass_gap_max_us;  /**< longest time between two passes over the ring
                                    (2026-11-09, BUGS.md T3: a gap longer than the
                                    ring lets the DMA lap the task unseen) */
    uint32_t pass_gaps_over;   /**< passes more than SPI_RX_GAP_WARN_US apart */
    uint32_t desc_max;         /**< most descriptors taken in one pass (DESC_COUNT
                                    or near it means the DMA may have lapped) */
} spi_rx_task_stats;

/** A gap between receive passes longer than this is counted (20 ms: under
 *  half the DMA ring at 307200, a sixth of it at 983000... see
 *  spi_rx_task.cpp for the ring size). */
#define SPI_RX_GAP_WARN_US 20000u

/**
 * @brief Set up g_rx_buffer. Call once from setup(), before any task starts.
 */
void spi_rx_task_init(void);

/**
 * @brief FreeRTOS task entry point. Runs the receiver forever.
 *
 * Programs SPI3 as a slave with a circular DMA ring, then loops every 1 ms:
 * copies every DMA buffer the hardware has handed back into g_rx_buffer and
 * returns it to the hardware. Never returns.
 *
 * @param[in] params  Unused.
 */
void spi_rx_task_run(void *params);

/**
 * @brief Snapshot of the receive counters.
 * @param[out] out  Receives the counters; must not be NULL.
 */
void spi_rx_task_get_stats(spi_rx_task_stats *out);

/**
 * @brief Print the DMA ring for the RXDMA console command: the slave's
 *        received-byte counter, the raw DMA interrupt flags, which descriptor
 *        the task expects next, and every descriptor's owner, length and
 *        buffer contents. Bench tool for seeing where line bytes went when a
 *        far end stops its clock mid-buffer.
 */
void spi_rx_task_print_dma(void);

#endif /* SPI_RX_TASK_H */
