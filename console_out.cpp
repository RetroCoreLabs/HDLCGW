/**************************************************************************
** CONSOLE_OUT - the one place text for the host leaves the firmware     **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#include "console_out.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "console_task.h"
#include "spi_rx_task.h"
#include "spi_tx_task.h"
#include "usb_tx_task.h"
#include "wifi_task.h"

uint8_t g_console_out_log_level = CONTROL_FRAME_LOG_OFF;
uint16_t g_console_out_stat_period_ms = 0;

/* The reply being collected. Only console_task writes replies (commands run
 * there), so one set of state is enough. */
static bool g_in_reply = false;
static uint8_t g_reply_id = 0;
static uint8_t g_chunk_counter = 0;
static uint8_t g_chunk[CONSOLE_OUT_CHUNK];
static int g_chunk_len = 0;

static console_out_stats g_stats;

/* Scratch for building and stuffing one frame. Too much for the stack of a
 * 4 KB line task that wants to log an error, so static and guarded; any task
 * may call console_out_send_frame(). Sized for the longest frame this module
 * sends (a full reply chunk), not for BYTE_FRAME_MAX: static RAM is tight on
 * this chip (the first build with 1 KB + 2 KB here overflowed the data
 * region by 1232 bytes, 2026-11-08). */
#define CONSOLE_OUT_MAX_FRAME (CONSOLE_OUT_CHUNK + CONTROL_FRAME_OVERHEAD)
static SemaphoreHandle_t g_frame_mutex = NULL;
static uint8_t g_frame[CONSOLE_OUT_MAX_FRAME];
static uint8_t g_stuffed[2 + 2 * CONSOLE_OUT_MAX_FRAME];

void console_out_init(void)
{
    g_console_out_log_level = CONTROL_FRAME_LOG_OFF;
    g_console_out_stat_period_ms = 0;
    g_in_reply = false;
    g_chunk_len = 0;
    memset(&g_stats, 0, sizeof(g_stats));
    if (g_frame_mutex == NULL)
    {
        g_frame_mutex = xSemaphoreCreateMutex();
    }
}

bool console_out_send_frame(uint8_t type, uint8_t id, uint8_t flags, const uint8_t *payload,
                            int payload_len)
{
    bool ok;
    int n;

    if (payload_len > CONSOLE_OUT_CHUNK)
    {
        g_stats.dropped++;
        return false;
    }
    if (g_frame_mutex == NULL || xSemaphoreTake(g_frame_mutex, pdMS_TO_TICKS(10)) != pdTRUE)
    {
        g_stats.dropped++;
        return false;
    }
    n = control_frame_build(type, id, flags, payload, payload_len, g_frame);
    if (n < 0)
    {
        ok = false;
    }
    else
    {
        n = byte_frame_encode(g_frame, n, g_stuffed);
        ok = usb_tx_task_push(g_stuffed, n);
    }
    xSemaphoreGive(g_frame_mutex);
    if (!ok)
    {
        g_stats.dropped++;
    }
    return ok;
}

/**
 * @brief Send the collected chunk as one REPLY frame and empty it.
 * @param[in] last   Set the LAST flag.
 * @param[in] error  Set the ERROR flag.
 */
static void console_out_send_chunk(bool last, bool error)
{
    uint8_t flags = (uint8_t)(g_chunk_counter << CONTROL_FRAME_CHUNK_SHIFT);

    if (last)
    {
        flags |= CONTROL_FRAME_FLAG_LAST;
    }
    if (error)
    {
        flags |= CONTROL_FRAME_FLAG_ERROR;
    }
    if (console_out_send_frame(CONTROL_FRAME_REPLY, g_reply_id, flags, g_chunk, g_chunk_len))
    {
        g_stats.replies++;
    }
    g_chunk_counter = (uint8_t)((g_chunk_counter + 1u) & 0x0Fu);
    g_chunk_len = 0;
}

/**
 * @brief Append text to the current sink.
 * @param[in] s  Bytes.
 * @param[in] n  Number of bytes.
 */
static void console_out_write(const char *s, int n)
{
    if (!g_in_reply)
    {
        Serial.write((const uint8_t *)s, (size_t)n);
        return;
    }
    while (n > 0)
    {
        int room = CONSOLE_OUT_CHUNK - g_chunk_len;
        int take = (n < room) ? n : room;

        memcpy(&g_chunk[g_chunk_len], s, (size_t)take);
        g_chunk_len += take;
        s += take;
        n -= take;
        if (g_chunk_len == CONSOLE_OUT_CHUNK)
        {
            console_out_send_chunk(false, false);
        }
    }
}

void console_out_print(const char *s)
{
    console_out_write(s, (int)strlen(s));
}

void console_out_println(const char *s)
{
    console_out_write(s, (int)strlen(s));
    console_out_write("\r\n", 2);
}

void console_out_printf(const char *fmt, ...)
{
    char text[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        return;
    }
    if (n >= (int)sizeof(text))
    {
        n = (int)sizeof(text) - 1;
    }
    console_out_write(text, n);
}

void console_out_reply_begin(uint8_t id)
{
    g_in_reply = true;
    g_reply_id = id;
    g_chunk_counter = 0;
    g_chunk_len = 0;
}

void console_out_reply_end(bool error)
{
    if (!g_in_reply)
    {
        return;
    }
    console_out_send_chunk(true, error);
    g_in_reply = false;
}

bool console_out_in_reply(void)
{
    return g_in_reply;
}

void console_out_log(uint8_t level, const char *fmt, ...)
{
    char text[200];
    va_list ap;
    int n;

    if (g_console_mode == CONSOLE_DEBUG)
    {
        /* A person is watching: show what the console always showed (up to
         * DEBUG), and TRACE too if asked for. */
        uint8_t limit = (g_console_out_log_level > CONTROL_FRAME_LOG_DEBUG)
                            ? g_console_out_log_level : CONTROL_FRAME_LOG_DEBUG;
        if (level > limit)
        {
            return;
        }
        va_start(ap, fmt);
        n = vsnprintf(text, sizeof(text), fmt, ap);
        va_end(ap);
        if (n < 0)
        {
            return;
        }
        Serial.print(text);
        Serial.print("\r\n");
        return;
    }

    if (level > g_console_out_log_level)
    {
        return;
    }

    /* LOG payload: level, millis since boot (u32 LE), text. */
    uint8_t payload[5 + sizeof(text)];
    uint32_t now = millis();

    va_start(ap, fmt);
    n = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        return;
    }
    if (n >= (int)sizeof(text))
    {
        n = (int)sizeof(text) - 1;
    }
    payload[0] = level;
    payload[1] = (uint8_t)(now & 0xFFu);
    payload[2] = (uint8_t)((now >> 8u) & 0xFFu);
    payload[3] = (uint8_t)((now >> 16u) & 0xFFu);
    payload[4] = (uint8_t)(now >> 24u);
    memcpy(&payload[5], text, (size_t)n);
    if (console_out_send_frame(CONTROL_FRAME_LOG, 0, 0, payload, 5 + n))
    {
        g_stats.logs++;
    }
}

void console_out_event(uint8_t code, const char *text)
{
    if (text == NULL)
    {
        text = "";
    }
    if (g_console_mode == CONSOLE_DEBUG)
    {
        Serial.printf("[event %u] %s\r\n", (unsigned)code, text);
        return;
    }

    uint8_t payload[1 + 200];
    int n = (int)strlen(text);

    if (n > 200)
    {
        n = 200;
    }
    payload[0] = code;
    memcpy(&payload[1], text, (size_t)n);
    (void)console_out_send_frame(CONTROL_FRAME_EVENT, 0, 0, payload, 1 + n);
}

/**
 * @brief Store one u32 little-endian.
 * @param[out] p  Four bytes.
 * @param[in]  v  Value.
 */
static void console_out_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
    p[2] = (uint8_t)((v >> 16u) & 0xFFu);
    p[3] = (uint8_t)(v >> 24u);
}

void console_out_stat_push(void)
{
    /* Field order is part of the protocol; the host accepts any multiple of
     * four bytes and treats missing trailing fields as "not supported", so
     * new fields go at the END only. */
    uint8_t payload[50 * 4];    /* 17, 6 WiFi, 3 line/USB bytes, 2 USB TX, 4 memory, 2 overlong,
                                 4 timing */
    spi_rx_task_stats rx;
    int i = 0;

    if (g_console_mode == CONSOLE_DEBUG)
    {
        return;
    }
    spi_rx_task_get_stats(&rx);

    console_out_put_u32(&payload[4 * i++], (uint32_t)g_sent_stats.total_frames);
    console_out_put_u32(&payload[4 * i++], (uint32_t)g_sent_stats.ok_frames);
    console_out_put_u32(&payload[4 * i++], (uint32_t)g_sent_stats.bad_frames);
    console_out_put_u32(&payload[4 * i++], (uint32_t)g_sent_stats.bytes);
    console_out_put_u32(&payload[4 * i++], (uint32_t)g_received_stats.total_frames);
    console_out_put_u32(&payload[4 * i++], (uint32_t)g_received_stats.ok_frames);
    console_out_put_u32(&payload[4 * i++], (uint32_t)g_received_stats.bad_frames);
    console_out_put_u32(&payload[4 * i++], (uint32_t)g_received_stats.bytes);
    console_out_put_u32(&payload[4 * i++], rx.bytes);
    console_out_put_u32(&payload[4 * i++], rx.idle_dropped);
    console_out_put_u32(&payload[4 * i++], rx.fifo_full);
    console_out_put_u32(&payload[4 * i++], rx.dma_errors);
    console_out_put_u32(&payload[4 * i++], rx.overflow);
    console_out_put_u32(&payload[4 * i++], rx.peeked);
    console_out_put_u32(&payload[4 * i++], usb_tx_task_dropped());
    console_out_put_u32(&payload[4 * i++], console_task_short_frames());
    console_out_put_u32(&payload[4 * i++], g_stats.dropped);
    /* Appended 2026-11-08 for the WiFi mode; the PC side tolerates fewer
     * fields, so new ones go at the END only. */
    wifi_task_stats wf;
    wifi_task_get_stats(&wf);
    console_out_put_u32(&payload[4 * i++], wf.frames_to_peer);
    console_out_put_u32(&payload[4 * i++], wf.bytes_to_peer);
    console_out_put_u32(&payload[4 * i++], wf.frames_from_peer);
    console_out_put_u32(&payload[4 * i++], wf.dropped_to_peer);
    console_out_put_u32(&payload[4 * i++], wf.peer_connects);
    console_out_put_u32(&payload[4 * i++], wf.wifi_connects);
    /* Appended 2026-11-09 for the rate sweep: fields 24..26 are line TX data
     * bytes, line TX idle bytes, USB RX bytes. */
    console_out_put_u32(&payload[4 * i++], spi_tx_task_data_bytes());
    console_out_put_u32(&payload[4 * i++], spi_tx_task_idle_bytes());
    console_out_put_u32(&payload[4 * i++], console_task_usb_rx_bytes());
    /* Appended 2026-11-09: field 27, USB writes the core took only in part
     * (usb_tx_task_short_writes). */
    console_out_put_u32(&payload[4 * i++], usb_tx_task_short_writes());
    /* Field 28: queued output thrown away because no host had the port open. */
    console_out_put_u32(&payload[4 * i++], usb_tx_task_host_closed_flushes());
    /* Fields 29..32 (2026-11-09): high water of the line RX, line TX and USB
     * TX rings in bytes, and the lowest free internal heap since boot. */
    console_out_put_u32(&payload[4 * i++], (uint32_t)ring_buffer_high_water(&g_rx_buffer));
    console_out_put_u32(&payload[4 * i++], (uint32_t)ring_buffer_high_water(&g_tx_buffer));
    console_out_put_u32(&payload[4 * i++], (uint32_t)ring_buffer_high_water(&g_usb_tx_buffer));
    console_out_put_u32(&payload[4 * i++],
                        (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    /* Fields 33..34 (2026-11-09): frames dropped for being longer than
     * BYTE_FRAME_MAX, from the host over USB and from the TCP peer. */
    console_out_put_u32(&payload[4 * i++], console_task_host_overlong());
    console_out_put_u32(&payload[4 * i++], wf.overlong_from_peer);
    /* Fields 35..38 (2026-11-09): microseconds in USB write, bytes written
     * to USB, microseconds on host input, microseconds draining line RX. */
    console_out_put_u32(&payload[4 * i++], usb_tx_task_write_us());
    console_out_put_u32(&payload[4 * i++], usb_tx_task_written_bytes());
    console_out_put_u32(&payload[4 * i++], console_task_host_input_us());
    console_out_put_u32(&payload[4 * i++], console_task_line_drain_us());
    /* Field 39 (2026-11-09): USB input bytes the core dropped, queue full. */
    console_out_put_u32(&payload[4 * i++], console_task_usb_rx_dropped());
    /* Fields 40..41 (2026-11-09): longest gap between host reads (us), and
     * gaps over 10 ms, the core's USB receive wait before it drops. */
    console_out_put_u32(&payload[4 * i++], console_task_read_gap_max_us());
    console_out_put_u32(&payload[4 * i++], console_task_read_gaps_over_limit());
    /* Fields 42..43 (2026-11-09): longest single USB write (us), and writes
     * over 20 ms (BUGS.md B4). */
    console_out_put_u32(&payload[4 * i++], usb_tx_task_write_max_us());
    console_out_put_u32(&payload[4 * i++], usb_tx_task_writes_slow());
    /* Fields 44..45 (2026-11-09): USB TX task 1 ms sleeps after a write
     * that took nothing, and with the ring empty (BUGS.md B4). */
    console_out_put_u32(&payload[4 * i++], usb_tx_task_zero_write_sleeps());
    console_out_put_u32(&payload[4 * i++], usb_tx_task_empty_sleeps());
    /* Field 46 (2026-11-09): longest run of back-to-back USB writes over
     * 1 ms each, us (BUGS.md B4). */
    console_out_put_u32(&payload[4 * i++], usb_tx_task_slow_run_max_us());
    /* Appended 2026-11-09 for the TCP path (BUGS.md T1/T2): fields 47..50 are
     * sends the TCP socket refused (full), passes a peer frame waited for
     * line room, peers dropped as stalled, TCP output ring high water. */
    console_out_put_u32(&payload[4 * i++], wf.send_full);
    console_out_put_u32(&payload[4 * i++], wf.line_full_waits);
    console_out_put_u32(&payload[4 * i++], wf.stalled_peers);
    console_out_put_u32(&payload[4 * i++], wf.ring_high_water);
    (void)console_out_send_frame(CONTROL_FRAME_STAT, 0, 0, payload, 4 * i);
}

void console_out_flush(void)
{
    uint32_t start = millis();

    console_out_reply_end(false);
    while (!ring_buffer_is_empty(&g_usb_tx_buffer) && millis() - start < 200u)
    {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    Serial.flush();
}

void console_out_host_reset(void)
{
    g_console_out_log_level = CONTROL_FRAME_LOG_OFF;
    g_console_out_stat_period_ms = 0;
}

void console_out_get_stats(console_out_stats *out)
{
    *out = g_stats;
}
