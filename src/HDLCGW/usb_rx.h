/**************************************************************************
** USB_RX - bytes from the host, read straight from TinyUSB              **
**                                                                       **
** Why: the Arduino core's USBCDC class takes every USB packet out of    **
** TinyUSB the moment it arrives (tud_cdc_rx_cb -> USBCDC::_onRX, core   **
** 3.3.12, in the USB task at priority 24) and puts it in its own        **
** FreeRTOS queue one byte at a time, waiting at most 10 ticks per byte. **
** When the wait runs out it throws the rest of the packet away. The PC  **
** is never held back, because the bytes have already left TinyUSB.      **
** Bench, 2026-11-09: those lost bytes went onto the line inside a      **
** frame, and the Pico got that frame short by exactly the lost bytes,   **
** with a bad FCS (BUGS.md B1). The per-byte queue also cost the USB     **
** task 36 to 43 percent of the core (B2).                               **
**                                                                       **
** With USB_RX_DIRECT set, the core's object is detached from TinyUSB's  **
** callbacks after Serial.begin() and the console reads TinyUSB itself.  **
** TinyUSB 0.21.0 (tusb.c, tu_edpt_stream_read_xfer) arms the next       **
** receive only when its FIFO has room for a whole packet, so while we   **
** are busy the USB hardware answers the PC "not ready" (NAK) and the PC **
** keeps the data: no byte can be lost. Detaching also stops the core's  **
** line-state and line-coding callbacks, which hold the "1200 baud means **
** restart into the flasher" logic the upload uses; usb_rx_poll() reads  **
** the same values from TinyUSB every console pass and hands them to the **
** core's own handlers (USBCDC::_onLineState, _onLineCoding), so that    **
** logic is still the core's code, not a copy.                           **
**                                                                       **
** USB_RX_DIRECT 0 builds the old path through Serial.read() so the two  **
** can be compared (Ronny, 2026-11-09).                                  **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#ifndef USB_RX_H
#define USB_RX_H

#include <stdint.h>

/** 1: read TinyUSB directly, the PC is paced by NAK and nothing is dropped.
 *  0: read through the core's USBCDC queue, which can drop (BUGS.md B1). */
#ifndef USB_RX_DIRECT
#define USB_RX_DIRECT 1
#endif

/**
 * @brief Take over reception from the core. Call once, after Serial.begin().
 *        Does nothing when USB_RX_DIRECT is 0.
 */
void usb_rx_begin(void);

/**
 * @brief Pass the host's line state and line coding to the core's handlers
 *        when they changed. Call every console pass. Does nothing when
 *        USB_RX_DIRECT is 0 (the core's own callbacks do it then).
 */
void usb_rx_poll(void);

/**
 * @brief Read up to max bytes the host has sent.
 * @param[out] buf  At least max bytes.
 * @param[in]  max  Most bytes to take.
 * @return Bytes read, 0 when nothing is waiting.
 */
int usb_rx_read(uint8_t *buf, int max);

#endif /* USB_RX_H */
