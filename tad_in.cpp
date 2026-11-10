/**************************************************************************
** TAD_IN - terminal sessions FROM a SINTRAN machine (CONNECT-TO D101)   **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "tad_in.h"

#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <libxmsg/tad/tad_responder.h>

#include "console_out.h"
#include "telnet_task.h"

#define IN_SIZE  512
#define OUT_SIZE 2048

/* In the output queue, 0xFF starts a control item: 'E' + 0/1 (echo off/on)
 * or 'S' + high + low (login state). XMSGGW's own text is ASCII and the
 * onward host's text has bit 7 cleared (telnet_task.cpp), so 0xFF never
 * occurs as text. */
#define OUT_MARK 0xFFu

/** @brief One responder and the user place it serves. */
typedef struct
{
    TadResponder r;
    bool used;                   /* tad_responder_init has run */
    volatile int place;          /* user place, -1 = none */
    volatile tad_in_state state;
    volatile bool want_hangup;
    volatile bool escape;
    bool greeted;                /* ECKM 01 and SYCN 0002 sent at RUNNING */
    char who[32];
    uint8_t in[IN_SIZE];
    int in_len;
    uint32_t lost_in;
    uint8_t out[OUT_SIZE];
    int out_len;
} tad_in_slot;

static tad_in_slot *s_slots = NULL;   /* TAD_IN_RESPONDERS, in PSRAM */
static SemaphoreHandle_t s_lock = NULL;

static bool lock(void)
{
    return s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

/** @brief The slot serving a user place, or NULL. */
static tad_in_slot *slot_for(int place)
{
    if (s_slots == NULL || place < 0)
    {
        return NULL;
    }
    for (int k = 0; k < TAD_IN_RESPONDERS; k++)
    {
        if (s_slots[k].place == place)
        {
            return &s_slots[k];
        }
    }
    return NULL;
}

int tad_in_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_slots == NULL)
    {
        s_slots = (tad_in_slot *)heap_caps_calloc(TAD_IN_RESPONDERS, sizeof(tad_in_slot),
                                                  MALLOC_CAP_SPIRAM);
        if (s_slots != NULL)
        {
            for (int k = 0; k < TAD_IN_RESPONDERS; k++)
            {
                s_slots[k].place = -1;
            }
        }
    }
    return (s_lock != NULL && s_slots != NULL) ? 0 : -1;
}

/* ------------------------------------------------------ responder callbacks */
/* ctx is the slot; all run in the node task. */

static int cb_incoming(void *ctx, const TadResponderCaller *who)
{
    tad_in_slot *t = (tad_in_slot *)ctx;
    int place = telnet_task_claim_place();
    if (place < 0)
    {
        console_out_log(CONTROL_FRAME_LOG_WARN,
                        "Incoming terminal session from system %u refused: all %d places in use",
                        (unsigned)who->system, TELNET_TASK_USERS);
        return 4; /* XRNSP: no free seat (tad_responder.h) */
    }
    if (lock())
    {
        t->in_len = 0;
        t->out_len = 0;
        unlock();
    }
    t->lost_in = 0;
    t->escape = false;
    t->want_hangup = false;
    t->greeted = false;
    (void)snprintf(t->who, sizeof t->who, "system %u port 0x%04X", (unsigned)who->system,
                   (unsigned)who->port);
    t->place = place;
    t->state = TAD_IN_COMING;
    console_out_log(CONTROL_FRAME_LOG_INFO, "Incoming terminal session from %s for %s: place %d",
                    t->who, who->target, place);
    return 0;
}

static void cb_state(void *ctx, TadResponderState state, int reason)
{
    tad_in_slot *t = (tad_in_slot *)ctx;
    console_out_log(CONTROL_FRAME_LOG_INFO, "Incoming terminal session, place %d: %s, %d",
                    t->place, tad_responder_state_name(state), reason);
    if (state == TAD_RESP_RUNNING)
    {
        t->state = TAD_IN_RUNNING;
    }
    else if (state == TAD_RESP_CLOSED || state == TAD_RESP_FAILED)
    {
        t->state = TAD_IN_ENDED;
    }
}

static void cb_text_in(void *ctx, const uint8_t *bytes, size_t len)
{
    tad_in_slot *t = (tad_in_slot *)ctx;
    if (!lock())
    {
        t->lost_in += (uint32_t)len;
        return;
    }
    for (size_t k = 0; k < len; k++)
    {
        if (t->in_len < IN_SIZE)
        {
            t->in[t->in_len++] = bytes[k];
        }
        else
        {
            t->lost_in++;
        }
    }
    unlock();
}

static void cb_escape(void *ctx)
{
    ((tad_in_slot *)ctx)->escape = true;
}

static void cb_status(void *ctx, uint16_t *in_use, uint16_t *total)
{
    (void)ctx;
    *in_use = (uint16_t)telnet_task_places_in_use();
    *total = (uint16_t)TELNET_TASK_USERS;
}

/** @brief Make a slot ready for a call. */
static void slot_prepare(tad_in_slot *t, int k)
{
    TadResponderConfig cfg;
    TadResponderCallbacks cb;
    tad_responder_config_default(&cfg);
    /* Each responder its own logical unit, 0x0300 + n as captured hosts do
     * (tad_responder.h: meaning inferred, not decoded). */
    cfg.logical_unit = (uint16_t)(0x0300u + (uint16_t)k);
    memset(&cb, 0, sizeof cb);
    cb.ctx = t;
    cb.incoming = cb_incoming;
    cb.state = cb_state;
    cb.text_in = cb_text_in;
    cb.escape = cb_escape;
    cb.status = cb_status;
    (void)tad_responder_init(&t->r, &cfg, &cb);
    t->used = true;
}

/* --------------------------------------------------------- node task */

void tad_in_letter(XmsgNode *node, const XmsgDatagram *dg, uint32_t now)
{
    if (s_slots == NULL)
    {
        return;
    }
    if (!tad_responder_is_connect(dg))
    {
        console_out_log(CONTROL_FRAME_LOG_DEBUG, "Port-0 letter that is not for *TADADM: dropped");
        return;
    }
    for (int k = 0; k < TAD_IN_RESPONDERS; k++)
    {
        tad_in_slot *t = &s_slots[k];
        /* A slot is offered the letter only when it has no user place:
         * the place is freed by the telnet task once the user is gone. */
        if (t->place >= 0)
        {
            continue;
        }
        if (!t->used || tad_responder_state(&t->r) == TAD_RESP_CLOSED
            || tad_responder_state(&t->r) == TAD_RESP_FAILED)
        {
            slot_prepare(t, k);
        }
        int rc = tad_responder_offer(&t->r, node, dg, now);
        if (rc == XMSG_EAGAIN)
        {
            continue;
        }
        if (rc < 0)
        {
            console_out_log(CONTROL_FRAME_LOG_WARN, "Incoming terminal session: offer failed, %d",
                            rc);
        }
        return;
    }
    if (tad_responder_is_status_query(dg))
    {
        /* Every responder busy: answer the status question all the same. */
        uint16_t in_use = (uint16_t)telnet_task_places_in_use();
        (void)tad_responder_status_reply(node, dg, in_use,
                                         (uint16_t)(TELNET_TASK_USERS - in_use), now);
        return;
    }
    console_out_log(CONTROL_FRAME_LOG_WARN,
                    "Incoming terminal session refused: all %d responders busy",
                    TAD_IN_RESPONDERS);
    (void)tad_responder_refuse(node, dg, 4u, now);
}

/** @brief Move queued output into the responder, in order, as far as it takes it. */
static void flush_out(tad_in_slot *t)
{
    int at = 0;
    if (!lock())
    {
        return;
    }
    while (at < t->out_len)
    {
        if (t->out[at] == OUT_MARK)
        {
            if (at + 1 >= t->out_len)
            {
                break;
            }
            int rc;
            int used;
            if (t->out[at + 1] == 'E' && at + 2 < t->out_len)
            {
                rc = tad_responder_echo(&t->r, (t->out[at + 2] != 0) ? 0x01u : 0xFFu);
                used = 3;
            }
            else if (t->out[at + 1] == 'S' && at + 3 < t->out_len)
            {
                rc = tad_responder_login_state(&t->r, (uint16_t)((t->out[at + 2] << 8u)
                                                                 | t->out[at + 3]));
                used = 4;
            }
            else
            {
                break;   /* the rest of the item is not queued yet */
            }
            if (rc != XMSG_OK)
            {
                break;   /* every chain full: try again on the next pass */
            }
            at += used;
            continue;
        }
        int run = 0;
        while (at + run < t->out_len && t->out[at + run] != OUT_MARK)
        {
            run++;
        }
        int took = tad_responder_write(&t->r, &t->out[at], (size_t)run);
        if (took <= 0)
        {
            break;
        }
        at += took;
        if (took < run)
        {
            break;
        }
    }
    memmove(t->out, &t->out[at], (size_t)(t->out_len - at));
    t->out_len -= at;
    unlock();
}

void tad_in_pass(XmsgNode *node, uint32_t now)
{
    (void)node;
    if (s_slots == NULL)
    {
        return;
    }
    for (int k = 0; k < TAD_IN_RESPONDERS; k++)
    {
        tad_in_slot *t = &s_slots[k];
        if (t->place < 0 || t->state == TAD_IN_ENDED)
        {
            continue;
        }
        TadResponderState st = tad_responder_state(&t->r);
        if (t->want_hangup)
        {
            t->want_hangup = false;
            /* What the user was told last ("bye") goes before the hang-up;
             * hangup() sends what was added (tad_responder.h). */
            if (st == TAD_RESP_RUNNING)
            {
                flush_out(t);
            }
            if (tad_responder_hangup(&t->r, now) != XMSG_OK)
            {
                tad_responder_abort(&t->r);
                t->state = TAD_IN_ENDED;
            }
            continue;
        }
        if (st != TAD_RESP_RUNNING)
        {
            continue;
        }
        if (!t->greeted)
        {
            /* As the real host starts a login (tad_responder.h): the asker
             * echoes typed keys, and the login is waiting for a user. */
            t->greeted = true;
            (void)tad_responder_echo(&t->r, 0x01u);
            (void)tad_responder_login_state(&t->r, TAD_SYCN_WAIT_USER);
        }
        flush_out(t);
        /* The user may type once everything we had to say is out of our
         * queue (it may still be in the responder's chains, ahead of the
         * grant, which is the order the real host uses: text, then RFI). */
        if (t->out_len == 0 && !tad_responder_input_granted(&t->r))
        {
            (void)tad_responder_grant_input(&t->r);
        }
    }
}

/* -------------------------------------------------------- telnet task */

tad_in_state tad_in_get_state(int place)
{
    tad_in_slot *t = slot_for(place);
    return (t == NULL) ? TAD_IN_NONE : t->state;
}

void tad_in_describe(int place, char *out, size_t size)
{
    tad_in_slot *t = slot_for(place);
    (void)snprintf(out, size, "%s", (t == NULL) ? "?" : t->who);
}

int tad_in_read(int place, uint8_t *out, int max)
{
    tad_in_slot *t = slot_for(place);
    int n;
    if (t == NULL || !lock())
    {
        return 0;
    }
    n = (t->in_len < max) ? t->in_len : max;
    memcpy(out, t->in, (size_t)n);
    memmove(t->in, &t->in[n], (size_t)(t->in_len - n));
    t->in_len -= n;
    unlock();
    return n;
}

bool tad_in_take_escape(int place)
{
    tad_in_slot *t = slot_for(place);
    if (t == NULL || !t->escape)
    {
        return false;
    }
    t->escape = false;
    return true;
}

/** @brief Queue bytes; all or nothing. */
static int queue_out(int place, const uint8_t *bytes, int len)
{
    tad_in_slot *t = slot_for(place);
    int took = 0;
    if (t == NULL || !lock())
    {
        return 0;
    }
    if (t->out_len + len <= OUT_SIZE)
    {
        memcpy(&t->out[t->out_len], bytes, (size_t)len);
        t->out_len += len;
        took = len;
    }
    unlock();
    return took;
}

int tad_in_write(int place, const uint8_t *bytes, int len)
{
    /* Text in pieces that fit, never splitting the queue's own items. */
    int done = 0;
    while (done < len)
    {
        int n = len - done;
        if (n > 256)
        {
            n = 256;
        }
        if (queue_out(place, &bytes[done], n) != n)
        {
            break;
        }
        done += n;
    }
    return done;
}

void tad_in_echo(int place, bool on)
{
    const uint8_t item[3] = { OUT_MARK, 'E', (uint8_t)(on ? 1u : 0u) };
    (void)queue_out(place, item, (int)sizeof item);
}

void tad_in_login_state(int place, uint16_t sycn)
{
    const uint8_t item[4] = { OUT_MARK, 'S', (uint8_t)(sycn >> 8u), (uint8_t)(sycn & 0xFFu) };
    (void)queue_out(place, item, (int)sizeof item);
}

void tad_in_hangup(int place)
{
    tad_in_slot *t = slot_for(place);
    if (t != NULL && t->state != TAD_IN_ENDED)
    {
        t->want_hangup = true;
    }
}

void tad_in_release(int place)
{
    tad_in_slot *t = slot_for(place);
    if (t == NULL)
    {
        return;
    }
    if (t->state != TAD_IN_ENDED)
    {
        /* The user went while the session was up: say it to the far end. */
        t->want_hangup = true;
        return;   /* released when the session has ended (telnet task asks again) */
    }
    t->place = -1;
    t->state = TAD_IN_NONE;
}
