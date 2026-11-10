/**************************************************************************
** TERM - terminal sessions (TAD CONNECT-TO) to systems on the peer      **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "term.h"

#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "console_out.h"
#include "settings.h"

#define TERM_IN_SIZE  2048
#define TERM_OUT_SIZE 256

/** @brief One session and its two text buffers. */
typedef struct
{
    TadAsker tad;
    bool open;                        /* initialised and attached, until CLOSED/FAILED */
    bool used;                        /* tad has been initialised at least once */
    char system[16];
    char user[TAD_IAM_USER_SIZE];     /* "" = libxmsg's default name */
    volatile bool want_connect;
    volatile bool want_hangup;
    volatile bool want_escape;
    volatile bool echo;               /* ECKM: echo typing unless the host says not to */
    uint8_t in[TERM_IN_SIZE];         /* host text not read yet */
    int in_len;
    uint32_t lost_in;
    uint8_t out[TERM_OUT_SIZE];       /* typed text waiting for an input grant */
    int out_len;
} term_slot;

static term_slot *s_slots = NULL;     /* TERM_SLOTS of them, in PSRAM */
static SemaphoreHandle_t s_lock = NULL;

/** @brief Take the buffer lock; false when it could not be had in 50 ms. */
static bool lock(void)
{
    return s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE;
}

/** @brief The slot for an index, or NULL. */
static term_slot *slot_at(int slot)
{
    if (s_slots == NULL || slot < 0 || slot >= TERM_SLOTS)
    {
        return NULL;
    }
    return &s_slots[slot];
}

int term_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_slots == NULL)
    {
        s_slots = (term_slot *)heap_caps_calloc(TERM_SLOTS, sizeof(term_slot), MALLOC_CAP_SPIRAM);
    }
    return (s_slots != NULL && s_lock != NULL) ? 0 : -1;
}

/* ------------------------------------------------------ asker callbacks */
/* ctx is the slot. */

static void cb_text(void *ctx, const uint8_t *bytes, size_t len)
{
    term_slot *t = (term_slot *)ctx;
    if (!lock())
    {
        t->lost_in += (uint32_t)len;
        return;
    }
    for (size_t i = 0; i < len; i++)
    {
        if (t->in_len < TERM_IN_SIZE)
        {
            t->in[t->in_len++] = bytes[i];
        }
        else
        {
            t->lost_in++;
        }
    }
    xSemaphoreGive(s_lock);
}

static void cb_state(void *ctx, TadAskerState state, int error)
{
    term_slot *t = (term_slot *)ctx;
    console_out_log(CONTROL_FRAME_LOG_INFO, "Terminal session %d: %s, error %d",
                    (int)(t - s_slots), tad_asker_state_name(state), error);
}

static void cb_echo(void *ctx, uint8_t eckm)
{
    ((term_slot *)ctx)->echo = (eckm != 0xFFu);
}

/** @brief The host's login ended; hang up, as the asker expects (tad_asker.h). */
static void cb_logged_out(void *ctx)
{
    ((term_slot *)ctx)->want_hangup = true;
}

/* --------------------------------------------------------- other tasks */

int term_open(int slot, const char *system, const char *user)
{
    term_slot *t = slot_at(slot);
    size_t n = strlen(system);
    if (t == NULL || n == 0 || n >= sizeof t->system || t->open || t->want_connect)
    {
        return -1;
    }
    memcpy(t->system, system, n + 1);
    t->user[0] = '\0';
    if (user != NULL && user[0] != '\0')
    {
        size_t u = strlen(user);
        if (u + sizeof "@xmsggw" <= sizeof t->user)
        {
            (void)snprintf(t->user, sizeof t->user, "%s@xmsggw", user);
        }
        else
        {
            if (u >= sizeof t->user)
            {
                u = sizeof t->user - 1u;
            }
            memcpy(t->user, user, u);
            t->user[u] = '\0';
        }
    }
    t->want_hangup = false;
    t->want_escape = false;
    t->want_connect = true;
    return 0;
}

int term_type(int slot, const uint8_t *text, int len)
{
    term_slot *t = slot_at(slot);
    int took = 0;
    if (t == NULL || !t->open)
    {
        return -1;
    }
    if (!lock())
    {
        return 0;
    }
    while (took < len && t->out_len < TERM_OUT_SIZE)
    {
        t->out[t->out_len++] = text[took++];
    }
    xSemaphoreGive(s_lock);
    return took;
}

void term_hangup(int slot)
{
    term_slot *t = slot_at(slot);
    if (t != NULL)
    {
        t->want_connect = false;
        t->want_hangup = true;
    }
}

void term_escape(int slot)
{
    term_slot *t = slot_at(slot);
    if (t != NULL && t->open)
    {
        t->want_escape = true;
    }
}

int term_read(int slot, uint8_t *out, int max)
{
    term_slot *t = slot_at(slot);
    int n;
    if (t == NULL || !lock())
    {
        return 0;
    }
    n = (t->in_len < max) ? t->in_len : max;
    memcpy(out, t->in, (size_t)n);
    memmove(t->in, &t->in[n], (size_t)(t->in_len - n));
    t->in_len -= n;
    xSemaphoreGive(s_lock);
    return n;
}

void term_get_status(int slot, term_status *out)
{
    term_slot *t = slot_at(slot);
    memset(out, 0, sizeof *out);
    out->state = "none";
    if (t == NULL)
    {
        return;
    }
    /* Read without the node's lock: a status line, at most one pass old. */
    if (t->used)
    {
        out->state = tad_asker_state_name(t->tad.state);
        out->error = t->tad.error;
        out->bytes_in = t->tad.stats.text_bytes_in;
        out->bytes_out = t->tad.stats.text_bytes_out;
        out->may_type = t->open && tad_asker_may_type(&t->tad);
    }
    out->open = t->open || t->want_connect;
    out->echo = t->echo;
    out->lost_in = t->lost_in;
    out->queued_out = t->out_len;
}

/* ---------------------------------------------------------- node task */

/**
 * @brief Start a session that was asked for.
 * @param[in] t     Slot.
 * @param[in] node  Node.
 * @param[in] now   millis().
 */
static void slot_connect(term_slot *t, XmsgNode *node, uint32_t now)
{
    int32_t peer = lapb_peer_node(&node->lapb);
    TadAskerConfig cfg;
    TadAskerCallbacks cb;
    int slot = (int)(t - s_slots);

    t->want_connect = false;
    if (peer < 0)
    {
        console_out_log(CONTROL_FRAME_LOG_WARN, "Terminal session %d: peer not known", slot);
        return;
    }
    tad_asker_config_default(&cfg, (uint16_t)peer, t->system);
    /* What TADA on the host shows for this session: terminal = slot + 1. */
    cfg.terminal_number = (uint16_t)(slot + 1);
    /* Terminal type, high byte first (tad_asker.h; settings.h says why it
     * is a setting). Read from NVS once per session open. */
    uint16_t ttyp = settings_ttyp();
    cfg.ttyp[0] = (uint8_t)(ttyp >> 8u);
    cfg.ttyp[1] = (uint8_t)(ttyp & 0xFFu);
    if (t->user[0] != '\0')
    {
        cfg.user_name = t->user;   /* copied by tad_asker_init */
    }
    memset(&cb, 0, sizeof cb);
    cb.ctx = t;
    cb.text_out = cb_text;
    cb.state = cb_state;
    cb.echo_mode = cb_echo;
    cb.logged_out = cb_logged_out;
    if (lock())
    {
        t->in_len = 0;
        t->out_len = 0;
        xSemaphoreGive(s_lock);
    }
    t->lost_in = 0;
    t->echo = true;
    if (tad_asker_init(&t->tad, &cfg, &cb) != XMSG_OK)
    {
        console_out_log(CONTROL_FRAME_LOG_ERROR, "Terminal session %d: bad system name", slot);
        return;
    }
    t->used = true;
    int rc = tad_asker_connect(&t->tad, node, now);
    if (rc != XMSG_OK)
    {
        console_out_log(CONTROL_FRAME_LOG_ERROR, "Terminal session %d: connect refused, %d", slot,
                        rc);
        return;
    }
    t->open = true;
}

void term_pass(XmsgNode *node, uint32_t now)
{
    if (s_slots == NULL)
    {
        return;
    }
    for (int i = 0; i < TERM_SLOTS; i++)
    {
        term_slot *t = &s_slots[i];

        if (t->want_connect && xmsg_node_is_ready(node))
        {
            slot_connect(t, node, now);
        }
        if (!t->open)
        {
            continue;
        }
        if (t->tad.state == TAD_ASKER_CLOSED || t->tad.state == TAD_ASKER_FAILED)
        {
            t->open = false;     /* the asker has detached itself from the node */
            t->want_hangup = false;
            continue;
        }
        if (t->want_hangup)
        {
            t->want_hangup = false;
            if (tad_asker_hangup(&t->tad, now) != XMSG_OK)
            {
                tad_asker_abort(&t->tad);
            }
            continue;
        }
        if (t->want_escape)
        {
            t->want_escape = false;
            /* Refused only before the session is up (tad_asker.h); then the
             * key is simply lost, as on a terminal before the host answers. */
            (void)tad_asker_escape(&t->tad, now);
        }
        if (t->out_len > 0 && tad_asker_may_type(&t->tad) && lock())
        {
            int n = (t->out_len < TAD_MAX_DATA) ? t->out_len : TAD_MAX_DATA;
            int took = tad_asker_write(&t->tad, t->out, (size_t)n, now);
            if (took > 0)
            {
                memmove(t->out, &t->out[took], (size_t)(t->out_len - took));
                t->out_len -= took;
            }
            xSemaphoreGive(s_lock);
        }
    }
}
