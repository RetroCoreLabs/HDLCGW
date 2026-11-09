/**************************************************************************
** COMMANDS - text commands, typed or sent in a COMMAND control frame    **
**                                                                       **
** The same table and handlers as HDLCGW commands.cpp for what applies   **
** to a gateway that runs its own node (HELP, BAUD, CLOCK, TASK, INFO,   **
** STAT, LOG, RESET, INSPECT, EXIT), plus NODE and LINK. HDLCGW's        **
** frame-sending and test commands are left out: here the node, not a    **
** person, talks to the line.                                            **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "commands.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "libhdlc/hdlc.h"
#include "libhdlc/esp32s2/spi_rx_task.h"
#include "libhdlc/esp32s2/spi_tx_task.h"

#include "console_out.h"
#include "console_task.h"
#include "node_task.h"
#include "settings.h"
#include "telnet_task.h"
#include "usb_tx_task.h"

/* Same limits as HDLCGW: 983000 is the most the SPI clock divider gives
 * cleanly; the design rate is 307200. */
#define MIN_BIT_RATE     9600u
#define MAX_BIT_RATE     983000u
#define DEFAULT_BIT_RATE 38400u

static void handle_help(const char *args);
static void handle_baud(const char *args);
static void handle_clock(const char *args);
static void handle_task(const char *args);
static void handle_info(const char *args);
static void handle_stat(const char *args);
static void handle_log(const char *args);
static void handle_reset(const char *args);
static void handle_inspect(const char *args);
static void handle_exit(const char *args);
static void handle_node(const char *args);
static void handle_link(const char *args);
static void handle_reach(const char *args);
static void handle_term(const char *args);
static void handle_wifi(const char *args);

static const command COMMANDS[] =
{
    { "HELP",    "All commands",                                               handle_help },
    { "STAT",    "Line, node and USB counters",                                handle_stat },
    { "NODE",    "NODE [n] - Show or store our node number (used after RESET)", handle_node },
    { "LINK",    "LINK START|STOP - Start the link or take it down",           handle_link },
    { "REACH",   "Ask the peer 'are you there?'; STAT shows asked / answered", handle_reach },
    { "TERM",    "TERM CONNECT <system>|TYPE <text>|SHOW|STATUS|HANGUP - terminal session",
      handle_term },
    { "WIFI",    "WIFI [STATUS]|SSID <name>|PASS <password>|CONNECT - network for telnet",
      handle_wifi },
    { "BAUD",    "BAUD <value>|MAX|DEFAULT - Set the transmit clock (stored)", handle_baud },
    { "CLOCK",   "CLOCK ON|OFF - Run or stop the transmit clock",              handle_clock },
    { "INSPECT", "INSPECT [BAD [0..3]] - Last frame, or n-th newest bad one",  handle_inspect },
    { "LOG",     "LOG [0..5] - 0 off, 1 error, 2 warn, 3 info, 4 debug, 5 trace", handle_log },
    { "TASK",    "List running tasks",                                         handle_task },
    { "INFO",    "Build, chip and memory",                                     handle_info },
    { "RESET",   "Restart the gateway",                                        handle_reset },
    { "EXIT",    "Leave the text console, back to control frames",             handle_exit },
};

#define NUM_COMMANDS ((int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])))

/**
 * @brief HELP command.
 * @param[in] args  Unused.
 */
static void handle_help(const char *args)
{
    (void)args;
    console_out_println("Available commands:");
    for (int i = 0; i < NUM_COMMANDS; i++)
    {
        console_out_printf("  %s: %s\r\n", COMMANDS[i].name, COMMANDS[i].help);
    }
}

/**
 * @brief Set and store the transmit bit rate.
 * @param[in] rate  Bits per second.
 * @return true when in range and set.
 */
static bool set_bit_rate(uint32_t rate)
{
    if (rate < MIN_BIT_RATE || rate > MAX_BIT_RATE)
    {
        console_out_printf("Bit rate must be %u..%u.\r\n", (unsigned)MIN_BIT_RATE,
                           (unsigned)MAX_BIT_RATE);
        return false;
    }
    spi_tx_task_set_bit_rate(rate);
    if (settings_set_rate(rate) != 0)
    {
        console_out_println("Rate set, but could not be stored.");
    }
    return true;
}

/**
 * @brief BAUD command. Sets the TRANSMIT clock only; the receiver follows
 *        whatever clock the far end sends. Stored, so it survives RESET.
 * @param[in] args  Rate, MAX, DEFAULT, or NULL to show.
 */
static void handle_baud(const char *args)
{
    uint32_t rate;

    if (args == NULL || *args == '\0')
    {
        console_out_printf("Current baud rate: %u\r\n", (unsigned)spi_tx_task_bit_rate());
        return;
    }
    if (strcasecmp(args, "MAX") == 0)
    {
        rate = MAX_BIT_RATE;
    }
    else if (strcasecmp(args, "DEFAULT") == 0)
    {
        rate = DEFAULT_BIT_RATE;
    }
    else
    {
        char *end = NULL;
        unsigned long value = strtoul(args, &end, 10);
        if (end == args || *end != '\0')
        {
            console_out_println("Usage: BAUD <value>|MAX|DEFAULT");
            return;
        }
        rate = (uint32_t)value;
    }
    if (set_bit_rate(rate))
    {
        console_out_printf("Baud rate set to %u\r\n", (unsigned)spi_tx_task_bit_rate());
    }
}

/**
 * @brief CLOCK command: run or stop the transmit clock.
 * @param[in] args  ON, OFF, or NULL to show.
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
        console_out_println("Transmit clock ON.");
    }
    else if (strcasecmp(args, "OFF") == 0)
    {
        spi_tx_task_set_clock_enabled(false);
        console_out_println("Transmit clock OFF: line quiet, frames wait until CLOCK ON.");
    }
    else
    {
        console_out_println("Usage: CLOCK ON|OFF");
    }
}

/**
 * @brief TASK command: every FreeRTOS task with its least free stack.
 * @param[in] args  Unused.
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

    console_out_println("Task Name    State    Priority   Stack    Task ID  Run us");
    console_out_println("------------------------------------------------------------");
    for (UBaseType_t i = 0; i < count; i++)
    {
        console_out_printf("%-12s %-8d %-10u %-8u %-8u %u\r\n", status[i].pcTaskName,
                           (int)status[i].eCurrentState, (unsigned)status[i].uxCurrentPriority,
                           (unsigned)status[i].usStackHighWaterMark,
                           (unsigned)status[i].xTaskNumber, (unsigned)status[i].ulRunTimeCounter);
    }
    console_out_printf("Total run us %u\r\n", (unsigned)total);
    free(status);
}

/**
 * @brief INFO command: build, chip, memory, and what the node takes.
 * @param[in] args  Unused.
 */
static void handle_info(const char *args)
{
    node_status st;

    (void)args;
    node_task_get_status(&st);
    console_out_printf("XMSGGW %s, build %s %s\r\n", XMSGGW_VERSION, __DATE__, __TIME__);
    console_out_printf("FreeRTOS version: %s\r\n", tskKERNEL_VERSION_NUMBER);
    console_out_printf("ESP-IDF: %s, chip %s rev %u, %u MHz\r\n", ESP.getSdkVersion(),
                       ESP.getChipModel(), (unsigned)ESP.getChipRevision(),
                       (unsigned)ESP.getCpuFreqMHz());
    console_out_printf("Internal RAM free %u (least %u), heap size %u\r\n",
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                       (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                       (unsigned)ESP.getHeapSize());
    console_out_printf("PSRAM: %u bytes, %u free\r\n", (unsigned)ESP.getPsramSize(),
                       (unsigned)ESP.getFreePsram());
    console_out_printf("XMSG node: %u bytes of static RAM\r\n", (unsigned)st.node_bytes);
}

/**
 * @brief STAT command: the node, the line and the USB port.
 * @param[in] args  Unused.
 */
static void handle_stat(const char *args)
{
    node_status st;
    spi_rx_task_stats rx;

    (void)args;
    node_task_get_status(&st);
    spi_rx_task_get_stats(&rx);

    if (st.own_node == 0)
    {
        console_out_println("Node: not set (NODE <n>, then RESET)");
    }
    else
    {
        console_out_printf("Node %u, LAPB state %d, %s%s, peer %ld\r\n", (unsigned)st.own_node,
                           st.lapb_state, st.up ? "up" : "down", st.ready ? ", ready" : "",
                           (long)st.peer_node);
    }
    console_out_printf("XMSG: datagrams in %u, out %u, bad headers %u\r\n",
                       (unsigned)st.datagrams_in, (unsigned)st.datagrams_out,
                       (unsigned)st.bad_headers);
    console_out_printf("Reachability: we sent %u, peer answered %u, peer asked us %u\r\n",
                       (unsigned)st.resyncs_asked, (unsigned)st.resyncs_done,
                       (unsigned)st.reach_in);
    console_out_printf("Line %u bit/s, clock %s\r\n", (unsigned)spi_tx_task_bit_rate(),
                       spi_tx_task_clock_enabled() ? "on" : "off");
    console_out_printf("  sent     %u frames, %u bytes; transmit ring full %u times\r\n",
                       (unsigned)st.frames_out, (unsigned)st.bytes_out, (unsigned)st.tx_full);
    console_out_printf("  received %u good frames, %u bytes\r\n", (unsigned)st.frames_in,
                       (unsigned)st.bytes_in);
    console_out_printf("  bad: FCS %u, odd bits %u, too long %u, aborts %u, short runs %u\r\n",
                       (unsigned)st.bad_crc, (unsigned)st.odd_bits, (unsigned)st.too_long,
                       (unsigned)st.aborts, (unsigned)st.short_frames);
    console_out_printf("  receive: bytes %u, FIFO overruns %u, DMA errors %u, ring overflow %u, "
                       "longest gap %u us\r\n",
                       (unsigned)rx.bytes, (unsigned)rx.fifo_full, (unsigned)rx.dma_errors,
                       (unsigned)rx.overflow, (unsigned)rx.pass_gap_max_us);
    console_out_printf("USB: in %u bytes, core dropped %u, host frames not control %u, "
                       "too long %u; out dropped %u\r\n",
                       (unsigned)console_task_usb_rx_bytes(),
                       (unsigned)console_task_usb_rx_dropped(),
                       (unsigned)console_task_host_dropped(),
                       (unsigned)console_task_host_overlong(), (unsigned)usb_tx_task_dropped());
    console_out_printf("Node task: least stack free %u bytes\r\n", (unsigned)st.stack_free);
}

/**
 * @brief LOG command: show or set the log level (the same level the
 *        LOG_LEVEL control frame sets).
 * @param[in] args  0..5, or NULL to show.
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
 * @brief RESET command: flush the reply so the host sees it, then restart.
 * @param[in] args  Unused.
 */
static void handle_reset(const char *args)
{
    (void)args;
    console_out_println("Resetting...");
    console_out_flush();
    ESP.restart();
}

/**
 * @brief Print a frame: control field, length, FCS verdict, bytes eight per
 *        line. The same layout as HDLCGW INSPECT.
 * @param[in] frame     Frame bytes, FCS included.
 * @param[in] len       Number of bytes, at least 2.
 * @param[in] crc_good  Whether the FCS matched.
 */
static void print_frame(const uint8_t *frame, int len, bool crc_good)
{
    int data_len = len - 2;
    uint16_t fcs = (uint16_t)(frame[len - 2] | (frame[len - 1] << 8u));
    hdlc_control_field f = hdlc_decode_control(frame[1]);

    console_out_printf("Frame type %s, %s, P/F %d", hdlc_frame_type_name(f.type), f.name,
                       f.poll_final ? 1 : 0);
    if (f.type == HDLC_FRAME_I)
    {
        console_out_printf(", N(S) %d", f.ns);
    }
    if (f.type == HDLC_FRAME_I || f.type == HDLC_FRAME_S)
    {
        console_out_printf(", N(R) %d", f.nr);
    }
    console_out_println();
    console_out_printf("Length %d without FCS, FCS 0x%04X", data_len, fcs);
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
 * @brief INSPECT command: the last frame off the line, or INSPECT BAD [n],
 *        the n-th newest one whose FCS was bad.
 * @param[in] args  NULL, or BAD [0..3].
 */
static void handle_inspect(const char *args)
{
    int len;
    bool crc_good;
    const uint8_t *frame;

    if (args != NULL && strncasecmp(args, "BAD", 3) == 0)
    {
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
        frame = node_task_last_bad_frame(index, &len);
        if (len < 2)
        {
            console_out_printf("No bad frame %d received yet\r\n", index);
            return;
        }
        console_out_printf("Bad frame %d (0 = newest):\r\n", index);
        print_frame(frame, len, false);
        return;
    }

    frame = node_task_last_frame(&len, &crc_good);
    if (len < 2)
    {
        console_out_println("No frame received yet");
        return;
    }
    print_frame(frame, len, crc_good);
}

/**
 * @brief EXIT command: back to control frames.
 * @param[in] args  Unused.
 */
static void handle_exit(const char *args)
{
    (void)args;
    console_out_println("Leaving the text console...");
    g_console_mode = CONSOLE_FORWARDING;
}

/**
 * @brief NODE command: show or store our node number.
 * @param[in] args  1..65535, or NULL to show.
 */
static void handle_node(const char *args)
{
    node_status st;

    if (args == NULL || *args == '\0')
    {
        node_task_get_status(&st);
        console_out_printf("Stored node number %u, running as %u (0 = not set)\r\n",
                           (unsigned)settings_node(), (unsigned)st.own_node);
        return;
    }
    char *end = NULL;
    unsigned long value = strtoul(args, &end, 10);
    if (end == args || *end != '\0' || value == 0 || value > 65535u)
    {
        console_out_println("NODE takes a number from 1 to 65535.");
        return;
    }
    if (settings_set_node((uint16_t)value) != 0)
    {
        console_out_println("Could not store the node number.");
        return;
    }
    console_out_printf("Node number %u stored; RESET to use it.\r\n", (unsigned)value);
}

/**
 * @brief LINK command: start the link (SABM) or take it down (DISC).
 * @param[in] args  START or STOP.
 */
static void handle_link(const char *args)
{
    if (args != NULL && strcasecmp(args, "START") == 0)
    {
        node_task_request_start();
        console_out_println("Link start asked for.");
    }
    else if (args != NULL && strcasecmp(args, "STOP") == 0)
    {
        node_task_request_stop();
        console_out_println("Link stop asked for.");
    }
    else
    {
        console_out_println("Usage: LINK START|STOP");
    }
}

/**
 * @brief REACH command: queue a reachability request to the peer. The node
 *        task sends it on its next pass once the peer number is known.
 * @param[in] args  Unused.
 */
static void handle_reach(const char *args)
{
    node_status st;

    (void)args;
    node_task_get_status(&st);
    if (!st.up || st.peer_node < 0)
    {
        console_out_println("The link is not up (or the peer is not known yet); nothing sent.");
        return;
    }
    node_task_request_reach();
    console_out_printf("Reachability request to node %ld queued (nothing new is sent while an "
                       "earlier one waits for its answer). Before it: sent %u, answered %u.\r\n", (long)st.peer_node, (unsigned)st.resyncs_asked,
                       (unsigned)st.resyncs_done);
}

/**
 * @brief Print the host text collected since the last SHOW. Bit 7 is
 *        cleared for display (a host may send parity); control bytes other
 *        than CR, LF, BS and TAB are shown as <xx>.
 */
static void term_show(void)
{
    uint8_t buf[128];
    int n;
    int total = 0;

    while ((n = term_read(TERM_CONSOLE_SLOT, buf, (int)sizeof buf)) > 0)
    {
        for (int i = 0; i < n; i++)
        {
            uint8_t c = (uint8_t)(buf[i] & 0x7Fu);
            if (c >= 32u || c == '\r' || c == '\n' || c == '\b' || c == '\t')
            {
                char s[2] = { (char)c, '\0' };
                console_out_print(s);
            }
            else
            {
                console_out_printf("<%02X>", (unsigned)c);
            }
        }
        total += n;
    }
    console_out_printf("\r\n[%d bytes of host text]\r\n", total);
}

/**
 * @brief TERM command: one terminal session to a system on the peer.
 * @param[in] args  CONNECT <system>, TYPE <text> (CR added), SHOW, STATUS
 *                  or HANGUP.
 */
static void handle_term(const char *args)
{
    if (args == NULL)
    {
        args = "";
    }
    if (strncasecmp(args, "CONNECT", 7) == 0)
    {
        const char *sys = args + 7;
        while (*sys == ' ')
        {
            sys++;
        }
        if (*sys == '\0')
        {
            console_out_println("Usage: TERM CONNECT <system>, for example TERM CONNECT D100");
        }
        else if (term_open(TERM_CONSOLE_SLOT, sys) != 0)
        {
            console_out_println("Not started: a session is open, or the name is too long.");
        }
        else
        {
            console_out_printf("Connect to %s asked for; TERM STATUS and TERM SHOW follow it.\r\n",
                               sys);
        }
    }
    else if (strncasecmp(args, "TYPE", 4) == 0)
    {
        const char *text = args + 4;
        uint8_t line[200];
        int len;
        if (*text == ' ')
        {
            text++;
        }
        len = (int)strlen(text);
        if (len > (int)sizeof line - 1)
        {
            len = (int)sizeof line - 1;
        }
        memcpy(line, text, (size_t)len);
        line[len++] = '\r';
        int took = term_type(TERM_CONSOLE_SLOT, line, len);
        if (took < 0)
        {
            console_out_println("No session open.");
        }
        else
        {
            console_out_printf("%d of %d bytes queued (CR included).\r\n", took, len);
        }
    }
    else if (strcasecmp(args, "SHOW") == 0)
    {
        term_show();
    }
    else if (strcasecmp(args, "HANGUP") == 0)
    {
        term_hangup(TERM_CONSOLE_SLOT);
        console_out_println("Hangup asked for.");
    }
    else if (strcasecmp(args, "STATUS") == 0 || *args == '\0')
    {
        term_status ts;
        term_get_status(TERM_CONSOLE_SLOT, &ts);
        console_out_printf("Session %s, error %d, %s; host text in %u bytes (lost %u), typed out "
                           "%u bytes, %d waiting\r\n",
                           ts.state, ts.error, ts.may_type ? "may type" : "may not type",
                           (unsigned)ts.bytes_in, (unsigned)ts.lost_in, (unsigned)ts.bytes_out,
                           ts.queued_out);
    }
    else
    {
        console_out_println("Usage: TERM CONNECT <system>|TYPE <text>|SHOW|STATUS|HANGUP");
    }
}

/**
 * @brief WIFI command. The password is stored but never printed.
 * @param[in] args  STATUS (or nothing), SSID <name>, PASS <password> or
 *                  CONNECT (read the settings again and join).
 */
static void handle_wifi(const char *args)
{
    if (args == NULL || *args == '\0' || strcasecmp(args, "STATUS") == 0)
    {
        telnet_task_stats ts;
        char ssid[SETTINGS_SSID_SIZE];
        char pass[SETTINGS_PASS_SIZE];
        int src = settings_wifi(ssid, pass);
        static const char *const SRC[] = { "XMSGGW's own", "HDLCGW's stored" };
        telnet_task_get_stats(&ts);
        memset(pass, 0, sizeof pass);
        if (src < 0)
        {
            console_out_println("WiFi: no network stored (WIFI SSID <name>, WIFI PASS <p>, "
                                "WIFI CONNECT)");
        }
        else
        {
            console_out_printf("WiFi network %s (%s settings), %s", ssid, SRC[src],
                               ts.wifi_up ? "joined" : "not joined");
            if (ts.wifi_up)
            {
                console_out_printf(", ip %u.%u.%u.%u, rssi %d dBm",
                                   (unsigned)(ts.ip & 0xFFu), (unsigned)((ts.ip >> 8u) & 0xFFu),
                                   (unsigned)((ts.ip >> 16u) & 0xFFu), (unsigned)(ts.ip >> 24u),
                                   ts.rssi);
            }
            console_out_println();
        }
        console_out_printf("Telnet port %u: %u of %u users on; users so far %u, turned away %u, "
                           "WiFi joins %u, typed %u bytes, sent %u bytes\r\n",
                           (unsigned)TELNET_TASK_PORT, (unsigned)ts.users_on,
                           (unsigned)TELNET_TASK_USERS,
                           (unsigned)ts.users, (unsigned)ts.refused, (unsigned)ts.wifi_connects,
                           (unsigned)ts.bytes_from_user, (unsigned)ts.bytes_to_user);
        return;
    }
    if (strncasecmp(args, "SSID ", 5) == 0)
    {
        if (settings_set_wifi_ssid(args + 5) != 0)
        {
            console_out_println("Not stored: 1 to 32 characters.");
            return;
        }
        console_out_println("Network name stored; WIFI CONNECT to use it.");
        return;
    }
    if (strncasecmp(args, "PASS", 4) == 0 && (args[4] == ' ' || args[4] == '\0'))
    {
        const char *p = (args[4] == ' ') ? args + 5 : "";
        if (settings_set_wifi_pass(p) != 0)
        {
            console_out_println("Not stored: at most 64 characters.");
            return;
        }
        console_out_println("Password stored (not shown); WIFI CONNECT to use it.");
        return;
    }
    if (strcasecmp(args, "CONNECT") == 0)
    {
        telnet_task_reconnect();
        console_out_println("Joining the network again; WIFI STATUS shows when it is up.");
        return;
    }
    console_out_println("Usage: WIFI [STATUS]|SSID <name>|PASS <password>|CONNECT");
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
