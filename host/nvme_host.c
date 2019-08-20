/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - NVMe Host Interface
 *
 * Copyright 2018 IT University of Copenhagen
 * 
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 */

#include <sys/queue.h>
#include <stdio.h>
#include <string.h>
#include <ox-fabrics.h>
#include <nvme.h>
#include <nvme-host-dev.h>

static struct nvme_host nvmeh;

int nvme_host_add_server_iface (const char *addr, uint16_t port)
{
    return oxf_host_add_server_iface (addr, port);
}

int nvme_host_create_queue (uint16_t qid)
{
    return oxf_host_create_queue (qid);
}

void nvme_host_destroy_queue (uint16_t qid)
{
    return oxf_host_destroy_queue (qid);
}

void nvmeh_callback (void *ctx, struct nvme_cqe *cqe)
{
    struct nvmeh_cmd_status *status = (struct nvmeh_cmd_status *) ctx;
    struct nvmeh_ctx *nvmeh_ctx = status->ctx;
    uint16_t cmd_i, first_status = 0;

    status->status = cqe->status;

    pthread_spin_lock (&nvmeh_ctx->spin);
    if (status->status)
        nvmeh_ctx->failed++;

    nvmeh_ctx->completed++;

    if (nvmeh_ctx->completed >= nvmeh_ctx->n_cmd) {

        pthread_spin_unlock (&nvmeh_ctx->spin);
        if (nvmeh_ctx->failed) {
            for (cmd_i = 0; cmd_i < nvmeh_ctx->n_cmd; cmd_i++) {
                if (nvmeh_ctx->cmd_status[cmd_i].status) {
                    first_status = nvmeh_ctx->cmd_status[cmd_i].status;
                    break;
                }
            }                
        }
        nvmeh_ctx->user_cb (nvmeh_ctx->user_ctx, first_status);
        (nvmeh_ctx->is_write) ? nvmeh_ctxw_put (nvmeh_ctx->host, nvmeh_ctx) :
                                nvmeh_ctxr_put (nvmeh_ctx->host, nvmeh_ctx);
        return;

    }

    pthread_spin_unlock (&nvmeh_ctx->spin);
}

uint16_t nvmeh_get_cmdid (struct nvme_host *host)
{
    uint16_t id;

    pthread_spin_lock (&host->cmdid_spin);
    host->cmdid = id = (host->cmdid < 65535) ? host->cmdid + 1 : 0;
    pthread_spin_unlock (&host->cmdid_spin);

    return id;
}

struct nvmeh_ctx *nvmeh_ctxr_get (struct nvme_host *host)
{
    struct nvmeh_ctx *ctxr;
    uint16_t retry = NVMEH_RETRY;

RETRY:
    pthread_spin_lock (&host->ctxr_spin);
    ctxr = TAILQ_FIRST(&host->ctxr_fh);
    if (!ctxr) {
        pthread_spin_unlock (&host->ctxr_spin);
        usleep (NVMEH_RETRY_DELAY);
        retry--;
        if (retry)
            goto RETRY;
        else
            return NULL;
    }
    TAILQ_REMOVE(&host->ctxr_fh, ctxr, entry);
    TAILQ_INSERT_TAIL(&host->ctxr_uh, ctxr, entry);
    pthread_spin_unlock (&host->ctxr_spin);

    memset (ctxr->cmd_status, 0x0,
                        NVMEH_MAX_CMD_BATCH * sizeof (struct nvmeh_cmd_status));
    ctxr->completed = 0;
    ctxr->failed = 0;
    ctxr->host = host;

    return ctxr;
}

void nvmeh_ctxr_put (struct nvme_host *host, struct nvmeh_ctx *ctxr)
{
    pthread_spin_lock (&host->ctxr_spin);
    TAILQ_REMOVE(&host->ctxr_uh, ctxr, entry);
    TAILQ_INSERT_TAIL(&host->ctxr_fh, ctxr, entry);
    pthread_spin_unlock (&host->ctxr_spin);
}

struct nvmeh_ctx *nvmeh_ctxw_get (struct nvme_host *host)
{
    struct nvmeh_ctx *ctxw;
    uint16_t retry = NVMEH_RETRY;

RETRY:
    pthread_spin_lock (&host->ctxw_spin);
    ctxw = TAILQ_FIRST(&host->ctxw_fh);
    if (!ctxw) {
        pthread_spin_unlock (&host->ctxw_spin);
        usleep (NVMEH_RETRY_DELAY);
        retry--;
        if (retry)
            goto RETRY;
        else
            return NULL;
    }
    TAILQ_REMOVE(&host->ctxw_fh, ctxw, entry);
    TAILQ_INSERT_TAIL(&host->ctxw_uh, ctxw, entry);
    pthread_spin_unlock (&host->ctxw_spin);

    memset (ctxw->cmd_status, 0x0,
                        NVMEH_MAX_CMD_BATCH * sizeof (struct nvmeh_cmd_status));
    ctxw->completed = 0;
    ctxw->failed = 0;
    ctxw->host = host;

    return ctxw;
}

void nvmeh_ctxw_put (struct nvme_host *host, struct nvmeh_ctx *ctxw)
{
    pthread_spin_lock (&host->ctxw_spin);
    TAILQ_REMOVE(&host->ctxw_uh, ctxw, entry);
    TAILQ_INSERT_TAIL(&host->ctxw_fh, ctxw, entry);
    pthread_spin_unlock (&host->ctxw_spin);
}

int nvmeh_init_ctx_write (struct nvme_host *host, uint32_t entries)
{
    uint32_t ent_i;

    host->ctxw_ent = calloc (entries, sizeof (struct nvmeh_ctx));
    if (!host->ctxw_ent)
        return -1;

    if (pthread_spin_init (&host->ctxw_spin, 0))
        goto FREE;

    TAILQ_INIT(&host->ctxw_fh);
    TAILQ_INIT(&host->ctxw_uh);

    for (ent_i = 0; ent_i < entries; ent_i++) {
        if (pthread_spin_init (&host->ctxw_ent[ent_i].spin, 0))
            goto SPIN;

        host->ctxw_ent[ent_i].ctx_id = ent_i;
        host->ctxw_ent[ent_i].is_write = 1;
        TAILQ_INSERT_TAIL(&host->ctxw_fh, &host->ctxw_ent[ent_i], entry);
    }

    host->ctxw_entries = entries;

    return 0;

SPIN:
    while (ent_i) {
        ent_i--;
        pthread_spin_destroy (&host->ctxw_ent[ent_i].spin);
    }
    pthread_spin_destroy (&host->ctxw_spin);
FREE:
    free (host->ctxw_ent);
    return -1;
}

int nvmeh_init_ctx_read (struct nvme_host *host, uint32_t entries)
{
    uint32_t ent_i;

    host->ctxr_ent = calloc (entries, sizeof (struct nvmeh_ctx));
    if (!host->ctxr_ent)
        return -1;

    if (pthread_spin_init (&host->ctxr_spin, 0))
        goto FREE;

    TAILQ_INIT(&host->ctxr_fh);
    TAILQ_INIT(&host->ctxr_uh);

    for (ent_i = 0; ent_i < entries; ent_i++) {
        if (pthread_spin_init (&host->ctxr_ent[ent_i].spin, 0))
            goto SPIN;

        host->ctxr_ent[ent_i].ctx_id = ent_i;
        host->ctxr_ent[ent_i].is_write = 0;
        TAILQ_INSERT_TAIL(&host->ctxr_fh, &host->ctxr_ent[ent_i], entry);
    }

    host->ctxr_entries = entries;

    return 0;

SPIN:
    while (ent_i) {
        ent_i--;
        pthread_spin_destroy (&host->ctxr_ent[ent_i].spin);
    }
    pthread_spin_destroy (&host->ctxr_spin);
FREE:
    free (host->ctxr_ent);
    return -1;
}

void nvmeh_exit_ctx_write (struct nvme_host *host)
{
    uint32_t ent_i = host->ctxw_entries;

    while (ent_i) {
        ent_i--;
        pthread_spin_destroy (&host->ctxw_ent[ent_i].spin);
    }

    pthread_spin_destroy (&host->ctxw_spin);
    free (host->ctxw_ent);
}

void nvmeh_exit_ctx_read (struct nvme_host *host)
{
    uint32_t ent_i = host->ctxr_entries;

    while (ent_i) {
        ent_i--;
        pthread_spin_destroy (&host->ctxr_ent[ent_i].spin);
    }

    pthread_spin_destroy (&host->ctxr_spin);
    free (host->ctxr_ent);
}

static int nvmeh_rw (uint8_t *buf, uint64_t size, uint64_t slba,
                        uint8_t is_write, oxf_host_callback_fn *cb, void *ctx)
{
    struct nvme_cmd_rw *cmd;
    struct nvme_sgl_desc *desc;
    struct nvme_cqe cqe;
    struct nvmeh_ctx *nvmeh_ctx;
    uint32_t nblk, blk_per_cmd, n_cmd, cmd_i;
    uint16_t n_queues;
    uint8_t *buf_off[1];
    uint32_t buf_sz[1];

    /* For now, we limit the alignment to 4KB */
    if (size % OXF_BLK_SIZE != 0) {
        printf ("[nvme: Buffer is not aligned. Alignment: %d bytes.]\n",
                                                                  OXF_BLK_SIZE);
        return -1;
    }

    if (!buf || !size) {
        printf ("[nvme: Buffer is empty.]\n");
        return -1;
    }

    n_queues = oxf_host_queue_count();

    nblk = size / OXF_BLK_SIZE;
    blk_per_cmd = OXF_SQC_MAX_DATA / OXF_BLK_SIZE;

    n_cmd = nblk / blk_per_cmd;
    n_cmd += (nblk % blk_per_cmd != 0) ? 1 : 0;

    if (n_cmd > NVMEH_MAX_CMD_BATCH) {
        printf ("[nvme: Buffer is too big. Max of %d bytes.]\n",
                            blk_per_cmd * OXF_BLK_SIZE * NVMEH_MAX_CMD_BATCH);
        return -1;
    }

    cmd = calloc (n_cmd, sizeof (struct nvme_cmd));
    if (!cmd)
        return -1;

    buf_off[0] = (uint8_t *) buf;
    buf_sz[0]  = blk_per_cmd * OXF_BLK_SIZE;

    nvmeh_ctx = nvmeh_ctxw_get (&nvmeh);
    if (!nvmeh_ctx)
        goto FULL;

    nvmeh_ctx->user_ctx = ctx;
    nvmeh_ctx->user_cb = cb;
    nvmeh_ctx->n_cmd = n_cmd;

    for (cmd_i = 0; cmd_i < n_cmd; cmd_i++) {

        if (buf_off[0] + buf_sz[0] > (uint8_t *) buf + size)
            buf_sz[0] = (uint8_t *) buf + size - buf_off[0];

        desc = oxf_host_alloc_sgl (buf_off, buf_sz, 1);
        if (!desc)
            goto REQUEUE;

        nvmeh_ctx->cmd_status[cmd_i].ctx = nvmeh_ctx;
        nvmeh_ctx->cmd_status[cmd_i].status = 0;

        cmd[cmd_i].slba = slba + (cmd_i * blk_per_cmd);
        cmd[cmd_i].nlb = buf_sz[0] / OXF_BLK_SIZE - 1;
        cmd[cmd_i].opcode = (is_write) ? NVME_CMD_WRITE : NVME_CMD_READ;

        if (oxf_host_submit_io ((cmd_i % (n_queues - 1)) + 1,
                            (struct nvme_cmd *) &cmd[cmd_i], desc, 1,
                            nvmeh_callback, &nvmeh_ctx->cmd_status[cmd_i])) {
            oxf_host_free_sgl (desc);
            goto REQUEUE;
        }

        buf_off[0] += buf_sz[0];

        oxf_host_free_sgl (desc);
    }

    return 0;

REQUEUE:
    for (cmd_i = cmd_i; cmd_i < n_cmd; cmd_i++) {
        cqe.status = NVME_NOT_SUBMITTED;
        nvmeh_ctx->cmd_status[cmd_i].ctx = nvmeh_ctx;
        nvmeh_ctx->cmd_status[cmd_i].status = 0;
        nvmeh_callback ((void *) &nvmeh_ctx->cmd_status[cmd_i], &cqe);
    } 

FULL:
    free (cmd);
    return -1;
}

int nvmeh_read (uint8_t *buf, uint64_t size, uint64_t slba,
                                           oxf_host_callback_fn *cb, void *ctx)
{
    return nvmeh_rw (buf, size, slba, 0, cb, ctx);
}

int nvmeh_write (uint8_t *buf, uint64_t size, uint64_t slba,
                                           oxf_host_callback_fn *cb, void *ctx)
{
    return nvmeh_rw (buf, size, slba, 1, cb, ctx);
}

void nvmeh_exit (void)
{
    oxf_host_exit ();
    pthread_spin_destroy (&nvmeh.cmdid_spin);
    nvmeh_exit_ctx_read (&nvmeh);
    nvmeh_exit_ctx_write (&nvmeh);
}

int nvmeh_init (void)
{
    if (nvmeh_init_ctx_write (&nvmeh, OXF_QUEUE_SIZE))
        return -1;

    if (nvmeh_init_ctx_read (&nvmeh, OXF_QUEUE_SIZE))
        goto EXIT_CTX;

    nvmeh.cmdid = 0;
    if (pthread_spin_init (&nvmeh.cmdid_spin, 0))
        goto EXIT_CTXR;

    if (oxf_host_init ())
        goto SPIN;

    return 0;

SPIN:
    pthread_spin_destroy (&nvmeh.cmdid_spin);
EXIT_CTXR:
    nvmeh_exit_ctx_read (&nvmeh);
EXIT_CTX:
    nvmeh_exit_ctx_write (&nvmeh);
    return -1;
}