/**************************************************************************
** CONSOLE_OUT - the one place text for the host leaves the firmware     **
**                                                                       **
** Taken from HDLCGW console_out.cpp, which explains the design. The     **
** only change is where the STAT numbers come from: the line frames are  **
** the node task's, and XMSGGW has no WiFi or TCP, so those fields are   **
** sent as 0 (the field order is part of the protocol and stays the same **
** as HDLCGW's, so one host program reads both).                         **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "console_out.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "libhdlc/byte_frame.h"
#include "libhdlc/ring_buffer.h"
#include "libhdlc/esp32s2/spi_rx_task.h"
#include "libhdlc/esp32s2/spi_tx_task.h"

#include "console_task.h"
#include "node_task.h"
#include "usb_tx_task.h"

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

/* Scratch for building and stuffing one frame, static and guarded so any
 * task may call console_out_send_frame() (the node task logs from inside
 * libxmsg). Sized for the longest frame this module sends, a full reply
 * chunk. */
#define CONSOLE_OUT_MAX_FRAME (CONSOLE_OUT_CHUNK + CONTROL_FRAME_OVERHEAD)
static SemaphoreHandle_t g_frame_mutex = NULL;
static uint8_t g_frame[CONSOLE_OUT_MAX_FRAME];
static uint8_t g_stuffed[2 + 2 * CONSOLE_OUT_MAX_FRAME];

/* Text written in DEBUG mode by more than one task (console replies, node
 * log lines) is kept whole by this lock. */
static SemaphoreHandle_t g_text_mutex = NULL;

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
    if (g_text_mutex == NULL)
    {
        g_text_mutex = xSemaphoreCreateMutex();
    }
}

/**
 * @brief Write text straight to the USB port, in one piece.
 * @param[in] s  Bytes.
 * @param[in] n  Number of bytes.
 */
static void console_out_serial(const char *s, int n)
{
    bool locked = (g_text_mutex != NULL
                   && xSemaphoreTake(g_text_mutex, pdMS_TO_TICKS(50)) == pdTRUE);
    Serial.write((const uint8_t *)s, (size_t)n);
    if (locked)
    {
        xSemaphoreGive(g_text_mutex);
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
        /* Outside a reply the port is the text console only in DEBUG mode;
         * in FORWARDING it carries frames and loose text would break them. */
        if (g_console_mode == CONSOLE_DEBUG)
        {
            console_out_serial(s, n);
        }
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
        /* A person is watching: show up to DEBUG, and TRACE too if asked. */
        uint8_t limit = (g_console_out_log_level > CONTROL_FRAME_LOG_DEBUG)
                            ? g_console_out_log_level : CONTROL_FRAME_LOG_DEBUG;
        if (level > limit)
        {
            return;
        }
        va_start(ap, fmt);
        n = vsnprintf(text, sizeof(text) - 2, fmt, ap);
        va_end(ap);
        if (n < 0)
        {
            return;
        }
        if (n >= (int)sizeof(text) - 2)
        {
            n = (int)sizeof(text) - 3;
        }
        text[n++] = '\r';
        text[n++] = '\n';
        console_out_serial(text, n);
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
        char line[220];
        int n = snprintf(line, sizeof line, "[event %u] %s\r\n", (unsigned)code, text);
        if (n > 0)
        {
            console_out_serial(line, (n < (int)sizeof line) ? n : (int)sizeof line - 1);
        }
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
    /* Field order is HDLCGW's (its console_out.cpp names each field and the
     * date it was added); the host accepts any multiple of four bytes, and
     * new fields go at the END only. "Sent" is node -> line, "received" is
     * line -> node. Fields marked 0 do not exist in XMSGGW: no WiFi, no TCP,
     * and the host sends no line frames, so there is nothing to time there. */
    uint8_t payload[50 * 4];
    spi_rx_task_stats rx;
    node_status st;
    int i = 0;
    uint32_t bad_in;

    if (g_console_mode == CONSOLE_DEBUG)
    {
        return;
    }
    spi_rx_task_get_stats(&rx);
    node_task_get_status(&st);
    bad_in = st.bad_crc + st.odd_bits + st.too_long + st.aborts;

    /* 0..3 sent: total, ok, bad, bytes. A frame refused for lack of transmit
     * room is retried by libxmsg, so it is not a bad frame. */
    console_out_put_u32(&payload[4 * i++], st.frames_out);
    console_out_put_u32(&payload[4 * i++], st.frames_out);
    console_out_put_u32(&payload[4 * i++], 0);
    console_out_put_u32(&payload[4 * i++], st.bytes_out);
    /* 4..7 received: total, ok, bad, bytes. ok counts frames handed to
     * libxmsg, which needs a node number. */
    console_out_put_u32(&payload[4 * i++], st.frames_in + bad_in);
    console_out_put_u32(&payload[4 * i++], st.frames_in);
    console_out_put_u32(&payload[4 * i++], bad_in);
    console_out_put_u32(&payload[4 * i++], st.bytes_in);
    /* 8..16 */
    console_out_put_u32(&payload[4 * i++], rx.bytes);
    console_out_put_u32(&payload[4 * i++], rx.idle_dropped);
    console_out_put_u32(&payload[4 * i++], rx.fifo_full);
    console_out_put_u32(&payload[4 * i++], rx.dma_errors);
    console_out_put_u32(&payload[4 * i++], rx.overflow);
    console_out_put_u32(&payload[4 * i++], rx.peeked);
    console_out_put_u32(&payload[4 * i++], usb_tx_task_dropped());
    console_out_put_u32(&payload[4 * i++], st.short_frames);
    console_out_put_u32(&payload[4 * i++], g_stats.dropped);
    /* 17..22 WiFi and TCP peer: none. */
    for (int k = 0; k < 6; k++)
    {
        console_out_put_u32(&payload[4 * i++], 0);
    }
    /* Numbers here count from 0. HDLCGW's comments from this field on say
     * one more (its "24..26" are fields 23..25); the order is the same. */
    /* 23..25: line TX data bytes, line TX idle bytes, USB RX bytes. */
    console_out_put_u32(&payload[4 * i++], spi_tx_task_data_bytes());
    console_out_put_u32(&payload[4 * i++], spi_tx_task_idle_bytes());
    console_out_put_u32(&payload[4 * i++], console_task_usb_rx_bytes());
    /* 26..27: partial USB writes, output thrown away with no host. */
    console_out_put_u32(&payload[4 * i++], usb_tx_task_short_writes());
    console_out_put_u32(&payload[4 * i++], usb_tx_task_host_closed_flushes());
    /* 28..31: ring high water (line RX, line TX, USB TX), least free heap. */
    console_out_put_u32(&payload[4 * i++], (uint32_t)ring_buffer_high_water(&g_rx_buffer));
    console_out_put_u32(&payload[4 * i++], (uint32_t)ring_buffer_high_water(&g_tx_buffer));
    console_out_put_u32(&payload[4 * i++], (uint32_t)ring_buffer_high_water(&g_usb_tx_buffer));
    console_out_put_u32(&payload[4 * i++],
                        (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    /* 32..33: overlong host frames, overlong TCP frames (none). */
    console_out_put_u32(&payload[4 * i++], console_task_host_overlong());
    console_out_put_u32(&payload[4 * i++], 0);
    /* 34..37: USB write us, USB bytes written, host input us, line drain us
     * (the node task drains the line, not the console: 0). */
    console_out_put_u32(&payload[4 * i++], usb_tx_task_write_us());
    console_out_put_u32(&payload[4 * i++], usb_tx_task_written_bytes());
    console_out_put_u32(&payload[4 * i++], console_task_host_input_us());
    console_out_put_u32(&payload[4 * i++], 0);
    /* 38..40: USB input dropped by the core, longest read gap, gaps > 10 ms. */
    console_out_put_u32(&payload[4 * i++], console_task_usb_rx_dropped());
    console_out_put_u32(&payload[4 * i++], console_task_read_gap_max_us());
    console_out_put_u32(&payload[4 * i++], console_task_read_gaps_over_limit());
    /* 41..45: USB TX task timing. */
    console_out_put_u32(&payload[4 * i++], usb_tx_task_write_max_us());
    console_out_put_u32(&payload[4 * i++], usb_tx_task_writes_slow());
    console_out_put_u32(&payload[4 * i++], usb_tx_task_zero_write_sleeps());
    console_out_put_u32(&payload[4 * i++], usb_tx_task_empty_sleeps());
    console_out_put_u32(&payload[4 * i++], usb_tx_task_slow_run_max_us());
    /* 46..49 TCP path: none. */
    for (int k = 0; k < 4; k++)
    {
        console_out_put_u32(&payload[4 * i++], 0);
    }
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
