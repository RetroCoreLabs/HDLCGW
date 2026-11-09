/**************************************************************************
** BYTE_FRAME - byte-stuffed frames between the host PC and this device  **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#include "byte_frame.h"
#include "hot_path.h"

int HOT_PATH_CODE byte_frame_encode(const uint8_t *frame, int len, uint8_t *out)
{
    int n = 0;

    out[n++] = BYTE_FRAME_FLAG;
    for (int i = 0; i < len; i++)
    {
        uint8_t b = frame[i];
        if (b == BYTE_FRAME_FLAG || b == BYTE_FRAME_ESCAPE || b < 0x20u)
        {
            out[n++] = BYTE_FRAME_ESCAPE;
            out[n++] = (uint8_t)(b ^ BYTE_FRAME_ESCAPE_MASK);
        }
        else
        {
            out[n++] = b;
        }
    }
    out[n++] = BYTE_FRAME_FLAG;
    return n;
}

void byte_frame_decoder_reset(byte_frame_decoder *dec)
{
    dec->length = 0;
    dec->active = false;
    dec->escaped = false;
    dec->overflow = false;
    dec->done = false;
}

bool HOT_PATH_CODE byte_frame_decoder_feed(byte_frame_decoder *dec, uint8_t data)
{
    if (data == BYTE_FRAME_FLAG)
    {
        /* A flag both ends a frame and opens the next. Back-to-back flags
         * (length 0) are just idle and produce nothing. A frame that was too
         * long, or that ended right after an escape byte, is dropped. */
        bool ready = dec->active && !dec->done && !dec->escaped && !dec->overflow
                     && dec->length > 0;
        if (dec->overflow)
        {
            /* Shown by STAT (2026-11-09): until then a too-long frame from
             * the host or a TCP peer vanished without a trace. */
            dec->overlong = dec->overlong + 1u;
        }
        dec->active = true;
        dec->escaped = false;
        dec->overflow = false;
        if (ready)
        {
            /* Leave buffer/length for the caller. The next data byte starts
             * the new frame at 0. */
            dec->done = true;
        }
        else
        {
            dec->length = 0;
        }
        return ready;
    }

    if (!dec->active)
    {
        return false;   /* noise before the first flag */
    }

    if (dec->done)
    {
        dec->done = false;
        dec->length = 0;
    }

    if (data == BYTE_FRAME_ESCAPE)
    {
        dec->escaped = true;
        return false;
    }

    if (dec->escaped)
    {
        data ^= BYTE_FRAME_ESCAPE_MASK;
        dec->escaped = false;
    }

    if (dec->length < BYTE_FRAME_MAX)
    {
        dec->buffer[dec->length++] = data;
    }
    else
    {
        dec->overflow = true;
    }
    return false;
}

bool byte_frame_decoder_is_idle(const byte_frame_decoder *dec)
{
    return !dec->active || dec->done || dec->length == 0;
}
