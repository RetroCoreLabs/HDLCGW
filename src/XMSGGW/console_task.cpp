/**************************************************************************
** CONSOLE_TASK - the USB serial port: control frames or a text console  **
**                                                                       **
** Taken from HDLCGW console_task.cpp, host side only. Kept: the bulk    **
** read through usb_rx, the control-frame handling (HELLO, COMMAND,      **
** LOG_LEVEL, STAT_PUSH, PING), the ESC ESC ESC hatch to the text        **
** console, the USB disconnect and overflow events, read-gap timing and  **
** the STAT push timer. Left out: everything that forwards host frames   **
** to the line, PACKET_GEN, LED, and the line drain (node_task's job).   **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "console_task.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <Arduino.h>
#include <esp_timer.h>

#include "libhdlc/byte_frame.h"
#include "libhdlc/control_frame.h"

#include "commands.h"
#include "console_out.h"
#include "usb_rx.h"
#include "usb_tx_task.h"
#include "settings.h"
#include "telnet_task.h"
#include "../common/board.h"

/** @brief board_ask_ops.print. */
static void ask_print(const char *text)
{
    console_out_print(text);
}

/* Questions for a missing setting when the text console opens: the WiFi
 * network and the node number (../common/board.h). */
static const board_ask_ops ASK_OPS = {
    ask_print, board_set_wifi_ssid, board_set_wifi_pass, telnet_task_reconnect,
    settings_node, settings_set_node,
};

console_mode g_console_mode = CONSOLE_FORWARDING;

/* Set from the USB CDC event callback (ESP event task) when the host drops
 * DTR or pulls the cable; the console loop acts on it. */
static volatile bool g_host_lost = false;

/* STAT push timer. */
static uint32_t g_last_stat_push_ms = 0;

static byte_frame_decoder g_host_decoder;
static uint32_t g_host_dropped = 0;
static int g_esc_count = 0;     /* ESC presses in a row while idle, 3 = DEBUG */

static volatile uint32_t g_usb_rx_bytes = 0;

/* One command line, from a COMMAND frame or typed in DEBUG mode. */
#define INPUT_MAX 256
static char g_input[INPUT_MAX + 1];
static int g_input_pos = 0;

uint32_t console_task_usb_rx_bytes(void)
{
    return g_usb_rx_bytes;
}

uint32_t console_task_host_overlong(void)
{
    return g_host_decoder.overlong;
}

uint32_t console_task_host_dropped(void)
{
    return g_host_dropped;
}

/**
 * @brief NAK a control frame.
 * @param[in] id      Request id.
 * @param[in] reason  CONTROL_FRAME_NAK_*.
 */
static void send_nak(uint8_t id, uint8_t reason)
{
    (void)console_out_send_frame(CONTROL_FRAME_NAK, id, CONTROL_FRAME_FLAG_LAST, &reason, 1);
}

/**
 * @brief Answer HELLO: protocol version, then key=value lines, the same
 *        keys HDLCGW sends so one host program reads both.
 * @param[in] cf  The HELLO frame.
 */
static void handle_control_hello(const control_frame *cf)
{
    char text[200];
    uint8_t payload[1 + sizeof(text)];
    int n;

    /* A fresh host session starts quiet. */
    console_out_host_reset();

    n = snprintf(text, sizeof(text),
                 "name=XMSGGW\nversion=%s\nbuild=%s %s\nmaxframe=%d\n"
                 "caps=cmd,log,stat,event,clock\n"
                 "chip=ESP32-S2\nboard=LOLIN S2 Mini\n",
                 XMSGGW_VERSION, __DATE__, __TIME__, BYTE_FRAME_MAX);
    if (n < 0)
    {
        n = 0;
    }
    if (n >= (int)sizeof(text))
    {
        n = (int)sizeof(text) - 1;
    }
    payload[0] = CONTROL_FRAME_VERSION;
    memcpy(&payload[1], text, (size_t)n);
    (void)console_out_send_frame(CONTROL_FRAME_HELLO_REPLY, cf->id, CONTROL_FRAME_FLAG_LAST,
                                 payload, 1 + n);
}

/**
 * @brief Run a command line from a COMMAND frame; its output becomes REPLY
 *        frames. RESET flushes its reply before restarting (commands.cpp).
 * @param[in] cf  The COMMAND frame.
 */
static void handle_control_command(const control_frame *cf)
{
    if (cf->payload_len > INPUT_MAX)
    {
        send_nak(cf->id, CONTROL_FRAME_NAK_BAD_LENGTH);
        return;
    }
    memcpy(g_input, cf->payload, (size_t)cf->payload_len);
    g_input[cf->payload_len] = '\0';

    console_out_reply_begin(cf->id);
    commands_process(g_input);
    console_out_reply_end(false);
}

/**
 * @brief One control frame from the host, already parsed.
 * @param[in] cf  The frame; payload points into the host decoder buffer.
 */
static void handle_control_frame(const control_frame *cf)
{
    switch (cf->type)
    {
    case CONTROL_FRAME_HELLO:
        handle_control_hello(cf);
        break;

    case CONTROL_FRAME_COMMAND:
        handle_control_command(cf);
        break;

    case CONTROL_FRAME_LOG_LEVEL:
        if (cf->payload_len == 1 && cf->payload[0] <= CONTROL_FRAME_LOG_TRACE)
        {
            g_console_out_log_level = cf->payload[0];
            console_out_reply_begin(cf->id);
            console_out_printf("Log level %u", (unsigned)g_console_out_log_level);
            console_out_reply_end(false);
        }
        else
        {
            send_nak(cf->id, CONTROL_FRAME_NAK_BAD_LENGTH);
        }
        break;

    case CONTROL_FRAME_STAT_PUSH:
        if (cf->payload_len == 2)
        {
            uint16_t period = (uint16_t)(cf->payload[0] | (cf->payload[1] << 8u));
            if (period != 0 && period < 100u)
            {
                period = 100u;
            }
            g_console_out_stat_period_ms = period;
            g_last_stat_push_ms = millis();
            console_out_reply_begin(cf->id);
            console_out_printf("Stat push every %u ms", (unsigned)period);
            console_out_reply_end(false);
        }
        else
        {
            send_nak(cf->id, CONTROL_FRAME_NAK_BAD_LENGTH);
        }
        break;

    case CONTROL_FRAME_PING:
        (void)console_out_send_frame(CONTROL_FRAME_REPLY, cf->id, CONTROL_FRAME_FLAG_LAST, NULL, 0);
        break;

    default:
        send_nak(cf->id, CONTROL_FRAME_NAK_UNKNOWN_TYPE);
        break;
    }
}

/**
 * @brief One byte from the host in FORWARDING mode.
 *
 * Three ESC in a row between frames opens the text console. Anything else
 * goes to the byte-frame decoder; a complete control frame is handled, any
 * other frame is dropped and counted (the node, not the host, talks to the
 * line here).
 *
 * @param[in] c  Byte from the host.
 */
static void handle_forwarding_input(uint8_t c)
{
    control_frame cf;

    if (byte_frame_decoder_is_idle(&g_host_decoder))
    {
        if (c == 0x1Bu)
        {
            g_esc_count++;
            if (g_esc_count >= 3)
            {
                g_esc_count = 0;
                g_console_mode = CONSOLE_DEBUG;
                console_out_println("\r\n*** XMSGGW text console. HELP lists the commands, "
                                    "EXIT returns to frames. ***");
                /* No login on USB: whoever opens the console is the admin.
                 * Ask for what is missing (../common/board.h). */
                if (!board_ask_start(&ASK_OPS))
                {
                    console_task_show_prompt();
                }
            }
            return;
        }
        g_esc_count = 0;
    }

    if (byte_frame_decoder_feed(&g_host_decoder, c))
    {
        if (control_frame_parse(g_host_decoder.buffer, g_host_decoder.length, &cf))
        {
            handle_control_frame(&cf);
        }
        else
        {
            g_host_dropped++;
        }
    }
}

/**
 * @brief One keystroke on the text console: line editing and dispatch.
 * @param[in] c  Character. Backspace and DEL erase; CR or LF runs the line.
 */
static void handle_debug_input(char c)
{
    if (c == '\b' || c == 0x7f)
    {
        if (g_input_pos > 0)
        {
            g_input_pos--;
            g_input[g_input_pos] = '\0';
            console_out_print("\b \b");
        }
    }
    else if (c == '\n' || c == '\r')
    {
        console_out_println();
        if (board_ask_active())
        {
            /* The line answers a question (board_ask_start); empty skips it. */
            g_input[g_input_pos] = '\0';
            board_ask_answer(g_input);
            memset(g_input, 0, (size_t)g_input_pos);   /* may be a password */
            g_input_pos = 0;
            if (!board_ask_active() && g_console_mode == CONSOLE_DEBUG)
            {
                console_task_show_prompt();
            }
            return;
        }
        if (g_input_pos > 0)
        {
            g_input[g_input_pos] = '\0';
            commands_process(g_input);
            g_input_pos = 0;
        }
        if (g_console_mode == CONSOLE_DEBUG)
        {
            console_task_show_prompt();
        }
    }
    else if (c >= 32 && c < 127 && g_input_pos < INPUT_MAX)
    {
        char echo[2] = { board_ask_hidden() ? '*' : c, '\0' };
        g_input[g_input_pos++] = c;
        console_out_print(echo);
    }
    else if (g_input_pos >= INPUT_MAX)
    {
        console_out_println("\r\nError: input exceeds maximum length.");
        g_input_pos = 0;
        console_task_show_prompt();
    }
}

/** Most host bytes taken per pass (HDLCGW: a "while available" loop never
 *  returned while the PC wrote non-stop). */
#define HOST_INPUT_CHUNK 256

/** @brief Read waiting host bytes in one bulk call and route them by mode. */
static void handle_host_input(void)
{
    console_mode before = g_console_mode;
    uint8_t chunk[HOST_INPUT_CHUNK];
    int got = usb_rx_read(chunk, (int)sizeof(chunk));

    if (got > 0)
    {
        g_usb_rx_bytes = g_usb_rx_bytes + (uint32_t)got;
    }
    for (int i = 0; i < got; i++)
    {
        if (g_console_mode == CONSOLE_DEBUG)
        {
            handle_debug_input((char)chunk[i]);
        }
        else
        {
            handle_forwarding_input(chunk[i]);
        }
    }

    if (g_console_mode != before)
    {
        if (g_console_mode == CONSOLE_FORWARDING)
        {
            /* Start the frame channel clean after typing. */
            byte_frame_decoder_reset(&g_host_decoder);
        }
        console_out_event(CONTROL_FRAME_EVENT_MODE_CHANGE,
                          (g_console_mode == CONSOLE_DEBUG) ? "DEBUG" : "FORWARDING");
    }
}

/** Bytes the Arduino core threw away because its USB receive queue was full
 *  (HDLCGW console_task.cpp explains where; core 3.3.12). */
static volatile uint32_t g_usb_rx_dropped = 0;

/**
 * @brief USB CDC event callback for ARDUINO_USB_CDC_RX_OVERFLOW_EVENT, on the
 *        ESP event task. Adds the dropped byte count.
 */
static void on_usb_rx_overflow(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    if (event_data != NULL)
    {
        const arduino_usb_cdc_event_data_t *p = (const arduino_usb_cdc_event_data_t *)event_data;
        g_usb_rx_dropped = g_usb_rx_dropped + (uint32_t)p->rx_overflow.dropped_bytes;
    }
}

uint32_t console_task_usb_rx_dropped(void)
{
    return g_usb_rx_dropped;
}

/**
 * @brief USB CDC event callback: the host closed the port (DTR dropped) or
 *        the cable went. Only sets a flag; the console loop does the work.
 */
static void on_usb_disconnected(void *arg, esp_event_base_t event_base, int32_t event_id,
                                void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;
    g_host_lost = true;
}

static volatile uint32_t g_host_input_us = 0;

/** The core's USB receive code waits 10 ms for queue room, then drops
 *  (HDLCGW console_task.cpp). */
#define READ_GAP_LIMIT_US 10000u
static volatile uint32_t g_read_gap_max_us = 0;
static volatile uint32_t g_read_gaps_over_limit = 0;

uint32_t console_task_read_gap_max_us(void)
{
    return g_read_gap_max_us;
}

uint32_t console_task_read_gaps_over_limit(void)
{
    return g_read_gaps_over_limit;
}

uint32_t console_task_host_input_us(void)
{
    return g_host_input_us;
}

/**
 * @brief Once per loop: a lost host turns logging and stat push off and
 *        empties the USB ring; the STAT push timer.
 */
static void handle_housekeeping(void)
{
    if (g_host_lost)
    {
        g_host_lost = false;
        console_out_host_reset();
        ring_buffer_clear(&g_usb_tx_buffer);
        console_out_log(CONTROL_FRAME_LOG_INFO, "USB host lost, log and stat push off");
    }

    if (g_console_out_stat_period_ms != 0
        && millis() - g_last_stat_push_ms >= g_console_out_stat_period_ms)
    {
        g_last_stat_push_ms = millis();
        console_out_stat_push();
    }
}

void console_task_show_prompt(void)
{
    console_out_print("XMSG# ");   /* USB console (Ronny, 2026-11-12; was "# "); telnet shows "XMSG> " */
}

void console_task_run(void *params)
{
    (void)params;

    byte_frame_decoder_reset(&g_host_decoder);

    /* "Disconnected" is posted when DTR drops and on cable removal. */
    Serial.onEvent(ARDUINO_USB_CDC_DISCONNECTED_EVENT, on_usb_disconnected);
    Serial.onEvent(ARDUINO_USB_CDC_RX_OVERFLOW_EVENT, on_usb_rx_overflow);

    int64_t last_read_end = esp_timer_get_time();

    while (true)
    {
        int64_t t0 = esp_timer_get_time();
        uint32_t gap = (uint32_t)(t0 - last_read_end);
        if (gap > g_read_gap_max_us)
        {
            g_read_gap_max_us = gap;
        }
        if (gap > READ_GAP_LIMIT_US)
        {
            g_read_gaps_over_limit = g_read_gaps_over_limit + 1u;
        }
        handle_host_input();
        int64_t t1 = esp_timer_get_time();
        last_read_end = t1;
        g_host_input_us = g_host_input_us + (uint32_t)(t1 - t0);
        handle_housekeeping();
        usb_rx_poll();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
