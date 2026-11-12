/**************************************************************************
** COMMANDS - text commands of the debug console                         **
**                                                                       **
** The command set is the one RC422Dev (the Pico version of this         **
** gateway) has, so the same host software and the same habits work on   **
** both: HELP SEND BAUD LOOPBACK ECHO DUMP CLEAR . HDLC F TASK INFO STAT **
** RESET INSPECT DECODE EXIT PACKETGEN. The additions are marked in the  **
** help text: F with no bytes sends a built-in test frame, STAT adds     **
** lines with the receiver's and the control-frame counters, CLOCK,      **
** RXDMA and LOG.                                                        **
** HDLC and ECHO became DEBUG HDLC|ECHO ON|OFF (Ronny, 2026-11-12).      **
**                                                                       **
** Output goes through console_out (console_out.h), never to Serial      **
** directly: the same handler answers a person in the DEBUG console and  **
** a COMMAND control frame from the host program, and in the second case **
** the text has to travel inside REPLY frames.                           **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#include "commands.h"

#include <stdlib.h>
#include <string.h>

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "libhdlc/byte_frame.h"
#include "console_out.h"
#include "console_task.h"
#include "libhdlc/hdlc.h"
#include "libhdlc/esp32s2/spi_rx_task.h"
#include "libhdlc/esp32s2/spi_tx_task.h"
#include "usb_tx_task.h"
#include "wifi_task.h"
#include "../common/board.h"

/* Transmit clock range, same limits as RC422Dev's setTXBaudRate(). The
 * SPI master divides 80 MHz, so any value in the range is reachable within
 * a fraction of a percent; the receive side follows the far end's clock. */
#define MIN_BIT_RATE     9600u
#define MAX_BIT_RATE     983000u
#define DEFAULT_BIT_RATE 38400u

/* Longest byte string a command accepts, before the FCS is added. */
#define CMD_MAX_BYTES 256

static void handle_help(const char *args);
static void handle_send(const char *args);
static void handle_baud(const char *args);
static void handle_loopback(const char *args);
static void handle_debug(const char *args);
static void handle_dump(const char *args);
static void handle_clear(const char *args);
static void handle_send_debug(const char *args);
static void handle_frame(const char *args);
static void handle_task(const char *args);
static void handle_info(const char *args);
static void handle_stat(const char *args);
static void handle_reset(const char *args);
static void handle_inspect(const char *args);
static void handle_decode(const char *args);
static void handle_exit(const char *args);
static void handle_packet_gen(const char *args);
static void handle_clock(const char *args);
static void handle_rxdma(const char *args);
static void handle_log(const char *args);
static void handle_wifi(const char *args);
static void handle_tcp(const char *args);
static void handle_mirror(const char *args);
static void handle_firmware(const char *args);

// clang-format off
static const command COMMANDS[] =
{
    { "HELP",      "HELP [WIFI] - All commands, or only the WiFi and TCP ones", handle_help },
    { "SEND",      "SEND <hex bytes> - Send raw bytes, no framing",          handle_send },
    { "BAUD",      "BAUD <value>|MAX|DEFAULT - Set the transmit clock (stored, shared with XMSGGW)", handle_baud },
    { "LOOPBACK",  "LOOPBACK ON|OFF - Enable or disable loopback mode",      handle_loopback },
    { "DEBUG",     "DEBUG [HDLC|ECHO ON|OFF] - Frame decoding / line data on this console", handle_debug },
    { "DUMP",      "Dump received buffer",                                   handle_dump },
    { "CLEAR",     "Clear received buffer",                                  handle_clear },
    { ".",         ". [count] - Send debug data, bytes 0..count-1",          handle_send_debug },
    { "F",         "F <hex bytes> - Send HDLC frame, FCS added (F alone: test)", handle_frame },
    { "TASK",      "List running tasks",                                     handle_task },
    { "INFO",      "Show OS info",                                           handle_info },
    { "STAT",      "Show HDLC stat",                                         handle_stat },
    { "RESET",     "Reset CPU",                                              handle_reset },
    { "FIRMWARE",  "FIRMWARE [SWITCH] - Show both firmware slots, or restart into XMSGGW", handle_firmware },
    { "INSPECT",   "INSPECT [BAD [0..3]] - Last frame, or n-th newest bad one", handle_inspect },
    { "DECODE",    "DECODE <hex bytes> - Decode an HDLC frame, nothing sent", handle_decode },
    { "EXIT",      "Exit debug mode, back to forwarding frames",             handle_exit },
    { "PACKETGEN", "Switch to packet generation mode",                       handle_packet_gen },
    { "CLOCK",     "CLOCK ON|OFF - Run or stop the transmit clock (HDLCGW)",  handle_clock },
    { "RXDMA",     "Dump the receive DMA ring (HDLCGW)",                      handle_rxdma },
    { "WIFI",      "WIFI SSID <n>|PASS <p>|ON|OFF|CONNECT|STATUS|SCAN|SHOW|CLEAR",   handle_wifi },
    { "TCP",       "TCP SERVER [port]|CLIENT <host> <port>|OFF (frames over TCP)", handle_tcp },
    { "MIRROR",    "MIRROR ON|OFF - Line frames to USB too while a TCP peer is on", handle_mirror },
    { "LOG",       "LOG [0..5] - 0 off, 1 error, 2 warn, 3 info, 4 debug, 5 trace", handle_log },
};
// clang-format on

#define NUM_COMMANDS ((int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])))

/**
 * @brief The WiFi and TCP part of HELP, one line per subcommand.
 *
 * The command table has room for one line per command; WIFI alone has nine
 * subcommands, so they are spelled out here (Ronny asked 2026-11-09).
 */
static void print_wifi_help(void)
{
    console_out_println(
        "WiFi and TCP (settings are kept in flash and survive a reset and a re-flash):");
    console_out_println(
        "  WIFI or WIFI STATUS  - State, SSID, IP, signal, host name, last disconnect reason,");
    console_out_println("                         then the TCP mode and MIRROR");
    console_out_println(
        "  WIFI SCAN            - List the networks the board can see (about 3 s; 2.4 GHz only)");
    console_out_println(
        "  WIFI SHOW            - Show the stored network name and password (USB console only)");
    console_out_println(
        "  WIFI SSID <name>     - Store the network name (up to 32 characters, spaces allowed)");
    console_out_println(
        "  WIFI PASS <pass>     - Store the passphrase (up to 64; empty for an open network)");
    console_out_println("  WIFI ON              - Turn WiFi on: connect now and after every reset");
    console_out_println("  WIFI OFF             - Turn WiFi off: radio down, frames stay on USB");
    console_out_println(
        "  WIFI CONNECT         - Drop the current connection and connect again now");
    console_out_println(
        "  WIFI CLEAR           - Erase all settings: WiFi off, no SSID, TCP off, MIRROR off");
    console_out_println(
        "  TCP SERVER [port]    - Accept one TCP peer on port (default 1362, mDNS hdlcgw.local)");
    console_out_println("  TCP CLIENT <host> <port> - Dial out to host:port, retry every 5 s");
    console_out_println("  TCP OFF              - No TCP; line frames go to USB");
    console_out_println(
        "  MIRROR ON|OFF        - With a TCP peer, send line frames to USB as well");
    console_out_println(
        "  The TCP stream carries RFC 1662 frames with FCS, like USB; commands stay on USB.");
}

/**
 * @brief HELP command: every command, then the WiFi and TCP subcommands.
 *
 * HELP WIFI (or HELP TCP) prints only the WiFi and TCP part.
 *
 * @param[in] args  Rest of the line, or NULL.
 */
static void handle_help(const char *args)
{
    if (args != NULL && (strcasecmp(args, "WIFI") == 0 || strcasecmp(args, "TCP") == 0))
    {
        print_wifi_help();
        return;
    }
    console_out_println("Available commands:");
    for (int i = 0; i < NUM_COMMANDS; i++)
    {
        console_out_printf("  %s: %s\r\n", COMMANDS[i].name, COMMANDS[i].help);
    }
    console_out_println("");
    print_wifi_help();
}

/**
 * @brief Parse pairs of hex digits (spaces allowed) into bytes.
 * @param[in]  args  NUL-terminated text.
 * @param[out] buf   Receives the bytes.
 * @param[in]  max   Capacity of buf.
 * @return Number of bytes, or -1 on a bad character, an odd digit count, or
 *         more than max bytes.
 */
static int parse_hex(const char *args, uint8_t *buf, int max)
{
    int n = 0;
    int nibbles = 0;
    uint8_t value = 0;

    for (; *args != '\0'; args++)
    {
        char c = *args;
        int d;

        if (c == ' ')
        {
            continue;
        }
        if (c >= '0' && c <= '9')
        {
            d = c - '0';
        }
        else if (c >= 'a' && c <= 'f')
        {
            d = c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'F')
        {
            d = c - 'A' + 10;
        }
        else
        {
            return -1;
        }

        value = (uint8_t)((value << 4u) | (uint8_t)d);
        nibbles++;
        if (nibbles == 2)
        {
            if (n == max)
            {
                return -1;
            }
            buf[n++] = value;
            nibbles = 0;
            value = 0;
        }
    }
    return (nibbles == 0) ? n : -1;
}

/**
 * @brief Print a decoded control field, same layout as RC422Dev's
 *        DumpControlField().
 * @param[in] control  The control byte.
 */
static void print_control_field(uint8_t control)
{
    hdlc_control_field f = hdlc_decode_control(control);

    console_out_printf("Frame Type      : %s\r\n", hdlc_frame_type_name(f.type));
    console_out_printf("Description     : %s\r\n", hdlc_frame_type_description(f.type));
    console_out_println();
    console_out_printf("Msg name        : %s\r\n", f.name);
    console_out_printf("Msg description : %s\r\n", f.description);
    console_out_printf("Poll/Final      : %d\r\n", f.poll_final ? 1 : 0);
    if (f.type == HDLC_FRAME_U)
    {
        console_out_printf("U:M1            : %d\r\n", f.m1);
        console_out_printf("U:M2            : %d\r\n", f.m2);
    }
    if (f.type == HDLC_FRAME_I)
    {
        console_out_printf("N(S)            : %d\r\n", f.ns);
    }
    if (f.type == HDLC_FRAME_I || f.type == HDLC_FRAME_S)
    {
        console_out_printf("N(R)            : %d\r\n", f.nr);
    }
    console_out_println("------");
}

/**
 * @brief Print a frame the way RC422Dev's DumpFrame() does: length without
 *        FCS, the FCS and its verdict, then the bytes eight per line.
 * @param[in] frame     Frame bytes, FCS included.
 * @param[in] len       Number of bytes, at least 2.
 * @param[in] crc_good  Whether the FCS matched.
 */
static void print_frame(const uint8_t *frame, int len, bool crc_good)
{
    int data_len = len - 2;
    uint16_t fcs = (uint16_t)(frame[len - 2] | (frame[len - 1] << 8u));

    if (len >= 2)
    {
        print_control_field(frame[1]);
    }
    console_out_printf("HDLC FrameLength = %d\r\n", data_len);
    console_out_printf("Received CRC=0x%04X", fcs);
    if (crc_good)
    {
        console_out_println(" [OK]");
    }
    else
    {
        uint16_t crc = HDLC_CRC_INIT;
        for (int i = 0; i < data_len; i++)
        {
            crc = hdlc_crc_byte(crc, frame[i]);
        }
        console_out_printf(" [NOT OK (Correct=0x%04X)]\r\n", (uint16_t)~crc);
    }
    console_out_println("\r\nData:");
    for (int i = 0; i < data_len; i++)
    {
        console_out_printf("0x%02X ", frame[i]);
        if ((i + 1) % 8 == 0)
        {
            console_out_println();
        }
    }
    console_out_println();
}

/**
 * @brief Build a frame: copy the bytes and append the FCS.
 * @param[in]  data   Address, control and data bytes.
 * @param[in]  n      Number of bytes, at most CMD_MAX_BYTES.
 * @param[out] frame  Receives n + 2 bytes.
 * @return Frame length with FCS, n + 2.
 */
static int build_frame(const uint8_t *data, int n, uint8_t *frame)
{
    uint16_t crc = HDLC_CRC_INIT;
    uint16_t fcs;

    for (int i = 0; i < n; i++)
    {
        frame[i] = data[i];
        crc = hdlc_crc_byte(crc, data[i]);
    }
    fcs = (uint16_t)~crc;
    frame[n] = (uint8_t)(fcs & 0xFFu);
    frame[n + 1] = (uint8_t)(fcs >> 8u);
    return n + 2;
}

/**
 * @brief SEND command: raw bytes onto the line, no flags, no FCS.
 * @param[in] args  Rest of the line, or NULL.
 */
static void handle_send(const char *args)
{
    uint8_t bytes[CMD_MAX_BYTES];
    int n = (args == NULL) ? -1 : parse_hex(args, bytes, CMD_MAX_BYTES);

    if (n <= 0)
    {
        console_out_println("Usage: SEND <hex bytes>");
        return;
    }
    if (spi_tx_task_push(bytes, n) != 0)
    {
        console_out_println("Too many bytes for the transmit buffer.");
        return;
    }
    console_out_println("Data sent to buffer.");
}

/**
 * @brief Set the transmit clock if the value is in range.
 * @param[in] rate  Requested bit/s.
 * @return true when set.
 */
static bool set_bit_rate(uint32_t rate)
{
    if (rate < MIN_BIT_RATE || rate > MAX_BIT_RATE)
    {
        console_out_println("Invalid baud rate selection!");
        return false;
    }
    spi_tx_task_set_bit_rate(rate);
    /* Stored in the settings both firmwares share (../common/board.h), so it
     * survives a restart and a switch to XMSGGW. */
    if (board_set_rate(rate) != 0)
    {
        console_out_println("Rate set, but could not be stored.");
    }
    return true;
}

/**
 * @brief BAUD command. Sets the TRANSMIT clock only; the receiver follows
 *        whatever clock the far end sends.
 * @param[in] args  Rest of the line, or NULL.
 */
static void handle_baud(const char *args)
{
    if (args == NULL || *args == '\0')
    {
        console_out_printf("Current baud rate: %u\r\n", (unsigned)spi_tx_task_bit_rate());
        return;
    }

    if (strcasecmp(args, "MAX") == 0)
    {
        if (set_bit_rate(MAX_BIT_RATE))
        {
            console_out_printf("Baud rate set to MAX (%u)\r\n", (unsigned)spi_tx_task_bit_rate());
        }
        return;
    }
    if (strcasecmp(args, "DEFAULT") == 0)
    {
        if (set_bit_rate(DEFAULT_BIT_RATE))
        {
            console_out_printf("Baud rate set to DEFAULT (%u)\r\n",
                               (unsigned)spi_tx_task_bit_rate());
        }
        return;
    }

    char *end = NULL;
    unsigned long value = strtoul(args, &end, 10);
    if (end == args || *end != '\0')
    {
        console_out_println("Invalid baud rate selection!");
        return;
    }
    if (set_bit_rate((uint32_t)value))
    {
        console_out_printf("Baud rate set to %u\r\n", (unsigned)spi_tx_task_bit_rate());
    }
}

/**
 * @brief LOOPBACK command.
 * @param[in] args  Rest of the line, or NULL.
 */
static void handle_loopback(const char *args)
{
    if (args == NULL || *args == '\0')
    {
        console_out_printf("Loopback is currently %s\r\n", g_loopback_enabled ? "ON" : "OFF");
    }
    else if (strcasecmp(args, "ON") == 0)
    {
        g_loopback_enabled = true;
        console_out_println("Loopback mode enabled.");
    }
    else if (strcasecmp(args, "OFF") == 0)
    {
        g_loopback_enabled = false;
        console_out_println("Loopback mode disabled.");
    }
    else
    {
        console_out_println("Invalid parameter. Use ON or OFF.");
    }
}

/**
 * @brief DEBUG command (Ronny, 2026-11-12; replaces RC422Dev's HDLC and ECHO,
 *        which flipped the switch and left you guessing its state):
 *
 *   DEBUG                 show both switches
 *   DEBUG HDLC ON|OFF     cut the line bytes into frames while the console
 *                         is open: checked, counted (STAT), kept (INSPECT),
 *                         one line each. Outside the console decoding is
 *                         always on.
 *   DEBUG ECHO ON|OFF     with HDLC on: print each good frame's bytes too;
 *                         with HDLC off: each raw line byte, as a TRACE log
 *                         line (LOG 5).
 *
 * Neither lets frames pass to or from the PC: EXIT does that.
 * @param[in] args  Rest of the line, or NULL.
 */
static void handle_debug(const char *args)
{
    char what[8];
    char state[8];
    bool on;
    int n = 0;

    /* Two words by hand: sscanf would link in 17.6 KB of scanf code (measured). */
    what[0] = '\0';
    state[0] = '\0';
    if (args != NULL)
    {
        const char *p = args;
        char *dst[2] = { what, state };
        while (n < 2)
        {
            size_t k = 0;
            while (*p == ' ')
            {
                p++;
            }
            if (*p == '\0')
            {
                break;
            }
            while (*p != '\0' && *p != ' ')
            {
                if (k < sizeof what - 1u)
                {
                    dst[n][k++] = *p;
                }
                p++;
            }
            dst[n][k] = '\0';
            n++;
        }
    }
    if (n <= 0)
    {
        console_out_printf("HDLC decoding %s, ECHO %s\r\n", g_hdlc_mode ? "ON" : "OFF",
                           g_echo_enabled ? "ON" : "OFF");
        return;
    }
    if (n != 2 || (strcasecmp(state, "ON") != 0 && strcasecmp(state, "OFF") != 0))
    {
        console_out_println("Usage: DEBUG [HDLC|ECHO ON|OFF]");
        return;
    }
    on = (strcasecmp(state, "ON") == 0);
    if (strcasecmp(what, "HDLC") == 0)
    {
        g_hdlc_mode = on;
        console_out_printf("HDLC decoding %s.\r\n", on ? "ON" : "OFF");
    }
    else if (strcasecmp(what, "ECHO") == 0)
    {
        g_echo_enabled = on;
        console_out_printf("ECHO %s.\r\n", on ? "ON" : "OFF");
    }
    else
    {
        console_out_println("Usage: DEBUG [HDLC|ECHO ON|OFF]");
    }
}

/**
 * @brief DUMP command.
 * @param[in] args  Rest of the line, or NULL.
 */
static void handle_dump(const char *args)
{
    int len = ring_buffer_length(&g_rx_buffer);

    (void)args;
    if (len == 0)
    {
        console_out_println("Nothing to dump");
        return;
    }

    console_out_printf("Dumping %d bytes\r\n", len);
    for (int i = 0; i < len; i++)
    {
        console_out_printf("0x%02X ", ring_buffer_read(&g_rx_buffer));
        if ((i + 1) % 8 == 0)
        {
            console_out_println();
        }
    }
    console_out_println();
}

/**
 * @brief CLEAR command.
 * @param[in] args  Rest of the line, or NULL.
 */
static void handle_clear(const char *args)
{
    (void)args;
    ring_buffer_clear(&g_rx_buffer);
    console_out_println("Cleared receive buffer");
}

/**
 * @brief "." command: raw bytes 0, 1, 2, ... count-1 onto the line.
 * @param[in] args  Count, or NULL for 10.
 */
static void handle_send_debug(const char *args)
{
    uint8_t bytes[CMD_MAX_BYTES];
    int count = 10;

    if (args != NULL && *args != '\0')
    {
        char *end = NULL;
        unsigned long value = strtoul(args, &end, 10);
        if (end == args || *end != '\0' || value == 0 || value > CMD_MAX_BYTES)
        {
            console_out_printf("Usage: . [count], 1..%d\r\n", CMD_MAX_BYTES);
            return;
        }
        count = (int)value;
    }

    console_out_printf("Sending %d bytes\r\n", count);
    for (int i = 0; i < count; i++)
    {
        bytes[i] = (uint8_t)i;
    }
    (void)spi_tx_task_push(bytes, count);     /* count <= CMD_MAX_BYTES always fits */
}


/**
 * @brief F command: build a frame with FCS, show it, send it.
 *
 * With bytes it is RC422Dev's F. With no bytes (HDLCGW addition) it sends
 * the built-in test frame: address 55, control 00, data A..T, 24 bytes with
 * FCS. The bench test in spi_rx_task.h relies on that.
 *
 * @param[in] args  Hex bytes, or NULL / empty for the test frame.
 */
static void handle_frame(const char *args)
{
    uint8_t bytes[CMD_MAX_BYTES];
    uint8_t frame[CMD_MAX_BYTES + 2];
    int n;

    if (args == NULL || *args == '\0')
    {
        bytes[0] = 0x55u;   /* address */
        bytes[1] = 0x00u;   /* control: I frame, N(S) 0, N(R) 0 */
        for (int i = 0; i < 20; i++)
        {
            bytes[2 + i] = (uint8_t)('A' + i);
        }
        n = 22;
    }
    else
    {
        n = parse_hex(args, bytes, CMD_MAX_BYTES);
        if (n <= 0)
        {
            console_out_println("Usage: F <hex bytes>");
            return;
        }
    }

    int total = build_frame(bytes, n, frame);
    print_frame(frame, total, true);
    console_out_println("=> Sending frame!");
    console_task_send_line_frame(frame, total);
}

/**
 * @brief TASK command: every FreeRTOS task with state, priority, free stack
 *        and CPU time since boot.
 *
 * "Run us" is FreeRTOS's per-task run time (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
 * with the esp_timer clock in core 3.3.12's sdkconfig, so microseconds; a uint32
 * that wraps after 71 min). Take TASK before and after a run and subtract: that
 * is CPU time, unlike the STAT "Time" counters, which are wall clock and include
 * time a higher-priority task took (added 2026-11-09).
 *
 * @param[in] args  Rest of the line, or NULL.
 */
static void handle_task(const char *args)
{
    UBaseType_t count = uxTaskGetNumberOfTasks();
    TaskStatus_t *status = (TaskStatus_t *)malloc(count * sizeof(TaskStatus_t));

    (void)args;
    if (status == NULL)
    {
        console_out_println("Out of memory");
        return;
    }
    configRUN_TIME_COUNTER_TYPE total = 0;
    count = uxTaskGetSystemState(status, count, &total);

    console_out_println();
    console_out_println("Task Name    State    Priority   Stack    Task ID  Run us");
    console_out_println("------------------------------------------------------------");
    for (UBaseType_t i = 0; i < count; i++)
    {
        console_out_printf("%-12s %-8d %-10u %-8u %-8u %u\r\n", status[i].pcTaskName,
                      (int)status[i].eCurrentState, (unsigned)status[i].uxCurrentPriority,
                      (unsigned)status[i].usStackHighWaterMark, (unsigned)status[i].xTaskNumber,
                      (unsigned)status[i].ulRunTimeCounter);
    }
    console_out_printf("Total run us %u\r\n", (unsigned)total);
    free(status);
}

/**
 * @brief INFO command: build time, FreeRTOS version, heap, chip.
 * @param[in] args  Rest of the line, or NULL.
 */
static void handle_info(const char *args)
{
    (void)args;
    console_out_printf("Build: %s %s\r\n", __DATE__, __TIME__);
    console_out_printf("FreeRTOS version: %s\r\n", tskKERNEL_VERSION_NUMBER);
    console_out_printf("ESP-IDF: %s, chip %s rev %u, %u MHz\r\n", ESP.getSdkVersion(),
                  ESP.getChipModel(), (unsigned)ESP.getChipRevision(),
                  (unsigned)ESP.getCpuFreqMHz());
    console_out_printf("Free Heap: %u bytes\r\n", (unsigned)ESP.getFreeHeap());
    console_out_printf("Minimum Ever Free Heap: %u bytes\r\n", (unsigned)ESP.getMinFreeHeap());
    console_out_printf("Heap size: %u bytes\r\n", (unsigned)ESP.getHeapSize());
    console_out_printf("PSRAM: %u bytes, %u free (TCP ring and bad-frame history; "
                       "the line and USB rings are internal)\r\n",
                       (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    wifi_task_print_status();
}

/**
 * @brief Copy the first word of args (up to a space) into word, return the rest.
 * @param[in]  args  Text after the command name.
 * @param[out] word  Receives the word, NUL terminated.
 * @param[in]  max   Size of word.
 * @return Pointer to the text after the word and its following spaces.
 */
static const char *take_word(const char *args, char *word, int max)
{
    int n = 0;

    while (*args == ' ')
    {
        args++;
    }
    while (*args != '\0' && *args != ' ' && n < max - 1)
    {
        word[n++] = *args++;
    }
    word[n] = '\0';
    while (*args == ' ')
    {
        args++;
    }
    return args;
}

/**
 * @brief WIFI command (HDLCGW addition): station settings and state.
 *        Settings are kept in NVS and survive a reset.
 * @param[in] args  SSID <name>, PASS <passphrase>, ON, OFF, CONNECT, STATUS,
 *                  CLEAR; NULL or empty shows the status.
 */
static void handle_wifi(const char *args)
{
    char word[16];
    const char *rest;

    if (args == NULL || *args == '\0')
    {
        wifi_task_print_status();
        return;
    }
    rest = take_word(args, word, (int)sizeof(word));

    if (strcasecmp(word, "SSID") == 0)
    {
        if (wifi_task_set_ssid(rest) != 0)
        {
            console_out_printf("SSID must be 1..%d characters.\r\n", WIFI_TASK_SSID_MAX);
            return;
        }
        console_out_println("SSID stored. WIFI ON or WIFI CONNECT to use it.");
    }
    else if (strcasecmp(word, "PASS") == 0)
    {
        if (wifi_task_set_pass(rest) != 0)
        {
            console_out_printf("Passphrase is at most %d characters.\r\n", WIFI_TASK_PASS_MAX);
            return;
        }
        console_out_println("Passphrase stored.");
    }
    else if (strcasecmp(word, "ON") == 0)
    {
        wifi_task_set_enabled(true);
        console_out_println("WiFi ON: connecting, and again after every reset.");
    }
    else if (strcasecmp(word, "OFF") == 0)
    {
        wifi_task_set_enabled(false);
        console_out_println("WiFi OFF: radio down, frames stay on USB.");
    }
    else if (strcasecmp(word, "CONNECT") == 0)
    {
        wifi_task_connect_now();
        console_out_println("Reconnecting.");
    }
    else if (strcasecmp(word, "STATUS") == 0)
    {
        wifi_task_print_status();
    }
    else if (strcasecmp(word, "CLEAR") == 0)
    {
        wifi_task_clear_settings();
        console_out_println("All WiFi and TCP settings erased; WiFi off, TCP off.");
    }
    else if (strcasecmp(word, "SCAN") == 0)
    {
        wifi_task_scan();
    }
    else if (strcasecmp(word, "SHOW") == 0)
    {
        wifi_task_show();
    }
    else
    {
        console_out_println("Use WIFI SSID <name>, PASS <passphrase>, ON, OFF, CONNECT, STATUS, "
                            "SCAN, SHOW or CLEAR.");
    }
}

/**
 * @brief TCP command (HDLCGW addition): how the line's frames travel over
 *        the network. Stored in NVS.
 * @param[in] args  SERVER [port], CLIENT <host> <port>, OFF; NULL or empty
 *                  shows the status.
 */
static void handle_tcp(const char *args)
{
    char word[16];
    char host[WIFI_TASK_HOST_MAX + 1];
    char number[8];
    const char *rest;

    if (args == NULL || *args == '\0')
    {
        wifi_task_print_status();
        return;
    }
    rest = take_word(args, word, (int)sizeof(word));

    if (strcasecmp(word, "DEBUG") == 0)
    {
        wifi_task_peer_debug();
        return;
    }
    if (strcasecmp(word, "SERVER") == 0)
    {
        uint16_t port = WIFI_TASK_DEFAULT_PORT;

        if (*rest != '\0')
        {
            char *end = NULL;
            unsigned long value = strtoul(rest, &end, 10);
            if (end == rest || *end != '\0' || value == 0 || value > 65535u)
            {
                console_out_println("Port must be 1..65535.");
                return;
            }
            port = (uint16_t)value;
        }
        (void)wifi_task_set_tcp(WIFI_TASK_TCP_SERVER, NULL, port);
        console_out_printf("TCP SERVER on port %u: nd100x dials hdlcgw.local:%u.\r\n",
                           (unsigned)port, (unsigned)port);
    }
    else if (strcasecmp(word, "CLIENT") == 0)
    {
        rest = take_word(rest, host, (int)sizeof(host));
        (void)take_word(rest, number, (int)sizeof(number));
        char *end = NULL;
        unsigned long value = strtoul(number, &end, 10);
        if (host[0] == '\0' || number[0] == '\0' || end == number || *end != '\0' || value == 0
            || value > 65535u)
        {
            console_out_println("Use TCP CLIENT <host> <port>.");
            return;
        }
        if (wifi_task_set_tcp(WIFI_TASK_TCP_CLIENT, host, (uint16_t)value) != 0)
        {
            console_out_printf("Host name is at most %d characters.\r\n", WIFI_TASK_HOST_MAX);
            return;
        }
        console_out_printf("TCP CLIENT: dialing %s:%lu whenever WiFi is up.\r\n", host, value);
    }
    else if (strcasecmp(word, "OFF") == 0)
    {
        (void)wifi_task_set_tcp(WIFI_TASK_TCP_OFF, NULL, WIFI_TASK_DEFAULT_PORT);
        console_out_println("TCP OFF: frames stay on USB.");
    }
    else
    {
        console_out_println("Use TCP SERVER [port], TCP CLIENT <host> <port> or TCP OFF.");
    }
}

/**
 * @brief MIRROR command (HDLCGW addition).
 * @param[in] args  ON or OFF; NULL or empty shows the state.
 */
static void handle_mirror(const char *args)
{
    if (args == NULL || *args == '\0')
    {
        console_out_printf("MIRROR is %s\r\n", wifi_task_mirror() ? "ON" : "OFF");
    }
    else if (strcasecmp(args, "ON") == 0)
    {
        wifi_task_set_mirror(true);
        console_out_println("MIRROR ON: line frames go to USB as well as to the TCP peer.");
    }
    else if (strcasecmp(args, "OFF") == 0)
    {
        wifi_task_set_mirror(false);
        console_out_println("MIRROR OFF: line frames go to the TCP peer only while one is on.");
    }
    else
    {
        console_out_println("Invalid parameter. Use ON or OFF.");
    }
}

/**
 * @brief STAT command.
 * @param[in] args  Rest of the line, or NULL.
 */
static void handle_stat(const char *args)
{
    (void)args;
    console_out_println("HDLC frames   Total#   OK    BAD     BYTES");
    console_out_println("------------  ------ ------ ------ ----------");
    console_out_printf("SENT          %6d %6d %6d %10d\r\n", g_sent_stats.total_frames,
                  g_sent_stats.ok_frames, g_sent_stats.bad_frames, g_sent_stats.bytes);
    console_out_printf("RECEIVED      %6d %6d %6d %10d\r\n", g_received_stats.total_frames,
                  g_received_stats.ok_frames, g_received_stats.bad_frames,
                  g_received_stats.bytes);

    spi_rx_task_stats rx;
    spi_rx_task_get_stats(&rx);
    console_out_printf("Line RX: %u bytes, %u idle dropped, %u FIFO overruns, %u DMA errors, "
                  "%u ring overflow, %u words peeked\r\n",
                  (unsigned)rx.bytes, (unsigned)rx.idle_dropped, (unsigned)rx.fifo_full,
                  (unsigned)rx.dma_errors, (unsigned)rx.overflow, (unsigned)rx.peeked);
    console_out_printf("Line RX passes: longest gap %u us, %u gaps over %u us, most descriptors "
                       "in one pass %u of %d (ring of 4-byte words)\r\n",
                       (unsigned)rx.pass_gap_max_us, (unsigned)rx.pass_gaps_over,
                       (unsigned)SPI_RX_GAP_WARN_US, (unsigned)rx.desc_max, 512);
    console_out_printf("Line TX: %u data bytes, %u idle bytes\r\n",
                  (unsigned)spi_tx_task_data_bytes(), (unsigned)spi_tx_task_idle_bytes());
    console_out_printf("USB RX: %u bytes from the host, %u dropped by the core (queue full)\r\n",
                  (unsigned)console_task_usb_rx_bytes(), (unsigned)console_task_usb_rx_dropped());
    console_out_printf("USB TX: %u frames dropped (host slower than line), "
                  "%u short writes resumed, %u flushes with the port closed\r\n",
                  (unsigned)usb_tx_task_dropped(), (unsigned)usb_tx_task_short_writes(),
                  (unsigned)usb_tx_task_host_closed_flushes());
    console_out_printf("Rings high water: line RX %d of %d, line TX %d of %d, USB TX %d of %d\r\n",
                  ring_buffer_high_water(&g_rx_buffer), ring_buffer_size(&g_rx_buffer),
                  ring_buffer_high_water(&g_tx_buffer), ring_buffer_size(&g_tx_buffer),
                  ring_buffer_high_water(&g_usb_tx_buffer), ring_buffer_size(&g_usb_tx_buffer));
    console_out_printf("Internal heap: %u free, %u lowest since boot\r\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    console_out_printf("Line decoder: %u short flag-to-flag runs ignored\r\n",
                  (unsigned)console_task_short_frames());

    wifi_task_stats wf;
    wifi_task_get_stats(&wf);
    console_out_printf("TCP: %u frames to peer, %u bytes, %u frames from peer, %u dropped, "
                       "%u peer connects, %u wifi connects\r\n",
                       (unsigned)wf.frames_to_peer, (unsigned)wf.bytes_to_peer,
                       (unsigned)wf.frames_from_peer, (unsigned)wf.dropped_to_peer,
                       (unsigned)wf.peer_connects, (unsigned)wf.wifi_connects);
    console_out_printf("TCP flow: socket full %u times, peer frame waited for line room %u "
                       "times, %u stalled peers dropped, ring high water %u of %u\r\n",
                       (unsigned)wf.send_full, (unsigned)wf.line_full_waits,
                       (unsigned)wf.stalled_peers, (unsigned)wf.ring_high_water,
                       (unsigned)wf.ring_size);
    console_out_printf("Too long (over %d bytes), dropped: %u from USB, %u from TCP\r\n",
                       BYTE_FRAME_MAX, (unsigned)console_task_host_overlong(),
                       (unsigned)wf.overlong_from_peer);
    console_out_printf("Time (us, wraps): USB write %u for %u bytes, host input %u, "
                       "line drain %u\r\n",
                       (unsigned)usb_tx_task_write_us(), (unsigned)usb_tx_task_written_bytes(),
                       (unsigned)console_task_host_input_us(),
                       (unsigned)console_task_line_drain_us());
    console_out_printf("Host read gap: longest %u us, %u over 10 ms (core drops after 10 ms)\r\n",
                       (unsigned)console_task_read_gap_max_us(),
                       (unsigned)console_task_read_gaps_over_limit());
    console_out_printf("USB write: longest %u us, %u over 20 ms, sleeps %u after 0 taken, "
                       "%u ring empty\r\n",
                       (unsigned)usb_tx_task_write_max_us(),
                       (unsigned)usb_tx_task_writes_slow(),
                       (unsigned)usb_tx_task_zero_write_sleeps(),
                       (unsigned)usb_tx_task_empty_sleeps());
    console_out_printf("USB write: longest run of writes over 1 ms each: %u us\r\n",
                       (unsigned)usb_tx_task_slow_run_max_us());

    console_out_stats co;
    console_out_get_stats(&co);
    console_out_printf("Control frames: %u replies, %u logs, %u dropped; log level %u, "
                       "stat push %u ms\r\n",
                       (unsigned)co.replies, (unsigned)co.logs, (unsigned)co.dropped,
                       (unsigned)g_console_out_log_level,
                       (unsigned)g_console_out_stat_period_ms);
}

/**
 * @brief RESET command: restart the chip. The reply is closed and the USB
 *        ring drained first, so a host that asked through a control frame
 *        sees the answer.
 * @param[in] args  Rest of the line, or NULL.
 */
/**
 * @brief FIRMWARE command: show what both slots hold, or (SWITCH) restart
 *        into the other one (../common/board.h).
 * @param[in] args  NULL / empty, or SWITCH.
 */
static void handle_firmware(const char *args)
{
    char text[512];

    if (args == NULL || *args == '\0')
    {
        board_firmware_report(text, sizeof text);
        console_out_printf("%s", text);
        return;
    }
    if (strcasecmp(args, "SWITCH") != 0)
    {
        console_out_println("Usage: FIRMWARE [SWITCH]");
        return;
    }
    if (board_firmware_switch(text, sizeof text) != 0)
    {
        console_out_printf("%s", text);
        return;
    }
    console_out_printf("%s", text);
    console_out_flush();
    ESP.restart();
}

static void handle_reset(const char *args)
{
    (void)args;
    console_out_println("Resetting...");
    console_out_flush();
    ESP.restart();
}

/**
 * @brief LOG command: show or set the log level (HDLCGW addition). The same
 *        level the LOG_LEVEL control frame sets; in DEBUG mode it only
 *        matters for TRACE, which is off unless asked for.
 * @param[in] args  0..5, or NULL / empty to show.
 */
static void handle_log(const char *args)
{
    static const char *const NAMES[] = { "OFF", "ERROR", "WARN", "INFO", "DEBUG", "TRACE" };

    if (args != NULL && *args != '\0')
    {
        char *end = NULL;
        unsigned long value = strtoul(args, &end, 10);
        if (end == args || *end != '\0' || value > CONTROL_FRAME_LOG_TRACE)
        {
            console_out_println("Usage: LOG [0..5]");
            return;
        }
        g_console_out_log_level = (uint8_t)value;
    }
    console_out_printf("Log level %u (%s)\r\n", (unsigned)g_console_out_log_level,
                       NAMES[g_console_out_log_level]);
}

/**
 * @brief INSPECT command: show the last frame received from the line.
 * @param[in] args  Rest of the line, or NULL.
 */
static void handle_inspect(const char *args)
{
    int len;
    bool crc_good;
    const uint8_t *frame;

    if (args != NULL && strncasecmp(args, "BAD", 3) == 0)
    {
        /* HDLCGW addition: INSPECT BAD [n], the n-th newest frame that failed
         * its FCS (0..3, default 0); such frames are never forwarded to the
         * host and so cannot be seen any other way. */
        int index = 0;
        const char *rest = args + 3;

        while (*rest == ' ')
        {
            rest++;
        }
        if (*rest != '\0')
        {
            char *end = NULL;
            unsigned long value = strtoul(rest, &end, 10);
            if (end == rest || *end != '\0' || value > 3u)
            {
                console_out_println("Use INSPECT BAD [0..3], 0 = newest.");
                return;
            }
            index = (int)value;
        }
        frame = console_task_last_bad_frame(index, &len);
        crc_good = false;
        if (len < 2)
        {
            console_out_printf("No bad frame %d received yet\r\n", index);
            return;
        }
        console_out_printf("Bad frame %d (0 = newest):\r\n", index);
        print_frame(frame, len, crc_good);
        return;
    }

    frame = console_task_last_frame(&len, &crc_good);
    if (len < 2)
    {
        console_out_println("No frame received yet");
        return;
    }
    print_frame(frame, len, crc_good);
}

/**
 * @brief DECODE command: build a frame from hex and show it; nothing is sent.
 * @param[in] args  Hex bytes, or NULL.
 */
static void handle_decode(const char *args)
{
    uint8_t bytes[CMD_MAX_BYTES];
    uint8_t frame[CMD_MAX_BYTES + 2];
    int n = (args == NULL) ? -1 : parse_hex(args, bytes, CMD_MAX_BYTES);

    if (n <= 0)
    {
        console_out_println("Usage: DECODE <hex bytes>");
        return;
    }
    int total = build_frame(bytes, n, frame);
    print_frame(frame, total, true);
}

/**
 * @brief EXIT command.
 * @param[in] args  Rest of the line, or NULL.
 */
static void handle_exit(const char *args)
{
    (void)args;
    console_out_println("Exiting debug mode...");
    g_console_mode = CONSOLE_FORWARDING;
}

/**
 * @brief PACKETGEN command.
 * @param[in] args  Rest of the line, or NULL.
 */
static void handle_packet_gen(const char *args)
{
    (void)args;
    console_out_println("Switching to Packet Generation mode...");
    console_out_println("Every character you type will generate an HDLC frame.");
    console_out_println("Press ESC three times to return to FORWARDING mode.");
    g_console_mode = CONSOLE_PACKET_GEN;
}

/**
 * @brief CLOCK command: run or stop the transmit clock. HDLCGW addition, not
 *        in RC422Dev; a quiet line is needed when a far end's receiver has to
 *        be tested without a continuous clock.
 * @param[in] args  ON, OFF, or NULL / empty to show the state.
 */
static void handle_clock(const char *args)
{
    if (args == NULL || *args == '\0')
    {
        console_out_printf("Transmit clock is %s\r\n", spi_tx_task_clock_enabled() ? "ON" : "OFF");
    }
    else if (strcasecmp(args, "ON") == 0)
    {
        spi_tx_task_set_clock_enabled(true);
        console_out_println("Transmit clock ON: idle ones are clocked out between frames.");
    }
    else if (strcasecmp(args, "OFF") == 0)
    {
        spi_tx_task_set_clock_enabled(false);
        console_out_println("Transmit clock OFF: line quiet, frames wait until CLOCK ON.");
    }
    else
    {
        console_out_println("Invalid parameter. Use ON or OFF.");
    }
}

/** @brief Print one text line from libhdlc on the console. */
static void print_text(const char *text)
{
    console_out_print(text);
}

/**
 * @brief RXDMA command: show the receive DMA descriptors. HDLCGW addition.
 * @param[in] args  Rest of the line, or NULL.
 */
static void handle_rxdma(const char *args)
{
    (void)args;
    spi_rx_task_print_dma(print_text);
}

void commands_process(char *line)
{
    char *name = strtok(line, " ");
    char *args = strtok(NULL, "");

    if (name == NULL)
    {
        return;
    }

    for (int i = 0; i < NUM_COMMANDS; i++)
    {
        if (strcasecmp(name, COMMANDS[i].name) == 0)
        {
            COMMANDS[i].handler(args);
            return;
        }
    }

    console_out_printf("Unknown command '%s'. Type HELP for a list of commands.\r\n", name);
}
