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

/** @brief Whether the waiting answer is a password: echo '*' instead of the letters. */
bool board_ask_hidden(void);

/**
 * @brief The line typed as the answer (may be empty: skip). Prints the next
 *        question, or the end of the questions.
 * @param[in] line  The line, without CR LF.
 */
void board_ask_answer(const char *line);

/**
 * @brief A whole line of digits as a node number.
 * @param[in]  text  The line.
 * @param[out] node  1..65535 when the answer is 0.
 * @return 0 for a good number, -1 for digits out of range (0 too), -2 when the
 *         line is not a number at all.
 */
int board_parse_node(const char *text, uint16_t *node);

#endif /* BOARD_H */
