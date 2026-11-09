/**************************************************************************
** CONTROL_FRAME - messages between the host PC and this firmware,       **
**                 carried on the same byte-stuffed channel as the data  **
**                 frames                                                **
**                                                                       **
** WHY: in FORWARDING mode the USB port carries HDLC frames for the      **
** line, and the only way to reach the firmware itself used to be ESC    **
** ESC ESC into a text console, which takes the whole port out of        **
** forwarding and leaves the host to scrape text. A control frame is a   **
** byte-stuffed frame like any other, but the firmware keeps it: it      **
** carries a text command, a reply, a log line or statistics, and it     **
** never reaches the line. Decided 2026-11-08 (Ronny).                   **
**                                                                       **
** HOW A CONTROL FRAME IS TOLD FROM A DATA FRAME: two locks.             **
**  1. The HDLC address byte is CONTROL_FRAME_ADDRESS (0xFE, Ronny's     **
**     choice). ISO 13239 defines 0x00 (no station) and 0xFF (all        **
**     stations); X.25 / ND LAPB links use 0x01 and 0x03. 0xFE is none   **
**     of those and is not escaped on the wire.                          **
**  2. The two check bytes at the end are the CRC-16/X.25 register       **
**     stored WITHOUT the usual inversion. Running the CRC over a good   **
**     HDLC frame (FCS included) always leaves 0xF0B8 in the register;   **
**     running it over a control frame always leaves 0x0000. No byte     **
**     string can satisfy both, so a data frame with a good FCS can      **
**     never be taken for a control frame even if it is addressed to     **
**     0xFE. The only frame that could be misread is one the host sends  **
**     with a deliberately WRONG FCS to address 0xFE whose wrong FCS      **
**     happens to give residue 0 (1 in 65536): the host avoids that      **
**     address for such tests.                                           **
**                                                                       **
** Layout inside the byte stuffing (little-endian where it matters):     **
**   0      address      0xFE                                            **
**   1      type         bit 7 clear = host -> firmware, set = reply     **
**   2      id           request id 1..255 chosen by the host and copied **
**                       into every reply chunk; 0 = unsolicited          **
**   3      flags        bit 0 LAST, bit 1 ERROR, bits 4..7 chunk count   **
**   4..    payload      up to CONTROL_FRAME_MAX_PAYLOAD bytes            **
**   last 2 check        un-inverted CRC, low byte first                  **
**                                                                       **
** Pure C-style code, no Arduino, so the PC can unit test the same       **
** logic; the C# twin is ControlFrame.cs in HdlcGatewayTest.Core and     **
** the two must agree byte for byte.                                     **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#ifndef CONTROL_FRAME_H
#define CONTROL_FRAME_H

#include <stdint.h>
#include <stdbool.h>

#include "byte_frame.h"

#define CONTROL_FRAME_ADDRESS   0xFEu   /**< HDLC address that marks a control frame */
#define CONTROL_FRAME_VERSION   1u      /**< protocol version reported in HELLO_REPLY */
#define CONTROL_FRAME_HEADER    4       /**< address, type, id, flags */
#define CONTROL_FRAME_CHECK     2       /**< the un-inverted CRC */
#define CONTROL_FRAME_OVERHEAD  (CONTROL_FRAME_HEADER + CONTROL_FRAME_CHECK)
/** Longest payload that still fits in BYTE_FRAME_MAX. */
#define CONTROL_FRAME_MAX_PAYLOAD (BYTE_FRAME_MAX - CONTROL_FRAME_OVERHEAD)
/** CRC register after a control frame including its check bytes. */
#define CONTROL_FRAME_RESIDUE   0x0000u

/** @name Message types, host to firmware (bit 7 clear)
 *  @{ */
#define CONTROL_FRAME_HELLO      0x01u  /**< identify; resets log level and stat push to off */
#define CONTROL_FRAME_COMMAND    0x02u  /**< payload: a console command line, no CR/LF */
#define CONTROL_FRAME_LOG_LEVEL  0x03u  /**< payload: one byte, 0 off .. 5 trace */
#define CONTROL_FRAME_STAT_PUSH  0x04u  /**< payload: u16 period in ms, 0 = off, min 100 */
#define CONTROL_FRAME_PING       0x05u  /**< empty; answered with an empty REPLY */
/** @} */

/** @name Message types, firmware to host (bit 7 set)
 *  @{ */
#define CONTROL_FRAME_HELLO_REPLY 0x81u /**< version byte, then key=value lines */
#define CONTROL_FRAME_REPLY       0x82u /**< text produced by a command, chunked */
#define CONTROL_FRAME_LOG         0x83u /**< id 0: level, u32 millis, text */
#define CONTROL_FRAME_STAT        0x84u /**< id 0: list of u32 counters */
#define CONTROL_FRAME_EVENT       0x85u /**< id 0: event code, then text */
#define CONTROL_FRAME_NAK         0x86u /**< reason byte, then text */
/** @} */

/** @name Flags
 *  @{ */
#define CONTROL_FRAME_FLAG_LAST   0x01u /**< final chunk of a reply */
#define CONTROL_FRAME_FLAG_ERROR  0x02u /**< the command failed or was refused */
#define CONTROL_FRAME_CHUNK_SHIFT 4u    /**< chunk counter lives in bits 4..7 */
/** @} */

/** @name Log levels (LOG_LEVEL payload and LOG first byte)
 *  @{ */
#define CONTROL_FRAME_LOG_OFF   0u
#define CONTROL_FRAME_LOG_ERROR 1u
#define CONTROL_FRAME_LOG_WARN  2u
#define CONTROL_FRAME_LOG_INFO  3u
#define CONTROL_FRAME_LOG_DEBUG 4u
#define CONTROL_FRAME_LOG_TRACE 5u
/** @} */

/** @name Event codes (EVENT first byte)
 *  @{ */
#define CONTROL_FRAME_EVENT_BOOT           1u  /**< firmware started; text = banner */
#define CONTROL_FRAME_EVENT_MODE_CHANGE    2u  /**< text = FORWARDING / DEBUG / PACKET_GEN */
#define CONTROL_FRAME_EVENT_LINE_RX_ACTIVE 3u  /**< first byte from the line after idle */
#define CONTROL_FRAME_EVENT_LINE_RX_IDLE   4u  /**< no byte from the line for 2 s */
#define CONTROL_FRAME_EVENT_CLOCK_ON       5u
#define CONTROL_FRAME_EVENT_CLOCK_OFF      6u
#define CONTROL_FRAME_EVENT_USB_HOST_LOST  7u  /**< seen by the NEXT host, in its log */
/* 16 and up: the WiFi mode (wifi_task.h). Text is key=value, space separated. */
#define CONTROL_FRAME_EVENT_WIFI_CONNECTING    16u /**< station begins; text ssid= */
#define CONTROL_FRAME_EVENT_WIFI_UP            17u /**< got an IP; text ip= rssi= */
#define CONTROL_FRAME_EVENT_WIFI_DOWN          18u /**< network lost or turned off; text = state */
#define CONTROL_FRAME_EVENT_TCP_LISTENING      19u /**< server socket open; text port= */
#define CONTROL_FRAME_EVENT_TCP_PEER_CONNECTED 20u /**< peer on; text peer=ip:port or host= port= */
#define CONTROL_FRAME_EVENT_TCP_PEER_LOST      21u /**< peer gone; line frames go to USB again */
#define CONTROL_FRAME_EVENT_TCP_CONNECTING     22u /**< client mode dials; text host= port= */
#define CONTROL_FRAME_EVENT_TCP_CONNECT_FAILED 23u /**< the dial failed; retried after 5 s */
/** @} */

/** @name NAK reasons
 *  @{ */
#define CONTROL_FRAME_NAK_UNKNOWN_TYPE 1u
#define CONTROL_FRAME_NAK_BAD_LENGTH   2u
#define CONTROL_FRAME_NAK_REFUSED      3u
/** @} */

/** @brief A parsed control frame. payload points into the caller's buffer. */
typedef struct
{
    uint8_t type;               /**< CONTROL_FRAME_* message type */
    uint8_t id;                 /**< request id, 0 = unsolicited */
    uint8_t flags;              /**< CONTROL_FRAME_FLAG_* and the chunk counter */
    const uint8_t *payload;     /**< first payload byte, valid while the frame buffer is */
    int payload_len;            /**< 0..CONTROL_FRAME_MAX_PAYLOAD */
} control_frame;

/**
 * @brief Whether a frame is a control frame, and if so, its fields.
 *
 * Both locks must hold: the address byte is CONTROL_FRAME_ADDRESS and the CRC
 * over the whole frame leaves CONTROL_FRAME_RESIDUE. The type is NOT checked
 * here; an unknown type is still a control frame and the caller answers it
 * with a NAK, so a newer host never has its message clocked onto the line.
 *
 * @param[in]  frame  Un-stuffed frame bytes, check included.
 * @param[in]  len    Number of bytes in frame.
 * @param[out] out    Receives the fields when the frame is a control frame;
 *                    untouched otherwise. Must not be NULL.
 * @retval true   A control frame; out is filled in.
 * @retval false  Not a control frame: forward it to the line as data.
 */
bool control_frame_parse(const uint8_t *frame, int len, control_frame *out);

/**
 * @brief Whether a frame carries a good HDLC FCS (residue HDLC_CRC_GOOD).
 *
 * A frame for which this is true can never be a control frame. Provided so
 * callers can state the data case explicitly; control_frame_parse() alone
 * is sufficient because the two residues are exclusive.
 *
 * @param[in] frame  Un-stuffed frame bytes, FCS included.
 * @param[in] len    Number of bytes in frame.
 * @return true when len >= 2 and the FCS is good.
 */
bool control_frame_is_data(const uint8_t *frame, int len);

/**
 * @brief Build a control frame: header, payload, un-inverted CRC.
 * @param[in]  type         CONTROL_FRAME_* message type.
 * @param[in]  id           Request id to echo, or 0.
 * @param[in]  flags        CONTROL_FRAME_FLAG_* plus chunk counter.
 * @param[in]  payload      Payload bytes, may be NULL when payload_len is 0.
 * @param[in]  payload_len  0..CONTROL_FRAME_MAX_PAYLOAD.
 * @param[out] out          Receives the frame; room for
 *                          payload_len + CONTROL_FRAME_OVERHEAD bytes.
 * @return Frame length, or -1 when payload_len is out of range.
 */
int control_frame_build(uint8_t type, uint8_t id, uint8_t flags, const uint8_t *payload,
                        int payload_len, uint8_t *out);

#endif /* CONTROL_FRAME_H */
