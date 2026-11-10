/**************************************************************************
** USB_RX - bytes from the host, read straight from TinyUSB              **
**                                                                       **
** See usb_rx.h for why. Sources the code rests on, all read 2026-11-09: **
**  - core 3.3.12 USBCDC.cpp: `USBCDC *devices[CFG_TUD_CDC]` is a plain   **
**    global; begin() sets devices[itf] = this, and every TinyUSB CDC    **
**    callback (rx, tx, line state, line coding) does nothing while the  **
**    entry is NULL. _onLineState and _onLineCoding are public and skip  **
**    unchanged values themselves.                                       **
**  - TinyUSB 0.21.0 tusb.c: tu_edpt_stream_read() reads the FIFO and    **
**    re-arms the OUT endpoint only if a whole packet fits.              **
**  - boards.txt: lolin_s2_mini.upload.use_1200bps_touch=true, so the    **
**    upload resets the board by setting 1200 baud, a lasting value that **
**    polling cannot miss.                                               **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#include "usb_rx.h"

#include <Arduino.h>
#include <USBCDC.h>
#include <esp32-hal-tinyusb.h>

#if USB_RX_DIRECT

/* The core's table of CDC objects (USBCDC.cpp, core 3.3.12). */
extern USBCDC *devices[];

/** Last line coding handed to the core, to call it only on a change. */
static cdc_line_coding_t g_last_coding;
/** Last line state handed to the core (bit 0 DTR, bit 1 RTS). */
static uint8_t g_last_state = 0xFFu;

void usb_rx_begin(void)
{
    /* From here on TinyUSB keeps what the host sends until usb_rx_read()
     * takes it. The core's write path does not use this table and keeps
     * working. */
    devices[0] = NULL;
    memset(&g_last_coding, 0, sizeof(g_last_coding));
}

void usb_rx_poll(void)
{
    cdc_line_coding_t coding;
    uint8_t state = tud_cdc_n_get_line_state(0);

    /* Line coding first: the upload's 1200 baud touch restarts the chip
     * into the flasher inside _onLineCoding. */
    tud_cdc_n_get_line_coding(0, &coding);
    if (memcmp(&coding, &g_last_coding, sizeof(coding)) != 0)
    {
        g_last_coding = coding;
        Serial._onLineCoding(coding.bit_rate, coding.stop_bits, coding.parity,
                             coding.data_bits);
    }
    if (state != g_last_state)
    {
        g_last_state = state;
        Serial._onLineState((state & 0x01u) != 0, (state & 0x02u) != 0);
    }
}

/* Not HOT_PATH_CODE: it runs once per block, and tud_cdc_n_read(), where
 * the bytes are copied, sits in the core's library in flash anyway; in IRAM
 * it overflowed dram0_0_seg by 16 bytes (2026-11-09). */
int usb_rx_read(uint8_t *buf, int max)
{
    int got = 0;

    /* Each tud_cdc_n_read() frees FIFO room and lets TinyUSB arm the next
     * packet, so keep reading while packets keep coming. */
    while (got < max)
    {
        uint32_t n = tud_cdc_n_read(0, buf + got, (uint32_t)(max - got));
        if (n == 0)
        {
            break;
        }
        got += (int)n;
    }
    return got;
}

#else /* USB_RX_DIRECT */

void usb_rx_begin(void)
{
}

void usb_rx_poll(void)
{
}

int usb_rx_read(uint8_t *buf, int max)
{
    return (int)Serial.read(buf, (size_t)max);
}

#endif /* USB_RX_DIRECT */
