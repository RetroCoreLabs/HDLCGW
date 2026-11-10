/**************************************************************************
** ROUTE_LIST - LIST: read the host's routing table for a telnet user    **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "route_list.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <libxmsg/xrout/xrout_route.h>
#include <libxmsg/xrout/xrout_walk.h>

#include "console_out.h"
#include "names.h"

#define TEXT_SIZE 4096

static XroutWalk *s_walk = NULL;     /* in PSRAM; lives as long as the program */
static char *s_text = NULL;          /* TEXT_SIZE, in PSRAM */
static int s_len = 0;
static uint32_t s_lost = 0;
static SemaphoreHandle_t s_lock = NULL;

static volatile int s_owner = -1;    /* who asked; -1 = nobody */
static volatile bool s_want = false; /* start it on the next pass */
static volatile bool s_cancel = false;
static volatile bool s_running = false;
static volatile bool s_ended = false;
static volatile bool s_abandoned = false; /* the owner left; nobody reads the end */

int route_list_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_walk == NULL)
    {
        s_walk = (XroutWalk *)heap_caps_calloc(1, sizeof(XroutWalk), MALLOC_CAP_SPIRAM);
    }
    if (s_text == NULL)
    {
        s_text = (char *)heap_caps_calloc(1, TEXT_SIZE, MALLOC_CAP_SPIRAM);
    }
    return (s_lock != NULL && s_walk != NULL && s_text != NULL) ? 0 : -1;
}

/** @brief Add text for the owner; counted as lost when it does not fit. */
static void put(const char *fmt, ...)
{
    char line[120];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0)
    {
        return;
    }
    if (n >= (int)sizeof line)
    {
        n = (int)sizeof line - 1;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        s_lost += (uint32_t)n;
        return;
    }
    if (s_len + n <= TEXT_SIZE)
    {
        memcpy(&s_text[s_len], line, (size_t)n);
        s_len += n;
    }
    else
    {
        s_lost += (uint32_t)n;
    }
    xSemaphoreGive(s_lock);
}

int route_list_start(int owner)
{
    if (s_walk == NULL || s_owner >= 0)
    {
        return -1;
    }
    s_len = 0;
    s_lost = 0;
    s_ended = false;
    s_cancel = false;
    s_abandoned = false;
    s_owner = owner;
    s_want = true;
    return 0;
}

void route_list_cancel(int owner)
{
    if (s_owner == owner)
    {
        s_abandoned = true;
        s_cancel = true;
    }
}

int route_list_read(int owner, char *out, int max, bool *done)
{
    int n = 0;
    *done = false;
    if (s_owner != owner || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        return 0;
    }
    n = (s_len < max) ? s_len : max;
    memcpy(out, s_text, (size_t)n);
    memmove(s_text, &s_text[n], (size_t)(s_len - n));
    s_len -= n;
    if (s_ended && s_len == 0)
    {
        *done = true;
        s_owner = -1;        /* free for the next LIST */
    }
    xSemaphoreGive(s_lock);
    return n;
}

/* ------------------------------------------------------------ node task */

static void on_entry(void *ctx, const XroutRouteEntry *e)
{
    (void)ctx;
    put("  %6u  %u %-19s %6u  %4u  %4u\r\n", e->system, e->how, xrout_route_how_name(e->how),
        e->detail, xrout_route_hops(e), xrout_route_wans(e));
}

static void on_done(void *ctx, int result, unsigned entries)
{
    (void)ctx;
    if (result == XMSG_OK)
    {
        put("%u system(s)\r\n", entries);
    }
    else if (result == XMSG_ETIMEDOUT)
    {
        put("LIST: the host did not answer (after %u system(s))\r\n", entries);
    }
    else if (result == XMSG_ESTATE)
    {
        put("LIST: stopped\r\n");
    }
    else
    {
        put("LIST: the host refused the question (error %d, after %u system(s))\r\n", result,
            entries);
    }
    if (s_lost > 0)
    {
        put("LIST: %lu bytes of the list did not fit\r\n", (unsigned long)s_lost);
    }
    console_out_log(CONTROL_FRAME_LOG_INFO, "LIST for slot %d: result %d, %u systems", s_owner,
                    result, entries);
    s_running = false;
    s_ended = true;
}

bool route_list_busy(void)
{
    return s_want || s_running;
}

void route_list_pass(XmsgNode *node, uint32_t now)
{
    if (s_walk == NULL)
    {
        return;
    }
    if (s_want && !names_busy())   /* a name-table walk ends first */
    {
        s_want = false;
        int32_t peer = lapb_peer_node(&node->lapb);
        if (peer < 0)
        {
            put("LIST: the host's node number is not known yet\r\n");
            s_ended = true;
            return;
        }
        XroutWalkCallbacks cb = { NULL, on_entry, on_done };
        /* The host's XMSG system number is taken to be its node number, as on
         * every captured machine (libxmsg xmsg_node.h, "Routing questions"). */
        int rc = xrout_walk_start(s_walk, node, (uint16_t)peer, (uint16_t)peer, 10000u, &cb, now);
        if (rc == XMSG_ESTATE)
        {
            put("LIST: the link to the host is not ready\r\n");
            s_ended = true;
        }
        else if (rc == XMSG_ENOSPC)
        {
            put("LIST: every session slot is in use; try again later\r\n");
            s_ended = true;
        }
        else if (rc != XMSG_OK)
        {
            put("LIST: cannot start (%d)\r\n", rc);
            s_ended = true;
        }
        else
        {
            s_running = true;
            put("Routing table of node %ld:\r\n"
                "  system  how                   detail  hops  WANs\r\n",
                (long)peer);
        }
        return;
    }
    if (s_cancel)
    {
        s_cancel = false;
        if (s_running)
        {
            (void)xrout_walk_cancel(s_walk);   /* on_done ends it */
        }
    }
    if (s_abandoned && s_ended && !s_want)
    {
        s_abandoned = false;
        s_len = 0;
        s_owner = -1;                          /* free for the next LIST */
    }
}
