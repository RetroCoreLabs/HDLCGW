/**************************************************************************
** FILE_FETCH - read one SINTRAN file for the FTP server                 **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "file_fetch.h"

#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <libcosmos/fa/fa_client.h>

#include "console_out.h"
#include "sintran_accounts.h"

static FaClient *s_client = NULL;   /* PSRAM; node task only once started */
static uint8_t *s_buf = NULL;       /* FILE_FETCH_MAX, PSRAM */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* Shared between the tasks, under s_mux. */
static file_fetch_status s_st;
static bool s_abandoned = false;    /* the FTP side let go while the client still ran */

/* The request, written by the FTP side while IDLE, read by the node task once ASKED. */
static uint16_t s_system = 0;
static char s_user[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
static char s_password[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
static char s_spec[FA_CLIENT_SPEC];

int file_fetch_init(void)
{
    if (s_client == NULL)
    {
        s_client = (FaClient *)heap_caps_calloc(1, sizeof(FaClient), MALLOC_CAP_SPIRAM);
    }
    if (s_buf == NULL)
    {
        s_buf = (uint8_t *)heap_caps_malloc(FILE_FETCH_MAX, MALLOC_CAP_SPIRAM);
    }
    memset(&s_st, 0, sizeof s_st);
    return (s_client != NULL && s_buf != NULL) ? 0 : -1;
}

/* ------------------------------------------------- client callbacks (node task) */

static void cb_opened(void *ctx, uint32_t length)
{
    (void)ctx;
    portENTER_CRITICAL(&s_mux);
    s_st.length = length;
    s_st.too_long = (length > FILE_FETCH_MAX);
    portEXIT_CRITICAL(&s_mux);
    console_out_log(CONTROL_FRAME_LOG_INFO, "FTP fetch: %s is %lu bytes", s_spec, (unsigned long)length);
}

static void cb_data(void *ctx, uint32_t offset, const uint8_t *bytes, size_t n)
{
    (void)ctx;
    /* A file over FILE_FETCH_MAX is not kept: the client has no way to stop early, so its
     * bytes are dropped here and the fetch ends FAILED with too_long set. */
    if (s_st.too_long || s_abandoned || (uint64_t)offset + n > FILE_FETCH_MAX)
    {
        return;
    }
    memcpy(&s_buf[offset], bytes, n);
    portENTER_CRITICAL(&s_mux);
    if (offset + (uint32_t)n > s_st.have)
    {
        s_st.have = offset + (uint32_t)n;
    }
    portEXIT_CRITICAL(&s_mux);
}

static void cb_done(void *ctx, int result, uint16_t sintran_error)
{
    (void)ctx;
    portENTER_CRITICAL(&s_mux);
    s_st.result = result;
    s_st.sintran_error = sintran_error;
    if (s_abandoned)
    {
        s_abandoned = false;
        s_st.state = FILE_FETCH_IDLE;
    }
    else
    {
        s_st.state = (result == 0 && !s_st.too_long) ? FILE_FETCH_DONE : FILE_FETCH_FAILED;
    }
    portEXIT_CRITICAL(&s_mux);
    console_out_log(CONTROL_FRAME_LOG_INFO, "FTP fetch: %s ended, result %d, SINTRAN error %u", s_spec,
                    result, (unsigned)sintran_error);
}

/* ------------------------------------------------------------ FTP side */

int file_fetch_start(uint16_t system, const char *user, const char *password, const char *spec)
{
    if (s_client == NULL || strlen(user) >= sizeof s_user || strlen(password) >= sizeof s_password
        || strlen(spec) >= sizeof s_spec)
    {
        return -1;
    }
    portENTER_CRITICAL(&s_mux);
    /* A finished fetch the FTP side has not released counts as free; one still asked or
     * running (or abandoned and not yet over) does not. */
    bool idle = (s_st.state == FILE_FETCH_IDLE || s_st.state == FILE_FETCH_DONE
                 || s_st.state == FILE_FETCH_FAILED) && !s_abandoned;
    portEXIT_CRITICAL(&s_mux);
    if (!idle)
    {
        return -1;
    }
    s_system = system;
    (void)snprintf(s_user, sizeof s_user, "%s", user);
    (void)snprintf(s_password, sizeof s_password, "%s", password);
    (void)snprintf(s_spec, sizeof s_spec, "%s", spec);
    portENTER_CRITICAL(&s_mux);
    memset(&s_st, 0, sizeof s_st);
    s_st.state = FILE_FETCH_ASKED;   /* the request fields are complete before this is seen */
    portEXIT_CRITICAL(&s_mux);
    return 0;
}

void file_fetch_get(file_fetch_status *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

size_t file_fetch_copy(uint32_t offset, uint8_t *out, size_t max)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t have = s_st.have;
    portEXIT_CRITICAL(&s_mux);
    if (offset >= have)
    {
        return 0;
    }
    size_t n = (size_t)(have - offset);
    if (n > max)
    {
        n = max;
    }
    memcpy(out, &s_buf[offset], n);
    return n;
}

void file_fetch_release(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_st.state == FILE_FETCH_RUNNING || s_st.state == FILE_FETCH_ASKED)
    {
        /* Still working (or about to start): cb_done sets IDLE when the client is over. */
        s_abandoned = true;
    }
    else
    {
        s_st.state = FILE_FETCH_IDLE;
    }
    portEXIT_CRITICAL(&s_mux);
    memset(s_password, 0, sizeof s_password);
}

/* ----------------------------------------------------------- node task */

void file_fetch_pass(XmsgNode *node, uint32_t now)
{
    portENTER_CRITICAL(&s_mux);
    bool asked = (s_st.state == FILE_FETCH_ASKED);
    portEXIT_CRITICAL(&s_mux);
    if (!asked || node == NULL || s_client == NULL)
    {
        return;
    }
    FaClientConfig cfg;
    FaClientCallbacks cb = { NULL, cb_opened, cb_data, cb_done };
    fa_client_config_default(&cfg);
    int rc = fa_client_init(s_client, &cfg, &cb);
    /* The machine's number goes in as both the node and the system: the request is
     * addressed to that machine, and the host relays it there, as SINTRAN's own
     * LIST-NAMES does (gw_console.cpp svc_names, captured 2026-11-10). Verified for the
     * name table; for file access it is verified only against D100, the host itself. */
    if (rc == XMSG_OK)
    {
        rc = fa_client_read(s_client, node, s_system, s_system, s_user, s_user, s_password, s_spec, now);
    }
    memset(s_password, 0, sizeof s_password);   /* the client keeps only its fold */
    portENTER_CRITICAL(&s_mux);
    if (rc == XMSG_OK)
    {
        s_st.state = FILE_FETCH_RUNNING;
    }
    else if (s_abandoned)
    {
        s_abandoned = false;   /* let go before it could start, and it did not */
        s_st.state = FILE_FETCH_IDLE;
    }
    else
    {
        s_st.state = FILE_FETCH_FAILED;
        s_st.result = rc;
    }
    portEXIT_CRITICAL(&s_mux);
    console_out_log(CONTROL_FRAME_LOG_INFO, "FTP fetch: %s from system %u as %s: start %d", s_spec,
                    (unsigned)s_system, s_user, rc);
}

void file_fetch_reason(const file_fetch_status *st, char *out, size_t cap)
{
    if (st->too_long)
    {
        (void)snprintf(out, cap, "the file is %lu bytes; the gateway reads at most %lu",
                       (unsigned long)st->length, (unsigned long)FILE_FETCH_MAX);
    }
    else if (st->result == FA_CLIENT_EREFUSED)
    {
        (void)snprintf(out, cap, "SINTRAN refused it: error %u (octal %o)", (unsigned)st->sintran_error,
                       (unsigned)st->sintran_error);
    }
    else if (st->result == XMSG_ENOSPC)
    {
        (void)snprintf(out, cap, "the gateway has no free XMSG session");
    }
    else
    {
        (void)snprintf(out, cap, "the transfer failed: XMSG error %d", st->result);
    }
}
