/**************************************************************************
** ROUTE_LIST - LIST: read the host's routing table for a telnet user    **
**                                                                       **
** Asks the host (the LAPB peer, D100 on the bench) for every system it  **
** reaches, one question per entry (libxmsg xrout_walk.h). Every line    **
** shown comes from the host, none from XMSGGW's own table (Ronny,       **
** 2026-11-10: a local LIST would only ever show D100).                  **
**                                                                       **
** One walk at a time. Another task asks (route_list_start) and reads    **
** the text (route_list_read); the node task runs the walk               **
** (route_list_pass), since only it touches the node.                    **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#ifndef ROUTE_LIST_H
#define ROUTE_LIST_H

#include <stdbool.h>
#include <stdint.h>

#include <libxmsg.h>

/** @brief Buffers in PSRAM. Call once before the node task starts. @return 0 or -1. */
int route_list_init(void);

/**
 * @brief Ask for a LIST for one user.
 * @param[in] owner  The asker's slot number; route_list_read only answers it.
 * @return 0 when queued, -1 when another LIST is running.
 */
int route_list_start(int owner);

/** @brief Stop the owner's LIST (the user left). */
void route_list_cancel(int owner);

/**
 * @brief Take the text made so far.
 * @param[in]  owner  The asker.
 * @param[out] out    Room for max bytes.
 * @param[out] done   true once the last text has been taken and the walk ended.
 * @return Bytes copied.
 */
int route_list_read(int owner, char *out, int max, bool *done);

/** @brief Node task: start, run and end the walk. */
void route_list_pass(XmsgNode *node, uint32_t now);

#endif /* ROUTE_LIST_H */
