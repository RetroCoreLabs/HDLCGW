/**************************************************************************
** HDLC - bit-level HDLC/SDLC framing                                    **
**                                                                       **
** Bit stuffing on transmit; bit un-stuffing, flag and abort detection   **
** on receive; CRC-16/IBM-SDLC frame check. Nothing here depends on      **
** Arduino or FreeRTOS, so it can be unit tested on a PC.                **
**                                                                       **
** Wire convention (same as the SPI link and as Mattis Lind's            **
** SDLCBridge): bits go out LSB first. A byte handed to the encoder is   **
** walked from bit 0 to bit 7; the output byte is built by shifting      **
** RIGHT and setting bit 7, so the first bit put on the wire ends up in  **
** bit 0 of the output byte. The SPI peripheral must run LSBFIRST.       **
**                                                                       **
** Logic taken from SDLCBridge.ino (MattisLind/alfaskop_emu,             **
** Utils/SDLCBridge), proven against a real IBM SDLC terminal (Informer  **
** 213) and an Intel 8274 board. Two details from that work are kept on  **
** purpose:                                                              **
**  - a stuffed zero IS inserted when five ones precede the closing      **
**    flag, although the flag itself starts with a zero. Without it the  **
**    terminal rejected every frame whose last FCS byte ended in 11111.  **
**  - after a flag the receive bit counter is cleared, so byte alignment **
**    on the wire does not matter. Frames may start on any bit of an SPI **
**    byte.                                                              **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#ifndef HDLC_H
#define HDLC_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @name CRC-16/IBM-SDLC (also called CRC-16/X-25)
 * Poly 0x1021 reflected (0x8408), init 0xFFFF, bits fed LSB first, result
 * inverted before sending. Running the CRC over a good frame INCLUDING its
 * two FCS bytes leaves HDLC_CRC_GOOD in the register.
 * @{
 */
#define HDLC_CRC_INIT 0xFFFFu   /**< starting value of the CRC register */
#define HDLC_CRC_GOOD 0xF0B8u   /**< residue after a good frame plus FCS */
/** @} */

#define HDLC_FLAG  0x7Eu    /**< 01111110, frame delimiter */
#define HDLC_ABORT 0x7Fu    /**< 01111111, seven ones */
#define HDLC_IDLE  0xFFu    /**< marking line, nothing sent */

/**
 * @brief One CRC step over one byte.
 * @param[in] crc   Current CRC register.
 * @param[in] data  Next frame byte.
 * @return Updated CRC register.
 */
uint16_t hdlc_crc_byte(uint16_t crc, uint8_t data);

/* ------------------------------------------------------------ encoder */

/**
 * @brief Turns frame bytes into the bit-stuffed byte stream for the line.
 *
 * Use: hdlc_encoder_open() once, hdlc_encoder_write() for every byte of
 * address, control, data and FCS, then hdlc_encoder_close(). Every call
 * fills the caller's output buffer and returns how many bytes it produced:
 * 0, 1 or 2, never more, because one input byte adds at most 8 data bits
 * plus 2 stuffed zeros. The caller pushes those bytes to the transmit ring
 * buffer. Not thread safe; one encoder per sending task.
 */
typedef struct
{
    uint8_t shifter;    /**< output byte being built, filled from bit 7 down */
    int bit_count;      /**< bits in shifter, 0..7 */
    int one_count;      /**< ones in a row sent so far, 0..5 */
    uint16_t crc;       /**< running CRC over the bytes written */
} hdlc_encoder;

/**
 * @brief Return an encoder to the idle state. Pending bits are discarded.
 * @param[out] enc  Encoder; must not be NULL.
 */
void hdlc_encoder_reset(hdlc_encoder *enc);

/**
 * @brief Write the opening flag and start a frame.
 *
 * The flag is always byte aligned because close/abort reset the bit
 * counter. Also restarts the running CRC.
 *
 * @param[in,out] enc  Encoder; must not be NULL.
 * @param[out]    out  Receives the line bytes; room for 2.
 * @return Number of bytes written to out (always 1).
 */
int hdlc_encoder_open(hdlc_encoder *enc, uint8_t *out);

/**
 * @brief Write one frame byte, bit stuffed. Updates the running CRC.
 * @param[in,out] enc   Encoder; must not be NULL.
 * @param[in]     data  Frame byte (address, control, data or FCS).
 * @param[out]    out   Receives the line bytes; room for 2.
 * @return Number of bytes written to out, 0..2.
 */
int hdlc_encoder_write(hdlc_encoder *enc, uint8_t data, uint8_t *out);

/**
 * @brief Write the two FCS bytes for everything written since open.
 *
 * The FCS is the inverted CRC, low byte first. Use only when the frame does
 * not already carry an FCS.
 *
 * @param[in,out] enc  Encoder; must not be NULL.
 * @param[out]    out  Receives the line bytes; room for 4.
 * @return Number of bytes written to out, 2..4.
 */
int hdlc_encoder_write_fcs(hdlc_encoder *enc, uint8_t *out);

/**
 * @brief Write the closing flag and end the frame.
 *
 * A stuffed zero is inserted first if five ones are pending. Residual bits
 * are followed by the flag bits and then padded with idle ones to a whole
 * byte. Leaves the encoder reset, so the next open is byte aligned.
 *
 * @param[in,out] enc  Encoder; must not be NULL.
 * @param[out]    out  Receives the line bytes; room for 2.
 * @return Number of bytes written to out, 1..2.
 */
int hdlc_encoder_close(hdlc_encoder *enc, uint8_t *out);

/**
 * @brief Write an abort sequence (seven ones) instead of a closing flag.
 * @param[in,out] enc  Encoder; must not be NULL.
 * @param[out]    out  Receives the line bytes; room for 2.
 * @return Number of bytes written to out, 1..2.
 */
int hdlc_encoder_abort(hdlc_encoder *enc, uint8_t *out);

/* ------------------------------------------------------------ decoder */

/**
 * @brief Where hdlc_decoder_feed() delivers frames.
 *
 * The callbacks run on the caller's task, inside hdlc_decoder_feed().
 * ctx is passed back unchanged.
 */
typedef struct
{
    /** Handed to every callback unchanged. */
    void *ctx;
    /** One un-stuffed frame byte, in order. The FCS bytes come last. */
    void (*on_byte)(void *ctx, uint8_t data);
    /** Closing flag after at least one byte. crc_good is true when the CRC
     *  over all delivered bytes (FCS included) left HDLC_CRC_GOOD. */
    void (*on_end)(void *ctx, bool crc_good);
    /** Seven or more ones inside a frame; the frame is dropped. */
    void (*on_abort)(void *ctx);
    /** The bytes delivered since the last flag were fewer than
     *  HDLC_MIN_FRAME_BYTES and are not a frame: forget them, count nothing.
     *  Without this the sink kept a padding byte and the next frame arrived
     *  with it stuck on the front (bench 2026-11-08, 91 of 155 frames). */
    void (*on_discard)(void *ctx);
} hdlc_frame_sink;

/**
 * @brief Pulls frames out of the raw byte stream read from the line.
 *
 * Not thread safe; one decoder per receiving task.
 */
typedef struct
{
    bool active;        /**< opening flag seen, collecting bits */
    bool have_data;     /**< at least one byte delivered for this frame */
    int byte_count;     /**< bytes delivered for this frame */
    uint8_t shifter;    /**< input byte being built, filled from bit 7 down */
    int bit_count;      /**< bits in shifter, 0..7 */
    int one_count;      /**< ones in a row seen so far, capped at 8 */
    uint16_t crc;       /**< running CRC over the bytes delivered */
    uint32_t short_frames;  /**< flag-to-flag runs of 1..3 bytes, ignored (see feed) */
} hdlc_decoder;

/** Shortest run between two flags that counts as a frame: address, control
 *  and FCS = 32 bits (ISO 13239 4.3.3, "invalid frame" rule; SDLC says the
 *  same). Anything shorter is ignored, not counted as a bad frame. */
#define HDLC_MIN_FRAME_BYTES 4

/**
 * @brief Return a decoder to the idle, waiting-for-flag state.
 * @param[out] dec  Decoder; must not be NULL.
 */
void hdlc_decoder_reset(hdlc_decoder *dec);

/**
 * @brief Feed one byte as read from the SPI (LSB first on the wire).
 *
 * Idle bytes (0xFF) outside a frame return at once. Inside a frame every
 * bit is examined; callbacks fire from here.
 *
 * Runs of fewer than HDLC_MIN_FRAME_BYTES between two flags are dropped in
 * silence and counted in short_frames. They happen whenever one frame
 * follows another directly: the transmitter pads the byte after the closing
 * flag with 1..7 idle ones, and those plus the next opening flag make 8 or
 * more bits, which this decoder would otherwise deliver as a one-byte frame
 * with a bad FCS. Seen on the bench 2026-11-08: 1339 such phantoms in 5521
 * real frames, every real frame good.
 *
 * @param[in,out] dec   Decoder; must not be NULL.
 * @param[in]     data  Byte from the line.
 * @param[in]     sink  Callbacks for frame bytes and frame ends; must not be
 *                      NULL and all four function pointers must be set.
 */
void hdlc_decoder_feed(hdlc_decoder *dec, uint8_t data, const hdlc_frame_sink *sink);

/* ------------------------------------------------------ control field */

/** @brief The three HDLC frame classes, told apart by the low control bits. */
typedef enum
{
    HDLC_FRAME_UNKNOWN,     /**< could not classify (never happens for 8-bit control) */
    HDLC_FRAME_I,           /**< information: bit 0 = 0, carries N(S) and N(R) */
    HDLC_FRAME_S,           /**< supervisory: bits 1..0 = 01, carries N(R) */
    HDLC_FRAME_U            /**< unnumbered: bits 1..0 = 11, M1 and M2 name the type */
} hdlc_frame_type;

/**
 * @brief A decoded 8-bit (modulo 8) control field, as the DECODE, F and
 *        INSPECT console commands print it. Same layout and names as the
 *        RC422Dev console so both gateways read alike.
 */
typedef struct
{
    hdlc_frame_type type;       /**< I, S or U */
    int ns;                     /**< N(S), I frames only, bits 3..1 */
    int nr;                     /**< N(R), I and S frames, bits 7..5 */
    int m1;                     /**< U frames: bits 3..2 */
    int m2;                     /**< U frames: bits 7..5 */
    bool poll_final;            /**< bit 4, P/F */
    const char *name;           /**< short type name: RR, SABM, UA, ... "" for I */
    const char *description;    /**< one-line meaning of name */
} hdlc_control_field;

/**
 * @brief Decode a control byte (modulo 8 numbering, ISO 13239 5.4).
 *
 * U-frame types are found by M1/M2 in a table shared with RC422Dev; the
 * first match wins where a command and a response share the bits (UI, XID,
 * TEST, DM/SARM). Unknown M1/M2 pairs decode as "UNKNOWN".
 *
 * @param[in] control  The control byte, normally the second byte of a frame.
 * @return The decoded fields; name and description point at static strings.
 */
hdlc_control_field hdlc_decode_control(uint8_t control);

/**
 * @brief One-letter class name.
 * @param[in] type  Frame class.
 * @return "I", "S", "U" or "?".
 */
const char *hdlc_frame_type_name(hdlc_frame_type type);

/**
 * @brief Long class name.
 * @param[in] type  Frame class.
 * @return "Information Frame", "Supervisory Frame", "Unnumbered Frame" or
 *         "Unknown Frame".
 */
const char *hdlc_frame_type_description(hdlc_frame_type type);

#endif /* HDLC_H */
