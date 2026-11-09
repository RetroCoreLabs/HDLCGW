/**************************************************************************
** WIFI_TASK - the line's frames over TCP instead of USB                 **
**                                                                       **
** Everything that touches the radio or a socket runs on this task; the  **
** console only writes settings (under a mutex) and reads counters. The  **
** station state is polled rather than taken from WiFi event callbacks,  **
** because those run on the system event task and this module wants to   **
** send EVENT frames from one task only.                                 **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#include "wifi_task.h"

#include <stdlib.h>
#include <string.h>

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_private/wifi.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <errno.h>
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>
#include <lwip/priv/tcp_priv.h>

#include "libhdlc/byte_frame.h"
#include "console_out.h"
#include "console_task.h"
#include "libhdlc/control_frame.h"
#include "libhdlc/ring_buffer.h"
#include "libhdlc/esp32s2/spi_tx_task.h"

/* Frames waiting for the peer. Internal RAM like the other three rings
 * (Ronny, 2026-11-09: every byte to the peer passes through it). It was
 * 16 KB in PSRAM; internal RAM cannot spare 16 KB (15.7 KB was the lowest
 * free during the first TCP run), and with the socket written without
 * blocking (run_peer_io) the ring only has to cover the PC's ACK delays on
 * top of lwIP's own 5.7 KB send buffer. 4 KB is about 33 ms at 983000. */
#define RING_SIZE          4096
#define LOOP_MS            10       /* pass interval with nothing to move */
#define BUSY_LOOP_MS       1        /* pass interval while bytes wait either way */
/* With bytes waiting to go to the peer and none accepted by the socket for
 * this long, the peer is dead or stuck: drop it so a new one can connect
 * (BUGS.md T2: a stalled peer was kept for ever). */
#define TCP_STALL_MS       10000
/* TCP keepalive on the peer socket: probe after 5 s of silence, every 1 s,
 * give up after 5 unanswered. Finds a peer that vanished while idle. */
#define TCP_KEEP_IDLE_S    5
#define TCP_KEEP_INTVL_S   1
#define TCP_KEEP_COUNT     5
/* 0 leaves keepalive off. It was off from 11:15 to 12:10 on 2026-11-09 while
 * BUGS.md T2 was found: lwIP aborted a peer after 14 s because the peer's
 * answers could not get in (no free WiFi receive buffer). With WIFI_RX_COPY
 * that cannot happen, and keepalive is what finds a peer that vanished. */
#define TCP_KEEPALIVE_ON   1
/* 1: the station joins with 802.11b/g only (experiment, see run_station). */
#define WIFI_NO_11N        0   /* tried 1 on 2026-11-09 11:28: same failure on 11g (BUGS.md T2) */
/* 1: copy every received WiFi frame out of the driver's buffer before lwIP
 * sees it (wifi_rx_copy, BUGS.md T2). 0: ESP-IDF's own zero-copy receive. */
#define WIFI_RX_COPY       1
/* Between connect attempts while the AP is away. Long on purpose: a WPA2
 * association can take 5..8 s, and calling WiFi.begin() again in the middle
 * of one tears it down (the driver then reports reason 36, STA_LEAVING, and
 * hides the real reason). Seen on the bench 2026-11-08 with 10 s. */
#define WIFI_RETRY_MS      20000
#define TCP_RETRY_MS       5000     /* between dial attempts in client mode */
#define TCP_CONNECT_MS     3000     /* dial timeout */
#define TCP_WRITE_CHUNK    1460     /* one TCP segment per write */
#define TCP_READ_CHUNK     1024

#define HOSTNAME "hdlcgw"
#define NVS_NAMESPACE "hdlcgw"

/** @brief Settings, as stored in NVS and as the console changes them. */
typedef struct
{
    char ssid[WIFI_TASK_SSID_MAX + 1];
    char pass[WIFI_TASK_PASS_MAX + 1];
    char host[WIFI_TASK_HOST_MAX + 1];
    uint16_t port;
    uint8_t tcp_mode;           /* wifi_task_tcp_mode */
    bool wifi_on;
    bool mirror;
} wifi_settings;

static wifi_settings g_cfg;                     /* written by the console under g_cfg_mutex */
static volatile bool g_cfg_dirty = false;        /* console changed g_cfg */
static volatile bool g_reconnect = false;        /* WIFI CONNECT asked */
static SemaphoreHandle_t g_cfg_mutex = NULL;

static ring_buffer g_tcp_tx_buffer;              /* storage from the heap, see init */
static bool g_ring_ok = false;

static wifi_task_stats g_stats;
static volatile bool g_peer_connected = false;   /* read by console_task */

static NetworkServer g_server;
static NetworkClient g_peer;
static byte_frame_decoder g_peer_decoder;

/* Peer I/O state kept between passes (run_peer_io). Bytes read from the
 * socket and not yet fed to the decoder; a decoded frame waiting for room on
 * the line; bytes taken from the ring and not yet accepted by the socket. */
static uint8_t *g_in_buf = NULL;     /* TCP_READ_CHUNK, internal heap (dram0_0_seg is full) */
static int g_in_len = 0;
static int g_in_pos = 0;
static bool g_frame_waiting = false;
static uint8_t *g_out_buf = NULL;    /* TCP_WRITE_CHUNK, internal heap */
static int g_out_len = 0;
static int g_out_pos = 0;
static uint32_t g_last_send_ms = 0;
static uint32_t g_last_read_ms = 0;              /* millis() of the last socket read with data */
static uint32_t g_dbg_reads = 0;                 /* socket reads that returned data */
static uint32_t g_dbg_avail_zero = 0;            /* passes where available() said nothing */
static uint32_t g_peer_since_ms = 0;            /* millis() when the peer was adopted */
static const char *g_drop_why = "none";         /* reason for the last peer drop */
static int g_drop_err = 0;                      /* errno or socket error with it */
static uint32_t g_drop_after_ms = 0;            /* how long that peer had been connected */

/* Task-local view of the settings and the radio. */
static wifi_settings g_live;
static bool g_server_running = false;
static bool g_wifi_was_up = false;
static bool g_mdns_started = false;
static uint32_t g_next_wifi_try = 0;
static uint32_t g_next_tcp_try = 0;
static bool g_wifi_begun = false;
static volatile uint8_t g_last_reason = 0;       /* wifi_err_reason_t of the last disconnect */

/**
 * @brief WiFi driver event, runs on the system event task: only records the
 *        disconnect reason; the WiFi task reports it. Reason codes are
 *        wifi_err_reason_t (esp_wifi_types.h): 2 AUTH_EXPIRE, 15 4WAY_HANDSHAKE
 *        TIMEOUT (wrong passphrase), 201 NO_AP_FOUND, 202 AUTH_FAIL, 205
 *        CONNECTION_FAIL.
 * @param[in] event  Which event.
 * @param[in] info   Its details.
 */
static void on_wifi_event(arduino_event_id_t event, arduino_event_info_t info)
{
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    {
        g_last_reason = info.wifi_sta_disconnected.reason;
    }
}

/**
 * @brief Read every setting from NVS into g_cfg; defaults when absent.
 */
static void load_settings(void)
{
    Preferences prefs;

    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.port = WIFI_TASK_DEFAULT_PORT;
    g_cfg.tcp_mode = WIFI_TASK_TCP_SERVER;
    if (prefs.begin(NVS_NAMESPACE, true))
    {
        prefs.getString("ssid", g_cfg.ssid, sizeof(g_cfg.ssid));
        prefs.getString("pass", g_cfg.pass, sizeof(g_cfg.pass));
        prefs.getString("thost", g_cfg.host, sizeof(g_cfg.host));
        g_cfg.port = prefs.getUShort("tport", WIFI_TASK_DEFAULT_PORT);
        g_cfg.tcp_mode = prefs.getUChar("tmode", WIFI_TASK_TCP_SERVER);
        g_cfg.wifi_on = prefs.getUChar("won", 0) != 0;
        g_cfg.mirror = prefs.getUChar("mirror", 0) != 0;
        prefs.end();
    }
    if (g_cfg.port == 0)
    {
        g_cfg.port = WIFI_TASK_DEFAULT_PORT;
    }
    if (g_cfg.tcp_mode > WIFI_TASK_TCP_CLIENT)
    {
        g_cfg.tcp_mode = WIFI_TASK_TCP_SERVER;
    }
}

/**
 * @brief Write one string setting to NVS.
 * @param[in] key    NVS key.
 * @param[in] value  NUL-terminated value.
 */
static void store_string(const char *key, const char *value)
{
    Preferences prefs;

    if (prefs.begin(NVS_NAMESPACE, false))
    {
        prefs.putString(key, value);
        prefs.end();
    }
}

/**
 * @brief Write one small integer setting to NVS.
 * @param[in] key    NVS key.
 * @param[in] value  0..65535.
 */
static void store_number(const char *key, uint16_t value)
{
    Preferences prefs;

    if (prefs.begin(NVS_NAMESPACE, false))
    {
        if (value > 255u)
        {
            prefs.putUShort(key, value);
        }
        else
        {
            prefs.putUChar(key, (uint8_t)value);
        }
        prefs.end();
    }
}

/**
 * @brief Take the settings mutex; the task and the console both use it.
 */
static void cfg_lock(void)
{
    if (g_cfg_mutex != NULL)
    {
        (void)xSemaphoreTake(g_cfg_mutex, portMAX_DELAY);
    }
}

/** @brief Release the settings mutex. */
static void cfg_unlock(void)
{
    if (g_cfg_mutex != NULL)
    {
        xSemaphoreGive(g_cfg_mutex);
    }
}

void wifi_task_init(void)
{
    g_cfg_mutex = xSemaphoreCreateMutex();
    load_settings();
    memset(&g_stats, 0, sizeof(g_stats));
    byte_frame_decoder_reset(&g_peer_decoder);

    /* Internal heap, not static (dram0_0_seg is full) and not PSRAM (hot
     * path, see RING_SIZE). Without it TCP output is simply off. */
    g_ring_ok = ring_buffer_alloc_internal(&g_tcp_tx_buffer, RING_SIZE);
    g_in_buf = (uint8_t *)heap_caps_malloc(TCP_READ_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    g_out_buf = (uint8_t *)heap_caps_malloc(TCP_WRITE_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (g_in_buf == NULL || g_out_buf == NULL)
    {
        /* No peer I/O without them: run_peer_io() checks g_ring_ok. */
        g_ring_ok = false;
    }
}

bool wifi_task_take_host_frame(const uint8_t *encoded, int n)
{
    if (!g_peer_connected || !g_ring_ok)
    {
        return false;
    }
    if (ring_buffer_write_block(&g_tcp_tx_buffer, encoded, n))
    {
        g_stats.frames_to_peer++;
    }
    else
    {
        g_stats.dropped_to_peer++;
    }
    return true;
}

bool wifi_task_tcp_connected(void)
{
    return g_peer_connected;
}

bool wifi_task_mirror(void)
{
    return g_cfg.mirror;
}

int wifi_task_set_ssid(const char *ssid)
{
    size_t len = strlen(ssid);

    if (len == 0 || len > WIFI_TASK_SSID_MAX)
    {
        return -1;
    }
    cfg_lock();
    strncpy(g_cfg.ssid, ssid, sizeof(g_cfg.ssid) - 1);
    g_cfg.ssid[sizeof(g_cfg.ssid) - 1] = '\0';
    g_cfg_dirty = true;
    cfg_unlock();
    store_string("ssid", g_cfg.ssid);
    return 0;
}

int wifi_task_set_pass(const char *pass)
{
    if (strlen(pass) > WIFI_TASK_PASS_MAX)
    {
        return -1;
    }
    cfg_lock();
    strncpy(g_cfg.pass, pass, sizeof(g_cfg.pass) - 1);
    g_cfg.pass[sizeof(g_cfg.pass) - 1] = '\0';
    g_cfg_dirty = true;
    cfg_unlock();
    store_string("pass", g_cfg.pass);
    return 0;
}

void wifi_task_set_enabled(bool enabled)
{
    cfg_lock();
    g_cfg.wifi_on = enabled;
    g_cfg_dirty = true;
    cfg_unlock();
    store_number("won", enabled ? 1u : 0u);
}

void wifi_task_connect_now(void)
{
    g_reconnect = true;
}

int wifi_task_set_tcp(wifi_task_tcp_mode mode, const char *host, uint16_t port)
{
    if (port == 0 || (host != NULL && strlen(host) > WIFI_TASK_HOST_MAX))
    {
        return -1;
    }
    cfg_lock();
    g_cfg.tcp_mode = (uint8_t)mode;
    g_cfg.port = port;
    if (host != NULL)
    {
        strncpy(g_cfg.host, host, sizeof(g_cfg.host) - 1);
        g_cfg.host[sizeof(g_cfg.host) - 1] = '\0';
    }
    g_cfg_dirty = true;
    cfg_unlock();
    store_number("tmode", (uint16_t)mode);
    store_number("tport", port);
    if (host != NULL)
    {
        store_string("thost", g_cfg.host);
    }
    return 0;
}

void wifi_task_set_mirror(bool on)
{
    cfg_lock();
    g_cfg.mirror = on;
    cfg_unlock();
    store_number("mirror", on ? 1u : 0u);
}

void wifi_task_clear_settings(void)
{
    Preferences prefs;

    if (prefs.begin(NVS_NAMESPACE, false))
    {
        prefs.clear();
        prefs.end();
    }
    cfg_lock();
    load_settings();
    g_cfg.tcp_mode = WIFI_TASK_TCP_OFF;
    g_cfg_dirty = true;
    cfg_unlock();
}

void wifi_task_get_stats(wifi_task_stats *out)
{
    *out = g_stats;
    out->overlong_from_peer = g_peer_decoder.overlong;
    out->ring_high_water = g_ring_ok ? (uint32_t)ring_buffer_high_water(&g_tcp_tx_buffer) : 0u;
    out->ring_size = g_ring_ok ? (uint32_t)ring_buffer_size(&g_tcp_tx_buffer) : 0u;
}

/**
 * @brief The station's state as one word for WIFI STATUS and events.
 * @return Static string.
 */
static const char *wifi_state_name(void)
{
    if (!g_live.wifi_on)
    {
        return "OFF";
    }
    switch (WiFi.status())
    {
    case WL_CONNECTED:
        return "CONNECTED";
    case WL_NO_SSID_AVAIL:
        return "NO_SSID";
    case WL_CONNECT_FAILED:
        return "FAILED";
    case WL_CONNECTION_LOST:
        return "LOST";
    case WL_DISCONNECTED:
        return "DISCONNECTED";
    case WL_IDLE_STATUS:
        /* fall through */
    default:
        return g_wifi_begun ? "CONNECTING" : "IDLE";
    }
}

void wifi_task_print_status(void)
{
    char ip[20];
    wifi_settings c;

    cfg_lock();
    c = g_cfg;
    cfg_unlock();

    if (WiFi.status() == WL_CONNECTED)
    {
        snprintf(ip, sizeof(ip), "%s", WiFi.localIP().toString().c_str());
    }
    else
    {
        snprintf(ip, sizeof(ip), "none");
    }
    int rssi = (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0;
    console_out_printf("WiFi: %s ssid=%s state=%s ip=%s rssi=%d hostname=%s reason=%u\r\n",
                       c.wifi_on ? "ON" : "OFF", c.ssid[0] != '\0' ? c.ssid : "(none)",
                       wifi_state_name(), ip, rssi, HOSTNAME, (unsigned)g_last_reason);
    switch (c.tcp_mode)
    {
    case WIFI_TASK_TCP_SERVER:
        console_out_printf("TCP: SERVER port=%u listening=%s peer=%s\r\n", (unsigned)c.port,
                           g_server_running ? "yes" : "no",
                           g_peer_connected ? g_peer.remoteIP().toString().c_str() : "none");
        break;
    case WIFI_TASK_TCP_CLIENT:
        console_out_printf("TCP: CLIENT host=%s port=%u state=%s\r\n",
                           c.host[0] != '\0' ? c.host : "(none)", (unsigned)c.port,
                           g_peer_connected ? "CONNECTED" : "CONNECTING");
        break;
    case WIFI_TASK_TCP_OFF:
        /* fall through */
    default:
        console_out_println("TCP: OFF");
        break;
    }
    console_out_printf("MIRROR: %s\r\n", c.mirror ? "ON" : "OFF");
    console_out_printf("TCP last drop: %s, error %d, after %u ms connected (%u drops)\r\n",
                       g_drop_why, g_drop_err, (unsigned)g_drop_after_ms,
                       (unsigned)g_stats.peer_drops);
}

/**
 * @brief Drop the peer: close the socket, forget half a frame, empty the
 *        ring, report it.
 *
 * Every caller names its reason, shown by WIFI STATUS (2026-11-09, BUGS.md
 * T2: the ESP closed a peer and nothing said which path did it).
 *
 * @param[in] why  Static text, the reason.
 * @param[in] err  errno or socket error that came with it, 0 if none.
 */
static void drop_peer(const char *why, int err)
{
    if (g_peer_connected)
    {
        g_drop_why = why;
        g_drop_err = err;
        g_drop_after_ms = millis() - g_peer_since_ms;
        g_stats.peer_drops++;
        g_peer.stop();
        g_peer_connected = false;
        byte_frame_decoder_reset(&g_peer_decoder);
        g_in_len = 0;
        g_in_pos = 0;
        g_frame_waiting = false;
        g_out_len = 0;
        g_out_pos = 0;
        if (g_ring_ok)
        {
            ring_buffer_clear(&g_tcp_tx_buffer);
        }
        console_out_event(CONTROL_FRAME_EVENT_TCP_PEER_LOST, "tcp peer lost");
    }
}

/**
 * @brief A socket became our peer: announce it, start clean.
 * @param[in] text  Where it came from, for the event.
 */
static void adopt_peer(const char *text)
{
    int one = 1;
    int idle = TCP_KEEP_IDLE_S;
    int intvl = TCP_KEEP_INTVL_S;
    int count = TCP_KEEP_COUNT;

    g_peer.setNoDelay(true);
    one = TCP_KEEPALIVE_ON;
    (void)g_peer.setSocketOption(SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    (void)g_peer.setSocketOption(IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    (void)g_peer.setSocketOption(IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    (void)g_peer.setSocketOption(IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    byte_frame_decoder_reset(&g_peer_decoder);
    g_in_len = 0;
    g_in_pos = 0;
    g_frame_waiting = false;
    g_out_len = 0;
    g_out_pos = 0;
    g_last_send_ms = millis();
    g_peer_since_ms = g_last_send_ms;
    if (g_ring_ok)
    {
        ring_buffer_clear(&g_tcp_tx_buffer);
    }
    g_peer_connected = true;
    g_stats.peer_connects++;
    console_out_event(CONTROL_FRAME_EVENT_TCP_PEER_CONNECTED, text);
}

#if WIFI_RX_COPY
/** The station's lwIP interface, set by wifi_rx_copy_install(). */
static struct netif *g_sta_netif = NULL;
static volatile uint32_t g_rx_copied = 0;      /* frames copied and handed to lwIP */
static volatile uint32_t g_rx_copy_nomem = 0;  /* frames dropped: no memory for the copy */

/**
 * @brief The station's receive callback: copy the frame out of the WiFi
 *        driver's buffer, give the buffer back at once, pass the copy to lwIP.
 *
 * Why (BUGS.md T2, 2026-11-09): the core's ESP-IDF is built without
 * CONFIG_LWIP_L2_TO_L3_COPY, so lwIP keeps the driver's receive buffer for
 * as long as it holds a packet, and TCP's out-of-order queue holds packets
 * without limit. The driver has 32 such buffers (WiFiGeneric.cpp, core
 * 3.3.12). A peer sending tiny segments therefore filled them all with
 * queued segments after one loss, and the retransmission that would have
 * emptied the queue had no buffer to arrive in. This does what that build
 * option does in ESP-IDF's own wlanif.c: pbuf_alloc(PBUF_RAW, PBUF_RAM),
 * copy, free the driver buffer, netif->input().
 *
 * Runs in the WiFi driver's task.
 *
 * @param[in] buffer  Frame, Ethernet header first.
 * @param[in] len     Bytes.
 * @param[in] eb      The driver's buffer handle, to free.
 * @return ESP_OK always (the frame is consumed either way).
 */
static esp_err_t wifi_rx_copy(void *buffer, uint16_t len, void *eb)
{
    struct netif *nif = g_sta_netif;
    struct pbuf *p = NULL;

    if (nif != NULL && buffer != NULL && netif_is_up(nif))
    {
        p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
        if (p != NULL)
        {
            memcpy(p->payload, buffer, len);
        }
    }
    if (eb != NULL)
    {
        esp_wifi_internal_free_rx_buffer(eb);
    }
    if (p == NULL)
    {
        if (nif != NULL)
        {
            g_rx_copy_nomem = g_rx_copy_nomem + 1u;
        }
        return ESP_OK;
    }
    if (nif->input(p, nif) != ERR_OK)
    {
        pbuf_free(p);
        return ESP_OK;
    }
    g_rx_copied = g_rx_copied + 1u;
    return ESP_OK;
}

/**
 * @brief Put wifi_rx_copy() in place of ESP-IDF's receive callback.
 *
 * ESP-IDF registers its own callback when the station starts
 * (esp_wifi_register_if_rxcb), so this is called each time the station
 * gets an IP, after that has happened.
 */
static void wifi_rx_copy_install(void)
{
    esp_netif_t *en = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    g_sta_netif = (en != NULL) ? (struct netif *)esp_netif_get_netif_impl(en) : NULL;
    if (g_sta_netif != NULL)
    {
        (void)esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wifi_rx_copy);
    }
}
#endif /* WIFI_RX_COPY */

/**
 * @brief Keep the station in the state the settings ask for.
 * @param[in] now  millis().
 */
static void run_station(uint32_t now)
{
    bool up = WiFi.status() == WL_CONNECTED;

    if (!g_live.wifi_on || g_live.ssid[0] == '\0')
    {
        if (g_wifi_begun)
        {
            WiFi.disconnect(true, false);
            WiFi.mode(WIFI_OFF);
            g_wifi_begun = false;
            g_mdns_started = false;
            if (g_wifi_was_up)
            {
                g_wifi_was_up = false;
                console_out_event(CONTROL_FRAME_EVENT_WIFI_DOWN, "wifi off");
            }
        }
        return;
    }

    if (g_reconnect)
    {
        g_reconnect = false;
        WiFi.disconnect(false, false);
        g_wifi_begun = false;
        g_next_wifi_try = now;
    }

    if (!up && (!g_wifi_begun || (int32_t)(now - g_next_wifi_try) >= 0))
    {
        char text[80];

        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
#if WIFI_NO_11N
        /* 802.11b/g only, no 11n, so no A-MPDU receive reordering in the
         * driver. Experiment 2026-11-09 (BUGS.md T2): lwIP waited for a
         * missing segment that never arrived, retransmits included, while
         * later segments sat in its out-of-order queue. */
        (void)esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
#endif
        WiFi.setHostname(HOSTNAME);
        WiFi.begin(g_live.ssid, g_live.pass[0] != '\0' ? g_live.pass : NULL);
        g_wifi_begun = true;
        g_next_wifi_try = now + WIFI_RETRY_MS;
        snprintf(text, sizeof(text), "ssid=%s", g_live.ssid);
        console_out_event(CONTROL_FRAME_EVENT_WIFI_CONNECTING, text);
        return;
    }

    if (up && !g_wifi_was_up)
    {
        char text[80];

        g_wifi_was_up = true;
        g_stats.wifi_connects++;
#if WIFI_RX_COPY
        wifi_rx_copy_install();
#endif
        snprintf(text, sizeof(text), "ip=%s rssi=%d", WiFi.localIP().toString().c_str(),
                 (int)WiFi.RSSI());
        console_out_event(CONTROL_FRAME_EVENT_WIFI_UP, text);
        if (!g_mdns_started && MDNS.begin(HOSTNAME))
        {
            MDNS.addService("hdlc", "tcp", g_live.port);
            g_mdns_started = true;
        }
    }
    else if (!up && g_wifi_was_up)
    {
        g_wifi_was_up = false;
        g_mdns_started = false;
        console_out_event(CONTROL_FRAME_EVENT_WIFI_DOWN, wifi_state_name());
        drop_peer("wifi down", 0);
        if (g_server_running)
        {
            g_server.end();
            g_server_running = false;
        }
    }
}

/**
 * @brief Run the TCP side for the current mode.
 * @param[in] now  millis().
 */
static void run_tcp(uint32_t now)
{
    bool up = WiFi.status() == WL_CONNECTED;

    if (!up || g_live.tcp_mode == WIFI_TASK_TCP_OFF)
    {
        drop_peer(up ? "tcp off" : "wifi not connected", 0);
        if (g_server_running)
        {
            g_server.end();
            g_server_running = false;
        }
        return;
    }

    if (g_live.tcp_mode == WIFI_TASK_TCP_SERVER)
    {
        if (!g_server_running)
        {
            char text[40];

            g_server.begin(g_live.port);
            g_server.setNoDelay(true);
            g_server_running = true;
            snprintf(text, sizeof(text), "port=%u", (unsigned)g_live.port);
            console_out_event(CONTROL_FRAME_EVENT_TCP_LISTENING, text);
        }
        if (g_server.hasClient())
        {
            NetworkClient fresh = g_server.accept();
            char text[60];

            /* A new client replaces the old one (Ronny's rule). */
            drop_peer("replaced by a new client", 0);
            g_peer = fresh;
            snprintf(text, sizeof(text), "peer=%s:%u", g_peer.remoteIP().toString().c_str(),
                     (unsigned)g_peer.remotePort());
            adopt_peer(text);
        }
    }
    else
    {
        if (g_server_running)
        {
            g_server.end();
            g_server_running = false;
        }
        if (!g_peer_connected && g_live.host[0] != '\0' && (int32_t)(now - g_next_tcp_try) >= 0)
        {
            char text[100];

            g_next_tcp_try = now + TCP_RETRY_MS;
            snprintf(text, sizeof(text), "host=%s port=%u", g_live.host, (unsigned)g_live.port);
            console_out_event(CONTROL_FRAME_EVENT_TCP_CONNECTING, text);
            if (g_peer.connect(g_live.host, g_live.port, TCP_CONNECT_MS))
            {
                adopt_peer(text);
            }
            else
            {
                console_out_event(CONTROL_FRAME_EVENT_TCP_CONNECT_FAILED, text);
            }
        }
    }

    if (g_peer_connected)
    {
        /* Ask the socket why before NetworkClient::connected() can close it:
         * fd -1 means the library already closed it itself after a failed
         * read (NetworkClient::available(), core 3.3.12). */
        int fd = g_peer.fd();
        int soerr = 0;
        socklen_t len = sizeof(soerr);

        if (fd >= 0)
        {
            (void)getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
        }
        if (!g_peer.connected())
        {
            drop_peer(fd < 0 ? "closed by the library after a read error"
                             : "socket reports not connected",
                      fd < 0 ? errno : soerr);
        }
    }
}

/**
 * @brief Try to put the decoded peer frame on the line without waiting.
 * @return true when it went (or the peer is gone), false when the line TX
 *         ring has no room yet and the frame must wait.
 */
static bool peer_frame_to_line(void)
{
    /* Worst case on the line: two bytes per frame byte after bit stuffing,
     * the two flags, the closing pad and the idle byte
     * (console_task_send_line_frame). */
    int need = 2 * g_peer_decoder.length + 6;

    if (!spi_tx_task_has_room(need))
    {
        return false;
    }
    g_stats.frames_from_peer++;
    console_task_send_line_frame(g_peer_decoder.buffer, g_peer_decoder.length);
    return true;
}

/**
 * @brief Peer to line: decode what the socket holds, send complete frames
 *        to the line, never wait.
 *
 * Until 2026-11-09 this pushed every frame through spi_tx_task_push(), which
 * sleeps 1 ms per try while the line TX ring is full; the task then neither
 * read nor wrote the socket for long stretches (BUGS.md T1, T2). Now a
 * frame that does not fit stays in the decoder, the rest stays in g_in_buf
 * and in lwIP, and TCP's own window holds the PC back.
 *
 * @return true when bytes are still waiting (call again soon).
 */
static bool peer_input(void)
{
    if (g_frame_waiting)
    {
        if (!peer_frame_to_line())
        {
            g_stats.line_full_waits++;
            return true;
        }
        g_frame_waiting = false;
    }
    while (true)
    {
        if (g_in_pos == g_in_len)
        {
            int avail = g_peer.available();
            if (avail <= 0)
            {
                g_dbg_avail_zero++;
                return false;
            }
            int n = g_peer.read(g_in_buf,
                                avail > TCP_READ_CHUNK ? TCP_READ_CHUNK : (size_t)avail);
            if (n <= 0)
            {
                return false;
            }
            g_in_len = n;
            g_in_pos = 0;
            g_dbg_reads++;
            g_last_read_ms = millis();
        }
        while (g_in_pos < g_in_len)
        {
            uint8_t b = g_in_buf[g_in_pos++];
            if (byte_frame_decoder_feed(&g_peer_decoder, b) && !peer_frame_to_line())
            {
                g_frame_waiting = true;
                g_stats.line_full_waits++;
                return true;
            }
        }
    }
}

/**
 * @brief Line to peer: hand the socket as much as it takes, never wait.
 *
 * send() with MSG_DONTWAIT on the socket itself, not NetworkClient::write(),
 * which retries a full socket up to 10 times with a 1 s select each (core
 * 3.3.12, NetworkClient.cpp) and returned short writes that this task read
 * as a broken socket. Bytes the socket did not take stay in g_out_buf.
 *
 * @param[in] now  millis().
 * @return true when bytes are still waiting (call again soon).
 */
static bool peer_output(uint32_t now)
{
    int fd = g_peer.fd();

    while (true)
    {
        if (g_out_pos == g_out_len)
        {
            if (!g_ring_ok || ring_buffer_is_empty(&g_tcp_tx_buffer))
            {
                g_last_send_ms = now;   /* nothing waiting is not a stall */
                return false;
            }
            g_out_len = ring_buffer_read_block(&g_tcp_tx_buffer, g_out_buf, TCP_WRITE_CHUNK);
            g_out_pos = 0;
        }
        int res = send(fd, g_out_buf + g_out_pos, (size_t)(g_out_len - g_out_pos), MSG_DONTWAIT);
        if (res > 0)
        {
            g_out_pos += res;
            g_stats.bytes_to_peer += (uint32_t)res;
            g_last_send_ms = now;
            continue;
        }
        if (res < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            g_stats.send_full++;
            if (now - g_last_send_ms > TCP_STALL_MS)
            {
                g_stats.stalled_peers++;
                drop_peer("stalled: socket took nothing for 10 s", 0);
                return false;
            }
            return true;
        }
        /* Any other error: the connection is gone; the bytes with it. */
        drop_peer(res == 0 ? "send returned 0" : "send error", res < 0 ? errno : 0);
        return false;
    }
}

/**
 * @brief Move bytes both ways between the peer and the line.
 * @param[in] now  millis().
 * @return true when bytes are waiting in either direction.
 */
static bool run_peer_io(uint32_t now)
{
    bool busy;

    if (!g_peer_connected || g_in_buf == NULL || g_out_buf == NULL)
    {
        return false;
    }
    /* Output first: it is the side that drops frames when it falls behind. */
    busy = peer_output(now);
    if (!g_peer_connected)
    {
        return false;
    }
    busy = peer_input() || busy;
    return busy;
}

void wifi_task_peer_debug(void)
{
    /* Read from the console task while the WiFi task owns the socket: lwIP
     * socket calls are safe from any task, and MSG_PEEK takes nothing away.
     * Added 2026-11-09 for BUGS.md T2 (the ESP stops acknowledging a peer
     * that only sends to it, while ping still answers). */
    int fd = g_peer_connected ? g_peer.fd() : -1;
    int pending = -1;
    int soerr = 0;
    int peek = -2;
    int peek_errno = 0;
    uint8_t dummy;
    socklen_t len = sizeof(soerr);

    if (fd >= 0)
    {
        (void)lwip_ioctl(fd, FIONREAD, &pending);
        (void)getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
        peek = recv(fd, &dummy, 1, MSG_PEEK | MSG_DONTWAIT);
        peek_errno = (peek < 0) ? errno : 0;
    }
    console_out_printf("TCP peer: connected=%d fd=%d FIONREAD=%d SO_ERROR=%d peek=%d errno=%d\r\n",
                       g_peer_connected ? 1 : 0, fd, pending, soerr, peek, peek_errno);
    console_out_printf("TCP peer: in_buf %d of %d used, frame waiting %d, decoder length %d, "
                       "out_buf %d of %d sent\r\n",
                       g_in_pos, g_in_len, g_frame_waiting ? 1 : 0, g_peer_decoder.length,
                       g_out_pos, g_out_len);
    console_out_printf("TCP peer: last read %u ms ago, last send %u ms ago, %u reads, "
                       "%u available() zero\r\n",
                       (unsigned)(millis() - g_last_read_ms), (unsigned)(millis() - g_last_send_ms),
                       (unsigned)g_dbg_reads, (unsigned)g_dbg_avail_zero);

    /* lwIP's own view of every active TCP connection, copied under the core
     * lock (CONFIG_LWIP_TCPIP_CORE_LOCKING=y in core 3.3.12) and printed after
     * it is released. Fields from lwip/tcp.h (struct tcp_pcb). */
    typedef struct
    {
        int state;
        uint16_t local_port;
        uint16_t remote_port;
        uint32_t rcv_nxt;
        uint32_t rcv_wnd;
        uint32_t rcv_ann_wnd;
        uint32_t snd_nxt;
        uint32_t lastack;
        uint32_t snd_wnd;
        uint32_t snd_buf;
        int refused;
        int ooseq;
        int ooseq_segs;
        int ooseq_pbufs;
        int unacked;
        int unsent;
        int nrtx;
        int persist;
        uint32_t flags;
    } pcb_copy;
    pcb_copy copies[4];
    int count = 0;
    uint8_t proto = 0;
    wifi_phy_mode_t phy = WIFI_PHY_MODE_LR;

    (void)esp_wifi_get_protocol(WIFI_IF_STA, &proto);
    (void)esp_wifi_sta_get_negotiated_phymode(&phy);
    console_out_printf("WiFi protocol bits 0x%x (1=b 2=g 4=n), negotiated phy mode %d\r\n",
                       (unsigned)proto, (int)phy);
#if WIFI_RX_COPY
    console_out_printf("WiFi receive copy: %s, %u frames copied, %u dropped for no memory\r\n",
                       g_sta_netif != NULL ? "on" : "NOT installed", (unsigned)g_rx_copied,
                       (unsigned)g_rx_copy_nomem);
#else
    console_out_println("WiFi receive copy: off (zero-copy, ESP-IDF's own)");
#endif

    LOCK_TCPIP_CORE();
    for (struct tcp_pcb *p = tcp_active_pcbs; p != NULL && count < 4; p = p->next)
    {
        pcb_copy *c = &copies[count++];
        c->state = (int)p->state;
        c->local_port = p->local_port;
        c->remote_port = p->remote_port;
        c->rcv_nxt = p->rcv_nxt;
        c->rcv_wnd = p->rcv_wnd;
        c->rcv_ann_wnd = p->rcv_ann_wnd;
        c->snd_nxt = p->snd_nxt;
        c->lastack = p->lastack;
        c->snd_wnd = p->snd_wnd;
        c->snd_buf = p->snd_buf;
        c->refused = (p->refused_data != NULL) ? (int)p->refused_data->tot_len : 0;
        c->ooseq = 0;
        c->ooseq_segs = 0;
        c->ooseq_pbufs = 0;
        for (struct tcp_seg *s = p->ooseq; s != NULL; s = s->next)
        {
            c->ooseq += (int)s->len;
            c->ooseq_segs++;
            c->ooseq_pbufs += (int)pbuf_clen(s->p);
        }
        c->unacked = 0;
        for (struct tcp_seg *s = p->unacked; s != NULL; s = s->next)
        {
            c->unacked++;
        }
        c->unsent = 0;
        for (struct tcp_seg *s = p->unsent; s != NULL; s = s->next)
        {
            c->unsent++;
        }
        c->nrtx = (int)p->nrtx;
        c->persist = (int)p->persist_backoff;
        c->flags = (uint32_t)p->flags;
    }
    UNLOCK_TCPIP_CORE();

    for (int i = 0; i < count; i++)
    {
        pcb_copy *c = &copies[i];
        console_out_printf("lwIP pcb %d: state %d port %u->%u rcv_nxt %u rcv_wnd %u ann_wnd %u "
                           "refused %d ooseq %d bytes in %d segments, %d pbufs\r\n",
                           i, c->state, (unsigned)c->local_port, (unsigned)c->remote_port,
                           (unsigned)c->rcv_nxt, (unsigned)c->rcv_wnd, (unsigned)c->rcv_ann_wnd,
                           c->refused, c->ooseq, c->ooseq_segs, c->ooseq_pbufs);
        console_out_printf("lwIP pcb %d: snd_nxt %u lastack %u snd_wnd %u snd_buf %u unacked %d "
                           "unsent %d nrtx %d persist %d flags 0x%x\r\n",
                           i, (unsigned)c->snd_nxt, (unsigned)c->lastack, (unsigned)c->snd_wnd,
                           (unsigned)c->snd_buf, c->unacked, c->unsent, c->nrtx, c->persist,
                           (unsigned)c->flags);
    }
}

void wifi_task_scan(void)
{
    int16_t n;

    console_out_println("Scanning (about 3 s)...");
    n = WiFi.scanNetworks(false, true);
    if (n <= 0)
    {
        console_out_printf("No networks found (%d).\r\n", (int)n);
        return;
    }
    for (int16_t i = 0; i < n; i++)
    {
        console_out_printf("  %-32s ch %2d  %4d dBm  %s\r\n", WiFi.SSID((uint8_t)i).c_str(),
                           (int)WiFi.channel((uint8_t)i), (int)WiFi.RSSI((uint8_t)i),
                           WiFi.encryptionType((uint8_t)i) == WIFI_AUTH_OPEN ? "open" : "secured");
    }
    WiFi.scanDelete();
}

void wifi_task_run(void *params)
{
    (void)params;

    cfg_lock();
    g_live = g_cfg;
    cfg_unlock();

    Network.onEvent(on_wifi_event, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    while (true)
    {
        uint32_t now = millis();

        if (g_cfg_dirty)
        {
            wifi_settings before = g_live;

            cfg_lock();
            g_live = g_cfg;
            g_cfg_dirty = false;
            cfg_unlock();

            /* A changed TCP mode or port restarts the TCP side; a changed
             * network restarts the station. */
            if (before.tcp_mode != g_live.tcp_mode || before.port != g_live.port
                || strcmp(before.host, g_live.host) != 0)
            {
                drop_peer("tcp settings changed", 0);
                if (g_server_running)
                {
                    g_server.end();
                    g_server_running = false;
                }
                g_next_tcp_try = now;
            }
            if (strcmp(before.ssid, g_live.ssid) != 0 || strcmp(before.pass, g_live.pass) != 0)
            {
                g_reconnect = true;
            }
        }

        run_station(now);
        run_tcp(now);
        bool busy = run_peer_io(now);

        vTaskDelay(pdMS_TO_TICKS(busy ? BUSY_LOOP_MS : LOOP_MS));
    }
}
