/**************************************************************************
** NAMES - the system names (aliases) the host knows, read from its XROUT **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "names.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <libxmsg/xrout/xrout_name.h>
#include <libxmsg/xrout/xrout_names_walk.h>

#include "console_out.h"
#include "route_list.h"

typedef struct
{
    char name[XROUT_NAME_LEN_MAX + 1];
    uint16_t system;
} name_entry;

typedef struct
{
    name_entry e[NAMES_MAX];
    int count;
} name_table;

static XroutNamesWalk *s_walk = NULL; /* PSRAM */
static name_table *s_have = NULL;     /* the last complete table; read by other tasks */
static name_table *s_new = NULL;      /* filled during a walk, node task only */
static name_table *s_copy = NULL;     /* names_list's copy, console task only */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool s_running = false;        /* node task only */
static bool s_was_ready = false;      /* node task only */
static uint32_t s_last_ms = 0;        /* when the last walk ended */
static bool s_walked = false;         /* a walk has ended since the link became ready */
static volatile bool s_want = false;  /* names_refresh() */
static unsigned s_dropped = 0;        /* names over NAMES_MAX in the last walk */
static int s_last_result = 1;         /* 1 = none yet; else the walk's result */
static uint32_t s_have_ms = 0;        /* when s_have was taken */

int names_init(void)
{
    if (s_walk == NULL)
    {
        s_walk = (XroutNamesWalk *)heap_caps_calloc(1, sizeof(XroutNamesWalk), MALLOC_CAP_SPIRAM);
    }
    if (s_have == NULL)
    {
        s_have = (name_table *)heap_caps_calloc(1, sizeof(name_table), MALLOC_CAP_SPIRAM);
    }
    if (s_new == NULL)
    {
        s_new = (name_table *)heap_caps_calloc(1, sizeof(name_table), MALLOC_CAP_SPIRAM);
    }
    if (s_copy == NULL)
    {
        s_copy = (name_table *)heap_caps_calloc(1, sizeof(name_table), MALLOC_CAP_SPIRAM);
    }
    /* All in PSRAM: internal RAM is full once WiFi is linked in. */
    return (s_walk != NULL && s_have != NULL && s_new != NULL && s_copy != NULL) ? 0 : -1;
}

/** @brief One row of the host's table: keep it when it is a system name. */
static void on_row(void *ctx, const XroutNameRow *row)
{
    (void)ctx;
    if (!xrout_name_is_system(row))
    {
        return;   /* a named port such as *TADADM, not a system */
    }
    if (s_new->count >= NAMES_MAX)
    {
        s_dropped++;
        return;
    }
    name_entry *e = &s_new->e[s_new->count];
    size_t len = strnlen(row->name, XROUT_NAME_LEN_MAX);
    memcpy(e->name, row->name, len);
    e->name[len] = '\0';
    /* LIST-NAMES on D102 prints these names with a dot after them
     * ("D100."). Whether the dot is in the row or added by the printout is
     * not verified, so a trailing dot is dropped: a user types "D100". */
    while (len > 0 && e->name[len - 1] == '.')
    {
        e->name[--len] = '\0';
    }
    if (len == 0)
    {
        return;
    }
    e->system = row->system;
    s_new->count++;
}

/** @brief The walk ended: a complete table replaces the old one. */
static void on_done(void *ctx, int result, unsigned rows)
{
    (void)ctx;
    s_running = false;
    s_walked = true;
    s_last_ms = millis();
    s_last_result = result;
    if (result == XMSG_OK)
    {
        portENTER_CRITICAL(&s_mux);
        memcpy(s_have, s_new, sizeof *s_have);
        s_have_ms = s_last_ms;
        portEXIT_CRITICAL(&s_mux);
    }
    /* Not complete: the old table stays, a part of one would hide names. */
    console_out_log(CONTROL_FRAME_LOG_INFO,
                    "Host name table: result %d, %u rows, %d system names%s", result, rows,
                    s_new->count, (s_dropped > 0) ? ", some dropped (table full)" : "");
}

/** @brief Start a walk now. */
static void start(XmsgNode *node, uint32_t now)
{
    int32_t peer = lapb_peer_node(&node->lapb);
    if (peer < 0)
    {
        return;
    }
    XroutNamesWalkCallbacks cb = { NULL, on_row, on_done };
    s_new->count = 0;
    s_dropped = 0;
    /* The host's XMSG system number is taken to be its node number, as LIST
     * does (route_list.cpp). */
    int rc = xrout_names_walk_start(s_walk, node, (uint16_t)peer, (uint16_t)peer, 10000u, &cb,
                                    now);
    if (rc == XMSG_OK)
    {
        s_running = true;
        return;
    }
    /* No slot or not ready: try again after a while, not on every pass. */
    s_walked = true;
    s_last_ms = now;
    s_last_result = rc;
}

void names_pass(XmsgNode *node, uint32_t now)
{
    if (s_walk == NULL)
    {
        return;
    }
    bool ready = xmsg_node_is_ready(node);
    if (ready && !s_was_ready)
    {
        s_walked = false;   /* a new link: read the table again */
    }
    s_was_ready = ready;
    if (!ready || s_running || route_list_busy())
    {
        return;
    }
    if (s_want || !s_walked
        || (uint32_t)(now - s_last_ms) >= (s_last_result == XMSG_OK ? NAMES_REFRESH_MS : 60000u))
    {
        s_want = false;
        start(node, now);
    }
}

bool names_busy(void)
{
    return s_running;
}

void names_refresh(void)
{
    s_want = true;
}

int names_lookup(const char *name, uint16_t *system)
{
    int rc = -1;
    if (s_have == NULL || name == NULL || system == NULL)
    {
        return -1;
    }
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_have->count; i++)
    {
        if (strcasecmp(s_have->e[i].name, name) == 0)
        {
            *system = s_have->e[i].system;
            rc = 0;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return rc;
}

int names_list(char *out, size_t size)
{
    uint32_t taken;
    size_t at = 0;
    out[0] = '\0';
    if (s_have == NULL || s_copy == NULL)
    {
        return 0;
    }
    name_table &copy = *s_copy;
    portENTER_CRITICAL(&s_mux);
    memcpy(&copy, s_have, sizeof copy);
    taken = s_have_ms;
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < copy.count; i++)
    {
        int len = snprintf(&out[at], size - at, "%-16s %u\r\n", copy.e[i].name,
                           (unsigned)copy.e[i].system);
        if (len <= 0 || (size_t)len >= size - at)
        {
            return copy.count;
        }
        at += (size_t)len;
    }
    if (taken == 0u)
    {
        (void)snprintf(&out[at], size - at, "(never read from the host");
    }
    else
    {
        (void)snprintf(&out[at], size - at, "(read %lu s ago",
                       (unsigned long)((millis() - taken) / 1000u));
    }
    at += strlen(&out[at]);
    if (s_last_result == 1)
    {
        (void)snprintf(&out[at], size - at, "; no walk yet)\r\n");
    }
    else if (s_last_result == XMSG_OK)
    {
        (void)snprintf(&out[at], size - at, ")\r\n");
    }
    else
    {
        /* The walk's own record of why (xrout_names_walk.h): the XROUT status
         * byte of a refused reply and the XMSG error of a refusal frame. */
        (void)snprintf(&out[at], size - at,
                       "; last walk failed: result %d, XROUT status %u, XMSG error %d)\r\n",
                       s_last_result, (unsigned)s_walk->last_status, (int)s_walk->last_error);
    }
    return copy.count;
}
