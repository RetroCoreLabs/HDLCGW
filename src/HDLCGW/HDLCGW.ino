/**************************************************************************
** HDLCGW - HDLC gateway for the LOLIN ESP32-S2 Mini                     **
**                                                                       **
** The synchronous line has a clock per direction. spi_tx_task.cpp       **
** transmits on SPI2 as master and generates the transmit clock;          **
** spi_rx_task.cpp receives on SPI3 as slave, following the far end's    **
** clock. hdlc.cpp does bit stuffing, flag/abort detection and the FCS.  **
** The USB serial port carries RFC 1662 byte-stuffed frames to the host  **
** (byte_frame.cpp) and, after three ESC presses, a text console         **
** (console_task.cpp, commands.cpp). A host program talks to the         **
** firmware without leaving the frame channel through control frames     **
** (control_frame.cpp): frames addressed to 0xFE carry commands,         **
** replies, log lines and counters, and never reach the line.            **
**                                                                       **
** File naming: a file holding a FreeRTOS task is <module>_task.cpp with **
** entry point <module>_task_run; every function in a file starts with   **
** the file's name.                                                      **
**                                                                       **
** Board: https://done.land/components/microcontroller/families/esp/     **
**        esp32/s2/s2mini                                                **
** Bit stuffing logic from the SDLCBridge by Mattis Lind:                **
**   https://github.com/MattisLind/alfaskop_emu/blob/master/Utils/       **
**   SDLCBridge/SDLCBridge.ino                                           **
** Host framing and operating modes as in RC422Dev, the Pico version of  **
** this gateway: https://github.com/RetroCoreLabs/RC422Dev               **
**                                                                       **
** Ronny Hansen                                                          **
***************************************************************************/
#include <Arduino.h>
/* The shared line code (HDLC framing, host framing, control frames, ring
 * buffer) lives in the libhdlc Arduino library. Included first so the IDE
 * finds it before any file below needs one of its headers. */
#include <libhdlc.h>

#include "console_out.h"
#include "console_task.h"
#include "libhdlc/esp32s2/spi_rx_task.h"
#include "libhdlc/esp32s2/spi_tx_task.h"
#include "usb_rx.h"
#include "libhdlc/esp32s2/line_log.h"
#include "usb_tx_task.h"
#include "wifi_task.h"
#include "../common/board.h"

/**
 * @brief Arduino start-up: open the USB serial port and start the four tasks.
 *
 * The line starts at power-up whether or not a host has opened the USB port
 * (Ronny's decision 2026-11-08): a gateway in a cabinet must clock the line
 * and receive frames with no PC attached. So there is no wait for the host
 * here; the version banner is only seen by a host that is already connected
 * at boot, and USB CDC writes with no host connected are dropped, they do
 * not block. In FORWARDING the port is a binary frame channel, so the banner
 * is the only text the host sees unless it opens the console with ESC ESC
 * ESC. The line tasks run above the console task so the line is never
 * starved. Stack sizes are in bytes on the ESP32 port of FreeRTOS.
 */
/** @brief Error text from the libhdlc line tasks before the console is up:
 *        straight to USB serial, as the tasks did before they moved. */
static void line_error_early(const char *text)
{
    Serial.println(text);
}

/** @brief Error text from the libhdlc line tasks once the console is up:
 *        the console log. */
static void line_error(const char *text)
{
    console_out_log(CONTROL_FRAME_LOG_ERROR, "%s", text);
}

void setup(void)
{
    /* USB CDC receive queue. The core's default is 256 bytes and bytes are
     * DROPPED when it is full (USBCDC::_onRX). A host pushing frames at full
     * USB speed fills 256 bytes in well under the console task's 1 ms loop,
     * so give it room for a few maximum-size byte-stuffed frames. Must be
     * set before begin(). With USB_RX_DIRECT (usb_rx.h) that queue is not
     * used at all: TinyUSB holds the bytes and paces the host, so the
     * default 256 bytes stay and the 8 KB are not taken from internal RAM. */
#if !USB_RX_DIRECT
    Serial.setRxBufferSize(8192);
#endif
    /* First: the firmware LEDs, the firmware select button (it may restart
     * the board into XMSGGW) and the settings both firmwares share. */
    board_start(BOARD_FW_HDLCGW, HDLCGW_VERSION);
    Serial.begin(115200);
    usb_rx_begin();
    Serial.println("HDLC GW version " HDLCGW_VERSION);

    line_log_set_error(line_error_early);
    spi_tx_task_init();
    spi_rx_task_init();
    /* The line rate both firmwares share (BAUD stores it); none stored keeps
     * the transmit task's own start-up rate. */
    if (board_rate() != 0)
    {
        spi_tx_task_set_bit_rate(board_rate());
    }
    usb_tx_task_init();
    console_task_init();
    wifi_task_init();
    console_out_init();
    line_log_set_error(line_error);
    /* The same news as the text banner, as an EVENT frame for a host program
     * that is already listening on the frame channel. */
    console_out_event(CONTROL_FRAME_EVENT_BOOT, "HDLC GW version " HDLCGW_VERSION);
    /* Stack sizes from measured high-water marks (TASK command, 2026-11-09,
     * after HELP, WIFI SCAN, WIFI STATUS, RXDMA, INSPECT, DECODE, INFO, STAT
     * and two-board traffic): SPI rx used 392 bytes, SPI tx 804, USB tx 624,
     * Console 2148, each given at least 1 KB more. They were 4 and 8 KB; the
     * 10 KB freed holds the line and USB rings in internal RAM (hot_path.h).
     * WiFi keeps 8 KB: its TCP-peer path has not been measured yet. */
    /* Line receive at 24, above the WiFi driver (23) and lwIP (tiT, 18).
     * At 9 it was held off for up to 97.6 ms (95 gaps over 20 ms in 60 s)
     * during 10-byte two-way TCP traffic at 983000, and the receive DMA ring
     * (2048 bytes, 16.7 ms at that rate) wrapped over unread words: about
     * 5 percent of frames were never seen and 0.3 percent arrived bad
     * (BUGS.md T3, 2026-11-09). The task takes 5 to 7 percent of the CPU
     * and sleeps 1 ms between passes, so it cannot starve the radio. */
    xTaskCreate(spi_rx_task_run, "SPI rx", 2048, NULL, 24, NULL);
    xTaskCreate(spi_tx_task_run, "SPI tx", 2048, NULL, 8, NULL);
    xTaskCreate(console_task_run, "Console", 4096, NULL, 2, NULL);
    /* Below console_task: the USB write it does can spin while the host is
     * slow, and that must never take time from the line or the console.
     * Tested at the console's priority 2026-11-09 (BUGS.md B4): USB TX then
     * kept up with 10-byte frames at 983000, but the line RX ring overflowed
     * by 1.4 MB a minute and 10195 frames arrived bad, so it went back to 1. */
    xTaskCreate(usb_tx_task_run, "USB tx", 2048, NULL, 1, NULL);
    /* The radio and the sockets: same level as USB output, below the line and
     * the console; a slow network must never cost line bytes. */
    xTaskCreate(wifi_task_run, "WiFi", 8192, NULL, 1, NULL);
}

/**
 * @brief Arduino main loop. Everything runs in the tasks; this only sleeps.
 */
void loop(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    board_tick(millis());
}
