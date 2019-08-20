/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX NVMe over Fabrics (server side) 
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

#include <stdlib.h>
#include <stdio.h>
#include <sys/queue.h>
#include <string.h>
#include <libox.h>
#include <nvme.h>
#include <ox-fabrics.h>

#define OXF_MAX_ENT 4096

struct oxf_tgt_reply {
    uint8_t                     type;
    uint8_t                     cli[32];
    uint8_t                     is_write;
    uint32_t                    data_sz;
    struct oxf_server_con      *con;
    struct nvmef_capsule_sq     capsule;
    struct oxf_capsule_cq       cq_capsule;
    TAILQ_ENTRY(oxf_tgt_reply)  entry;
};

struct oxf_tgt_queue_reply {
    struct oxf_tgt_reply                 *reply_ent;
    TAILQ_HEAD(reply_free, oxf_tgt_reply) reply_fh;
    TAILQ_HEAD(reply_used, oxf_tgt_reply) reply_uh;
    pthread_spinlock_t                    reply_spin;
    uint8_t                               in_use;
};

struct oxf_tgt_fabrics {
    uint8_t                      running;
    struct oxf_server           *server;
    struct oxf_tgt_queue_reply   reply[OXF_SERVER_MAX_CON];
};

static struct oxf_tgt_fabrics fabrics;
extern struct core_struct core;
extern uint16_t pending_conn;

/* OX Fabrics uses In-capsule data for now, so RDMA is a memory copy */
int oxf_rdma (void *buf, uint32_t size, uint64_t prp, uint8_t dir)
{
    switch (dir) {
        case NVM_DMA_TO_HOST:
            memcpy ((void *) prp, buf, size);
        case NVM_DMA_FROM_HOST:
            memcpy (buf, (void *) prp, size);
    }

    return 0;
}

int oxf_complete (NvmeCqe *cqe, void *ctx)
{
    struct oxf_tgt_reply *reply = (struct oxf_tgt_reply *) ctx;
    struct oxf_capsule_cq *capsule = &reply->cq_capsule;
    struct oxf_tgt_queue_reply *q_reply;

    capsule->type = OXF_CQE_BYTE;
    capsule->size = (reply->is_write) ? OXF_FAB_CQE_SZ :
                                         OXF_FAB_CQE_SZ + reply->data_sz;
    memcpy (&capsule->cqc.cqe, cqe, sizeof (struct nvme_cqe));

    /* Read: data has been copied to cq_capsule via "DMA" by bottom layers */

    if (fabrics.server->ops->reply (reply->con, capsule,
                                capsule->size, (void *) reply->cli))
        return -1;

    q_reply = &fabrics.reply[cqe->sq_id];
    pthread_spin_lock (&q_reply->reply_spin);
    TAILQ_REMOVE(&q_reply->reply_uh, reply, entry);
    TAILQ_INSERT_TAIL(&q_reply->reply_fh, reply, entry);
    pthread_spin_unlock (&q_reply->reply_spin);

    return 0;
}

int oxf_get_sgl_desc_length (NvmeSGLDesc *desc)
{
    int length = 0;

    switch (desc->type) {
        case NVME_SGL_DATA_BLOCK:
            return desc->data.length;
        case NVME_SGL_KEYED_DATA:
            memcpy (&length, desc->keyed.length, 3);
            return length;
        case NVME_SGL_5BYTE_KEYS:
            return desc->keyed5.length;
        case NVME_SGL_SEGMENT:
            return desc->data.length;
        case NVME_SGL_LAST_SEGMENT:
            return desc->data.length;
        case NVME_SGL_BIT_BUCKET:
            return desc->data.length;
        default:
            return 0;
    }
}

static uint32_t oxf_fabrics_set_sgl (struct nvmef_capsule_sq *capsule,
        struct nvmef_capsule_cq *cq_capsule, uint32_t caps_sz, uint8_t is_write)
{
    NvmeSGLDesc *desc = (NvmeSGLDesc *) capsule->sgl;
    uint16_t desc_i = 0;
    uint32_t bytes = 0;
    uint64_t offset;

    if (is_write)
        offset = (uint64_t) capsule->data;
    else
        offset = (uint64_t) cq_capsule->data;

    if (caps_sz <= OXF_NVME_CMD_SZ) {
        memset (capsule->sgl, 0x0, NVMEF_SGL_SZ);
        return 0;
    }

    while (oxf_get_sgl_desc_length (&desc[desc_i])) {

        if (desc[desc_i].type == NVME_SGL_BIT_BUCKET)
            goto NEXT;

        if (    (desc[desc_i].type != NVME_SGL_DATA_BLOCK) &&
                (desc[desc_i].type != NVME_SGL_KEYED_DATA) &&
                (desc[desc_i].type != NVME_SGL_5BYTE_KEYS)) {
            log_err ("[ox-fabrics: Next Segment descriptors NOT supported.]");
            return 0;
        }

        /* Treat SGL as memory addresses from now on */
        desc[desc_i].subtype = NVME_SGL_SUB_ADDR;
        switch (desc[desc_i].type) {
            case NVME_SGL_KEYED_DATA:
                desc[desc_i].keyed.addr = offset;
                offset += *((uint32_t *) &desc[desc_i].keyed.length);
                bytes += *((uint32_t *) &desc[desc_i].keyed.length);
                break;
            case NVME_SGL_5BYTE_KEYS:
                desc[desc_i].keyed5.addr = offset;
                offset += desc[desc_i].keyed5.length;
                bytes += desc[desc_i].keyed5.length;
                break;
            case NVME_SGL_DATA_BLOCK:
            default:
                desc[desc_i].data.addr = offset;
                offset += desc[desc_i].data.length;
                bytes += desc[desc_i].data.length;
        }        

NEXT:
        desc_i++;

        /* Check SGL capsule boundary */
        if (desc_i * sizeof (struct NvmeSGLDesc) >= NVMEF_SGL_SZ)
            break;
    }

    /* Set in-command SGL entry as Last Segment pointing to the SGL */
    capsule->cmd.sgl.type        = NVME_SGL_LAST_SEGMENT;
    capsule->cmd.sgl.subtype     = NVME_SGL_SUB_ADDR;
    capsule->cmd.sgl.data.addr   = (uint64_t) capsule->sgl;
    capsule->cmd.sgl.data.length = desc_i * sizeof (struct NvmeSGLDesc);

    return bytes;
}

static void oxf_fabrics_set_direction (struct nvme_cmd *cmd, 
                                                    struct oxf_tgt_reply *rep)
{
    switch (cmd->opcode) {
        case NVME_CMD_WRITE:
        case NVME_CMD_WRITE_NULL:
        case NVME_CMD_ELEOS_FLUSH:
            rep->is_write = 1;
            break;
        case NVME_CMD_READ:
        case NVME_CMD_READ_NULL:
        case NVME_CMD_ELEOS_READ:
        default:
            rep->is_write = 0;
    }

    if (cmd->psdt != CMD_PSDT_SGL && cmd->psdt != CMD_PSDT_SGL_MD)
        log_err ("[ox-fabrics: WARNING: Command does not contain an SGL.]");
}

static void oxf_fabrics_rcv_fn (uint32_t size, void *arg, void *recv_cli)
{
    struct oxf_capsule_sq *capsule = (struct oxf_capsule_sq *) arg;
    struct oxf_tgt_reply *reply;
    struct oxf_tgt_queue_reply *q_reply;
    uint16_t retry = OXF_RETRY, sq_id;

    if (OXF_DEBUG)
        printf ("[OX-FABRICS: Received capsule: %d bytes]\n", size);

    switch (capsule->type) {
        case OXF_CMD_BYTE:
            if ( (size < OXF_FAB_CMD_SZ) || (size > OXF_FAB_CAPS_SZ)) {
                log_err ("[ox-fabrics: Invalid capsule size: %d bytes.]\n", size);
                return;
            }

            while (retry) {

                sq_id = capsule->sqc.cmd.cid / OXF_QUEUE_SIZE;
                if (sq_id >= OXF_SERVER_MAX_CON) {
                    log_err ("[ox-fabrics (recv): Invalid SQ ID: %d]\n", sq_id);
                    return;
                }

                q_reply = &fabrics.reply[sq_id];
                pthread_spin_lock (&q_reply->reply_spin);
                reply = TAILQ_FIRST(&q_reply->reply_fh);
                if (!reply) {
                    pthread_spin_unlock (&q_reply->reply_spin);
                    usleep (OXF_RETRY_DELAY);
                    retry--;
                    continue;
                }
                TAILQ_REMOVE(&q_reply->reply_fh, reply, entry);
                TAILQ_INSERT_TAIL(&q_reply->reply_uh, reply, entry);
                pthread_spin_unlock (&q_reply->reply_spin);

                /* Client structure must be maximum of 32 bytes */
                switch (reply->type) {
                    case OXF_UDP:
                        memcpy (reply->cli, recv_cli, sizeof (struct sockaddr));
                        break;
                    case OXF_TCP:
                        default:
                        memcpy (reply->cli, recv_cli, sizeof (int));
                        break;
                }
                reply->con = fabrics.server->connections[sq_id];

                /* Copy from socket buffer to fabrics cache */
                memcpy (&reply->capsule, &capsule->sqc,
                                                      size - OXF_FAB_HEADER_SZ);

                oxf_fabrics_set_direction (&reply->capsule.cmd, reply);

                reply->data_sz = oxf_fabrics_set_sgl (&reply->capsule,
                            &reply->cq_capsule.cqc, size - OXF_FAB_HEADER_SZ,
                            reply->is_write);

                if (nvmef_process_capsule ( (NvmeCmd *) &reply->capsule.cmd,
                                                            (void *) reply )) {

                    pthread_spin_lock (&q_reply->reply_spin);
                    TAILQ_REMOVE(&q_reply->reply_uh, reply, entry);
                    TAILQ_INSERT_TAIL(&q_reply->reply_fh, reply, entry);
                    pthread_spin_unlock (&q_reply->reply_spin);

                    retry = 0;
                    break;

                }
                
                break;
            }

            if (!retry)
                log_err ("[ox-fabrics: Capsule not processed.]\n");
            
            break;
        case OXF_RDMA_BYTE:
            // RDMA not implemented
            break;
        default:
            log_err ("[ox-fabrics: Unknown capsule: %x.]\n", capsule->type);
            break;
    }
}

static int oxf_create_queue (uint16_t qid)
{
    uint32_t ent_i;
    uint16_t iface_id;
    struct oxf_tgt_queue_reply *reply;
    struct oxf_server_con *con;
    struct oxf_server *s = fabrics.server;

    if (qid >= OXF_SERVER_MAX_CON)
        return -1;

    reply = &fabrics.reply[qid];

    /* This sets a pending QID connection in the UDP/TCP layer */
    pending_conn = qid;

    if (reply->in_use)
        return 0;

    reply->reply_ent = ox_calloc (OXF_MAX_ENT, sizeof (struct oxf_tgt_reply),
                                                                OX_MEM_FABRICS);
    if (!reply->reply_ent)
        return -1;

    if (pthread_spin_init (&reply->reply_spin, 0))
        goto FREE;

    iface_id = qid % fabrics.server->n_ifaces;

    /* Do not bind if already accepting connections */
    if (!fabrics.server->connections[iface_id]) {
        con = s->ops->bind (s, qid, fabrics.server->ifaces[iface_id].addr,
                                        fabrics.server->ifaces[iface_id].port);
        if (!con)
            goto SPIN;

        if (s->ops->start (con, oxf_fabrics_rcv_fn))
            goto UNBIND;
    } else {
        con = fabrics.server->connections[iface_id];
    }

    TAILQ_INIT(&reply->reply_fh);
    TAILQ_INIT(&reply->reply_uh);
    for (ent_i = 0; ent_i < OXF_MAX_ENT; ent_i++) {
        reply->reply_ent[ent_i].type = OXF_PROTOCOL;
        TAILQ_INSERT_TAIL(&reply->reply_fh, &reply->reply_ent[ent_i], entry);
    }

    reply->in_use = 1;

    log_info ("[ox-fabrics: Queue %d started. Binding %s:%d]\n", qid,
                                             con->haddr.addr, con->haddr.port);

    return 0;

UNBIND:
    s->ops->unbind (con);
SPIN:
    pthread_spin_destroy (&reply->reply_spin);
FREE:
    ox_free (reply->reply_ent, OX_MEM_FABRICS);
    return -1;
}

static void oxf_destroy_queue (uint16_t qid)
{
    struct oxf_tgt_queue_reply *reply;
    struct oxf_server_con *con;

    if (qid >= OXF_SERVER_MAX_CON)
        return;

    reply = &fabrics.reply[qid];
    if (!reply->in_use)
        return;

    con = fabrics.server->connections[qid];
    fabrics.server->ops->stop (con);
    fabrics.server->ops->unbind (con);

    TAILQ_INIT(&reply->reply_fh);
    TAILQ_INIT(&reply->reply_uh);

    pthread_spin_destroy (&reply->reply_spin);
    ox_free (reply->reply_ent, OX_MEM_FABRICS);
    reply->reply_ent = NULL;

    reply->in_use = 0;

    log_info ("[ox-fabrics: Queue %d destroyed.]", qid);
}

static void oxf_exit (void)
{
    uint32_t cid;

    if (fabrics.running) {
        for (cid = 0; cid < OXF_SERVER_MAX_CON; cid++)
            oxf_destroy_queue (cid);

        (OXF_PROTOCOL == OXF_UDP) ? oxf_udp_server_exit (fabrics.server) :
                                    oxf_tcp_server_exit (fabrics.server);
        fabrics.running = 0;
    }

    log_info ("[ox-fabrics: Stopped successfully.]\n");
}

struct nvm_fabrics_ops ox_fabrics_ops = {
    .complete   = oxf_complete,
    .rdma       = oxf_rdma,
    .exit       = oxf_exit,
    .create     = oxf_create_queue,
    .destroy    = oxf_destroy_queue
};

struct nvm_fabrics ox_fabrics = {
    .name           = "OX_FABRICS",
    .ops            = &ox_fabrics_ops
};

int ox_fabrics_init (void)
{
    uint16_t int_i;

    if (!ox_mem_create_type ("OX_FABRICS", OX_MEM_FABRICS))
        return -1;

    fabrics.server = (OXF_PROTOCOL == OXF_UDP) ? oxf_udp_server_init () :
                                                 oxf_tcp_server_init ();
    if (!fabrics.server)
        return EMEM;

    for (int_i = 0; int_i < core.net_ifaces_count; int_i++) {
        fabrics.server->ifaces[int_i].port = core.net_ifaces[int_i].port;
        memcpy (fabrics.server->ifaces[int_i].addr, core.net_ifaces[int_i].addr,
                                           strlen(core.net_ifaces[int_i].addr));

        fabrics.server->n_ifaces = core.net_ifaces_count;
    }

    fabrics.running = 1;

    log_info ("[ox-fabrics: Started successfully.]");

    return ox_register_fabrics (&ox_fabrics);
}