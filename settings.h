/**************************************************************************
** SETTINGS - XMSGGW values kept in NVS across restarts                  **
**                                                                       **
** One NVS namespace, "xmsggw". The node task and the console task both  **
** use it; every call opens and closes its own handle, and ESP-IDF NVS   **
** serialises access itself, so no lock is needed here.                  **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>

/**
 * @brief Our LAPB node number.
 * @return The stored number, or 0 when none has been set (NODE command).
 *         The gateway does not start the link without one: there is no
 *         number it could safely guess.
 */
uint16_t settings_node(void);

/**
 * @brief Store our LAPB node number. Used from the next restart.
 * @param[in] node  1..65535.
 * @return 0 on success, -1 when the NVS write failed.
 */
int settings_set_node(uint16_t node);

/**
 * @brief Line bit rate.
 * @return The stored rate in bit/s, or 0 when none is stored (the line task
 *         then keeps its own start-up rate).
 */
uint32_t settings_rate(void);

/**
 * @brief Store the line bit rate.
 * @param[in] rate  Bit/s.
 * @return 0 on success, -1 when the NVS write failed.
 */
int settings_set_rate(uint32_t rate);

/**
 * @brief Flags1 counter for one peer machine (libxmsg XmsgFlags1Port.load).
 * @return 0 and *next filled when stored, -1 otherwise.
 */
int settings_flags1_load(void *ctx, uint16_t peer_node, uint16_t *next);

/** @brief Store the Flags1 counter for one peer (XmsgFlags1Port.save). @return 0 or -1. */
int settings_flags1_save(void *ctx, uint16_t peer_node, uint16_t next);

/** @brief Port-word seed (XmsgFlags1Port.load_port_seed). @return 0 when stored, -1 otherwise. */
int settings_port_seed_load(void *ctx, uint16_t *seed);

/** @brief Store the port-word seed (XmsgFlags1Port.save_port_seed). @return 0 or -1. */
int settings_port_seed_save(void *ctx, uint16_t seed);

/** Longest WiFi network name and password kept, NUL included (WiFi limits: 32 and 64). */
#define SETTINGS_SSID_SIZE 33
#define SETTINGS_PASS_SIZE 65

/**
 * @brief The WiFi network to join.
 *
 * XMSGGW's own "wssid"/"wpass" keys. While XMSGGW has none, HDLCGW's stored
 * network (namespace "hdlcgw", keys "ssid"/"pass") is read instead, only
 * read, never written or printed: the same board ran HDLCGW, and the network
 * it joined is the one to use. The password is never shown by any command.
 *
 * @param[out] ssid  SETTINGS_SSID_SIZE bytes; "" when none is stored anywhere.
 * @param[out] pass  SETTINGS_PASS_SIZE bytes; "" for an open network.
 * @return 0 from XMSGGW's keys, 1 from HDLCGW's, -1 when none is stored.
 */
int settings_wifi(char *ssid, char *pass);

/**
 * @brief Store the WiFi network name (XMSGGW's own key).
 * @param[in] ssid  Name, at most 32 characters.
 * @return 0 on success, -1 on a bad length or a failed write.
 */
int settings_set_wifi_ssid(const char *ssid);

/**
 * @brief Store the WiFi password (XMSGGW's own key).
 * @param[in] pass  Password, at most 64 characters; "" for an open network.
 * @return 0 on success, -1 on a bad length or a failed write.
 */
int settings_set_wifi_pass(const char *pass);

#endif /* SETTINGS_H */
