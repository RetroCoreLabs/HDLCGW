/**************************************************************************
** WIFI_TASK - the line's frames over TCP instead of USB                 **
**                                                                       **
** WHY: nd100x (the ND-100 emulator) speaks HDLC over a plain TCP        **
** stream of RFC 1662 byte-stuffed frames (its --hdlc option), the same  **
** framing this gateway uses on USB. With WiFi the gateway can sit next  **
** to the synchronous line and the emulator can be anywhere on the LAN,  **
** no PC in between. Decided 2026-11-08 (Ronny), shape agreed with the   **
** PC-side session the same day:                                         **
**  - WiFi station only; SSID, password, mode, host, port and the mirror **
**    switch live in NVS (Preferences, namespace "hdlcgw") and survive a **
**    reset; WiFi.persistent(false) so the driver never writes flash on  **
**    its own.                                                           **
**  - TCP SERVER on port 1362 by default (nd100x dials us); CLIENT mode  **
**    selectable (we dial host:port, reconnect with backoff).            **
**  - The TCP stream carries pure RFC 1662 frames with their FCS, no     **
**    idle flags, never a control frame. Control frames, the console and **
**    the log stay on USB.                                               **
**  - While a peer is connected, frames from the line go to the peer and **
**    frames from the peer go to the line; USB keeps getting line frames **
**    only when MIRROR is on. With no peer, USB gets them as before.     **
**  - A new TCP client replaces the old one.                             **
**  - The 16 KB ring of frames waiting for the peer is allocated from    **
**    the heap: static data on this build is capped near 122 KB.         **
**  - Hostname hdlcgw, announced with mDNS, so "hdlcgw.local" works.     **
**  - WiFi.setSleep(false): modem sleep adds tens of ms of latency.       **
**  - State changes are EVENT control frames 16..23 (control_frame.h).   **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#ifndef WIFI_TASK_H
#define WIFI_TASK_H

#include <stdint.h>
#include <stdbool.h>

#define WIFI_TASK_SSID_MAX   32     /**< longest SSID, bytes (802.11 limit) */
#define WIFI_TASK_PASS_MAX   64     /**< longest WPA passphrase, bytes */
#define WIFI_TASK_HOST_MAX   64     /**< longest TCP client target host name */
#define WIFI_TASK_DEFAULT_PORT 1362 /**< nd100x's HDLC port, also the PC tool's default */

/** @brief How the TCP side is used. Stored in NVS. */
typedef enum
{
    WIFI_TASK_TCP_OFF = 0,      /**< no TCP; frames stay on USB */
    WIFI_TASK_TCP_SERVER = 1,   /**< listen on port, one peer at a time */
    WIFI_TASK_TCP_CLIENT = 2    /**< dial host:port, reconnect with backoff */
} wifi_task_tcp_mode;

/** @brief Counters for STAT. */
typedef struct
{
    uint32_t frames_to_peer;    /**< line frames handed to the TCP ring */
    uint32_t bytes_to_peer;     /**< bytes written to the peer, stuffing included */
    uint32_t frames_from_peer;  /**< complete frames decoded from the peer and sent to the line */
    uint32_t dropped_to_peer;   /**< line frames dropped because the TCP ring was full */
    uint32_t peer_connects;     /**< TCP connections accepted or made */
    uint32_t wifi_connects;     /**< times the station got an IP */
    uint32_t overlong_from_peer; /**< peer frames dropped for being longer than
                                      BYTE_FRAME_MAX */
    uint32_t send_full;         /**< sends the socket refused because its buffer was full
                                     (BUGS.md T1, 2026-11-09) */
    uint32_t line_full_waits;   /**< passes where a peer frame waited for room in the
                                     line TX ring (BUGS.md T2) */
    uint32_t stalled_peers;     /**< peers dropped because nothing moved for
                                     TCP_STALL_MS while data was waiting (BUGS.md T2) */
    uint32_t peer_drops;        /**< peers dropped for any reason (WIFI STATUS names the last) */
    uint32_t ring_high_water;   /**< most bytes ever waiting in the TCP output ring */
    uint32_t ring_size;         /**< size of that ring, bytes */
} wifi_task_stats;

/**
 * @brief Load the settings from NVS and allocate the TCP ring. Call once from
 *        setup(), before any task starts. Does not touch the radio.
 */
void wifi_task_init(void);

/**
 * @brief FreeRTOS task entry point. Runs the station, the TCP side and the
 *        frame routing forever.
 *
 * Every 10 ms: apply settings changed by the console, keep the station up
 * when enabled (reconnect with backoff), run the TCP server or client, read
 * the peer into a byte-frame decoder and send complete frames to the line,
 * and write the TCP ring to the peer. Never returns.
 *
 * @param[in] params  Unused.
 */
void wifi_task_run(void *params);

/**
 * @brief Offer a byte-stuffed line frame to the TCP peer.
 *
 * Called by console_task for every good frame off the line. Never blocks:
 * when the ring is full the frame is dropped and counted.
 *
 * @param[in] encoded  The frame, already byte-stuffed with flags.
 * @param[in] n        Number of bytes.
 * @retval true   A peer is connected and TCP is where line frames go now
 *                (queued, or dropped and counted). The caller sends the
 *                frame to USB as well only when wifi_task_mirror() is on.
 * @retval false  No peer; the caller sends the frame to USB as before.
 */
bool wifi_task_take_host_frame(const uint8_t *encoded, int n);

/**
 * @brief Whether a TCP peer is connected right now.
 * @return true when connected.
 */
bool wifi_task_tcp_connected(void);

/**
 * @brief Whether line frames also go to USB while a peer is connected.
 * @return true when MIRROR is on.
 */
bool wifi_task_mirror(void);

/**
 * @brief Set the station's SSID (stored in NVS). Takes effect at the next
 *        connect.
 * @param[in] ssid  1..WIFI_TASK_SSID_MAX bytes.
 * @return 0, or -1 when the length is out of range.
 */
int wifi_task_set_ssid(const char *ssid);

/**
 * @brief Set the station's passphrase (stored in NVS). Empty for an open
 *        network.
 * @param[in] pass  0..WIFI_TASK_PASS_MAX bytes.
 * @return 0, or -1 when too long.
 */
int wifi_task_set_pass(const char *pass);

/**
 * @brief Turn the station on or off (stored in NVS). On connects now and
 *        after every reset; off disconnects and powers the radio down.
 * @param[in] enabled  true for on.
 */
void wifi_task_set_enabled(bool enabled);

/**
 * @brief Drop the current association and connect again now.
 */
void wifi_task_connect_now(void);

/**
 * @brief Choose the TCP side (stored in NVS).
 * @param[in] mode  OFF, SERVER or CLIENT.
 * @param[in] host  Client target host, ignored for the other modes; may be NULL.
 * @param[in] port  Port to listen on or to dial, 1..65535.
 * @return 0, or -1 when host is too long or port is 0.
 */
int wifi_task_set_tcp(wifi_task_tcp_mode mode, const char *host, uint16_t port);

/**
 * @brief Mirror line frames to USB while a peer is connected (stored in NVS).
 * @param[in] on  true for on.
 */
void wifi_task_set_mirror(bool on);

/**
 * @brief Erase every stored setting: WiFi off, TCP off, mirror off, no SSID.
 */
void wifi_task_clear_settings(void);

/**
 * @brief Print the WIFI STATUS text through console_out: three lines,
 *        "WiFi: ...", "TCP: ..." and "MIRROR: ...", fields as key=value so
 *        the PC side can parse them.
 */
void wifi_task_print_status(void);

/**
 * @brief Scan for networks and print them through console_out: SSID,
 *        channel, signal, open or secured. Blocks the caller about 3 s and
 *        interrupts a connection attempt in progress (the station retries).
 */
void wifi_task_scan(void);

/**
 * @brief TCP DEBUG: print the peer socket's state as lwIP and this task see
 *        it (bytes lwIP holds for us, socket error, our buffers, last read).
 */
void wifi_task_peer_debug(void);

/**
 * @brief Snapshot of the counters.
 * @param[out] out  Receives the counters; must not be NULL.
 */
void wifi_task_get_stats(wifi_task_stats *out);

#endif /* WIFI_TASK_H */
