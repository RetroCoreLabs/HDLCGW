/**************************************************************************
** HDLC - bit-level HDLC/SDLC framing                                    **
**                                                                       **
** See hdlc.h for the wire convention and where the logic comes from.    **
** Function by function this mirrors SDLCBridge.ino: shiftInOne /        **
** shiftInZero, processHDLCforSending, endHDLCProcessing, sendAbort,     **
** processRxOneHDLCBit and processRxZeroHDLCBit.                         **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#include "hdlc.h"
#include "hot_path.h"

/* Reflected CRC-16 table for poly 0x1021 (0x8408 reflected). Same table as
 * RFC 1662 Appendix C; the bit-serial loop in SDLCBridge.ino
 * (calculateCrcChar) computes exactly the same function. */
// clang-format off
/* Read once per byte in both directions: internal RAM, not flash. */
static const HOT_PATH_DATA uint16_t CRC_TABLE[256] =
{
    0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
    0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
    0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
    0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
    0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
    0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
    0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
    0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
    0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
    0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
    0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
    0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
    0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
    0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
    0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
    0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
    0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
    0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
    0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
    0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
    0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
    0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
    0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
    0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
    0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
    0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
    0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
    0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
    0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
    0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
    0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
    0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
};
// clang-format on

uint16_t HOT_PATH_CODE hdlc_crc_byte(uint16_t crc, uint8_t data)
{
    return (uint16_t)((crc >> 8u) ^ CRC_TABLE[(crc ^ data) & 0xFFu]);
}

/* ------------------------------------------------------------ encoder */

void hdlc_encoder_reset(hdlc_encoder *enc)
{
    enc->shifter = 0;
    enc->bit_count = 0;
    enc->one_count = 0;
    enc->crc = HDLC_CRC_INIT;
}

/**
 * @brief Shift a one bit into the output byte.
 * @param[in,out] enc  Encoder.
 * @param[out]    out  Line bytes so far.
 * @param[in]     n    Bytes already in out.
 * @return New count of bytes in out.
 */
static HOT_PATH_CODE int shift_one(hdlc_encoder *enc, uint8_t *out, int n)
{
    enc->shifter >>= 1u;
    enc->shifter |= 0x80u;
    enc->bit_count++;
    enc->one_count++;
    if (enc->bit_count == 8)
    {
        out[n++] = enc->shifter;
        enc->bit_count = 0;
    }
    return n;
}

/**
 * @brief Shift a zero bit into the output byte; resets the run of ones.
 * @param[in,out] enc  Encoder.
 * @param[out]    out  Line bytes so far.
 * @param[in]     n    Bytes already in out.
 * @return New count of bytes in out.
 */
static HOT_PATH_CODE int shift_zero(hdlc_encoder *enc, uint8_t *out, int n)
{
    enc->shifter >>= 1u;
    enc->bit_count++;
    enc->one_count = 0;
    if (enc->bit_count == 8)
    {
        out[n++] = enc->shifter;
        enc->bit_count = 0;
    }
    return n;
}

/**
 * @brief Shift a data bit, stuffing a zero first after five ones in a row.
 * @param[in,out] enc  Encoder.
 * @param[in]     bit  0 or 1.
 * @param[out]    out  Line bytes so far.
 * @param[in]     n    Bytes already in out.
 * @return New count of bytes in out.
 */
static HOT_PATH_CODE int shift_data_bit(hdlc_encoder *enc, uint8_t bit, uint8_t *out, int n)
{
    if (enc->one_count == 5)
    {
        n = shift_zero(enc, out, n);
    }
    return (bit != 0) ? shift_one(enc, out, n) : shift_zero(enc, out, n);
}

int hdlc_encoder_open(hdlc_encoder *enc, uint8_t *out)
{
    /* close/abort always leave bit_count at 0, so the flag is byte aligned
     * and goes out as-is. It is not subject to stuffing. */
    hdlc_encoder_reset(enc);
    out[0] = HDLC_FLAG;
    return 1;
}

int HOT_PATH_CODE hdlc_encoder_write(hdlc_encoder *enc, uint8_t data, uint8_t *out)
{
    int n = 0;

    enc->crc = hdlc_crc_byte(enc->crc, data);
    for (int i = 0; i < 8; i++)
    {
        n = shift_data_bit(enc, (uint8_t)((data >> i) & 1u), out, n);
    }
    return n;
}

int HOT_PATH_CODE hdlc_encoder_write_fcs(hdlc_encoder *enc, uint8_t *out)
{
    /* The FCS is the inverted CRC, sent low byte first. Writing it through
     * hdlc_encoder_write() also feeds it into crc, which is harmless: close
     * resets everything. */
    uint16_t fcs = (uint16_t)~enc->crc;
    int n = hdlc_encoder_write(enc, (uint8_t)(fcs & 0xFFu), out);
    n += hdlc_encoder_write(enc, (uint8_t)(fcs >> 8u), out + n);
    return n;
}

/**
 * @brief End the frame with a flag or an abort. Shared by close and abort.
 *
 * Follows endHDLCProcessing()/sendAbort() in SDLCBridge exactly.
 *
 * @param[in,out] enc         Encoder; reset on return.
 * @param[in]     terminator  HDLC_FLAG or HDLC_ABORT, walked LSB first.
 * @param[out]    out         Receives 1..2 line bytes.
 * @return Number of bytes written to out.
 */
static int terminate(hdlc_encoder *enc, uint8_t terminator, uint8_t *out)
{
    int n = 0;

    /* Five ones pending: stuff a zero even though the terminator starts
     * with a zero. See hdlc.h - a real terminal needs this. */
    if (enc->one_count == 5)
    {
        n = shift_zero(enc, out, n);
    }

    if (enc->bit_count == 0)
    {
        /* Nothing residual: the terminator goes out byte aligned. */
        out[n++] = terminator;
    }
    else
    {
        /* Shift the terminator bits in after the residual bits, unstuffed
         * (a flag must keep its six ones), then fill the rest with idle
         * ones. */
        for (int i = 0; i < 8; i++)
        {
            if (((terminator >> i) & 1u) != 0)
            {
                n = shift_one(enc, out, n);
            }
            else
            {
                n = shift_zero(enc, out, n);
            }
        }
        if (enc->bit_count != 0)
        {
            while (enc->bit_count < 8)
            {
                enc->shifter >>= 1u;
                enc->shifter |= 0x80u;
                enc->bit_count++;
            }
            out[n++] = enc->shifter;
        }
    }

    hdlc_encoder_reset(enc);
    return n;
}

int hdlc_encoder_close(hdlc_encoder *enc, uint8_t *out)
{
    return terminate(enc, HDLC_FLAG, out);
}

int hdlc_encoder_abort(hdlc_encoder *enc, uint8_t *out)
{
    return terminate(enc, HDLC_ABORT, out);
}

/* ------------------------------------------------------------ decoder */

void hdlc_decoder_reset(hdlc_decoder *dec)
{
    dec->active = false;
    dec->have_data = false;
    dec->byte_count = 0;
    dec->shifter = 0;
    dec->bit_count = 0;
    dec->one_count = 0;
    dec->crc = HDLC_CRC_INIT;
    dec->short_frames = 0;
}

/**
 * @brief A complete byte has been assembled inside a frame: CRC and deliver.
 * @param[in,out] dec   Decoder.
 * @param[in]     sink  Callbacks.
 */
static HOT_PATH_CODE void deliver_byte(hdlc_decoder *dec, const hdlc_frame_sink *sink)
{
    dec->bit_count = 0;
    dec->crc = hdlc_crc_byte(dec->crc, dec->shifter);
    dec->have_data = true;
    dec->byte_count++;
    sink->on_byte(sink->ctx, dec->shifter);
}

/**
 * @brief Handle a one bit. Mirrors processRxOneHDLCBit() in SDLCBridge.
 * @param[in,out] dec   Decoder.
 * @param[in]     sink  Callbacks.
 */
static HOT_PATH_CODE void bit_one(hdlc_decoder *dec, const hdlc_frame_sink *sink)
{
    if (dec->active)
    {
        if (dec->one_count == 6)
        {
            /* Seventh one in a row: abort. Normal when the line goes to
             * marking right after a closing flag, an error if we had a
             * frame's worth of data. Fewer bytes than that is the padding
             * case described in hdlc.h, ignored like a short frame. */
            dec->active = false;
            if (dec->byte_count >= HDLC_MIN_FRAME_BYTES)
            {
                sink->on_abort(sink->ctx);
            }
            else if (dec->have_data)
            {
                dec->short_frames++;
                sink->on_discard(sink->ctx);
            }
            dec->have_data = false;
            dec->byte_count = 0;
            dec->bit_count = 0;
            dec->crc = HDLC_CRC_INIT;
        }
        else
        {
            dec->shifter >>= 1u;
            dec->shifter |= 0x80u;
            dec->bit_count++;
            if (dec->bit_count == 8)
            {
                deliver_byte(dec, sink);
            }
        }
    }

    if (dec->one_count < 8)
    {
        dec->one_count++;
    }
}

/**
 * @brief Handle a zero bit. Mirrors processRxZeroHDLCBit() in SDLCBridge.
 * @param[in,out] dec   Decoder.
 * @param[in]     sink  Callbacks.
 */
static HOT_PATH_CODE void bit_zero(hdlc_decoder *dec, const hdlc_frame_sink *sink)
{
    if (dec->active)
    {
        if (dec->one_count == 6)
        {
            /* Flag: end of this frame. Stay active - the same flag may open
             * the next one. Re-align the bit counter on the flag. A run
             * shorter than a minimum frame is invalid per ISO 13239 and is
             * ignored (see hdlc.h), not reported as bad. */
            if (dec->byte_count >= HDLC_MIN_FRAME_BYTES)
            {
                sink->on_end(sink->ctx, dec->crc == HDLC_CRC_GOOD);
            }
            else if (dec->have_data)
            {
                dec->short_frames++;
                sink->on_discard(sink->ctx);
            }
            dec->have_data = false;
            dec->byte_count = 0;
            dec->crc = HDLC_CRC_INIT;
            dec->bit_count = 0;
        }
        else if (dec->one_count != 5)
        {
            /* Ordinary zero. (one_count == 5 is a stuffed zero: dropped.) */
            dec->shifter >>= 1u;
            dec->bit_count++;
            if (dec->bit_count == 8)
            {
                deliver_byte(dec, sink);
            }
        }
    }
    else if (dec->one_count == 6)
    {
        /* Opening flag. */
        dec->active = true;
        dec->have_data = false;
        dec->byte_count = 0;
        dec->bit_count = 0;
        dec->crc = HDLC_CRC_INIT;
    }

    dec->one_count = 0;
}

void HOT_PATH_CODE hdlc_decoder_feed(hdlc_decoder *dec, uint8_t data, const hdlc_frame_sink *sink)
{
    /* Idle line while waiting for a flag: all ones, nothing can start here.
     * one_count is left alone on purpose: the next zero cannot have exactly
     * six ones before it in any case. */
    if (data == HDLC_IDLE && !dec->active)
    {
        return;
    }

    for (int i = 0; i < 8; i++)
    {
        if (((data >> i) & 1u) != 0)
        {
            bit_one(dec, sink);
        }
        else
        {
            bit_zero(dec, sink);
        }
    }
}

/* ------------------------------------------------------ control field */

/** @brief One U-frame type: the M1/M2 bits and its names. */
typedef struct
{
    int m1;
    int m2;
    const char *name;
    const char *description;
} u_frame_entry;

/* Same table as RC422Dev (hdlc.h there, hdlc_u_frame_mappings). The first
 * match wins, so a command/response pair with the same bits decodes as the
 * command. Numbers from ISO 13239 table 5 / Wikipedia "HDLC", modulo 8. */
// clang-format off
static const u_frame_entry U_FRAMES[] =
{
    { 0, 4, "SNRM",  "Set Normal Response Mode" },
    { 3, 0, "SARM",  "Set Asynchronous Response Mode" },
    { 3, 1, "SABM",  "Set Asynchronous Balanced Mode" },
    { 0, 2, "DISC",  "Disconnect" },
    { 3, 6, "SNRME", "Set Normal Response Mode Extended" },
    { 3, 2, "SARME", "Set Asynchronous Response Mode Extended" },
    { 3, 3, "SABME", "Set Asynchronous Balanced Mode Extended" },
    { 1, 0, "SIM",   "Set Initialization Mode" },
    { 0, 1, "UP",    "Unnumbered Poll" },
    { 0, 0, "UI",    "Unnumbered Information" },
    { 3, 5, "XID",   "Exchange Identification" },
    { 3, 4, "RSET",  "Reset (combined station only)" },
    { 0, 7, "TEST",  "Test Command" },
    { 0, 6, "SM",    "Set Mode" },
    { 3, 7, "UIH",   "Unnumbered Information with Header check" },
    { 0, 3, "UA",    "Unnumbered Acknowledge" },
    { 1, 4, "FRMR",  "Frame Reject" },
    { 3, 0, "DM",    "Disconnected Mode" },
    { 0, 2, "RD",    "Request Disconnect" },
    { 1, 0, "RIM",   "Request Initialization Mode" },
};
// clang-format on

#define NUM_U_FRAMES ((int)(sizeof(U_FRAMES) / sizeof(U_FRAMES[0])))

static const char *const S_NAMES[4] = { "RR", "RNR", "REJ", "SREJ" };
static const char *const S_DESCRIPTIONS[4] =
{
    "Receive Ready", "Receive Not Ready", "Reject", "Selective Reject"
};

hdlc_control_field hdlc_decode_control(uint8_t control)
{
    hdlc_control_field f;

    f.type = HDLC_FRAME_UNKNOWN;
    f.ns = 0;
    f.nr = 0;
    f.m1 = 0;
    f.m2 = 0;
    f.poll_final = (control & 0x10u) != 0;
    f.name = "UNK";
    f.description = "Unknown frame";

    if ((control & 0x01u) == 0)
    {
        /* I frame: 0 N(S)x3 P/F N(R)x3 */
        f.type = HDLC_FRAME_I;
        f.ns = (int)((control >> 1u) & 0x07u);
        f.nr = (int)((control >> 5u) & 0x07u);
        f.name = "";
        f.description = "";
    }
    else if ((control & 0x03u) == 0x01u)
    {
        /* S frame: 1 0 SS P/F N(R)x3. RC422Dev has a bug here: its switch
         * uses 0x10 and 0x11 for REJ and SREJ, which are 16 and 17, so those
         * two never match. The values are 2 and 3. */
        int s = (int)((control >> 2u) & 0x03u);
        f.type = HDLC_FRAME_S;
        f.nr = (int)((control >> 5u) & 0x07u);
        f.name = S_NAMES[s];
        f.description = S_DESCRIPTIONS[s];
    }
    else
    {
        /* U frame: 1 1 M1x2 P/F M2x3 */
        f.type = HDLC_FRAME_U;
        f.m1 = (int)((control >> 2u) & 0x03u);
        f.m2 = (int)((control >> 5u) & 0x07u);
        f.name = "UNKNOWN";
        f.description = "Unknown frame type";
        for (int i = 0; i < NUM_U_FRAMES; i++)
        {
            if (U_FRAMES[i].m1 == f.m1 && U_FRAMES[i].m2 == f.m2)
            {
                f.name = U_FRAMES[i].name;
                f.description = U_FRAMES[i].description;
                break;
            }
        }
    }
    return f;
}

const char *hdlc_frame_type_name(hdlc_frame_type type)
{
    switch (type)
    {
    case HDLC_FRAME_I:
        return "I";
    case HDLC_FRAME_S:
        return "S";
    case HDLC_FRAME_U:
        return "U";
    case HDLC_FRAME_UNKNOWN:
        /* fall through */
    default:
        return "?";
    }
}

const char *hdlc_frame_type_description(hdlc_frame_type type)
{
    switch (type)
    {
    case HDLC_FRAME_I:
        return "Information Frame";
    case HDLC_FRAME_S:
        return "Supervisory Frame";
    case HDLC_FRAME_U:
        return "Unnumbered Frame";
    case HDLC_FRAME_UNKNOWN:
        /* fall through */
    default:
        return "Unknown Frame";
    }
}
