/**************************************************************************
** SPI_TX_TASK - transmit half of the synchronous line                   **
**                                                                       **
** Pins are the LOLIN S2 Mini SPI defaults from the board's               **
** pins_arduino.h (esp32 core 3.3.12, variants/lolin_s2_mini). SPI2_HOST **
** is the SPI2 (GPSPI2, "FSPI") peripheral on the ESP32-S2.              **
**   PIN_TX_CLOCK GPIO 7  (SCK)  -> transmit clock to the far end        **
**   PIN_TX_DATA  GPIO 11 (MOSI) -> TD, the data we transmit             **
** No chip select, no MISO: the receive half has its own peripheral and  **
** its own clock (spi_rx_task.cpp).                                      **
**                                                                       **
** Why the ESP-IDF driver and not the Arduino SPIClass: SPIClass         **
** busy-waits for every transfer, so a task that keeps the line clocking **
** never blocks and starves every task below its priority. Seen on the   **
** bench 2026-11-08: the console task (priority 2) never got a turn and  **
** the board was silent. spi_device_queue_trans() lets the DMA clock a   **
** chunk out while this task sleeps on the driver's queue; two chunks in **
** flight keep the gap between them to the driver's own overhead.        **
** data_io_default_level = true parks TD at 1 (marking) in that gap.     **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#include <esp_heap_caps.h>
#include "spi_tx_task.h"
#include "libhdlc/hot_path.h"

#include <string.h>

#include <Arduino.h>
#include <driver/spi_master.h>

#include "console_out.h"
#include "libhdlc/hdlc.h"

#define PIN_TX_CLOCK SCK    /* 7 */
#define PIN_TX_DATA  MOSI   /* 11 */

/* Bytes per DMA transaction: about CHUNK_MS of line time at the current bit
 * rate, between CHUNK_MIN and CHUNK_MAX, a multiple of 4. Two are queued, so
 * a frame pushed into g_tx_buffer starts after at most two chunk times.
 * Until 2026-11-09 a chunk was a fixed 64 bytes: at 983000 that is 0.52 ms,
 * about 1900 driver transactions a second, and the task took 12.6 percent of
 * the CPU while sending mostly idle ones; with 10-byte frames both ways the
 * CPU had no headroom left for decoding plus USB (BUGS.md B4). At 9600 the
 * old 64 bytes held the line 107 ms ahead; now it is about 13 ms. */
#define CHUNK_MS 2u
#define CHUNK_MIN 8
#define CHUNK_MAX 512
#define CHUNKS_IN_FLIGHT 2

/* 4 KB: three maximum-size stuffed frames from the host can wait here
 * before spi_tx_task_push() has to block console_task, which then stops
 * reading USB and so holds the host back; nothing is lost. Internal RAM,
 * not PSRAM, because every transmitted byte passes through it
 * (hot_path.h); it was 16 KB in PSRAM until 2026-11-09. STAT shows how full
 * it ever got (high water). */
#define BUFFER_SIZE 4096

/* DMA source buffers, CHUNK_MAX bytes each, from DMA-capable internal heap
 * (static RAM is full, see CLAUDE.md). Allocated once in spi_tx_task_run. */
static uint8_t *g_chunk[CHUNKS_IN_FLIGHT];
/* Bytes per transaction at the current rate, see CHUNK_MS. */
static int g_chunk_len = CHUNK_MIN;
static spi_transaction_t g_trans[CHUNKS_IN_FLIGHT];

ring_buffer g_tx_buffer;    /* storage allocated in init, see ring_buffer_alloc() */

static volatile uint32_t g_bit_rate = 38400;
static volatile bool g_clock_enabled = true;
static spi_device_handle_t g_device = NULL;

/* Bytes clocked onto the line since boot, split by origin. Written only by
 * spi_tx_task, read by any task; a 32-bit aligned store is atomic on this
 * core, so no lock. Added 2026-11-09: two-board throughput stayed near
 * 26 kbit/s at every rate from 38400 up, and these say whether the ESP's
 * line ran on data or on idle fill while the host had a full window queued. */
static volatile uint32_t g_tx_data_bytes = 0;
static volatile uint32_t g_tx_idle_bytes = 0;

void spi_tx_task_init(void)
{
    if (!ring_buffer_alloc_internal(&g_tx_buffer, BUFFER_SIZE))
    {
        /* Without the ring nothing can be sent; say so and stop here rather
         * than run with a buffer that does not exist. */
        Serial.println("spi_tx_task: no internal RAM for the transmit ring");
        while (true)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void spi_tx_task_set_bit_rate(uint32_t bits_per_second)
{
    g_bit_rate = bits_per_second;
}

uint32_t spi_tx_task_bit_rate(void)
{
    return g_bit_rate;
}

void spi_tx_task_set_clock_enabled(bool enabled)
{
    g_clock_enabled = enabled;
}

bool spi_tx_task_clock_enabled(void)
{
    return g_clock_enabled;
}

uint32_t spi_tx_task_data_bytes(void)
{
    return g_tx_data_bytes;
}

uint32_t spi_tx_task_idle_bytes(void)
{
    return g_tx_idle_bytes;
}

int spi_tx_task_push(const uint8_t *bytes, int n)
{
    if (n > BUFFER_SIZE - 1)
    {
        return -1;
    }

    /* One locked write for the whole block: the transmit task must never
     * find the ring empty in the middle of a frame, or it would clock idle
     * ones out mid-frame, which the far end reads as an abort. */
    while (!ring_buffer_write_block(&g_tx_buffer, bytes, n))
    {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return 0;
}

bool spi_tx_task_has_room(int n)
{
    /* The same test ring_buffer_write_block() makes. */
    return ring_buffer_size(&g_tx_buffer) - ring_buffer_length(&g_tx_buffer) >= n;
}

/**
 * @brief Attach SPI2 to the clock and data pins with DMA.
 * @return ESP_OK, or the error from spi_bus_initialize().
 */
static esp_err_t open_bus(void)
{
    spi_bus_config_t bus;

    memset(&bus, 0, sizeof(bus));
    bus.mosi_io_num = PIN_TX_DATA;
    bus.miso_io_num = -1;
    bus.sclk_io_num = PIN_TX_CLOCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.data_io_default_level = true;   /* TD = 1 between transactions */
    bus.max_transfer_sz = CHUNK_MAX;
    return spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
}

/**
 * @brief Create the SPI device that defines clock rate, mode and bit order.
 *
 * Removes the previous device first, so this is also how a new bit rate is
 * applied. Caller must have no transaction in flight.
 *
 * @param[in] bits_per_second  SPI clock = line bit rate.
 * @return ESP_OK, or the error from the driver.
 */
static esp_err_t open_device(uint32_t bits_per_second)
{
    spi_device_interface_config_t dev;
    esp_err_t err;

    if (g_device != NULL)
    {
        err = spi_bus_remove_device(g_device);
        g_device = NULL;
        if (err != ESP_OK)
        {
            return err;
        }
    }

    /* Chunk length for this rate: CHUNK_MS of line time, rounded down to a
     * multiple of 4 (whole DMA words). */
    uint32_t len = (bits_per_second / 8u) * CHUNK_MS / 1000u;
    len = len & ~3u;
    if (len < (uint32_t)CHUNK_MIN)
    {
        len = CHUNK_MIN;
    }
    if (len > (uint32_t)CHUNK_MAX)
    {
        len = CHUNK_MAX;
    }
    g_chunk_len = (int)len;

    memset(&dev, 0, sizeof(dev));
    dev.clock_speed_hz = (int)bits_per_second;
    dev.mode = 0;                           /* same as the receive side */
    dev.spics_io_num = -1;                  /* no chip select on an HDLC line */
    dev.flags = SPI_DEVICE_BIT_LSBFIRST;    /* HDLC coder puts first wire bit in bit 0 */
    dev.queue_size = CHUNKS_IN_FLIGHT;
    return spi_bus_add_device(SPI2_HOST, &dev, &g_device);
}

/**
 * @brief Fill one chunk from g_tx_buffer, idle ones when it runs empty.
 * @param[out] chunk  At least len bytes to send.
 * @param[in]  len    Bytes in this chunk (g_chunk_len).
 */
static HOT_PATH_CODE void fill_chunk(uint8_t *chunk, int len)
{
    /* One locked block read per chunk (per-byte reads cost a mutex round
     * each; bench 2026-11-09). Nothing to send still clocks idle ones out,
     * so the far end sees a marking line with a clock, not a dead one. */
    int n = ring_buffer_read_block(&g_tx_buffer, chunk, len);

    for (int i = n; i < len; i++)
    {
        chunk[i] = (uint8_t)HDLC_IDLE;
    }
    g_tx_data_bytes = g_tx_data_bytes + (uint32_t)n;
    g_tx_idle_bytes = g_tx_idle_bytes + (uint32_t)(len - n);
}

/**
 * @brief Fill chunk number i and hand it to the driver queue.
 * @param[in] i  Index into g_chunk / g_trans.
 * @return ESP_OK, or the error from spi_device_queue_trans().
 */
static esp_err_t queue_chunk(int i)
{
    int len = g_chunk_len;

    fill_chunk(g_chunk[i], len);
    memset(&g_trans[i], 0, sizeof(g_trans[i]));
    g_trans[i].length = (size_t)len * 8u;   /* in bits */
    g_trans[i].tx_buffer = g_chunk[i];
    return spi_device_queue_trans(g_device, &g_trans[i], portMAX_DELAY);
}

/**
 * @brief Wait for every queued chunk to finish. Used before a rate change.
 * @param[in] pending  Number of chunks in flight.
 */
static void drain(int pending)
{
    spi_transaction_t *done;

    for (int i = 0; i < pending; i++)
    {
        (void)spi_device_get_trans_result(g_device, &done, portMAX_DELAY);
    }
}

/**
 * @brief Print a driver error and park the task; the line stays dead.
 * @param[in] what  Which call failed.
 * @param[in] err   Its return code.
 */
static void fail_forever(const char *what, esp_err_t err)
{
    console_out_log(CONTROL_FRAME_LOG_ERROR, "spi_tx_task: %s failed: %s", what,
                    esp_err_to_name(err));
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief Queue every chunk; called at start and after a rate change.
 */
static void queue_all(void)
{
    for (int i = 0; i < CHUNKS_IN_FLIGHT; i++)
    {
        esp_err_t err = queue_chunk(i);
        if (err != ESP_OK)
        {
            fail_forever("spi_device_queue_trans", err);
        }
    }
}

void spi_tx_task_run(void *params)
{
    uint32_t rate = g_bit_rate;
    spi_transaction_t *done;
    esp_err_t err;

    (void)params;

    for (int i = 0; i < CHUNKS_IN_FLIGHT; i++)
    {
        g_chunk[i] = (uint8_t *)heap_caps_aligned_alloc(4, CHUNK_MAX,
                                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (g_chunk[i] == NULL)
        {
            fail_forever("heap_caps_aligned_alloc (DMA chunk)", ESP_ERR_NO_MEM);
        }
    }

    err = open_bus();
    if (err != ESP_OK)
    {
        fail_forever("spi_bus_initialize", err);
    }
    err = open_device(rate);
    if (err != ESP_OK)
    {
        fail_forever("spi_bus_add_device", err);
    }
    queue_all();

    while (true)
    {
        /* Blocks here while the DMA clocks the oldest chunk out. */
        err = spi_device_get_trans_result(g_device, &done, portMAX_DELAY);
        if (err != ESP_OK)
        {
            fail_forever("spi_device_get_trans_result", err);
        }

        if (g_bit_rate != rate)
        {
            drain(CHUNKS_IN_FLIGHT - 1);
            rate = g_bit_rate;
            err = open_device(rate);
            if (err != ESP_OK)
            {
                fail_forever("spi_bus_add_device", err);
            }
            queue_all();
            continue;
        }

        if (!g_clock_enabled)
        {
            /* CLOCK OFF: let the queued chunks finish, then leave the clock
             * at its idle level and TD at 1 until CLOCK ON. Frames pushed
             * meanwhile wait in g_tx_buffer. */
            drain(CHUNKS_IN_FLIGHT - 1);
            while (!g_clock_enabled)
            {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            queue_all();
            continue;
        }

        err = queue_chunk((int)(done - &g_trans[0]));
        if (err != ESP_OK)
        {
            fail_forever("spi_device_queue_trans", err);
        }
    }
}
