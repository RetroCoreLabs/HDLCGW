/**************************************************************************
** NODE_TASK - the XMSG node on the synchronous line                     **
**                                                                       **
** One task owns everything on the path between the line and libxmsg:   **
** it reads line bytes from the SPI receive ring, decodes them with      **
** libhdlc straight into one frame buffer (per-frame mode), hands each   **
** good frame to xmsg_node_feed_frame(), and bit-stuffs every frame the  **
** node sends into the SPI transmit ring. libxmsg runs in raw framing    **
** and adds and checks the FCS itself (transport_adds_fcs = false), so   **
** the line layer only stuffs and unstuffs.                              **
**                                                                       **
** Because only this task touches the node, no lock guards it. Other     **
** tasks ask for things through node_task_request_*(), which the task    **
** picks up on its next pass, and read numbers through                   **
** node_task_get_status(), a copy that may be one pass old.              **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef NODE_TASK_H
#define NODE_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** What the console shows. Copied by the node task, never written by others. */
typedef struct
{
    uint16_t own_node;        /**< 0 = not set, the link is not started */
    int lapb_state;           /**< LapbState value */
    bool up;                  /**< LAPB connected */
    bool ready;               /**< connected and settled, data may flow */
    int32_t peer_node;        /**< learned from the peer frames, -1 until then */
    uint32_t frames_in;       /**< good frames handed to libxmsg */
    uint32_t bytes_in;        /**< bytes in those frames, FCS included */
    uint32_t frames_out;      /**< frames sent to the line */
    uint32_t bytes_out;       /**< bytes in those frames before bit stuffing, FCS included */
    uint32_t short_frames;    /**< flag-to-flag runs too short to be a frame (libhdlc) */
    uint32_t tx_full;         /**< sends refused for lack of room in the transmit ring */
    uint32_t bad_crc;         /**< line frames with a bad FCS (libhdlc) */
    uint32_t odd_bits;        /**< line frames that did not end on a byte boundary */
    uint32_t too_long;        /**< line frames longer than N1 + 4 bytes */
    uint32_t aborts;          /**< line frames cut by seven ones */
    uint32_t datagrams_in;    /**< libxmsg XmsgNodeStats */
    uint32_t datagrams_out;
    uint32_t bad_headers;
    uint32_t resyncs_asked;   /**< reachability requests that went out (one per link-ready, one
                                   per REACH while none is waiting for its answer) */
    uint32_t resyncs_done;    /**< libxmsg XmsgNodeStats.resyncs: replies that reset our count */
    uint32_t reach_in;        /**< reachability requests the peer sent us (its restarts) */
    size_t node_bytes;        /**< sizeof(XmsgNode), static RAM the node takes */
    uint32_t stack_free;      /**< least stack this task has had free, bytes */
} node_status;

/**
 * @brief Read the node number from NVS and set up the node. Call in setup()
 *        after console_out_init() and the line tasks' init.
 */
void node_task_init(void);

/** @brief The task body, for xTaskCreate. */
void node_task_run(void *params);

/** @brief Ask the task to start the link (SABM). */
void node_task_request_start(void);

/** @brief Ask the task to take the link down (DISC). */
void node_task_request_stop(void);

/**
 * @brief Ask the task to send the peer a reachability request ("are you
 *        there?", XMSG subtype 0x19) - the same one sent at every link-up.
 *        The peer's answer (0x13) adds one to resyncs_done.
 */
void node_task_request_reach(void);

/**
 * @brief Latest status.
 * @param[out] out  Filled with a copy.
 */
void node_task_get_status(node_status *out);

/**
 * @brief The last frame that came off the line, good or bad, for INSPECT.
 *
 * Written by the node task, read by the console task without a lock, as in
 * HDLCGW: a frame that ends while it is being printed shows mixed bytes.
 *
 * @param[out] len       Bytes, FCS included; 0 when none yet.
 * @param[out] crc_good  Whether its FCS was good.
 * @return The frame bytes.
 */
const uint8_t *node_task_last_frame(int *len, bool *crc_good);

/**
 * @brief One of the last four frames with a bad FCS, for INSPECT BAD [n].
 *        A bad frame is dropped and never reaches libxmsg, so this is the
 *        only way to see one.
 * @param[in]  index  0 = newest .. 3.
 * @param[out] len    Bytes, FCS included; 0 when there is no such frame.
 * @return The frame bytes.
 */
const uint8_t *node_task_last_bad_frame(int index, int *len);

#endif /* NODE_TASK_H */
