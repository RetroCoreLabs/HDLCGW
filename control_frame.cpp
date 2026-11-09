/**************************************************************************
** CONTROL_FRAME - messages between the host PC and this firmware        **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#include "control_frame.h"

#include "hdlc.h"

/**
 * @brief CRC-16/X.25 register after running over every byte of a buffer.
 * @param[in] frame  Bytes.
 * @param[in] len    Number of bytes.
 * @return The register, not inverted.
 */
static uint16_t control_frame_crc(const uint8_t *frame, int len)
{
    uint16_t crc = HDLC_CRC_INIT;

    for (int i = 0; i < len; i++)
    {
        crc = hdlc_crc_byte(crc, frame[i]);
    }
    return crc;
}

bool control_frame_parse(const uint8_t *frame, int len, control_frame *out)
{
    if (len < CONTROL_FRAME_OVERHEAD || frame[0] != CONTROL_FRAME_ADDRESS)
    {
        return false;
    }
    if (control_frame_crc(frame, len) != CONTROL_FRAME_RESIDUE)
    {
        return false;
    }

    out->type = frame[1];
    out->id = frame[2];
    out->flags = frame[3];
    out->payload = &frame[CONTROL_FRAME_HEADER];
    out->payload_len = len - CONTROL_FRAME_OVERHEAD;
    return true;
}

bool control_frame_is_data(const uint8_t *frame, int len)
{
    if (len < 2)
    {
        return false;
    }
    return control_frame_crc(frame, len) == HDLC_CRC_GOOD;
}

int control_frame_build(uint8_t type, uint8_t id, uint8_t flags, const uint8_t *payload,
                        int payload_len, uint8_t *out)
{
    int n = 0;
    uint16_t crc;

    if (payload_len < 0 || payload_len > CONTROL_FRAME_MAX_PAYLOAD)
    {
        return -1;
    }

    out[n++] = CONTROL_FRAME_ADDRESS;
    out[n++] = type;
    out[n++] = id;
    out[n++] = flags;
    for (int i = 0; i < payload_len; i++)
    {
        out[n++] = payload[i];
    }
    /* Stored WITHOUT inversion on purpose: that is the second lock, see the
     * header. The receiver's register then ends at CONTROL_FRAME_RESIDUE. */
    crc = control_frame_crc(out, n);
    out[n++] = (uint8_t)(crc & 0xFFu);
    out[n++] = (uint8_t)(crc >> 8u);
    return n;
}
