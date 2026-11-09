/**************************************************************************
** USB_TX_TASK - binary output to the host, decoupled from everything   **
**               else                                                    **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#include "usb_tx_task.h"
#include "hot_path.h"

#include <Arduino.h>
#include <esp32-hal-tinyusb.h>
#include <esp_timer.h>   /* tud_cdc_n_connected(): the test USBCDC::write uses */

/* 4 KB holds three maximum-size frames: at 983 kbit/s that is about 33 ms
 * of line traffic for the USB side to catch up in; longer host stalls drop
 * whole frames and count them. Internal RAM, not PSRAM, because every byte
 * to the host passes through it (hot_path.h); it was 16 KB in PSRAM until
 * 2026-11-09. STAT shows how full it ever got (high water).
 *
 * Tried at 8 KB on 2026-11-09 (BUGS.md B4, 10-byte frames at 983000 dropped
 * at a full ring): 8 KB filled as well, because the cause was CPU, not ring
 * size. After the line TX chunks were sized by rate (spi_tx_task.cpp) the
 * same runs lost nothing and this ring's high water was 1322 bytes, so it is
 * 4 KB again and the internal heap keeps the other 4 KB. */
#define BUFFER_SIZE 4096

/* CONFIG_TINYUSB_CDC_TX_BUFSIZE is 64 in the core's build; writing more per
 * call only makes USBCDC::write loop internally. */
#define PIECE 64

ring_buffer g_usb_tx_buffer;    /* storage allocated in init, see ring_buffer_alloc() */

static volatile uint32_t g_dropped = 0;

/* Pieces the core's USBCDC::write took only in part while the host had the
 * port open: it gives up after its 250 ms tx timeout (USBCDC.cpp, core
 * 3.3.12, write loop) when the host stops reading. Until 2026-11-09 the
 * untaken tail of the piece was thrown away, so a frame reached the host
 * with bytes missing: the PC saw frames with a bad FCS that the ESP had
 * checked good (two-board sweep at 983000, the PC not reading this port for
 * long stretches). Now the tail is kept and written again; this counts the
 * pieces that needed it, once each. */
static volatile uint32_t g_short_writes = 0;

/* Times the queued output was thrown away because no host had the port
 * open. Whole frames only: see usb_tx_task_write_all(). */
static volatile uint32_t g_host_closed_flushes = 0;

void usb_tx_task_init(void)
{
    if (!ring_buffer_alloc_internal(&g_usb_tx_buffer, BUFFER_SIZE))
    {
        Serial.println("usb_tx_task: no internal RAM for the USB ring");
        while (true)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

bool usb_tx_task_push(const uint8_t *bytes, int n)
{
    if (!ring_buffer_write_block(&g_usb_tx_buffer, bytes, n))
    {
        g_dropped = g_dropped + 1u;     /* ++ on a volatile is deprecated in C++20 */
        return false;
    }
    return true;
}

uint32_t usb_tx_task_dropped(void)
{
    return g_dropped;
}

uint32_t usb_tx_task_short_writes(void)
{
    return g_short_writes;
}

uint32_t usb_tx_task_host_closed_flushes(void)
{
    return g_host_closed_flushes;
}

/** Microseconds spent inside Serial.write since boot (wraps after 71 min;
 *  read it as a delta). Added 2026-11-09 to find where the time goes when
 *  the PC writes and reads at full speed at once. */
static volatile uint32_t g_write_us = 0;
/** Bytes Serial.write accepted since boot. */
static volatile uint32_t g_written_bytes = 0;

/** A write of one piece (at most 64 bytes) slower than this is counted as a
 *  stall. 4 KB of ring is about 40 ms of 10-byte-frame traffic at 983000,
 *  so a 20 ms stall already uses half of it (bench 2026-11-09, BUGS.md B4). */
#define WRITE_SLOW_US 20000u
/** 1 ms sleeps because a write took 0 bytes, and because the ring was empty,
 *  since boot (2026-11-09, BUGS.md B4: with the ring full the idle task
 *  still got 13.8 percent, so this task was waiting somewhere). */
static volatile uint32_t g_zero_write_sleeps = 0;
/** See g_zero_write_sleeps. */
static volatile uint32_t g_empty_sleeps = 0;

/**
 * @brief 1 ms sleeps after a USB write that took nothing, since boot.
 * @return The count.
 */
uint32_t usb_tx_task_zero_write_sleeps(void)
{
    return g_zero_write_sleeps;
}

/**
 * @brief 1 ms sleeps with the USB TX ring empty, since boot.
 * @return The count.
 */
uint32_t usb_tx_task_empty_sleeps(void)
{
    return g_empty_sleeps;
}

/** A write slower than this counts toward a slow run (see below). */
#define SLOW_RUN_WRITE_US 1000u
/** Length of the current run of back-to-back writes over 1 ms each. */
static uint32_t g_slow_run_us = 0;
/** Longest such run since boot, microseconds (2026-11-09, BUGS.md B4: tells
 *  "the host stopped fetching for a long stretch" from "the console hands
 *  frames over in lumps"). */
static volatile uint32_t g_slow_run_max_us = 0;

/**
 * @brief Longest run of back-to-back USB writes over 1 ms each, since boot.
 * @return Microseconds.
 */
uint32_t usb_tx_task_slow_run_max_us(void)
{
    return g_slow_run_max_us;
}

/** Longest single USBCDC::write call since boot, microseconds. */
static volatile uint32_t g_write_max_us = 0;
/** USBCDC::write calls longer than WRITE_SLOW_US since boot. */
static volatile uint32_t g_writes_slow = 0;

/**
 * @brief Longest single USB CDC write call since boot.
 * @return Microseconds.
 */
uint32_t usb_tx_task_write_max_us(void)
{
    return g_write_max_us;
}

/**
 * @brief USB CDC write calls longer than 20 ms since boot.
 * @return The count.
 */
uint32_t usb_tx_task_writes_slow(void)
{
    return g_writes_slow;
}

/**
 * @brief Microseconds spent inside Serial.write since boot.
 * @return The count; wraps, read as a delta.
 */
uint32_t usb_tx_task_write_us(void)
{
    return g_write_us;
}

/**
 * @brief Bytes the USB CDC write accepted since boot.
 * @return The count.
 */
uint32_t usb_tx_task_written_bytes(void)
{
    return g_written_bytes;
}

/**
 * @brief Write one piece to the USB port completely, however long the host
 *        takes, unless no host has the port open.
 *
 * A slow host: the part the core did not take is written again, with a 1 ms
 * sleep when it took nothing, so the task never spins; meanwhile
 * g_usb_tx_buffer fills and usb_tx_task_push() drops whole frames and counts
 * them. The stream to the host stays made of whole frames.
 *
 * No host (the port is closed, so the core takes nothing at all): the rest of
 * the piece AND everything queued in g_usb_tx_buffer are thrown away and
 * counted once. The ring only ever holds whole frames (one locked write each),
 * so after the clear the next byte the host can get is the opening flag of a
 * whole frame. Without this the ring would hand up to 16 KB of stale frames
 * to the next program that opens the port.
 *
 * @param[in] piece  Bytes to write.
 * @param[in] n      Number of bytes, 1..PIECE.
 */
static void usb_tx_task_write_all(const uint8_t *piece, int n)
{
    int done = 0;
    bool counted = false;

    while (done < n)
    {
        size_t sent;

        /* The same test USBCDC::write makes (DTR set, USBCDC.cpp core 3.3.12),
         * NOT Serial's bool, which needs DTR AND RTS: a host that opens the
         * port with RTS off is still served by write(), and testing the bool
         * here threw its replies away (bench 2026-11-09: no HELLO reply). */
        if (!tud_cdc_n_connected(0))
        {
            ring_buffer_clear(&g_usb_tx_buffer);
            g_host_closed_flushes = g_host_closed_flushes + 1u;
            return;
        }
        int64_t t0 = esp_timer_get_time();
        sent = Serial.write(piece + done, (size_t)(n - done));
        uint32_t took = (uint32_t)(esp_timer_get_time() - t0);
        g_write_us = g_write_us + took;
        if (took > g_write_max_us)
        {
            g_write_max_us = took;
        }
        if (took > WRITE_SLOW_US)
        {
            g_writes_slow = g_writes_slow + 1u;
        }
        /* A run of writes that each took over 1 ms: a 64-byte packet takes
         * about 0.46 ms when the host keeps asking, so a run like this means
         * the host is not fetching. Its total length is what the ring must
         * cover. */
        if (took > SLOW_RUN_WRITE_US)
        {
            g_slow_run_us = g_slow_run_us + took;
            if (g_slow_run_us > g_slow_run_max_us)
            {
                g_slow_run_max_us = g_slow_run_us;
            }
        }
        else
        {
            g_slow_run_us = 0;
        }
        g_written_bytes = g_written_bytes + (uint32_t)sent;
        if ((int)sent < n - done && !counted)
        {
            g_short_writes = g_short_writes + 1u;
            counted = true;
        }
        done += (int)sent;
        if (sent == 0)
        {
            g_zero_write_sleeps = g_zero_write_sleeps + 1u;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

/**
 * @brief Take up to PIECE bytes from g_usb_tx_buffer.
 *
 * Its own function so the per-byte loop runs from internal RAM
 * (hot_path.h); inside usb_tx_task_run it would run from flash.
 *
 * @param[out] piece  At least PIECE bytes.
 * @return Number of bytes taken, 0 when the ring is empty.
 */
static HOT_PATH_CODE int usb_tx_task_fill_piece(uint8_t *piece)
{
    /* One locked block read per piece, not one mutex round per byte. */
    return ring_buffer_read_block(&g_usb_tx_buffer, piece, PIECE);
}

void usb_tx_task_run(void *params)
{
    uint8_t piece[PIECE];

    (void)params;

    while (true)
    {
        int n = usb_tx_task_fill_piece(piece);

        if (n == 0)
        {
            g_empty_sleeps = g_empty_sleeps + 1u;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        /* Blocks until the host has taken every byte of the piece. */
        usb_tx_task_write_all(piece, n);
    }
}
