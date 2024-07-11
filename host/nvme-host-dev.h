/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - NVMe Host Developer Header
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

#include <ox-fabrics.h>

/* Max of NVMe commands per user I/O (each NVMe command is up to 60K in size) */
#define NVMEH_MAX_CMD_BATCH     260 /* 60K * 260 = 15600K */

/* 10 seconds timeout */
#define NVMEH_RETRY         50000
#define NVMEH_RETRY_DELAY   200

struct nvmeh_ctx;
struct nvmeh_cmd_status {
    uint16_t           status;
    struct nvmeh_ctx  *ctx;
};

struct nvme_host;
struct nvmeh_ctx {
    uint64_t                ctx_id;
    void                    *user_ctx;
    oxf_host_callback_fn    *user_cb;
    struct nvmeh_cmd_status cmd_status[NVMEH_MAX_CMD_BATCH];
    uint32_t                n_cmd;
    uint32_t                completed;
    uint32_t                failed;
    pthread_spinlock_t      spin;
    TAILQ_ENTRY(nvmeh_ctx)  entry;
    struct nvme_host        *host;
    uint8_t                 is_write;
};

struct nvme_cmd_rw {
    uint8_t     opcode;		/* Dword  0 	*/
    uint8_t     fuse : 2;
    uint8_t     rsvd : 4;
    uint8_t     psdt : 2;
    uint16_t    cid;
    uint32_t    nsid;		/* Dword  1 	*/
    uint64_t    rsvd2;		/* Dword  2,  3	*/
    uint64_t    mptr;		/* Dword  4,  5	*/
    uint64_t    prp1;		/* Dword  6,  7	*/
    uint64_t    prp2;		/* Dword  8,  9	*/
    uint64_t    slba;		/* Dword 10, 11	*/
    uint16_t    nlb;		/* Dword 12 	*/
    uint16_t    control;
    uint32_t    dsmgmt;		/* Dword 13 	*/
    uint32_t    reftag;		/* Dword 14	*/
    uint16_t    apptag;		/* Dword 15	*/
    uint16_t    appmask;
};

struct nvme_cmd_identify {
    uint8_t     opcode;		/* Dword  0 	*/
    uint8_t     fuse : 2;
    uint8_t     rsvd : 4;
    uint8_t     psdt : 2;
    uint16_t    cid;
    uint32_t    nsid;		/* Dword  1 	*/
    uint64_t    rsvd2;		/* Dword  2,  3	*/
    uint64_t    mptr;		/* Dword  4,  5	*/
    uint64_t    prp1;		/* Dword  6,  7	*/
    uint64_t    prp2;		/* Dword  8,  9	*/
    uint8_t	cns;		/* Dword 10	*/
    uint8_t	rsvd3;
    uint16_t	cntid;
    uint16_t	nvmsetid;	/* Dword 11	*/
    uint16_t	rsvd4;
    uint32_t    rsvd5;		/* Dword 12 	*/
    uint32_t    rsvd6;		/* Dword 13 	*/
    uint32_t    uuid;		/* Dword 14	*/
    uint32_t    rsvd8;		/* Dword 15	*/
};

struct nvme_host {
    /* Write contexts */
    struct nvmeh_ctx                 *ctxw_ent;    
    TAILQ_HEAD(ctxw_free, nvmeh_ctx)  ctxw_fh;
    TAILQ_HEAD(ctxw_used, nvmeh_ctx)  ctxw_uh;
    pthread_spinlock_t                ctxw_spin;
    uint32_t                          ctxw_entries;

    /* Read contexts */
    struct nvmeh_ctx                 *ctxr_ent;
    TAILQ_HEAD(ctxr_free, nvmeh_ctx)  ctxr_fh;
    TAILQ_HEAD(ctxr_used, nvmeh_ctx)  ctxr_uh;
    pthread_spinlock_t                ctxr_spin;
    uint32_t                          ctxr_entries;

    volatile uint16_t                 cmdid;
    pthread_spinlock_t                cmdid_spin;
};

int  nvmeh_init_ctx_write (struct nvme_host *host, uint32_t entries);
int  nvmeh_init_ctx_read (struct nvme_host *host, uint32_t entries);
void nvmeh_exit_ctx_write (struct nvme_host *host);
void nvmeh_exit_ctx_read (struct nvme_host *host);
void nvmeh_ctxr_put (struct nvme_host *host, struct nvmeh_ctx *ctxr);
void nvmeh_ctxw_put (struct nvme_host *host, struct nvmeh_ctx *ctxw);
struct nvmeh_ctx *nvmeh_ctxr_get (struct nvme_host *host);
struct nvmeh_ctx *nvmeh_ctxw_get (struct nvme_host *host);
uint16_t nvmeh_get_cmdid (struct nvme_host *host);
void nvmeh_callback (void *ctx, struct nvme_cqe *cqe);
