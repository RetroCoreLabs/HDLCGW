/**************************************************************************
** BOARD - what both firmwares on one HDLC-ESP32-S2 board share           **
**                                                                       **
** One board holds two firmwares (Ronny, 2026-11-12):                    **
**                                                                       **
**   slot 1  partition app0, flash 0x10000    HDLCGW (frames over USB,   **
**                                            WiFi and TCP)              **
**   slot 2  partition app1, flash 0x150000   XMSGGW (XMSG node, telnet, **
**                                            FTP)                       **
**                                                                       **
** Which one starts is kept in the otadata partition (0xe000). This      **
** module:                                                               **
**                                                                       **
**  - switches to the other firmware (FIRMWARE SWITCH, or the button on  **
**    GPIO5 held while the board starts);                                **
**  - lets the bootloader go back to the old firmware when the new one   **
**    restarts before it has run BOARD_TRIAL_MS (rollback);              **
**  - lights the firmware's own LED: GPIO3 green = HDLCGW, GPIO1 red =   **
**    XMSGGW. It blinks during the trial and is steady after it;         **
**  - keeps the settings both firmwares need in one NVS namespace,       **
**    "board": the WiFi network name and password, and the line rate.    **
**    Each firmware's own settings stay in its own namespace ("hdlcgw",  **
**    "xmsggw", "xmsgusr", "xmsgftp"), and a switch touches none of      **
**    them: both firmwares share the one nvs partition (0x9000).         **
**                                                                       **
** The same source is built into both sketches: each has a one-line      **
** board.cpp that includes ../common/board.cpp, because Arduino only     **
** compiles the files in the sketch folder.                              **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Firmware select button: to GND, internal pull-up, so pressed reads low (kicad/README.md). */
#define BOARD_PIN_BUTTON 5

/** HDLCGW's LED, green, lit when high. */
#define BOARD_PIN_LED_HDLCGW 3

/** XMSGGW's LED, red, lit when high. */
#define BOARD_PIN_LED_XMSGGW 1

/** A firmware chosen by a switch must run this long before it is kept; a restart sooner
 *  sends the board back to the other one. */
#define BOARD_TRIAL_MS 10000u

/** Longest WiFi network name and password kept, NUL included (WiFi limits: 32 and 64). */
#define BOARD_SSID_SIZE 33
#define BOARD_PASS_SIZE 65

/** The two firmwares. The number is also the slot index: slot 1 is 0, slot 2 is 1. */
typedef enum
{
    BOARD_FW_HDLCGW = 0,
    BOARD_FW_XMSGGW = 1
} board_firmware;

/**
 * @brief First thing in setup(): LEDs, the button, the shared settings.
 *
 * Lights this firmware's LED and turns the other one off. When the button is
 * held, both LEDs light until it is let go and the board then restarts into
 * the other firmware (when that slot holds one). Records which firmware this
 * slot holds, for FIRMWARE. Copies the WiFi network and the line rate into
 * the shared namespace once, from the old per-firmware keys.
 *
 * @param[in] me       Which firmware this is.
 * @param[in] version  Its version text, e.g. "0.2.0".
 */
void board_start(board_firmware me, const char *version);

/**
 * @brief Call about once a second (loop()). Blinks the LED during the trial
 *        and keeps the firmware once it has run BOARD_TRIAL_MS.
 * @param[in] now_ms  millis().
 */
void board_tick(uint32_t now_ms);

/**
 * @brief FIRMWARE: what each slot holds, which one runs, and how to switch.
 * @param[out] out  Text, lines ended with CR LF.
 * @param[in]  cap  Size of out.
 */
void board_firmware_report(char *out, size_t cap);

/**
 * @brief FIRMWARE SWITCH: make the other slot start next. The caller then
 *        restarts the board (ESP.restart()) after its reply has gone out.
 * @param[out] out  One line saying what happens, or why not, CR LF ended.
 * @param[in]  cap  Size of out.
 * @return 0 when the other firmware starts after the restart, -1 when not
 *         (no firmware in that slot, or the boot choice could not be written).
 */
int board_firmware_switch(char *out, size_t cap);

/**
 * @brief The WiFi network both firmwares join.
 * @param[out] ssid  BOARD_SSID_SIZE bytes; "" when none is stored.
 * @param[out] pass  BOARD_PASS_SIZE bytes; "" for an open network. Never print it.
 * @return 0 when a network is stored, -1 when none.
 */
int board_wifi(char *ssid, char *pass);

/**
 * @brief Store the WiFi network name.
 * @param[in] ssid  1 to 32 characters.
 * @return 0, or -1 on a bad length or a failed write.
 */
int board_set_wifi_ssid(const char *ssid);

/**
 * @brief Store the WiFi password.
 * @param[in] pass  At most 64 characters; "" for an open network.
 * @return 0, or -1 on a bad length or a failed write.
 */
int board_set_wifi_pass(const char *pass);

/**
 * @brief Whether the admin answered NONE to the WiFi question: this board is
 *        meant to run without WiFi, so the console does not ask again. Cleared
 *        by board_set_wifi_ssid() and board_clear_wifi().
 */
bool board_no_wifi(void);

/**
 * @brief WIFI SCAN in both firmwares: list the networks the board can see.
 *        Blocks about 3 s (the radio listens on every channel; 2.4 GHz only).
 *        Turns the radio on as a station first when it is off.
 * @param[in] print  Prints text on the console, as typed.
 */
void board_wifi_scan(void (*print)(const char *text));

/**
 * @brief WIFI SHOW in both firmwares: the stored network name AND password.
 *        Ronny, 2026-11-12: shown on the USB console only. Both firmwares run
 *        WIFI commands only from the USB console; never call this from telnet.
 * @param[in] print  Prints text on the console, as typed.
 */
void board_wifi_show(void (*print)(const char *text));

/** @brief Forget the WiFi network name and password (and a NONE answer). @return 0 or -1. */
int board_clear_wifi(void);

/**
 * @brief The line bit rate both firmwares start with.
 * @return Bit/s, or 0 when none is stored (the firmware keeps its default).
 */
uint32_t board_rate(void);

/**
 * @brief Store the line bit rate.
 * @param[in] rate  Bit/s.
 * @return 0 or -1.
 */
int board_set_rate(uint32_t rate);

/* ------------------------------------------------- missing settings
 *
 * Ronny, 2026-11-12: a setting the firmware needs and does not have is asked
 * for when an admin arrives, instead of waiting for someone to know the
 * command. On the USB text console (no login there: whoever holds the cable
 * is the admin) that is the WiFi network, in both firmwares, and the node
 * number, in XMSGGW (0 is not a node number). Each question can be skipped
 * with an empty line, and comes back the next time the console opens. The
 * XMSGGW telnet admin login asks for the node number itself (gw_console.cpp).
 */

/** What the firmware gives the questions: how to print, and where answers go. */
typedef struct
{
    /** Print text on the console, as typed (CR LF where wanted). */
    void (*print)(const char *text);
    /** Store the WiFi network name. @return 0 or -1. */
    int (*set_ssid)(const char *ssid);
    /** Store the WiFi password. @return 0 or -1. */
    int (*set_pass)(const char *pass);
    /** Join the stored network now. */
    void (*wifi_join)(void);
    /** The node number, 0 when none is stored. NULL: this firmware has none. */
    uint16_t (*node)(void);
    /** Store the node number (1..65535). @return 0 or -1. */
    int (*set_node)(uint16_t node);
} board_ask_ops;

/**
 * @brief The text console has just opened: ask for the first missing setting.
 * @param[in] ops  The firmware's hooks; kept, so it must stay valid (a static).
 * @return true when a question is now waiting for a line (do not print the prompt),
 *         false when nothing is missing.
 */
bool board_ask_start(const board_ask_ops *ops);

/** @brief Whether a question is waiting: the next line typed is its answer. */
bool board_ask_active(void);

/**
 * @brief WIFI SETUP: ask for the WiFi network now, stored one or not.
 *
 * Scans, lists the networks by number, asks for a number (or a name, SCAN,
 * NONE, or Enter to keep what is stored), then for the password. Answers come
 * through board_ask_answer() like the questions at console start
 * (Ronny, 2026-11-14: "a wifi command that can be executed at any time").
 *
 * @param[in] ops  The same callbacks as board_ask_start(); must stay valid.
 * @return true when the question is now open; false when ops is unusable.
 */
bool board_ask_wifi(const board_ask_ops *ops);

/**
 * @brief Print why the board last started (power on, crash, watchdog, ...) and
 *        how long it has run since. For INFO in both firmwares: a board that
 *        restarts by itself must say why (bench 2026-11-14).
 * @param[in] print  Where the line goes.
 */
void board_print_restart(void (*print)(const char *text));

/**
 * @brief Limit the WiFi station to 802.11b. Call after WiFi.mode(WIFI_STA)
 *        and before WiFi.begin(), in both firmwares.
 *
 * BUGS.md B1: on ESP32-S2 v1.0 boards USB from the PC failed whenever the
 * radio sent at the g/n rates (2 of 2 runs), and never with 802.11b only
 * (4 of 4), full send power both times, 2026-11-23. Espressif PCN20230702
 * says v1.0 sends with more power at the fast g/n rates. Ronny chose 802.11b
 * on every board: up to 11 Mbit/s on the air, the gateway needs at most
 * half a megabit.
 */
void board_wifi_protocol(void);

/** DMA-capable internal RAM that must stay free under full traffic (Ronny,
 *  2026-11-23): the WiFi radio sends only from that kind of memory, and when
 *  it runs out sending stops (BUGS.md B2, docs/MEMORY-MAP.md). */
#define BOARD_DMA_MARGIN 4096u

/** Longest frame the gateways take, FCS included (Ronny, 2026-11-23: "Accept
 *  and say 4105", as the Pico). Buffers stay BYTE_FRAME_MAX (4112, libhdlc);
 *  the decoders and the line receive side refuse anything longer, count it as
 *  too long, and HELLO sends maxframe=4105. */
#define GW_FRAME_MAX 4105

/**
 * @brief Lowest free DMA-capable internal RAM since start, in bytes.
 * @return heap_caps_get_minimum_free_size(MALLOC_CAP_DMA).
 */
unsigned board_dma_least(void);

/**
 * @brief Print the hardware: chip model, revision, cores and radios, CPU clock,
 *        flash (physical size, size the image was built for, speed, mode),
 *        internal RAM and PSRAM. For INFO in both firmwares, so a board's
 *        fitness as a gateway is read from the board (Ronny, 2026-11-23, after
 *        testing plain ESP32 boards with no PSRAM).
 * @param[in] print  Where the lines go.
 */
void board_print_hardware(void (*print)(const char *text));

/**
 * @brief The same hardware facts as HELLO key=value lines, each ending '\n':
 *        cores, mhz, flash (bytes on the chip), ram (internal heap size),
 *        ramfree, psram (bytes, 0 when none). Key names agreed with RC422Dev
 *        so the PC tool shows one table for every board.
 * @param[out] out   Buffer for the text, NUL terminated.
 * @param[in]  size  Size of out.
 * @return Length written (0 when out is NULL or size is 0), truncated to size - 1.
 */
int board_hello_hardware(char *out, size_t size);

/** @brief Whether the waiting answer is a password: echo '*' instead of the letters. */
bool board_ask_hidden(void);

/**
 * @brief The line typed as the answer (may be empty: skip). Prints the next
 *        question, or the end of the questions.
 * @param[in] line  The line, without CR LF.
 */
void board_ask_answer(const char *line);

/**
 * @brief Copy a network name without the spaces and tabs at its two ends.
 *
 * A name typed with a space after it ("HackerCorp ") is a different network,
 * one that does not exist, and the board then tries to join it for ever
 * (bench 2026-11-14). Every place that stores a name goes through this.
 *
 * @param[in]  in   The name as typed.
 * @param[out] out  Receives the trimmed name, NUL terminated.
 * @param[in]  cap  Size of out; BOARD_SSID_SIZE.
 * @return Length of the trimmed name, or -1 when it is empty or longer than cap - 1.
 */
int board_trim_ssid(const char *in, char *out, size_t cap);

/**
 * @brief A whole line of digits as a node number.
 * @param[in]  text  The line.
 * @param[out] node  1..65535 when the answer is 0.
 * @return 0 for a good number, -1 for digits out of range (0 too), -2 when the
 *         line is not a number at all.
 */
int board_parse_node(const char *text, uint16_t *node);

#endif /* BOARD_H */
