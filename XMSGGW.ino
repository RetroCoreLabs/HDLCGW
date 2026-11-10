/**************************************************************************
** XMSGGW - an XMSG node for a Norsk Data machine, on an ESP32-S2        **
**                                                                       **
** The same board and line as HDLCGW (synchronous HDLC through the SPI   **
** line tasks in libhdlc), but instead of passing frames to a PC it runs **
** the XMSG node itself with libxmsg: LAPB to the machine, then (later)  **
** terminal sessions for telnet users and COSMOS file access.            **
**                                                                       **
** The task layout and the USB port are HDLCGW's (Ronny, 2026-11-09:     **
** "copy the console code structure from hdlcgw"): the port carries     **
** RFC 1662 control frames to a host program (HELLO, COMMAND, LOG, STAT, **
** EVENT), and three ESC presses open a text console. A file-transfer    **
** worker task is planned next to the node task once FTP is ready.      **
**                                                                       **
** Tasks (priority, stack in bytes):                                     **
**   SPI rx   24  2048  line receive, above the WiFi driver (HDLCGW T3)  **
**   SPI tx    8  2048  line transmit                                    **
**   Node      4  8192  libhdlc decoder + libxmsg node, owns the line    **
**   Console   2  4096  USB input: control frames, text console          **
**   USB tx    1  2048  USB output ring to the host                      **
**   Telnet    1  6144  WiFi station, telnet user <-> terminal session   **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include <Arduino.h>
/* arduino-cli finds a library only by a header included from the sketch. */
#include <libhdlc.h>
#include <libxmsg.h>

#include "libhdlc/esp32s2/line_log.h"
#include "libhdlc/esp32s2/spi_rx_task.h"
#include "libhdlc/esp32s2/spi_tx_task.h"

#include "console_out.h"
#include "console_task.h"
#include "node_task.h"
#include "settings.h"
#include "telnet_task.h"
#include "usb_rx.h"
#include "usb_tx_task.h"

/** @brief Error text from the line tasks before the console is up:
 *         straight to USB serial. */
static void line_error_early(const char *text)
{
    Serial.println(text);
}

/** @brief Error text from the line tasks once the console is up: a log line. */
static void line_error(const char *text)
{
    console_out_log(CONTROL_FRAME_LOG_ERROR, "%s", text);
}

/**
 * @brief Start-up: USB port, line, node, then the tasks.
 *
 * As in HDLCGW the line and the link start at power-up whether or not a PC
 * has the USB port open (a gateway in a cabinet runs with no PC). The text
 * banner is seen only by a host already connected at boot; a host program
 * gets the same news as a BOOT event.
 */
void setup(void)
{
    uint32_t rate;

#if !USB_RX_DIRECT
    Serial.setRxBufferSize(8192);
#endif
    Serial.begin(115200);
    usb_rx_begin();
    Serial.println("XMSGGW version " XMSGGW_VERSION);

    line_log_set_error(line_error_early);
    spi_tx_task_init();
    spi_rx_task_init();
    rate = settings_rate();
    if (rate != 0)
    {
        spi_tx_task_set_bit_rate(rate);
    }
    usb_tx_task_init();
    console_out_init();
    line_log_set_error(line_error);
    node_task_init();
    telnet_task_init();
    console_out_event(CONTROL_FRAME_EVENT_BOOT, "XMSGGW version " XMSGGW_VERSION);

    /* Line stacks and priorities are HDLCGW's measured ones (HDLCGW.ino).
     * The node task sits below the line and above the console: frames off
     * the line must be decoded before the 4 KB receive ring fills, and a
     * slow USB host must never delay LAPB. Its 8 KB stack is a first guess;
     * STAT and TASK show what it has used. */
    xTaskCreate(spi_rx_task_run, "SPI rx", 2048, NULL, 24, NULL);
    xTaskCreate(spi_tx_task_run, "SPI tx", 2048, NULL, 8, NULL);
    xTaskCreate(node_task_run, "Node", 12288, NULL, 4, NULL);   /* + the console (gw_console.h): PBKDF2, the command line */
    xTaskCreate(console_task_run, "Console", 4096, NULL, 2, NULL);
    xTaskCreate(usb_tx_task_run, "USB tx", 2048, NULL, 1, NULL);
    /* WiFi and telnet at the lowest level, as HDLCGW's WiFi task: a slow
     * network must never cost line bytes. 6 KB is a first guess; TASK shows
     * what it uses. */
    xTaskCreate(telnet_task_run, "Telnet", 6144, NULL, 1, NULL);
}

/** @brief Arduino main loop. Everything runs in the tasks; this only sleeps. */
void loop(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}
