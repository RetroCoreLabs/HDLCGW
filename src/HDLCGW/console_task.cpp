/**************************************************************************
** CONSOLE_TASK - the USB serial port: frame channel to the host, or a   **
**                text console                                           **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#include <esp_timer.h>
#include "console_task.h"
#include "libhdlc/hot_path.h"

#include <stdlib.h>
#include <string.h>

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "libhdlc/byte_frame.h"
#include "commands.h"
#include "console_out.h"
#include "libhdlc/control_frame.h"
#include "libhdlc/hdlc.h"
#include "libhdlc/esp32s2/spi_rx_task.h"
#include "usb_rx.h"
#include "libhdlc/esp32s2/spi_tx_task.h"
#include "usb_tx_task.h"
#include "wifi_task.h"

console_mode g_console_mode = CONSOLE_FORWARDING;
console_frame_stats g_sent_stats;
console_frame_stats g_received_stats;

bool g_loopback_enabled = false;
bool g_echo_enabled = false;
bool g_hdlc_mode = false;

/* Set from the USB CDC event callback (ESP event task) when the host drops
 * DTR or pulls the cable; the console loop acts on it. */
static volatile bool g_host_lost = false;

/* For the LINE_RX_ACTIVE / LINE_RX_IDLE events: when the last byte came off
 * the line, and whether we last reported the line as active. */
#define LINE_IDLE_MS 2000u
static uint32_t g_last_line_byte_ms = 0;
static bool g_line_active = false;

/* STAT push timer. */
static uint32_t g_last_stat_push_ms = 0;

/* On-board LED, GPIO 15 on the LOLIN S2 Mini (LED_BUILTIN). Blinks on every
 * byte received from the host. */
static bool g_led_state = HIGH;

/** @brief Flip the on-board LED. */
static void toggle_led(void)
{
    g_led_state = !g_led_state;
    digitalWrite(LED_BUILTIN, g_led_state);
}

/* ------------------------------------------------------ line -> host */

/**
 * @brief One frame being collected from the line.
 *
 * When it ends it is forwarded to the host (FORWARDING) or printed (DEBUG).
 */
typedef struct
{
    uint8_t frame[BYTE_FRAME_MAX];  /**< bytes so far, FCS included at the end */
    int len;                        /**< bytes delivered; may exceed the array */
} line_frame;

static line_frame g_line_frame;
static hdlc_decoder g_line_decoder;

/* Copy of the last complete frame off the line, for INSPECT. */
static uint8_t g_last_frame[BYTE_FRAME_MAX];
static int g_last_len = 0;
static bool g_last_crc_good = false;

/* Copy of the last frame off the line that FAILED its FCS, for INSPECT BAD.
 * A bad frame is never forwarded to the host, so without this copy there is
 * no way to see what the far end actually sent (3 in 1653 on the bench). */
/* 4 KB, allocated from PSRAM in console_task_init(): as a static array it
 * put dram0_0_seg 768 bytes over the top (2026-11-08). */
#define BAD_HISTORY 4
static uint8_t *g_bad_frames = NULL;    /* BAD_HISTORY slots of BYTE_FRAME_MAX */
static int g_bad_lens[BAD_HISTORY];
static int g_bad_next = 0;      /* slot the next bad frame goes into */
static int g_bad_count = 0;     /* frames stored so far, up to BAD_HISTORY */

/**
 * @brief Print a frame as hex, eight bytes per line. DEBUG mode only (the
 *        ECHO hex dump is a feature for a person at a terminal; it stays
 *        off the frame channel, Ronny 2026-11-08).
 * @param[in] lf  Frame; len must not exceed the array.
 */
static void dump_line_frame(const line_frame *lf)
{
    for (int i = 0; i < lf->len; i++)
    {
        console_out_printf("0x%02X ", lf->frame[i]);
        if ((i + 1) % 8 == 0)
        {
            console_out_println();
        }
    }
    console_out_println();
}

/**
 * @brief hdlc_frame_sink callback: store one frame byte.
 * @param[in,out] ctx   The line_frame.
 * @param[in]     data  Frame byte.
 */
static HOT_PATH_CODE void on_line_byte(void *ctx, uint8_t data)
{
    line_frame *lf = (line_frame *)ctx;

    if (lf->len < BYTE_FRAME_MAX)
    {
        lf->frame[lf->len] = data;
    }
    lf->len++;
}

/**
 * @brief hdlc_frame_sink callback: frame complete, forward or print it.
 * @param[in,out] ctx     The line_frame; emptied on return.
 * @param[in]     status  How the frame ended (libhdlc): only HDLC_RX_OK is
 *                        forwarded; CRC error, odd bit count and too long
 *                        are all bad frames.
 */
static void on_line_end(void *ctx, hdlc_rx_status status)
{
    line_frame *lf = (line_frame *)ctx;
    bool crc_good = (status == HDLC_RX_OK);
    bool fits = lf->len <= BYTE_FRAME_MAX;

    g_received_stats.total_frames++;

    if (fits)
    {
        memcpy(g_last_frame, lf->frame, (size_t)lf->len);
        g_last_len = lf->len;
        g_last_crc_good = crc_good;
        if (!crc_good)
        {
            if (g_bad_frames != NULL)
            {
                memcpy(&g_bad_frames[g_bad_next * BYTE_FRAME_MAX], lf->frame, (size_t)lf->len);
                g_bad_lens[g_bad_next] = lf->len;
                g_bad_next = (g_bad_next + 1) % BAD_HISTORY;
                if (g_bad_count < BAD_HISTORY)
                {
                    g_bad_count++;
                }
            }
        }
    }

    if (crc_good && fits)
    {
        g_received_stats.ok_frames++;
        g_received_stats.bytes += lf->len;

        if (g_console_mode != CONSOLE_DEBUG)
        {
            console_task_send_host_frame(lf->frame, lf->len);
        }
        /* Same text as always in DEBUG; as a DEBUG-level log frame in
         * FORWARDING when the host asked for that level. */
        console_out_log(CONTROL_FRAME_LOG_DEBUG, "HDLC frame, %d bytes, FCS good", lf->len);
        if (g_console_mode == CONSOLE_DEBUG && g_echo_enabled)
        {
            dump_line_frame(lf);
        }
    }
    else
    {
        g_received_stats.bad_frames++;
        /* The thing a host could never see before: a bad frame while
         * traffic flows, as a WARN log frame. */
        console_out_log(CONTROL_FRAME_LOG_WARN, "HDLC frame, %d bytes, %s", lf->len,
                        !fits ? "too long, dropped"
                        : (status == HDLC_RX_BAD_CRC) ? "FCS BAD" /* the PC tool filters on it */
                                                      : hdlc_rx_status_name(status));
    }
    lf->len = 0;
}

/**
 * @brief hdlc_frame_sink callback: frame aborted, drop it.
 * @param[in,out] ctx  The line_frame; emptied on return.
 */
static void on_line_abort(void *ctx)
{
    line_frame *lf = (line_frame *)ctx;

    g_received_stats.total_frames++;
    g_received_stats.bad_frames++;
    console_out_log(CONTROL_FRAME_LOG_WARN, "HDLC abort after %d bytes", lf->len);
    lf->len = 0;
}

/**
 * @brief hdlc_frame_sink callback: the bytes so far were inter-frame padding,
 *        not a frame. Forget them; nothing is counted.
 * @param[in,out] ctx  The line_frame; emptied on return.
 */
static void on_line_discard(void *ctx)
{
    line_frame *lf = (line_frame *)ctx;

    lf->len = 0;
}

static const hdlc_frame_sink LINE_SINK =
{
    &g_line_frame,
    on_line_byte,
    on_line_end,
    on_line_abort,
    on_line_discard,
    NULL            /* on_frame: HDLCGW uses per-byte mode, no buffer set */
};

/**
 * @brief Take what the link has received and act on it.
 *
 * At most 256 bytes per call so the host side stays responsive. Loopback
 * copies raw bytes back to the line; decoding runs in FORWARDING or when
 * the HDLC command is on; otherwise echo prints raw non-idle bytes.
 */
static HOT_PATH_CODE void handle_line_bytes(void)
{
    bool decode = (g_console_mode != CONSOLE_DEBUG) || g_hdlc_mode;

    if (!(decode || g_echo_enabled || g_loopback_enabled))
    {
        return;
    }

    /* One locked block read per pass, not one mutex round per byte (TASK run
     * time, bench 2026-11-09). */
    uint8_t block[256];
    int cnt = ring_buffer_read_block(&g_rx_buffer, block, (int)sizeof(block));

    for (int i = 0; i < cnt; i++)
    {
        uint8_t data = block[i];

        if (g_loopback_enabled)
        {
            ring_buffer_write(&g_tx_buffer, data);
        }

        if (decode)
        {
            hdlc_decoder_feed(&g_line_decoder, data, &LINE_SINK);
        }
        else if (g_echo_enabled && data != HDLC_IDLE)
        {
            console_out_log(CONTROL_FRAME_LOG_TRACE, "Received: 0x%02X", data);
        }
    }

    if (cnt > 0)
    {
        /* spi_rx_task drops repeated idle bytes, so anything in the ring
         * means the line carried something other than idle. */
        g_last_line_byte_ms = millis();
        if (!g_line_active)
        {
            g_line_active = true;
            console_out_event(CONTROL_FRAME_EVENT_LINE_RX_ACTIVE, "line receive active");
        }
    }
    else if (g_line_active && millis() - g_last_line_byte_ms >= LINE_IDLE_MS)
    {
        g_line_active = false;
        console_out_event(CONTROL_FRAME_EVENT_LINE_RX_IDLE, "line receive idle");
    }
}

void console_task_send_host_frame(const uint8_t *frame, int len)
{
    static uint8_t out[2 + 2 * BYTE_FRAME_MAX];
    int n;

    if (len < 0 || len > BYTE_FRAME_MAX)
    {
        return;
    }
    n = byte_frame_encode(frame, len, out);
    /* A connected TCP peer gets the line's frames; USB gets them too only
     * when MIRROR is on. Without a peer, USB as always. Never written to
     * Serial from here: see usb_tx_task.h for why. A drop is counted in the
     * ring that dropped it and shows in STAT. */
    if (!wifi_task_take_host_frame(out, n) || wifi_task_mirror())
    {
        (void)usb_tx_task_push(out, n);
    }
}

const uint8_t *console_task_last_frame(int *len, bool *crc_good)
{
    *len = g_last_len;
    *crc_good = g_last_crc_good;
    return g_last_frame;
}

const uint8_t *console_task_last_bad_frame(int index, int *len)
{
    int slot;

    if (g_bad_frames == NULL || index < 0 || index >= g_bad_count)
    {
        *len = 0;
        return NULL;
    }
    /* 0 = newest: one before g_bad_next, then back in time. */
    slot = (g_bad_next - 1 - index + BAD_HISTORY) % BAD_HISTORY;
    *len = g_bad_lens[slot];
    return &g_bad_frames[slot * BYTE_FRAME_MAX];
}

uint32_t console_task_short_frames(void)
{
    return g_line_decoder.stats.short_frames;
}

/* Bytes read from the USB serial port since boot, in every mode. Written
 * only by console_task. */
static volatile uint32_t g_usb_rx_bytes = 0;

uint32_t console_task_usb_rx_bytes(void)
{
    return g_usb_rx_bytes;
}

/* ------------------------------------------------------ host -> line */

static byte_frame_decoder g_host_decoder;

uint32_t console_task_host_overlong(void)
{
    return g_host_decoder.overlong;
}
static int g_esc_count = 0;     /* ESC presses in a row while idle, 3 = DEBUG */

/**
 * @brief One byte from the host in PACKET_GEN mode.
 *
 * Same as RC422Dev: three ESC return to FORWARDING; any other byte becomes
 * a frame "01 3F <byte> 00" plus FCS (address 1, SABM with P bit, the byte,
 * a pad), byte-stuffed back to the host. Nothing goes on the line.
 *
 * @param[in] c  Byte from the host.
 */
static void handle_packet_gen_input(uint8_t c)
{
    uint8_t frame[6];
    uint16_t crc = HDLC_CRC_INIT;
    uint16_t fcs;

    if (c == 0x1Bu)
    {
        g_esc_count++;
        if (g_esc_count >= 3)
        {
            g_esc_count = 0;
            g_console_mode = CONSOLE_FORWARDING;
            Serial.println("\r\n*** Switching to FORWARDING mode ***");
        }
        return;
    }
    g_esc_count = 0;

    frame[0] = 0x01u;
    frame[1] = 0x3Fu;
    frame[2] = c;
    frame[3] = 0x00u;
    for (int i = 0; i < 4; i++)
    {
        crc = hdlc_crc_byte(crc, frame[i]);
    }
    fcs = (uint16_t)~crc;
    frame[4] = (uint8_t)(fcs & 0xFFu);
    frame[5] = (uint8_t)(fcs >> 8u);
    console_task_send_host_frame(frame, (int)sizeof(frame));
}

/* Two tasks send frames to the line, console_task (USB) and wifi_task (TCP);
 * the static encode buffer below is shared, so one at a time. */
static SemaphoreHandle_t g_line_mutex = NULL;

static void send_line_frame_locked(const uint8_t *frame, int len, uint8_t *line,
                                   hdlc_encoder *encp, int *np);

void console_task_init(void)
{
    g_line_mutex = xSemaphoreCreateMutex();
    /* PSRAM first, internal heap as fallback; NULL leaves INSPECT BAD empty. */
    g_bad_frames = (uint8_t *)heap_caps_malloc(BAD_HISTORY * BYTE_FRAME_MAX, MALLOC_CAP_SPIRAM);
    if (g_bad_frames == NULL)
    {
        g_bad_frames = (uint8_t *)malloc(BAD_HISTORY * BYTE_FRAME_MAX);
    }
}

void console_task_send_line_frame(const uint8_t *frame, int len)
{
    /* Whole stuffed frame, built first and pushed in one piece: the encoder
     * returns at most 2 bytes per input byte, plus the opening and closing
     * flag and one idle byte. Shared by the two sending tasks under
     * g_line_mutex. */
    static uint8_t line[2 + 2 * BYTE_FRAME_MAX + 2];
    hdlc_encoder enc;
    int n = 0;

    if (g_line_mutex != NULL)
    {
        (void)xSemaphoreTake(g_line_mutex, portMAX_DELAY);
    }
    send_line_frame_locked(frame, len, line, &enc, &n);
    if (g_line_mutex != NULL)
    {
        xSemaphoreGive(g_line_mutex);
    }
}

/**
 * @brief The body of console_task_send_line_frame(), run with the mutex held.
 * @param[in]     frame  Address, control, data and FCS.
 * @param[in]     len    Number of bytes.
 * @param[out]    line   The shared encode buffer.
 * @param[in,out] enc    An encoder to use.
 * @param[out]    n      Scratch for the byte count.
 */
static HOT_PATH_CODE void send_line_frame_locked(const uint8_t *frame, int len, uint8_t *line,
                                                 hdlc_encoder *encp, int *np)
{
    hdlc_encoder &enc = *encp;
    int &n = *np;

    g_sent_stats.total_frames++;
    if (len < 0 || len > BYTE_FRAME_MAX)
    {
        g_sent_stats.bad_frames++;
        return;
    }

    n += hdlc_encoder_open(&enc, &line[n]);
    for (int i = 0; i < len; i++)
    {
        n += hdlc_encoder_write(&enc, frame[i], &line[n]);
    }
    n += hdlc_encoder_close(&enc, &line[n]);
    /* One whole byte of idle ones after the closing flag. The close above
     * pads its last byte with 1..7 ones; when the next frame is already
     * queued, those ones are all that separates two flags, and ISO 13239
     * knows no such fill: between frames the line carries either flags or
     * idle (7 ones or more). A real receiver (RC422Dev, bench 2026-11-08)
     * took exactly six pad ones plus the next flag's first zero for a
     * phantom flag and lost the frame after it, about every second frame.
     * With this byte the gap is always at least 8 ones, which every
     * receiver reads as idle and returns to hunting from. Cost: 8 bit
     * times per frame. */
    line[n++] = HDLC_IDLE;

    if (spi_tx_task_push(line, n) != 0)
    {
        g_sent_stats.bad_frames++;
        return;
    }
    g_sent_stats.ok_frames++;
    g_sent_stats.bytes += len;
}

/* ------------------------------------------------------ control frames */

#define INPUT_MAX 1024
static char g_input[INPUT_MAX + 1];
static int g_input_pos = 0;

/**
 * @brief Answer HELLO: protocol version, then key=value lines a host can
 *        read in a dump and RC422Dev can extend without a struct change.
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
                 "name=HDLCGW\nversion=%s\nbuild=%s %s\nmaxframe=%d\n"
                 "caps=cmd,log,stat,event,clock,wifi\n"
                 "chip=ESP32-S2\nboard=LOLIN S2 Mini\n",
                 HDLCGW_VERSION, __DATE__, __TIME__, BYTE_FRAME_MAX);
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
 *        frames.
 *
 * PACKETGEN is refused: after it the port would stop parsing control frames
 * and ESC ESC ESC would be the only way back. EXIT is harmless (already
 * forwarding). RESET flushes its reply before restarting (commands.cpp).
 *
 * @param[in] cf  The COMMAND frame.
 */
static void handle_control_command(const control_frame *cf)
{
    if (cf->payload_len > INPUT_MAX)
    {
        uint8_t reason = CONTROL_FRAME_NAK_BAD_LENGTH;
        (void)console_out_send_frame(CONTROL_FRAME_NAK, cf->id, CONTROL_FRAME_FLAG_LAST,
                                     &reason, 1);
        return;
    }
    memcpy(g_input, cf->payload, (size_t)cf->payload_len);
    g_input[cf->payload_len] = '\0';

    console_out_reply_begin(cf->id);
    if (strncasecmp(g_input, "PACKETGEN", 9) == 0)
    {
        console_out_println("PACKETGEN is not available through a control frame; "
                            "use the text console.");
        console_out_reply_end(true);
        return;
    }
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
            uint8_t reason = CONTROL_FRAME_NAK_BAD_LENGTH;
            (void)console_out_send_frame(CONTROL_FRAME_NAK, cf->id, CONTROL_FRAME_FLAG_LAST,
                                         &reason, 1);
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
            uint8_t reason = CONTROL_FRAME_NAK_BAD_LENGTH;
            (void)console_out_send_frame(CONTROL_FRAME_NAK, cf->id, CONTROL_FRAME_FLAG_LAST,
                                         &reason, 1);
        }
        break;

    case CONTROL_FRAME_PING:
        (void)console_out_send_frame(CONTROL_FRAME_REPLY, cf->id, CONTROL_FRAME_FLAG_LAST, NULL, 0);
        break;

    default:
    {
        uint8_t reason = CONTROL_FRAME_NAK_UNKNOWN_TYPE;
        (void)console_out_send_frame(CONTROL_FRAME_NAK, cf->id, CONTROL_FRAME_FLAG_LAST,
                                     &reason, 1);
        break;
    }
    }
}

/**
 * @brief One byte from the host while forwarding.
 *
 * Three ESC in a row between frames opens the debug console (same escape
 * hatch as RC422Dev, but checked for the whole session, not only before the
 * first frame). Anything else is fed to the byte-frame decoder. A complete
 * frame that is a control frame (address 0xFE and the un-inverted check,
 * control_frame.h) is handled here and never reaches the line; any other
 * frame goes onto the line as-is, the host supplies the FCS.
 *
 * @param[in] c  Byte from the host.
 */
static HOT_PATH_CODE void handle_forwarding_input(uint8_t c)
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
                Serial.println("\r\n*** Debug console. EXIT returns to forwarding. ***");
                console_task_show_prompt();
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
            console_task_send_line_frame(g_host_decoder.buffer, g_host_decoder.length);
        }
    }
}

/* ------------------------------------------------------ debug console */

/**
 * @brief One keystroke on the debug console: line editing and dispatch.
 * @param[in] c  Character from the host. Backspace and DEL erase; CR or LF
 *               runs the line through commands_process().
 */
static void handle_debug_input(char c)
{
    if (c == '\b' || c == 0x7f)
    {
        if (g_input_pos > 0)
        {
            g_input_pos--;
            g_input[g_input_pos] = '\0';
            Serial.write("\b \b");
        }
    }
    else if (c == '\n' || c == '\r')
    {
        Serial.println();
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
    else if (g_input_pos < INPUT_MAX)
    {
        g_input[g_input_pos++] = c;
        Serial.write(c);
    }
    else
    {
        Serial.println("\r\nError: input exceeds maximum length.");
        g_input_pos = 0;
        console_task_show_prompt();
    }
}

/** Most host bytes taken per console loop pass, so the line RX ring is
 *  drained between USB blocks however fast the PC writes. */
#define HOST_INPUT_CHUNK 256

/** @brief Read every waiting byte from the host and route it by mode. */
static void handle_host_input(void)
{
    console_mode before = g_console_mode;

    /* At most HOST_INPUT_CHUNK bytes per pass, read in one bulk call. The
     * old loop ran "while (Serial.available())": with the PC writing
     * non-stop (it sat in Write() for the whole 60 s run) the queue never
     * emptied, the loop never returned, handle_line_bytes() never ran, and
     * the line RX ring overflowed after its first 4096 bytes (bench
     * 2026-11-09, two-way runs at 983000 delivered 0 frames B->A). */
    uint8_t chunk[HOST_INPUT_CHUNK];
    int got = usb_rx_read(chunk, (int)sizeof(chunk));

    if (got > 0)
    {
        g_usb_rx_bytes = g_usb_rx_bytes + (uint32_t)got;
        toggle_led();
    }
    for (int i = 0; i < got; i++)
    {
        uint8_t c = chunk[i];

        if (g_console_mode == CONSOLE_DEBUG)
        {
            handle_debug_input((char)c);
        }
        else if (g_console_mode == CONSOLE_PACKET_GEN)
        {
            handle_packet_gen_input(c);
        }
        else
        {
            handle_forwarding_input(c);
        }
    }

    if (g_console_mode != before)
    {
        static const char *const NAMES[] = { "FORWARDING", "DEBUG", "PACKET_GEN" };
        console_out_event(CONTROL_FRAME_EVENT_MODE_CHANGE, NAMES[g_console_mode]);
    }
}

/** Bytes the Arduino core threw away because its USB receive queue was full
 *  (USBCDC::_onRX, core 3.3.12: xQueueSend with a 10 tick wait, then the rest
 *  of the USB packet is dropped and only an event and log_e say so). A lost
 *  byte inside a host frame puts that frame on the line with a bad FCS, since
 *  the ESP does not check the host's FCS. Added 2026-11-09. */
static volatile uint32_t g_usb_rx_dropped = 0;

/**
 * @brief USB CDC event callback for ARDUINO_USB_CDC_RX_OVERFLOW_EVENT, runs on
 *        the ESP event task. Adds the dropped byte count.
 * @param[in] arg         Unused.
 * @param[in] event_base  Unused.
 * @param[in] event_id    Unused.
 * @param[in] event_data  arduino_usb_cdc_event_data_t with rx_overflow set.
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

/**
 * @brief Bytes the core dropped from USB input because its queue was full.
 * @return The count since boot.
 */
uint32_t console_task_usb_rx_dropped(void)
{
    return g_usb_rx_dropped;
}

/**
 * @brief USB CDC event callback, runs on the ESP event task: the host closed
 *        the port (DTR dropped) or the cable went. Only sets a flag; the
 *        console loop does the work.
 * @param[in] arg         Unused.
 * @param[in] event_base  Unused.
 * @param[in] event_id    Unused (registered for DISCONNECTED only).
 * @param[in] event_data  Unused.
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

/** Microseconds the console loop spent reading host input / draining the
 *  line RX ring since boot (wrap after 71 min; read as deltas). Added
 *  2026-11-09 to measure where the time goes in two-way runs at 983000. */
static volatile uint32_t g_host_input_us = 0;
/** See g_host_input_us. */
static volatile uint32_t g_line_drain_us = 0;

/** The core's USB receive code (USBCDC::_onRX, core 3.3.12) waits 10 ticks
 *  (10 ms at CONFIG_FREERTOS_HZ=1000) for queue room per byte, then drops
 *  the rest of the packet. A gap longer than this between two host reads
 *  is the only way that wait can run out once the queue is full. */
#define READ_GAP_LIMIT_US 10000u

/** Longest time between the end of one host read and the start of the next
 *  since boot, microseconds (2026-11-09, to test whether field 39 drops come
 *  from console stalls). */
static volatile uint32_t g_read_gap_max_us = 0;
/** Number of host read gaps longer than READ_GAP_LIMIT_US since boot. */
static volatile uint32_t g_read_gaps_over_limit = 0;

/**
 * @brief Longest gap between two host reads since boot.
 * @return Microseconds.
 */
uint32_t console_task_read_gap_max_us(void)
{
    return g_read_gap_max_us;
}

/**
 * @brief Host read gaps longer than 10 ms since boot.
 * @return The count.
 */
uint32_t console_task_read_gaps_over_limit(void)
{
    return g_read_gaps_over_limit;
}

/**
 * @brief Microseconds spent on host input since boot.
 * @return The count; wraps, read as a delta.
 */
uint32_t console_task_host_input_us(void)
{
    return g_host_input_us;
}

/**
 * @brief Microseconds spent draining and decoding the line RX ring since boot.
 * @return The count; wraps, read as a delta.
 */
uint32_t console_task_line_drain_us(void)
{
    return g_line_drain_us;
}

/**
 * @brief Housekeeping once per loop: a lost host turns logging and stat push
 *        off and empties the USB ring (so the next host does not get a burst
 *        of stale frames); the STAT push timer.
 */
static void handle_housekeeping(void)
{
    if (g_host_lost)
    {
        g_host_lost = false;
        console_out_host_reset();
        ring_buffer_clear(&g_usb_tx_buffer);
        /* Nobody is listening now; this reaches the NEXT host only if it
         * asks for the log, which is the point: a record that it happened. */
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
    int rx_len = ring_buffer_length(&g_rx_buffer);
    int tx_len = ring_buffer_length(&g_tx_buffer);

    if (rx_len > 0 || tx_len > 0)
    {
        Serial.printf("[%d:%d]", rx_len, tx_len);
    }
    Serial.print(g_hdlc_mode ? "@" : "#");
    if (g_echo_enabled)
    {
        Serial.print(">");
    }
    Serial.print(" ");
}

void console_task_run(void *params)
{
    (void)params;

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, g_led_state);

    hdlc_decoder_reset(&g_line_decoder);
    byte_frame_decoder_reset(&g_host_decoder);
    g_line_frame.len = 0;

    /* "Disconnected" is posted when DTR drops (the PC tool sets DTR and RTS
     * on open, so closing the port drops it) and on cable removal
     * (USBCDC::_onLineState / _onUnplugged in the core). */
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
        handle_line_bytes();
        int64_t t2 = esp_timer_get_time();
        g_host_input_us = g_host_input_us + (uint32_t)(t1 - t0);
        g_line_drain_us = g_line_drain_us + (uint32_t)(t2 - t1);
        handle_housekeeping();
        usb_rx_poll();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
