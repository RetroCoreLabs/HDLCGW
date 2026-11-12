/**************************************************************************
** NODE_TASK - the XMSG node on the synchronous line                     **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "node_task.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

#include <libxmsg.h>

#include "libhdlc/hdlc.h"
#include "libhdlc/ring_buffer.h"
#include "libhdlc/esp32s2/spi_rx_task.h"
#include "libhdlc/esp32s2/spi_tx_task.h"

#include "console_out.h"
#include "gw_console.h"
#include "file_job.h"
#include "names.h"
#include "settings.h"
#include "term.h"

/* Longest line frame: address, control, N1 bytes of data, FCS. */
#define LINE_FRAME_MAX (XMSG_LAPB_N1 + 4)

/* Worst case after bit stuffing: one extra bit per five, plus the opening
 * and closing flags, the closing pad and the idle byte after it. */
#define LINE_ENCODED_MAX ((LINE_FRAME_MAX * 6) / 5 + 8)

/* Large (10,928 bytes with 12 sessions): never on a task stack (xmsg_node.h),
 * and allocated once in PSRAM by node_task_init(), because internal RAM is
 * full once WiFi is linked in (the link overflowed by 4,080 bytes with the
 * node static, 2026-11-09). The node runs at line speed, at most 307,200 bit/s
 * by design; the line rings, which the SPI receive task hits hardest, stay in
 * internal RAM as in HDLCGW. Only used while s_node_ok. */
static XmsgNode *s_np = NULL;
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
    (void)xmsg_node_feed_frame(s_np, s_rx_frame, (size_t)length, millis());
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
/** @brief A port-0 letter (node callback, node task). */
static void on_letter(void *ctx, const XmsgDatagram *dg, uint32_t now_ms)
{
    (void)ctx;
    gw_console_letter(s_np, dg, now_ms);
}

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

    if (term_init() != 0)
    {
        console_out_log(CONTROL_FRAME_LOG_ERROR, "No PSRAM for the terminal sessions");
    }
    if (names_init() != 0)
    {
        console_out_log(CONTROL_FRAME_LOG_ERROR, "No PSRAM for the host's name table");
    }
    hdlc_decoder_reset(&s_dec);
    hdlc_decoder_set_buffer(&s_dec, s_rx_frame, (int)sizeof s_rx_frame);

    memset(&s_status, 0, sizeof s_status);
    s_status.own_node = own;
    s_status.peer_node = -1;
    s_status.node_bytes = sizeof(XmsgNode);
    if (own == 0)
    {
        console_out_log(CONTROL_FRAME_LOG_WARN,
                        "No node number stored: NODE <n>, then RESET. The link stays down.");
        return;
    }
    s_np = (XmsgNode *)heap_caps_calloc(1, sizeof(XmsgNode), MALLOC_CAP_SPIRAM);
    if (s_np == NULL)
    {
        console_out_log(CONTROL_FRAME_LOG_ERROR, "No PSRAM for the XMSG node; the link stays down.");
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
    /* Port-0 letters: CONNECT-TO D101 and LI-SYS's status question
     * (gw_console.h). Routing questions the node answers itself. */
    cb.on_letter = on_letter;

    if (xmsg_node_init(s_np, &cfg, &ports, &cb) != XMSG_OK)
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
        s_status.lapb_state = (int)lapb_state(&s_np->lapb);
        s_status.up = xmsg_node_is_up(s_np);
        s_status.ready = xmsg_node_is_ready(s_np);
        s_status.peer_node = lapb_peer_node(&s_np->lapb);
        s_status.datagrams_in = s_np->stats.datagrams_in;
        s_status.datagrams_out = s_np->stats.datagrams_out;
        s_status.bad_headers = s_np->stats.bad_headers;
        s_status.resyncs_done = s_np->stats.resyncs;
        s_status.reach_in = s_np->stats.reach_in;
    }
}

void node_task_run(void *params)
{
    uint8_t block[256];
    (void)params;

    /* Here, not in node_task_init(): the first start writes the default
     * admin, one PBKDF2 run (about 0.5 s), too long for setup(). */
    (void)gw_console_init();

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
                (void)xmsg_node_link_stop(s_np, now);
            }
            if (s_want_start)
            {
                s_want_start = false;
                (void)xmsg_node_link_start(s_np, now);
            }
            if (s_want_resync)
            {
                int32_t peer = lapb_peer_node(&s_np->lapb);
                /* libxmsg sends nothing while an earlier request still waits for its answer
                 * and is younger than cfg.resync_timeout_ms (xmsg_node_resync returns XMSG_OK
                 * then too); after that it sends again (libxmsg 1fb0743). A request went out
                 * when none was waiting, or when the send time changed. */
                bool waiting = s_np->resync_pending;
                uint32_t sent_before = s_np->resync_sent_ms;
                /* The peer number comes from LAPB's first frames; it is known once the link is
                 * up. Until then try again on the next pass. */
                if (peer >= 0 && xmsg_node_resync(s_np, (uint16_t)peer, now) == XMSG_OK)
                {
                    s_want_resync = false;
                    if (!waiting || s_np->resync_sent_ms != sent_before)
                    {
                        s_resyncs++;
                    }
                    console_out_log(CONTROL_FRAME_LOG_INFO, "Resync with node %ld asked for.",
                                    (long)peer);
                }
            }
            term_pass(s_np, now);
            names_pass(s_np, now);
            gw_console_pass(s_np, now);
            file_job_pass_all(s_np, now);   /* starts and stops the FTP and console file jobs (file_job.h) */
            (void)xmsg_node_tick(s_np, now);
        }
        update_status();
        if (n == 0)
        {
            vTaskDelay(1);
        }
    }
}
