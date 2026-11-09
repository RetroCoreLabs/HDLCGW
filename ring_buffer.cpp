/**************************************************************************
** RING_BUFFER - byte ring buffer shared between FreeRTOS tasks          **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#include "ring_buffer.h"
#include "hot_path.h"

#include <stdlib.h>

#include <esp_heap_caps.h>

bool ring_buffer_alloc(ring_buffer *rb, int size)
{
    uint8_t *storage = (uint8_t *)heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);

    if (storage == NULL)
    {
        storage = (uint8_t *)malloc((size_t)size);
    }
    if (storage == NULL)
    {
        return false;
    }
    ring_buffer_init(rb, storage, size);
    return true;
}

bool ring_buffer_alloc_internal(ring_buffer *rb, int size)
{
    uint8_t *storage = (uint8_t *)heap_caps_malloc((size_t)size,
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (storage == NULL)
    {
        return false;
    }
    ring_buffer_init(rb, storage, size);
    return true;
}

int ring_buffer_high_water(const ring_buffer *rb)
{
    return rb->high_water;
}

int ring_buffer_size(const ring_buffer *rb)
{
    return rb->size;
}

void ring_buffer_init(ring_buffer *rb, uint8_t *storage, int size)
{
    rb->data = storage;
    rb->size = size;
    rb->mutex = xSemaphoreCreateMutex();
    rb->high_water = 0;
    ring_buffer_clear(rb);
}

void ring_buffer_clear(ring_buffer *rb)
{
    if (xSemaphoreTake(rb->mutex, portMAX_DELAY) == pdTRUE)
    {
        rb->in_ptr = 0;
        rb->out_ptr = 0;
        rb->empty = true;
        rb->full = false;
        xSemaphoreGive(rb->mutex);
    }
}

/**
 * @brief Bytes waiting, computed with the mutex already held.
 * @param[in] rb  Buffer.
 * @return 0 .. size.
 */
static HOT_PATH_CODE int length_locked(const ring_buffer *rb)
{
    if (rb->full)
    {
        return rb->size;
    }
    if (rb->in_ptr >= rb->out_ptr)
    {
        return rb->in_ptr - rb->out_ptr;
    }
    return rb->size - rb->out_ptr + rb->in_ptr;
}

/**
 * @brief Raise high_water to the current fill, mutex already held.
 * @param[in,out] rb  Buffer.
 */
static HOT_PATH_CODE void update_high_water(ring_buffer *rb)
{
    int length = length_locked(rb);

    if (length > rb->high_water)
    {
        rb->high_water = length;
    }
}

void ring_buffer_write(ring_buffer *rb, uint8_t data)
{
    if (xSemaphoreTake(rb->mutex, portMAX_DELAY) == pdTRUE)
    {
        if (!rb->full)
        {
            int next = rb->in_ptr + 1;
            if (next == rb->size)
            {
                next = 0;
            }
            rb->data[rb->in_ptr] = data;
            rb->in_ptr = next;
            rb->empty = false;
            rb->full = (next == rb->out_ptr);
            update_high_water(rb);
        }
        xSemaphoreGive(rb->mutex);
    }
}

bool HOT_PATH_CODE ring_buffer_write_block(ring_buffer *rb, const uint8_t *data, int n)
{
    bool written = false;

    if (n < 0 || n > rb->size - 1)
    {
        return false;
    }

    if (xSemaphoreTake(rb->mutex, portMAX_DELAY) == pdTRUE)
    {
        if (rb->size - length_locked(rb) >= n)
        {
            for (int i = 0; i < n; i++)
            {
                int next = rb->in_ptr + 1;
                if (next == rb->size)
                {
                    next = 0;
                }
                rb->data[rb->in_ptr] = data[i];
                rb->in_ptr = next;
            }
            if (n > 0)
            {
                rb->empty = false;
                rb->full = (rb->in_ptr == rb->out_ptr);
                update_high_water(rb);
            }
            written = true;
        }
        xSemaphoreGive(rb->mutex);
    }
    return written;
}

int HOT_PATH_CODE ring_buffer_write_some(ring_buffer *rb, const uint8_t *data, int n)
{
    int count = 0;

    if (n <= 0)
    {
        return 0;
    }
    if (xSemaphoreTake(rb->mutex, portMAX_DELAY) == pdTRUE)
    {
        int room = rb->size - length_locked(rb);

        count = (n < room) ? n : room;
        for (int i = 0; i < count; i++)
        {
            int next = rb->in_ptr + 1;
            if (next == rb->size)
            {
                next = 0;
            }
            rb->data[rb->in_ptr] = data[i];
            rb->in_ptr = next;
        }
        if (count > 0)
        {
            rb->empty = false;
            rb->full = (rb->in_ptr == rb->out_ptr);
            update_high_water(rb);
        }
        xSemaphoreGive(rb->mutex);
    }
    return count;
}

int HOT_PATH_CODE ring_buffer_read_block(ring_buffer *rb, uint8_t *out, int max)
{
    int count = 0;

    if (max <= 0)
    {
        return 0;
    }
    if (xSemaphoreTake(rb->mutex, portMAX_DELAY) == pdTRUE)
    {
        int length = length_locked(rb);

        count = (max < length) ? max : length;
        for (int i = 0; i < count; i++)
        {
            int next = rb->out_ptr + 1;
            if (next == rb->size)
            {
                next = 0;
            }
            out[i] = rb->data[rb->out_ptr];
            rb->out_ptr = next;
        }
        if (count > 0)
        {
            rb->full = false;
            rb->empty = (rb->out_ptr == rb->in_ptr);
        }
        xSemaphoreGive(rb->mutex);
    }
    return count;
}

uint8_t ring_buffer_read(ring_buffer *rb)
{
    uint8_t out = 0;

    if (xSemaphoreTake(rb->mutex, portMAX_DELAY) == pdTRUE)
    {
        if (!rb->empty)
        {
            int next = rb->out_ptr + 1;
            if (next == rb->size)
            {
                next = 0;
            }
            out = rb->data[rb->out_ptr];
            rb->out_ptr = next;
            rb->full = false;
            rb->empty = (next == rb->in_ptr);
        }
        xSemaphoreGive(rb->mutex);
    }
    return out;
}

bool ring_buffer_is_empty(const ring_buffer *rb)
{
    return rb->empty;
}

bool ring_buffer_is_full(const ring_buffer *rb)
{
    return rb->full;
}

int ring_buffer_length(ring_buffer *rb)
{
    int length = 0;

    if (xSemaphoreTake(rb->mutex, portMAX_DELAY) == pdTRUE)
    {
        length = length_locked(rb);
        xSemaphoreGive(rb->mutex);
    }
    return length;
}
