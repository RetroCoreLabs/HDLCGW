/**************************************************************************
** RING_BUFFER - byte ring buffer shared between FreeRTOS tasks          **
**                                                                       **
** Guarded by a FreeRTOS mutex, so it may only be used from tasks, never **
** from an interrupt handler. Fixed size, given at init; no allocation   **
** after that.                                                           **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * @brief A byte ring buffer over caller-owned storage.
 *
 * All fields are private to ring_buffer.cpp; use the functions below.
 */
typedef struct
{
    uint8_t *data;              /**< storage, size bytes, owned by the caller */
    int size;                   /**< capacity in bytes */
    volatile int in_ptr;        /**< next write position */
    volatile int out_ptr;       /**< next read position */
    volatile bool empty;        /**< true when nothing is waiting */
    volatile bool full;         /**< true when a write would be dropped */
    volatile int high_water;    /**< most bytes ever waiting at once since init */
    SemaphoreHandle_t mutex;    /**< guards every field above */
} ring_buffer;

/**
 * @brief Set up a buffer over caller-owned storage.
 *
 * Creates the mutex. Must run before the scheduler hands the buffer to a
 * second task.
 *
 * @param[out] rb       Buffer to set up; must not be NULL.
 * @param[in]  storage  Backing array of at least size bytes; stays owned by
 *                      the caller and must outlive the buffer.
 * @param[in]  size     Capacity in bytes, at least 2.
 */
void ring_buffer_init(ring_buffer *rb, uint8_t *storage, int size);

/**
 * @brief Allocate the storage and set the buffer up.
 *
 * The storage comes from PSRAM when the board has it (the LOLIN S2 Mini has
 * 2 MB, BOARD_HAS_PSRAM in its board definition) and from the internal heap
 * otherwise. Why not static: internal data RAM on this build is capped near
 * 122 KB and the WiFi stack alone needs 47 KB of it (bench 2026-11-08, link
 * failed with the three 16 KB rings static). A ring is read and written by
 * tasks only, never by DMA or an interrupt, so slower PSRAM is fine for it.
 *
 * @param[out] rb    Buffer to set up; must not be NULL.
 * @param[in]  size  Capacity in bytes, at least 2.
 * @retval true   Ready.
 * @retval false  Out of memory; rb is unusable.
 */
bool ring_buffer_alloc(ring_buffer *rb, int size);

/**
 * @brief Allocate the storage from INTERNAL RAM only and set the buffer up.
 *
 * For the rings every line or USB byte passes through: internal SRAM is
 * reached without the cache, PSRAM through it (Ronny's ruling 2026-11-09,
 * see hot_path.h). No fallback to PSRAM: a hot ring that silently landed in
 * PSRAM is the thing this function exists to prevent, so it fails instead.
 *
 * @param[out] rb    Buffer to set up; must not be NULL.
 * @param[in]  size  Capacity in bytes, at least 2.
 * @retval true   Ready.
 * @retval false  Not enough internal RAM; rb is unusable.
 */
bool ring_buffer_alloc_internal(ring_buffer *rb, int size);

/**
 * @brief The most bytes that were ever waiting in the buffer at once.
 *
 * Measures how much of the ring the traffic really needs; reported by STAT.
 *
 * @param[in] rb  Buffer; must not be NULL.
 * @return 0 .. size.
 */
int ring_buffer_high_water(const ring_buffer *rb);

/**
 * @brief Capacity in bytes, as given at init.
 * @param[in] rb  Buffer; must not be NULL.
 * @return size.
 */
int ring_buffer_size(const ring_buffer *rb);

/**
 * @brief Drop everything in the buffer.
 * @param[in,out] rb  Buffer; must not be NULL.
 */
void ring_buffer_clear(ring_buffer *rb);

/**
 * @brief Append one byte.
 *
 * Blocks only for the mutex. When the buffer is full the byte is silently
 * dropped; check ring_buffer_is_full() first when that matters.
 *
 * @param[in,out] rb    Buffer; must not be NULL.
 * @param[in]     data  Byte to append.
 */
void ring_buffer_write(ring_buffer *rb, uint8_t data);

/**
 * @brief Append n bytes as one unit, or nothing.
 *
 * Holds the mutex for the whole block, so a reader never sees part of it.
 * This is what keeps a bit-stuffed frame whole on the way to the line: a
 * reader that found the buffer empty halfway through a frame would send
 * idle ones in the middle of it, which is an HDLC abort (seen on the bench
 * 2026-11-08).
 *
 * @param[in,out] rb    Buffer; must not be NULL.
 * @param[in]     data  Bytes to append.
 * @param[in]     n     Number of bytes, at most size - 1.
 * @return true when all n bytes were appended, false when there was not
 *         enough room (nothing is written then) or n does not fit at all.
 */
bool ring_buffer_write_block(ring_buffer *rb, const uint8_t *data, int n);

/**
 * @brief Append as many of n bytes as fit, under one lock.
 *
 * One mutex round per call instead of one per byte: per-byte writes cost the
 * line receive task about 3.4 us a byte at 983000 (TASK run time, bench
 * 2026-11-09), most of it in the lock.
 *
 * @param[in,out] rb    Buffer; must not be NULL.
 * @param[in]     data  Bytes to append.
 * @param[in]     n     Number of bytes.
 * @return Bytes appended, 0..n; the caller counts the rest as lost.
 */
int ring_buffer_write_some(ring_buffer *rb, const uint8_t *data, int n);

/**
 * @brief Take up to max of the oldest bytes, under one lock.
 * @param[in,out] rb   Buffer; must not be NULL.
 * @param[out]    out  At least max bytes.
 * @param[in]     max  Most bytes to take.
 * @return Bytes taken, 0 when the buffer is empty.
 */
int ring_buffer_read_block(ring_buffer *rb, uint8_t *out, int max);

/**
 * @brief Take the oldest byte.
 * @param[in,out] rb  Buffer; must not be NULL.
 * @return The byte, or 0 when the buffer is empty. Check
 *         ring_buffer_is_empty() first to tell the two apart.
 */
uint8_t ring_buffer_read(ring_buffer *rb);

/**
 * @brief Whether nothing is waiting.
 * @param[in] rb  Buffer; must not be NULL.
 * @return true when empty. Read without the mutex; a stale answer is
 *         possible when another task writes at the same moment.
 */
bool ring_buffer_is_empty(const ring_buffer *rb);

/**
 * @brief Whether the next write would be dropped.
 * @param[in] rb  Buffer; must not be NULL.
 * @return true when full. Read without the mutex, see ring_buffer_is_empty().
 */
bool ring_buffer_is_full(const ring_buffer *rb);

/**
 * @brief Number of bytes waiting.
 * @param[in,out] rb  Buffer; must not be NULL (takes the mutex).
 * @return 0 .. size.
 */
int ring_buffer_length(ring_buffer *rb);

#endif /* RING_BUFFER_H */
