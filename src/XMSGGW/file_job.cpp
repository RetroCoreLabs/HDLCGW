/**************************************************************************
** FILE_JOB - SINTRAN file jobs, shared by the FTP server and the        **
**            console's file commands                                    **
**                                                                       **
** Ronny Hansen                                                          **
** SPDX-License-Identifier: MIT  Copyright (c) 2026 Ronny Hansen         **
***************************************************************************/
#include "file_job.h"

#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <libcosmos/fa/fa_client.h>

#include "console_out.h"
#include "sintran_accounts.h"

/** One job. The rings are written by the node task (the client callbacks) and read by the
 *  front end; the counters that make data visible move under mux, after the data. */
struct FileJob
{
    const char *name;
    portMUX_TYPE mux;
    FaClient *client;         /* PSRAM; node task only once started */

    /* Under mux. */
    file_job_state state;
    bool released;            /* the front end let go; stop and go IDLE when over */
    bool cancel_sent;         /* node task: fa_client_cancel called */
    int result;
    uint16_t sintran_error;
    uint32_t length;
    uint32_t bytes;           /* total bytes in = data_in */
    uint32_t entries;         /* total entries in */
    uint32_t entries_lost;
    bool data_lost;
    char directory[24];
    char user_full[24];

    /* The request: written by the front end while IDLE, read by the node task once ASKED. */
    bool list;
    const uint8_t *src;       /* write: the front end's bytes; NULL for read and list */
    uint32_t src_len;
    uint16_t system;
    char user[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
    char password[SINTRAN_ACCOUNT_FIELD_MAX + 1u];
    char spec[FA_CLIENT_SPEC];

    uint8_t *data;            /* data_cap, PSRAM */
    size_t data_cap;
    uint32_t data_out;        /* front end */
    FaFileEntry *ring;        /* entry_cap, PSRAM */
    size_t entry_cap;
    uint32_t ring_out;        /* front end */
};

static FileJob s_jobs[FILE_JOB_MAX];
static size_t s_job_count = 0;

FileJob *file_job_create(const char *name, size_t data_cap, size_t entry_cap)
{
    if (s_job_count >= FILE_JOB_MAX || data_cap == 0u || entry_cap == 0u)
    {
        return NULL;
    }
    FileJob *j = &s_jobs[s_job_count];
    memset(j, 0, sizeof *j);
    j->name = name;
    j->mux = portMUX_INITIALIZER_UNLOCKED;
    j->client = (FaClient *)heap_caps_calloc(1, sizeof(FaClient), MALLOC_CAP_SPIRAM);
    j->data = (uint8_t *)heap_caps_malloc(data_cap, MALLOC_CAP_SPIRAM);
    j->ring = (FaFileEntry *)heap_caps_calloc(entry_cap, sizeof(FaFileEntry), MALLOC_CAP_SPIRAM);
    if (j->client == NULL || j->data == NULL || j->ring == NULL)
    {
        heap_caps_free(j->client);
        heap_caps_free(j->data);
        heap_caps_free(j->ring);
        memset(j, 0, sizeof *j);
        return NULL;
    }
    j->data_cap = data_cap;
    j->entry_cap = entry_cap;
    s_job_count++;
    return j;
}

/** @brief What a job does, for the log. */
static const char *job_what(const FileJob *j)
{
    return (j->src != NULL) ? "write" : j->list ? "list" : "read";
}

/* ------------------------------------------------- client callbacks (node task) */

static void cb_opened(void *ctx, uint32_t length)
{
    FileJob *j = (FileJob *)ctx;
    portENTER_CRITICAL(&j->mux);
    j->length = length;
    portEXIT_CRITICAL(&j->mux);
}

static void cb_data(void *ctx, uint32_t offset, const uint8_t *bytes, size_t n)
{
    FileJob *j = (FileJob *)ctx;
    (void)offset;   /* the pieces come in order */
    portENTER_CRITICAL(&j->mux);
    uint32_t in = j->bytes;
    uint32_t out = j->data_out;
    bool drop = j->released || j->data_lost;
    portEXIT_CRITICAL(&j->mux);
    if (drop)
    {
        return;
    }
    size_t room = j->data_cap - (size_t)(in - out);
    size_t take = (n < room) ? n : room;
    for (size_t i = 0; i < take; i++)
    {
        j->data[(in + (uint32_t)i) % j->data_cap] = bytes[i];
    }
    portENTER_CRITICAL(&j->mux);
    j->bytes = in + (uint32_t)take;
    if (take < n)
    {
        j->data_lost = true;   /* the front end fell behind: the rest is dropped */
    }
    portEXIT_CRITICAL(&j->mux);
}

static void cb_directory(void *ctx, const char *directory, const char *user)
{
    FileJob *j = (FileJob *)ctx;
    portENTER_CRITICAL(&j->mux);
    (void)snprintf(j->directory, sizeof j->directory, "%s", directory);
    /* SINTRAN's full user name: "SYST" typed is "SYSTEM" here, as SINTRAN prints it. */
    (void)snprintf(j->user_full, sizeof j->user_full, "%s", user);
    portEXIT_CRITICAL(&j->mux);
}

static void cb_entry(void *ctx, const FaFileEntry *e)
{
    FileJob *j = (FileJob *)ctx;
    portENTER_CRITICAL(&j->mux);
    uint32_t in = j->entries;
    uint32_t out = j->ring_out;
    bool drop = j->released;
    portEXIT_CRITICAL(&j->mux);
    if (drop)
    {
        return;
    }
    if ((size_t)(in - out) >= j->entry_cap)
    {
        portENTER_CRITICAL(&j->mux);
        j->entries_lost++;
        portEXIT_CRITICAL(&j->mux);
        return;
    }
    j->ring[in % j->entry_cap] = *e;
    portENTER_CRITICAL(&j->mux);
    j->entries = in + 1u;
    portEXIT_CRITICAL(&j->mux);
}

/* Write: hand the client the bytes it asks for. It may ask for the same range again
 * when a frame is resent, so the front end keeps them until the job is over. */
static size_t cb_source(void *ctx, uint32_t offset, uint8_t *out, size_t n)
{
    FileJob *j = (FileJob *)ctx;
    if (j->src == NULL || offset > j->src_len || n > (size_t)(j->src_len - offset))
    {
        return 0;   /* the client then ends with XMSG_EIO */
    }
    memcpy(out, &j->src[offset], n);
    return n;
}

static void cb_done(void *ctx, int result, uint16_t sintran_error)
{
    FileJob *j = (FileJob *)ctx;
    portENTER_CRITICAL(&j->mux);
    j->result = result;
    j->sintran_error = sintran_error;
    j->state = j->released ? FILE_JOB_IDLE : FILE_JOB_ENDED;
    j->released = false;
    portEXIT_CRITICAL(&j->mux);
    console_out_log(CONTROL_FRAME_LOG_INFO, "%s file job: %s \"%s\" ended, result %d, SINTRAN error %u", j->name,
                    job_what(j), j->spec, result, (unsigned)sintran_error);
}

/* ------------------------------------------------------------ front end */

/* The request fields, the write source among them, are all set before the state says
 * ASKED: the node task may start the job the moment it sees that. */
static int job_start(FileJob *job, bool list, const uint8_t *src, uint32_t src_len, uint16_t system,
                     const char *user, const char *password, const char *spec)
{
    if (job == NULL || strlen(user) >= sizeof job->user || strlen(password) >= sizeof job->password
        || strlen(spec) >= sizeof job->spec)
    {
        return -1;
    }
    portENTER_CRITICAL(&job->mux);
    bool idle = (job->state == FILE_JOB_IDLE);
    portEXIT_CRITICAL(&job->mux);
    if (!idle)
    {
        return -1;
    }
    job->list = list;
    job->src = src;
    job->src_len = src_len;
    job->system = system;
    (void)snprintf(job->user, sizeof job->user, "%s", user);
    (void)snprintf(job->password, sizeof job->password, "%s", password);
    (void)snprintf(job->spec, sizeof job->spec, "%s", spec);
    portENTER_CRITICAL(&job->mux);
    job->released = false;
    job->cancel_sent = false;
    job->result = 0;
    job->sintran_error = 0;
    job->length = 0;
    job->bytes = 0;
    job->data_out = 0;
    job->data_lost = false;
    job->entries = 0;
    job->ring_out = 0;
    job->entries_lost = 0;
    job->directory[0] = '\0';
    job->user_full[0] = '\0';
    job->state = FILE_JOB_ASKED;   /* the request fields are complete before this is seen */
    portEXIT_CRITICAL(&job->mux);
    return 0;
}

int file_job_start(FileJob *job, bool list, uint16_t system, const char *user, const char *password,
                   const char *spec)
{
    return job_start(job, list, NULL, 0, system, user, password, spec);
}

int file_job_start_write(FileJob *job, uint16_t system, const char *user, const char *password,
                         const char *spec, const uint8_t *bytes, uint32_t length)
{
    if (bytes == NULL || length == 0u)
    {
        return -1;
    }
    return job_start(job, false, bytes, length, system, user, password, spec);
}

void file_job_get(FileJob *job, file_job_status *out)
{
    portENTER_CRITICAL(&job->mux);
    out->state = job->state;
    out->result = job->result;
    out->sintran_error = job->sintran_error;
    out->length = job->length;
    out->bytes = job->bytes;
    out->entries = job->entries;
    out->data_lost = job->data_lost;
    out->entries_lost = job->entries_lost;
    memcpy(out->directory, job->directory, sizeof out->directory);
    memcpy(out->user, job->user_full, sizeof out->user);
    portEXIT_CRITICAL(&job->mux);
}

size_t file_job_take_bytes(FileJob *job, uint8_t *out, size_t max)
{
    portENTER_CRITICAL(&job->mux);
    uint32_t in = job->bytes;
    uint32_t from = job->data_out;
    portEXIT_CRITICAL(&job->mux);
    size_t n = (size_t)(in - from);
    if (n > max)
    {
        n = max;
    }
    for (size_t i = 0; i < n; i++)
    {
        out[i] = job->data[(from + (uint32_t)i) % job->data_cap];
    }
    portENTER_CRITICAL(&job->mux);
    job->data_out = from + (uint32_t)n;
    portEXIT_CRITICAL(&job->mux);
    return n;
}

bool file_job_take_entry(FileJob *job, FaFileEntry *out)
{
    portENTER_CRITICAL(&job->mux);
    uint32_t in = job->entries;
    uint32_t from = job->ring_out;
    portEXIT_CRITICAL(&job->mux);
    if (from == in)
    {
        return false;
    }
    *out = job->ring[from % job->entry_cap];
    portENTER_CRITICAL(&job->mux);
    job->ring_out = from + 1u;
    portEXIT_CRITICAL(&job->mux);
    return true;
}

void file_job_release(FileJob *job)
{
    if (job == NULL)
    {
        return;
    }
    portENTER_CRITICAL(&job->mux);
    if (job->state == FILE_JOB_ASKED || job->state == FILE_JOB_RUNNING)
    {
        job->released = true;   /* the node task stops it; cb_done or the pass sets IDLE */
    }
    else
    {
        job->state = FILE_JOB_IDLE;
    }
    portEXIT_CRITICAL(&job->mux);
}

/* ----------------------------------------------------------- node task */

static void job_pass(FileJob *j, XmsgNode *node, uint32_t now)
{
    portENTER_CRITICAL(&j->mux);
    file_job_state st = j->state;
    bool released = j->released;
    bool stop = (st == FILE_JOB_RUNNING) && released && !j->cancel_sent;
    if (st == FILE_JOB_ASKED && released)
    {
        j->state = FILE_JOB_IDLE;   /* let go before it started */
        j->released = false;
        st = FILE_JOB_IDLE;
    }
    if (stop)
    {
        j->cancel_sent = true;
    }
    portEXIT_CRITICAL(&j->mux);
    if (st == FILE_JOB_IDLE && released)
    {
        memset(j->password, 0, sizeof j->password);
        return;
    }
    if (stop)
    {
        /* The front end stopped or left: stop SINTRAN now rather than read to the end.
         * The client ends with done(XMSG_ECANCELED), which sets IDLE (cb_done). */
        int rc = fa_client_cancel(j->client, now);
        console_out_log(CONTROL_FRAME_LOG_INFO, "%s file job: \"%s\" stopped: cancel %d", j->name, j->spec, rc);
        return;
    }
    if (st != FILE_JOB_ASKED || node == NULL)
    {
        return;
    }
    FaClientConfig cfg;
    FaClientCallbacks cb = { j, cb_opened, cb_data, cb_done, cb_directory, cb_entry, cb_source };
    fa_client_config_default(&cfg);
    int rc = fa_client_init(j->client, &cfg, &cb);
    /* The machine's number goes in as both the node and the system: the request is
     * addressed to that machine, and the host relays it there, as SINTRAN's own
     * LIST-NAMES does (gw_console.cpp svc_names, captured 2026-11-10). Verified for file
     * reads from D100 itself and from D102 through D100 (2026-11-11). */
    if (rc == XMSG_OK)
    {
        if (j->src != NULL)
        {
            rc = fa_client_write(j->client, node, j->system, j->system, j->user, j->user, j->password, j->spec,
                                 j->src_len, now);
        }
        else if (j->list)
        {
            rc = fa_client_list(j->client, node, j->system, j->system, j->user, j->user, j->password, j->spec, now);
        }
        else
        {
            rc = fa_client_read(j->client, node, j->system, j->system, j->user, j->user, j->password, j->spec, now);
        }
    }
    memset(j->password, 0, sizeof j->password);   /* the client keeps only its fold */
    console_out_log(CONTROL_FRAME_LOG_INFO, "%s file job: %s \"%s\" on system %u as %s: start %d", j->name,
                    job_what(j), j->spec, (unsigned)j->system, j->user, rc);
    portENTER_CRITICAL(&j->mux);
    if (rc == XMSG_OK)
    {
        j->state = FILE_JOB_RUNNING;
    }
    else if (j->released)
    {
        j->released = false;
        j->state = FILE_JOB_IDLE;
    }
    else
    {
        j->result = rc;
        j->state = FILE_JOB_ENDED;
    }
    portEXIT_CRITICAL(&j->mux);
}

void file_job_pass_all(XmsgNode *node, uint32_t now)
{
    for (size_t i = 0; i < s_job_count; i++)
    {
        job_pass(&s_jobs[i], node, now);
    }
}

void file_job_reason(const file_job_status *st, char *out, size_t cap)
{
    if (st->result == FA_CLIENT_EREFUSED)
    {
        (void)snprintf(out, cap, "SINTRAN refused it: error %u (octal %o)", (unsigned)st->sintran_error,
                       (unsigned)st->sintran_error);
    }
    else if (st->result == XMSG_ENOSPC)
    {
        (void)snprintf(out, cap, "the gateway has no free XMSG session");
    }
    else if (st->result == XMSG_EIO)
    {
        (void)snprintf(out, cap, "the gateway could not hand over the file's bytes");
    }
    else if (st->data_lost)
    {
        (void)snprintf(out, cap, "the receiver fell behind and bytes were dropped");
    }
    else
    {
        (void)snprintf(out, cap, "the transfer failed: XMSG error %d", st->result);
    }
}
