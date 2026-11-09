/**************************************************************************
** NODE_TASK - the XMSG node on the synchronous line                     **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "node_task.h"

#include <Arduino.h>
#include <string.h>

#include <libxmsg.h>

#include "libhdlc/hdlc.h"
#include "libhdlc/ring_buffer.h"
#include "libhdlc/esp32s2/spi_rx_task.h"
#include "libhdlc/esp32s2/spi_tx_task.h"

#include "console_out.h"
#include "settings.h"

/* Longest line frame: address, control, N1 bytes of data, FCS. */
#define LINE_FRAME_MAX (XMSG_LAPB_N1 + 4)

/* Worst case after bit stuffing: one extra bit per five, plus the opening
 * and closing flags, the closing pad and the idle byte after it. */
#define LINE_ENCODED_MAX ((LINE_FRAME_MAX * 6) / 5 + 8)

/* Large: static, never on a task stack (xmsg_node.h). */
static XmsgNode s_node;
static bool s_node_ok = false;

static hdlc_decoder s_dec;
static uint8_t s_rx_frame[LINE_FRAME_MAX];
static uint8_t s_tx_line[LINE_ENCODED_MAX];

static volatile bool s_want_start = false;
static volatile bool s_want_stop = false;

/* Set when the link becomes ready; the task then resyncs our message count with the peer.
 * See link_state(). */
static bool s_want_resync = false;
static uint32_t s_resyncs = 0;

static node_status s_status;
static uint32_t s_frames_in = 0;
static uint32_t s_frames_out = 0;
static uint32_t s_bytes_in = 0;
static uint32_t s_bytes_out = 0;
static uint32_t s_tx_full = 0;

/* INSPECT: the last frame off the line, and the last four with a bad FCS
 * (same history as HDLCGW console_task.cpp; four because one run lost three
 * frames in two minutes there). Internal RAM, 5 x 626 bytes. */
#define BAD_HISTORY 4
static uint8_t s_last_frame[LINE_FRAME_MAX];
static int s_last_len = 0;
static bool s_last_good = false;
static uint8_t s_bad_frames[BAD_HISTORY][LINE_FRAME_MAX];
static int s_bad_lens[BAD_HISTORY];
static int s_bad_next = 0;

/* ------------------------------------------------------- terminal session */

/* One TAD terminal session (libxmsg tad_asker.h), the first real XMSG session
 * XMSGGW runs; telnet users get one each later. The asker lives in this task
 * like the node. Text crosses to the console task through two small buffers
 * under s_term_lock. */
#define TERM_IN_SIZE  2048
#define TERM_OUT_SIZE 256
static TadAsker s_tad;
static bool s_tad_open = false;            /* initialised and attached, until CLOSED/FAILED */
static char s_term_system[16];
static volatile bool s_term_want_connect = false;
static volatile bool s_term_want_hangup = false;
static SemaphoreHandle_t s_term_lock = NULL;
static uint8_t s_term_in[TERM_IN_SIZE];    /* host text not yet read by TERM SHOW */
static int s_term_in_len = 0;
static uint32_t s_term_lost_in = 0;
static uint8_t s_term_out[TERM_OUT_SIZE];  /* typed text waiting for an input grant */
static int s_term_out_len = 0;

/** @brief Take the text buffer lock; false when it could not be had in 50 ms. */
static bool term_lock(void)
{
    return s_term_lock != NULL && xSemaphoreTake(s_term_lock, pdMS_TO_TICKS(50)) == pdTRUE;
}

/** @brief Asker callback: host text, kept for TERM SHOW. */
static void term_text(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    if (!term_lock())
    {
        s_term_lost_in += (uint32_t)len;
        return;
    }
    for (size_t i = 0; i < len; i++)
    {
        if (s_term_in_len < TERM_IN_SIZE)
        {
            s_term_in[s_term_in_len++] = bytes[i];
        }
        else
        {
            s_term_lost_in++;
        }
    }
    xSemaphoreGive(s_term_lock);
}

/** @brief Asker callback: state changes, as log lines. */
static void term_state(void *ctx, TadAskerState state, int error)
{
    (void)ctx;
    console_out_log(CONTROL_FRAME_LOG_INFO, "Terminal session: %s%s%d",
                    tad_asker_state_name(state), (error != 0) ? ", error " : "",
                    (error != 0) ? error : 0);
}

/** @brief Asker callback: the host's login ended; hang up, as the asker expects. */
static void term_logged_out(void *ctx)
{
    (void)ctx;
    console_out_log(CONTROL_FRAME_LOG_INFO, "Terminal session: host logged out, hanging up");
    s_term_want_hangup = true;
}

int node_task_term_connect(const char *system)
{
    if (strlen(system) >= sizeof s_term_system || s_tad_open || s_term_want_connect)
    {
        return -1;
    }
    memcpy(s_term_system, system, strlen(system) + 1);
    s_term_want_connect = true;
    return 0;
}

int node_task_term_type(const uint8_t *text, int len)
{
    int took = 0;
    if (!s_tad_open)
    {
        return -1;
    }
    if (!term_lock())
    {
        return 0;
    }
    while (took < len && s_term_out_len < TERM_OUT_SIZE)
    {
        s_term_out[s_term_out_len++] = text[took++];
    }
    xSemaphoreGive(s_term_lock);
    return took;
}

void node_task_term_hangup(void)
{
    s_term_want_hangup = true;
}

int node_task_term_read(uint8_t *out, int max)
{
    int n;
    if (!term_lock())
    {
        return 0;
    }
    n = (s_term_in_len < max) ? s_term_in_len : max;
    memcpy(out, s_term_in, (size_t)n);
    memmove(s_term_in, &s_term_in[n], (size_t)(s_term_in_len - n));
    s_term_in_len -= n;
    xSemaphoreGive(s_term_lock);
    return n;
}

void node_task_term_status(node_term_status *out)
{
    /* Read from the console task without the node's lock: a status line,
     * at most one pass old, as node_task_get_status. */
    out->state = (s_tad_open || s_tad.state != TAD_ASKER_IDLE) ? tad_asker_state_name(s_tad.state)
                                                                : "none";
    out->error = s_tad.error;
    out->may_type = s_tad_open && tad_asker_may_type(&s_tad);
    out->bytes_in = s_tad.stats.text_bytes_in;
    out->bytes_out = s_tad.stats.text_bytes_out;
    out->lost_in = s_term_lost_in;
    out->queued_out = s_term_out_len;
}

/**
 * @brief The terminal session's part of a node task pass: connect, type,
 *        hang up. Runs only in the node task.
 * @param[in] now  millis().
 */
static void term_pass(uint32_t now)
{
    if (s_term_want_connect && xmsg_node_is_ready(&s_node))
    {
        int32_t peer = lapb_peer_node(&s_node.lapb);
        TadAskerConfig cfg;
        TadAskerCallbacks cb;
        s_term_want_connect = false;
        if (peer < 0)
        {
            console_out_log(CONTROL_FRAME_LOG_WARN, "Terminal session: peer not known yet");
            return;
        }
        tad_asker_config_default(&cfg, (uint16_t)peer, s_term_system);
        memset(&cb, 0, sizeof cb);
        cb.text_out = term_text;
        cb.state = term_state;
        cb.logged_out = term_logged_out;
        if (term_lock())
        {
            s_term_in_len = 0;
            s_term_out_len = 0;
            xSemaphoreGive(s_term_lock);
        }
        s_term_lost_in = 0;
        if (tad_asker_init(&s_tad, &cfg, &cb) != XMSG_OK)
        {
            console_out_log(CONTROL_FRAME_LOG_ERROR, "Terminal session: bad system name");
            return;
        }
        int rc = tad_asker_connect(&s_tad, &s_node, now);
        if (rc != XMSG_OK)
        {
            console_out_log(CONTROL_FRAME_LOG_ERROR, "Terminal session: connect refused, %d", rc);
            return;
        }
        s_tad_open = true;
    }
    if (!s_tad_open)
    {
        return;
    }
    if (s_tad.state == TAD_ASKER_CLOSED || s_tad.state == TAD_ASKER_FAILED)
    {
        s_tad_open = false;   /* the asker has detached itself from the node */
        s_term_want_hangup = false;
        return;
    }
    if (s_term_want_hangup)
    {
        s_term_want_hangup = false;
        if (tad_asker_hangup(&s_tad, now) != XMSG_OK)
        {
            tad_asker_abort(&s_tad);
        }
        return;
    }
    if (s_term_out_len > 0 && tad_asker_may_type(&s_tad) && term_lock())
    {
        int n = (s_term_out_len < TAD_MAX_DATA) ? s_term_out_len : TAD_MAX_DATA;
        int took = tad_asker_write(&s_tad, s_term_out, (size_t)n, now);
        if (took > 0)
        {
            memmove(s_term_out, &s_term_out[took], (size_t)(s_term_out_len - took));
            s_term_out_len -= took;
        }
        xSemaphoreGive(s_term_lock);
    }
}

/* ------------------------------------------------------------ transport */

/**
 * @brief libxmsg transport write: one whole frame, FCS included, in raw
 *        framing. Bit-stuffs it and queues it for the line as one block.
 * @return len when queued, 0 when the transmit ring has no room (libxmsg
 *         keeps the frame and retries from its tick), -1 for a bad length.
 */
static int line_write(void *ctx, const uint8_t *bytes, size_t len)
{
    hdlc_encoder enc;
    int n = 0;
    (void)ctx;
    if (len == 0 || len > LINE_FRAME_MAX)
    {
        return -1;
    }
    hdlc_encoder_reset(&enc);
    n += hdlc_encoder_open(&enc, &s_tx_line[n]);
    for (size_t i = 0; i < len; i++)
    {
        n += hdlc_encoder_write(&enc, bytes[i], &s_tx_line[n]);
    }
    n += hdlc_encoder_close(&enc, &s_tx_line[n]);
    /* One whole idle byte after the closing flag, as HDLCGW sends: without
     * it a receiver can take the 1..7 pad ones plus the next flag for a
     * flag (RC422Dev bench, 2026-11-08; see HDLCGW console_task.cpp). */
    s_tx_line[n++] = HDLC_IDLE;
    if (!spi_tx_task_has_room(n))
    {
        s_tx_full++;
        return 0;
    }
    if (spi_tx_task_push(s_tx_line, n) != 0)
    {
        s_tx_full++;
        return 0;
    }
    s_frames_out++;
    s_bytes_out += (uint32_t)len;
    return (int)len;
}

/** @brief libxmsg transport: the line speed, for its timers. */
static uint32_t line_speed(void *ctx)
{
    (void)ctx;
    return spi_tx_task_bit_rate();
}

/* -------------------------------------------------------------- receive */

/** @brief libhdlc per-frame callback: a frame is in s_rx_frame. */
static void on_frame(void *ctx, int length, hdlc_rx_status status)
{
    int keep = (length < LINE_FRAME_MAX) ? length : LINE_FRAME_MAX;
    (void)ctx;
    memcpy(s_last_frame, s_rx_frame, (size_t)keep);
    s_last_len = keep;
    s_last_good = (status == HDLC_RX_OK);
    if (status == HDLC_RX_BAD_CRC)
    {
        memcpy(s_bad_frames[s_bad_next], s_rx_frame, (size_t)keep);
        s_bad_lens[s_bad_next] = keep;
        s_bad_next = (s_bad_next + 1) % BAD_HISTORY;
    }
    if (status != HDLC_RX_OK)
    {
        /* Counted by the decoder (stats); a bad frame is dropped whole and
         * LAPB recovers it (Ronny: whole-frame drops are acceptable). */
        return;
    }
    if (!s_node_ok)
    {
        return;     /* no node number set: the line is decoded, nothing listens */
    }
    s_frames_in++;
    s_bytes_in += (uint32_t)length;
    (void)xmsg_node_feed_frame(&s_node, s_rx_frame, (size_t)length, millis());
}

/** @brief Abort and short runs: nothing kept, the buffer is reused as it is. */
static void on_nothing(void *ctx)
{
    (void)ctx;
}

static const hdlc_frame_sink LINE_SINK =
{
    NULL,
    NULL,           /* on_byte: per-frame mode */
    NULL,           /* on_end: per-frame mode */
    on_nothing,     /* on_abort */
    on_nothing,     /* on_discard */
    on_frame
};

/* ------------------------------------------------------------ log, link */

/**
 * @brief libxmsg log sink: one LOG line (a text line in DEBUG mode).
 *
 * libxmsg levels run ERROR=0 .. TRACE=4; the control-frame levels run
 * ERROR=1 .. TRACE=5 with 0 meaning off, hence the + 1.
 */
static void log_write(void *ctx, XmsgLogCategory category, XmsgLogLevel level, const char *line)
{
    uint8_t lv = ((unsigned)level < 5u) ? (uint8_t)(level + 1) : CONTROL_FRAME_LOG_TRACE;
    (void)ctx;
    console_out_log(lv, "xmsg %d: %s", (int)category, line);
}

/** @brief libxmsg link change: say so on the console. */
static void link_state(void *ctx, LapbState state, bool up, bool ready, const char *reason)
{
    (void)ctx;
    /* Every time the link becomes ready, start our message count to the peer at 0 the way the
     * peer will (xmsg_node_resync). Our stored count may be behind what the peer expects - D100
     * still holds node 101's count from earlier use - and a SINTRAN host drops a number that is
     * behind in silence. MEASURED by libxmsg on RetroCore D100, node 101, 2026-11-09 (see
     * xmsg_node.h): no answer at all until a reachability request, then the session ran.
     * Not called from here: this runs inside the node, so the task does it on its next pass. */
    if (ready)
    {
        s_want_resync = true;
    }
    /* A log line, not an EVENT: control_frame.h has no link-change event code
     * yet, and the line RX codes mean something else. STAT and STATUS show
     * the state for a host that keeps logging off. */
    console_out_log(CONTROL_FRAME_LOG_INFO, "Link: state %d, %s%s%s%s", (int)state,
                    up ? "up" : "down", ready ? ", ready" : "",
                    (reason != NULL) ? " - " : "", (reason != NULL) ? reason : "");
}

/* ----------------------------------------------------------------- task */

void node_task_init(void)
{
    XmsgNodeConfig cfg;
    XmsgPorts ports;
    XmsgNodeCallbacks cb;
    uint16_t own = settings_node();

    if (s_term_lock == NULL)
    {
        s_term_lock = xSemaphoreCreateMutex();
    }
    hdlc_decoder_reset(&s_dec);
    hdlc_decoder_set_buffer(&s_dec, s_rx_frame, (int)sizeof s_rx_frame);

    memset(&s_status, 0, sizeof s_status);
    s_status.own_node = own;
    s_status.peer_node = -1;
    s_status.node_bytes = sizeof s_node;
    if (own == 0)
    {
        console_out_log(CONTROL_FRAME_LOG_WARN,
                        "No node number stored: NODE <n>, then RESET. The link stays down.");
        return;
    }

    xmsg_node_config_default(&cfg, own);
    cfg.framing = XMSG_FRAMING_RAW;
    cfg.transport_adds_fcs = false;   /* libxmsg appends and checks the FCS */

    memset(&ports, 0, sizeof ports);
    ports.transport.write = line_write;
    ports.transport.line_speed = line_speed;
    ports.flags1.load = settings_flags1_load;
    ports.flags1.save = settings_flags1_save;
    ports.flags1.load_port_seed = settings_port_seed_load;
    ports.flags1.save_port_seed = settings_port_seed_save;
    ports.log.write = log_write;

    memset(&cb, 0, sizeof cb);
    cb.link_state = link_state;

    if (xmsg_node_init(&s_node, &cfg, &ports, &cb) != XMSG_OK)
    {
        console_out_log(CONTROL_FRAME_LOG_ERROR, "xmsg_node_init failed; the link stays down.");
        return;
    }
    s_node_ok = true;
    /* A gateway in a cabinet starts its link with no PC attached, as
     * HDLCGW starts its line (Ronny, 2026-11-08). */
    s_want_start = true;
}

void node_task_request_start(void)
{
    s_want_start = true;
}

void node_task_request_stop(void)
{
    s_want_stop = true;
}

void node_task_request_reach(void)
{
    s_want_resync = true;
}

void node_task_get_status(node_status *out)
{
    *out = s_status;
}

const uint8_t *node_task_last_frame(int *len, bool *crc_good)
{
    *len = s_last_len;
    *crc_good = s_last_good;
    return s_last_frame;
}

const uint8_t *node_task_last_bad_frame(int index, int *len)
{
    int slot;
    if (index < 0 || index >= BAD_HISTORY)
    {
        *len = 0;
        return s_bad_frames[0];
    }
    slot = (s_bad_next - 1 - index + 2 * BAD_HISTORY) % BAD_HISTORY;
    *len = s_bad_lens[slot];
    return s_bad_frames[slot];
}

/** @brief Copy the numbers the console shows. */
static void update_status(void)
{
    s_status.frames_in = s_frames_in;
    s_status.frames_out = s_frames_out;
    s_status.bytes_in = s_bytes_in;
    s_status.bytes_out = s_bytes_out;
    s_status.short_frames = s_dec.stats.short_frames;
    s_status.tx_full = s_tx_full;
    s_status.bad_crc = s_dec.stats.bad_crc;
    s_status.odd_bits = s_dec.stats.odd_bits;
    s_status.too_long = s_dec.stats.too_long;
    s_status.aborts = s_dec.stats.aborts;
    s_status.stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    s_status.resyncs_asked = s_resyncs;
    if (s_node_ok)
    {
        s_status.lapb_state = (int)lapb_state(&s_node.lapb);
        s_status.up = xmsg_node_is_up(&s_node);
        s_status.ready = xmsg_node_is_ready(&s_node);
        s_status.peer_node = lapb_peer_node(&s_node.lapb);
        s_status.datagrams_in = s_node.stats.datagrams_in;
        s_status.datagrams_out = s_node.stats.datagrams_out;
        s_status.bad_headers = s_node.stats.bad_headers;
        s_status.resyncs_done = s_node.stats.resyncs;
        s_status.reach_in = s_node.stats.reach_in;
    }
}

void node_task_run(void *params)
{
    uint8_t block[256];
    (void)params;

    while (true)
    {
        int n = ring_buffer_read_block(&g_rx_buffer, block, (int)sizeof block);
        for (int i = 0; i < n; i++)
        {
            hdlc_decoder_feed(&s_dec, block[i], &LINE_SINK);
        }
        if (s_node_ok)
        {
            uint32_t now = millis();
            if (s_want_stop)
            {
                s_want_stop = false;
                (void)xmsg_node_link_stop(&s_node, now);
            }
            if (s_want_start)
            {
                s_want_start = false;
                (void)xmsg_node_link_start(&s_node, now);
            }
            if (s_want_resync)
            {
                int32_t peer = lapb_peer_node(&s_node.lapb);
                /* libxmsg sends nothing while an earlier request still waits for its answer
                 * and is younger than cfg.resync_timeout_ms (xmsg_node_resync returns XMSG_OK
                 * then too); after that it sends again (libxmsg 1fb0743). A request went out
                 * when none was waiting, or when the send time changed. */
                bool waiting = s_node.resync_pending;
                uint32_t sent_before = s_node.resync_sent_ms;
                /* The peer number comes from LAPB's first frames; it is known once the link is
                 * up. Until then try again on the next pass. */
                if (peer >= 0 && xmsg_node_resync(&s_node, (uint16_t)peer, now) == XMSG_OK)
                {
                    s_want_resync = false;
                    if (!waiting || s_node.resync_sent_ms != sent_before)
                    {
                        s_resyncs++;
                    }
                    console_out_log(CONTROL_FRAME_LOG_INFO, "Resync with node %ld asked for.",
                                    (long)peer);
                }
            }
            term_pass(now);
            (void)xmsg_node_tick(&s_node, now);
        }
        update_status();
        if (n == 0)
        {
            vTaskDelay(1);
        }
    }
}
