/**************************************************************************
** BYTE_FRAME - byte-stuffed frames between the host PC and this device  **
**                                                                       **
** RFC 1662 "PPP in HDLC-like Framing" byte stuffing over the USB serial **
** port, the same as RC422Dev (the Pico version of this gateway) so the  **
** same host software works with both.                                   **
**                                                                       **
**   0x7E            frame delimiter (also sent before every frame)      **
**   0x7D 0x5E       a 0x7E inside the frame                             **
**   0x7D 0x5D       a 0x7D inside the frame                             **
**   0x7D (x ^ 0x20) any byte below 0x20 is sent escaped too             **
**                                                                       **
** Frames carry address, control, data and the two FCS bytes exactly as  **
** they go on the synchronous line; this device neither adds nor strips  **
** the FCS, so the host can verify end to end. Nothing here depends on   **
** Arduino.                                                              **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#ifndef BYTE_FRAME_H
#define BYTE_FRAME_H

#include <stdint.h>
#include <stdbool.h>

#define BYTE_FRAME_FLAG        0x7Eu    /**< frame delimiter */
#define BYTE_FRAME_ESCAPE      0x7Du    /**< next byte is XOR 0x20 */
#define BYTE_FRAME_ESCAPE_MASK 0x20u    /**< applied to an escaped byte */

/** Longest frame accepted from the host, in bytes after un-escaping. */
#define BYTE_FRAME_MAX 1024

/**
 * @brief Byte-stuff one frame for the host.
 * @param[in]  frame  Frame bytes, FCS included.
 * @param[in]  len    Number of bytes in frame.
 * @param[out] out    Receives flag, escaped bytes, flag. Must hold
 *                    2 + 2 * len bytes (worst case, every byte escaped).
 * @return Number of bytes written to out.
 */
int byte_frame_encode(const uint8_t *frame, int len, uint8_t *out);

/**
 * @brief Collects frames from the byte stream coming from the host.
 *
 * Not thread safe; one decoder per receiving task.
 */
typedef struct
{
    uint8_t buffer[BYTE_FRAME_MAX]; /**< the frame being collected / last delivered */
    int length;                     /**< bytes in buffer */
    bool active;                    /**< opening flag seen */
    bool escaped;                   /**< last byte was the escape byte */
    bool overflow;                  /**< frame too long, dropped at the closing flag */
    bool done;                      /**< a frame was delivered; next data byte starts anew */
    uint32_t overlong;              /**< frames dropped for being longer than BYTE_FRAME_MAX;
                                         counts from zero storage, NOT cleared by
                                         byte_frame_decoder_reset(), so a reconnect does
                                         not lose the count */
} byte_frame_decoder;

/**
 * @brief Return a decoder to the idle state.
 * @param[out] dec  Decoder; must not be NULL.
 */
void byte_frame_decoder_reset(byte_frame_decoder *dec);

/**
 * @brief Feed one byte from the host.
 *
 * A flag both ends a frame and opens the next. Empty frames (two flags in a
 * row) are ignored. A frame longer than BYTE_FRAME_MAX, or one that ends
 * right after an escape byte, is dropped; the too-long ones are counted in
 * dec->overlong.
 *
 * @param[in,out] dec   Decoder; must not be NULL.
 * @param[in]     data  Byte from the host.
 * @retval true   A complete frame is in dec->buffer, dec->length bytes. Read
 *                it before the next feed; the next data byte overwrites it.
 * @retval false  Nothing complete yet.
 */
bool byte_frame_decoder_feed(byte_frame_decoder *dec, uint8_t data);

/**
 * @brief Whether the decoder is between frames.
 * @param[in] dec  Decoder; must not be NULL.
 * @return true when no frame bytes have been collected since the last flag.
 */
bool byte_frame_decoder_is_idle(const byte_frame_decoder *dec);

#endif /* BYTE_FRAME_H */
