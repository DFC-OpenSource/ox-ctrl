/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX NVMe over Fabrics (client side) 
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

#include <stdio.h>
#include <sys/queue.h>
#include <string.h>
#include <ox-fabrics.h>
#include <ox-mq.h>
#include <nvme.h>
#include <nvmef.h>

#define OXF_MAX_QUEUES  64
#define OXF_HOST_DEBUG  0

struct oxf_queue_cmd {
    uint32_t                    qid;
    uint32_t                    cid;
    uint8_t                     is_write;
    oxf_callback_fn            *cb_fn;
    void                       *ctx;
    struct ox_mq_entry         *mq_req;
    struct oxf_capsule_sq       capsule;
    TAILQ_ENTRY(oxf_queue_cmd)  entry;
};

struct oxf_queue_pair {
    struct ox_mq                       *mq;
    struct oxf_queue_cmd               *cmds;
    TAILQ_HEAD(cmd_free, oxf_queue_cmd) cmd_fh;
    TAILQ_HEAD(cmd_used, oxf_queue_cmd) cmd_uh;
    pthread_spinlock_t                  cmd_spin;
    uint8_t                             in_use;
};

struct oxf_host_fabrics {
    uint8_t                 running;
    struct oxf_client      *client;
    struct oxf_queue_pair   queues[OXF_MAX_QUEUES];
    uint32_t                n_queues;
    struct oxf_con_addr     net_iface;
};

static struct oxf_host_fabrics fabrics;

static int oxf_get_desc_length (NvmeSGLDesc *desc)
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

static void oxf_host_rcv_fn (uint32_t size, void *arg)
{
    struct oxf_capsule_cq *capsule = (struct oxf_capsule_cq *) arg;
    struct oxf_queue_cmd *qcmd;
    struct NvmeSGLDesc *desc;
    uint32_t sqid = capsule->cqc.cqe.sq_id;
    uint32_t cid = capsule->cqc.cqe.cid;
    uint32_t length, desc_i = 0;
    uint8_t *offset;

    if (OXF_HOST_DEBUG)
        printf (" [fabrics: Received capsule: %d bytes]\n", size);

    switch (capsule->type) {
        case OXF_CQE_BYTE:

            if ( (size < OXF_FAB_CQE_SZ) || (size > OXF_FAB_CAPS_SZ) ) {
                log_err ("[ox-fabrics: Invalid capsule size: %d bytes.]\n", size);
                return;
            }
            qcmd = &fabrics.queues[sqid].cmds[cid % OXF_QUEUE_SIZE];

            memcpy (&qcmd->capsule.cqc, &capsule->cqc, size);

            /* Reads: Copy data directly to the user */
            if (!qcmd->is_write) {
                desc = (NvmeSGLDesc *) qcmd->capsule.sqc.sgl;
                offset = qcmd->capsule.cqc.data;

                while (oxf_get_desc_length (&desc[desc_i])) {

                    if (desc[desc_i].type == NVME_SGL_BIT_BUCKET)
                        goto NEXT;

                    if ( (desc[desc_i].type != NVME_SGL_DATA_BLOCK) &&
                         (desc[desc_i].type != NVME_SGL_KEYED_DATA) &&
                         (desc[desc_i].type != NVME_SGL_5BYTE_KEYS)) {
                        log_err ("[ox-fabrics: Next Segment descriptors "
                                                            "NOT supported.]");
                        return;
                    }

                    length = oxf_get_desc_length (&desc[desc_i]);
                    memcpy ((void *) desc[desc_i].data.addr, offset, length);
                    offset += length;

NEXT:
                    desc_i++;

                    /* Check SGL capsule boundary */
                    if (desc_i * sizeof (struct NvmeSGLDesc) >= NVMEF_SGL_SZ)
                        break;
                }
            }

            ox_mq_complete_req (fabrics.queues[sqid].mq, qcmd->mq_req);

            break;

        default:
            printf ("[ox-fabrics: Unknown capsule type: %d]\n", capsule->type);
    }
}

/* This function returns the total bytes to be transferred in the capsule */
static uint32_t oxf_host_prepare_sq_capsule (struct nvmef_capsule_sq *capsule,
        struct nvme_cmd *ncmd, struct nvme_sgl_desc *desc, uint16_t sgl_size,
                                                            uint8_t is_write)
{
    uint16_t desc_i;
    uint32_t offset = 0;
    uint32_t bytes = (!sgl_size || !desc) ? OXF_FAB_CMD_SZ :
                                               OXF_FAB_CMD_SZ + NVMEF_SGL_SZ;

    memcpy (&capsule->cmd, ncmd, sizeof (struct nvme_cmd));
    memset (capsule->sgl, 0x0, NVMEF_SGL_SZ);

    if (!sgl_size || !desc)
        return bytes;

    memcpy (capsule->sgl, desc, sgl_size * sizeof (NvmeSGLDesc));

    for (desc_i = 0; desc_i < sgl_size; desc_i++) {
        if ( is_write && (desc[desc_i].type == NVME_SGL_DATA_BLOCK) ) {

            bytes += desc[desc_i].data.length;

            if (bytes > OXF_FAB_CAPS_SZ)
                return bytes;

            memcpy (&capsule->data[offset], (void *) desc[desc_i].data.addr,
                                                      desc[desc_i].data.length);
            offset += desc[desc_i].data.length;

        } else if ( is_write && (desc[desc_i].type == NVME_SGL_KEYED_DATA) ) {
            /* Not supported for writes yet */
        } else if ( is_write && (desc[desc_i].type == NVME_SGL_5BYTE_KEYS) ) {
            /* Not supported for writes yet */
        }
    }

    return bytes;
}

static void oxf_fabrics_set_direction (struct nvme_cmd *cmd,
                                                    struct oxf_queue_cmd *qcmd)
{
    switch (cmd->opcode) {
        case NVME_CMD_WRITE:
        case NVME_CMD_WRITE_NULL:
        case NVME_CMD_ELEOS_FLUSH:
            qcmd->is_write = 1;
            break;
        case NVME_CMD_READ:
        case NVME_CMD_READ_NULL:
        case NVME_CMD_ELEOS_READ:
        default:
            qcmd->is_write = 0;
    }

    if (cmd->psdt != CMD_PSDT_SGL && cmd->psdt != CMD_PSDT_SGL_MD)
        log_err ("[ox-fabrics: WARNING: Command does not contain an SGL.]");
}

/* Maximum data transfer per I/O is 'OXF_SQC_MAX_DATA' bytes */
int oxf_host_submit_io (uint16_t qid, struct nvme_cmd *ncmd,
                                struct nvme_sgl_desc *desc, uint16_t sgl_size,
                                oxf_callback_fn *cb, void *ctx)
{
    struct oxf_queue_cmd *qcmd;
    uint32_t retry = OXF_RETRY;
    uint32_t bytes;

    if (qid >= OXF_MAX_QUEUES || !fabrics.queues[qid].in_use) {
        printf ("[ox-fabrics (submit): Invalid QID %d]\n", qid);
        return -1;
    }

    if (desc && (sgl_size * sizeof (NvmeSGLDesc) > NVMEF_SGL_SZ)) {
        printf ("[ox-fabrics (submit): SGL exceed size limit (%lu/%d)]\n",
                              sgl_size * sizeof (NvmeSGLDesc), NVMEF_SGL_SZ);
        return -1;
    }

    while (retry) {
        pthread_spin_lock (&fabrics.queues[qid].cmd_spin);
        qcmd = TAILQ_FIRST(&fabrics.queues[qid].cmd_fh);
        if (!qcmd) {
            pthread_spin_unlock (&fabrics.queues[qid].cmd_spin);
            usleep (OXF_RETRY_DELAY);
            retry--;
            continue;
        }
        TAILQ_REMOVE(&fabrics.queues[qid].cmd_fh, qcmd, entry);
        TAILQ_INSERT_TAIL(&fabrics.queues[qid].cmd_uh, qcmd, entry);
        pthread_spin_unlock (&fabrics.queues[qid].cmd_spin);
        break;
    }
    if (!retry) {
        printf ("[ox-fabrics (submit-1): Command not submitted. Queue %d]\n", qid);
        return -1;
    }

    ncmd->cid  = qcmd->cid;
    ncmd->psdt = CMD_PSDT_SGL;
    ncmd->nsid = 1;

    /* Discard in-command descriptor, the entire SGL is right after command */
    ncmd->sgl.type = NVME_SGL_BIT_BUCKET;
    ncmd->sgl.data.length = 0;

    oxf_fabrics_set_direction (ncmd, qcmd);

    bytes = oxf_host_prepare_sq_capsule (&qcmd->capsule.sqc, ncmd, desc,
                                                      sgl_size, qcmd->is_write);
    if (bytes > OXF_FAB_CAPS_SZ) {
        printf ("[ox-fabrics (submit): Max capsule size exceeded: "
                                       "(%d/%d)]\n", bytes, OXF_FAB_CAPS_SZ);
        goto REQUEUE;
    }

    memset (&qcmd->capsule.cqc.cqe, 0x0, sizeof (struct nvme_cqe));
    qcmd->capsule.sqc.cmd.cid = qcmd->cid;
    qcmd->capsule.cqc.cqe.cid = qcmd->cid;
    qcmd->capsule.cqc.cqe.sq_id = qid;
    qcmd->capsule.type = OXF_CMD_BYTE;
    qcmd->capsule.size = bytes;
    qcmd->ctx = ctx;
    qcmd->cb_fn = cb;
    qcmd->mq_req = NULL;

    if (ox_mq_submit_req (fabrics.queues[qid].mq, 0, qcmd)) {
        printf ("[ox-fabrics (submit-2): Command not submitted. Queue %d]\n", qid);
        goto REQUEUE;
    }

    return 0;

REQUEUE:
    pthread_spin_lock (&fabrics.queues[qid].cmd_spin);
    TAILQ_REMOVE(&fabrics.queues[qid].cmd_uh, qcmd, entry);
    TAILQ_INSERT_TAIL(&fabrics.queues[qid].cmd_fh, qcmd, entry);
    pthread_spin_unlock (&fabrics.queues[qid].cmd_spin);
    return -1;
}

int oxf_host_submit_admin (struct nvme_cmd *ncmd, oxf_callback_fn *cb, void *ctx)
{
    return oxf_host_submit_io (0, ncmd, NULL, 0, cb, ctx);
}

struct nvme_sgl_desc *oxf_host_alloc_sgl (uint8_t **buf_list, uint32_t *buf_sz,
                                                              uint16_t entries)
{
    uint16_t ent_i;

    struct nvme_sgl_desc *desc = calloc (entries, sizeof(struct nvme_sgl_desc));
    if (!desc)
        return NULL;

    for (ent_i = 0; ent_i < entries; ent_i++) {
        desc[ent_i].type        = NVME_SGL_DATA_BLOCK;
        desc[ent_i].subtype     = NVME_SGL_SUB_ADDR;
        desc[ent_i].data.addr   = (uint64_t) buf_list[ent_i];
        desc[ent_i].data.length = buf_sz[ent_i];
    }

    return desc;
}

struct nvme_sgl_desc *oxf_host_alloc_keyed_sgl (uint8_t **buf_list,
                            uint32_t *buf_sz, uint64_t *keys, uint16_t entries)
{
    uint16_t ent_i;

    struct nvme_sgl_desc *desc = calloc (entries, sizeof(struct nvme_sgl_desc));
    if (!desc)
        return NULL;

    for (ent_i = 0; ent_i < entries; ent_i++) {
        if (buf_sz[ent_i] > (1 << 16)) {
            printf ("[ox-fabrics: Keyed SGL descriptor is limited to 64 KB.]\n");
            return NULL;
        }
        if (keys[ent_i] > ((uint64_t) 1 << 40)) {
            printf ("[ox-fabrics: Keyed SGL key limited to %lu.]\n",
                                                         ((uint64_t) 1 << 40));
            return NULL;
        }
        desc[ent_i].type         = NVME_SGL_5BYTE_KEYS;
        desc[ent_i].subtype      = NVME_SGL_SUB_ADDR;
        desc[ent_i].keyed5.addr   = (uint64_t) buf_list[ent_i];
        desc[ent_i].keyed5.length = buf_sz[ent_i];
        memcpy (desc[ent_i].keyed5.key, &keys[ent_i], 5);
    }

    return desc;
}

void oxf_host_free_sgl (struct nvme_sgl_desc *desc)
{
    free (desc);
}

static void oxf_host_process_sq (struct ox_mq_entry *req)
{
    struct oxf_queue_cmd *qcmd;
    struct oxf_client_con *con;
    uint32_t retry = OXF_RETRY;

    qcmd = (struct oxf_queue_cmd *) req->opaque;
    qcmd->mq_req = req;

    con = fabrics.client->connections[qcmd->qid];

    while (retry) {
        if (fabrics.client->ops->send (con, qcmd->capsule.size,
                                               (const void *) &qcmd->capsule)){
            retry--;
            usleep (OXF_RETRY_DELAY);
            continue;
        }
        break;
    }
    if (!retry) {
        printf ("[ox-fabrics (sq): Aborted command in SQ.]\n");
        qcmd = (struct oxf_queue_cmd *) req->opaque;
        qcmd->capsule.cqc.cqe.status = NVME_NOT_SUBMITTED;
        if (ox_mq_complete_req (fabrics.queues[req->qid].mq, req))
            printf ("[ox-fabrics (sq): Aborted command not completed]\n");
    }
}

static void oxf_host_process_cq (void *opaque)
{
    struct oxf_queue_cmd *qcmd =  (struct oxf_queue_cmd *) opaque;
    uint32_t qid = qcmd->qid;

    qcmd->cb_fn (qcmd->ctx, &qcmd->capsule.cqc.cqe);

    pthread_spin_lock (&fabrics.queues[qcmd->qid].cmd_spin);
    TAILQ_REMOVE(&fabrics.queues[qid].cmd_uh, qcmd, entry);
    TAILQ_INSERT_TAIL(&fabrics.queues[qid].cmd_fh, qcmd, entry);
    pthread_spin_unlock (&fabrics.queues[qcmd->qid].cmd_spin);
}

static void oxf_host_process_to (void **opaque, int counter)
{
    struct oxf_queue_cmd *qcmd;

    while (counter) {
        counter--;
        qcmd = (struct oxf_queue_cmd *) opaque[counter];
        qcmd->capsule.cqc.cqe.status = NVME_HOST_TIMEOUT;
    }
}

static void oxf_sync_callback (void *ctx, struct nvme_cqe *cqe)
{
    uint16_t *status = (uint16_t *) ctx;

    *status = cqe->status;
}

static int oxf_submit_sync_cmd (uint16_t qid, struct nvme_cmd *ncmd,
                                struct nvme_sgl_desc *desc, uint16_t sgl_size)
{
    uint16_t status = 0xff, time = 0;
    uint32_t timeout = 5 * 1000; /* 5 sec */

    if (oxf_host_submit_io (qid, ncmd, desc, sgl_size,
                                                    oxf_sync_callback, &status))
        return -1;

    do {
        usleep (1000);
        time++;
    } while ( (status == 0xff) && (time < timeout) );

    if (time >= timeout)
        return -1;

    return status;
}

static int oxf_host_send_connect (uint16_t qid)
{
    NvmefConnect             cmd;
    NvmefConnectData         data;
    struct nvme_sgl_desc    *desc;

    desc = (struct nvme_sgl_desc *) cmd.sgl1;
    desc->type           = NVME_SGL_DATA_BLOCK;
    desc->subtype        = NVME_SGL_SUB_OFFSET;

    /* Here we send In Capsule data, however, the Spec does not support it in 
        the Admin queue */
    desc->data.addr      = 0;
    desc->data.length    = sizeof (NvmefConnectData);

    memset (&data, 0x0, sizeof (NvmefConnectData));

    /* Controller ID, to be proper defined */
    data.cntlid = 0x3c3c;

    /* Host ID, to be user defined */
    memcpy (data.hostid, "--- OX-HOST ---\0", 16);

    /* Subsystem qualified name, to be proper defined */
    memcpy (data.subnqn, "--- OX-CTRL ---\0", 16);

    /* Host qualified name, to be proper defined */
    memcpy (data.hostnqn, "--- OX-HOST ---\0", 16);

    memset (&cmd, 0x0, sizeof (NvmefConnect));
    cmd.opcode = NVME_ADM_CMD_FABRICS;
    cmd.fctype = NVMEF_CMD_CONNECT;
    cmd.qid = qid;
    cmd.sqsize = OXF_QUEUE_SIZE;

    /* Submit command to the admin queue */
    return oxf_submit_sync_cmd (0, (struct nvme_cmd *) &cmd, desc, 1);
}

int oxf_host_create_queue (uint16_t qid)
{
    uint32_t ent_i, iface_id;
    struct ox_mq_config mq_config;

    if (qid >= OXF_MAX_QUEUES) {
        printf ("[ox-fabrics (create): Invalid QID (%d), maximum of %d\n]",
                                                          qid, OXF_MAX_QUEUES);
        return -1;
    }
    if (fabrics.queues[qid].in_use) {
        printf ("[ox-fabrics (create): Queue %d already initialized]\n", qid);
        return 0;
    }
    if (!fabrics.client->n_ifaces) {
        printf ("[ox-fabrics (create): No server interface has been added.]\n");
        return -1;
    }

    sprintf(mq_config.name, "%s-%d", "NVME_QUEUE", qid);
    mq_config.n_queues = 1;
    mq_config.q_size = OXF_QUEUE_SIZE;
    mq_config.sq_fn = oxf_host_process_sq;
    mq_config.cq_fn = oxf_host_process_cq;
    mq_config.to_fn = oxf_host_process_to;
    mq_config.to_usec = OXF_QUEUE_TO;
    mq_config.flags = (OX_MQ_TO_COMPLETE | OX_MQ_CPU_AFFINITY);

    /* Set completion thread affinity to a single core, if enabled */
    mq_config.sq_affinity[0] = 0;
    mq_config.cq_affinity[0] = 0;

#if ELEOS_HOST_CQ_CORE0
    mq_config.cq_affinity[0] |= ((uint64_t) 1 << 0);
#endif /* ELEOS_HOST_CQ_CORE0 */

    fabrics.queues[qid].mq = ox_mq_init(&mq_config);
    if (!fabrics.queues[qid].mq)
        return -1;

    fabrics.queues[qid].cmds = calloc (OXF_QUEUE_SIZE,
                                                 sizeof (struct oxf_queue_cmd));

    if (!fabrics.queues[qid].cmds)
        goto DESTROY_MQ;

    if (pthread_spin_init (&fabrics.queues[qid].cmd_spin, 0))
        goto FREE_CMD;

    /* For I/O queues, submit the connect command to admin queue */
    if (qid) {
        if (oxf_host_send_connect (qid))
            goto DESTROY_SPIN;
    }

    iface_id = qid % fabrics.client->n_ifaces;

    if (!fabrics.client->ops->connect(fabrics.client, qid,
                    fabrics.client->ifaces[iface_id].addr,
                    fabrics.client->ifaces[iface_id].port, oxf_host_rcv_fn))
        goto DESTROY_SPIN;

    /* Server needs some time to complete the connection */
    usleep (50000);

    TAILQ_INIT(&fabrics.queues[qid].cmd_fh);
    TAILQ_INIT(&fabrics.queues[qid].cmd_uh);
    for (ent_i = 0; ent_i < OXF_QUEUE_SIZE; ent_i++) {
        fabrics.queues[qid].cmds[ent_i].qid = qid;
        fabrics.queues[qid].cmds[ent_i].cid = (qid * OXF_QUEUE_SIZE) + ent_i;
        TAILQ_INSERT_TAIL(&fabrics.queues[qid].cmd_fh,
                                       &fabrics.queues[qid].cmds[ent_i], entry);
    }

    fabrics.n_queues++;
    fabrics.queues[qid].in_use = 1;

    if (OXF_HOST_DEBUG)
        printf ("[ox-fabrics: Queue %d created. Connected to %s:%d]\n", qid,
                fabrics.client->ifaces[iface_id].addr,
                fabrics.client->ifaces[iface_id].port);

    return 0;

DESTROY_SPIN:
    pthread_spin_destroy (&fabrics.queues[qid].cmd_spin);
FREE_CMD:
    free (fabrics.queues[qid].cmds);
DESTROY_MQ:
    ox_mq_destroy (fabrics.queues[qid].mq);
    return -1;
}

void oxf_host_destroy_queue (uint16_t qid)
{
    struct oxf_client_con *con;

    if ((qid >= OXF_MAX_QUEUES) || !fabrics.queues[qid].in_use)
        return;

    con = fabrics.client->connections[qid];
    fabrics.client->ops->disconnect (con);

    fabrics.queues[qid].in_use = 0;
    fabrics.n_queues--;

    TAILQ_INIT(&fabrics.queues[qid].cmd_fh);
    TAILQ_INIT(&fabrics.queues[qid].cmd_uh);

    pthread_spin_destroy (&fabrics.queues[qid].cmd_spin);    
    free (fabrics.queues[qid].cmds);
    ox_mq_destroy (fabrics.queues[qid].mq);
}

int oxf_host_add_server_iface (const char *addr, uint16_t port)
{
    if (!fabrics.running) {
        printf ("[ox-fabrics: Call 'oxf_host_init' before adding "
                                                        "server interfaces.]");
        return -1;
    }

    memcpy (fabrics.client->ifaces[fabrics.client->n_ifaces].addr,
                                                          addr, strlen (addr));
    fabrics.client->ifaces[fabrics.client->n_ifaces].port = port;
    fabrics.client->n_ifaces++;

    return 0;
}

uint16_t oxf_host_queue_count (void)
{
    return fabrics.n_queues;
}

int oxf_host_init (void)
{
    if (ox_mem_init ())
        return -1;

    if (!ox_mem_create_type ("OX_MQ", OX_MEM_OX_MQ))
        goto EXIT;

    fabrics.client = (OXF_PROTOCOL == OXF_UDP) ? oxf_udp_client_init () :
                                                 oxf_tcp_client_init ();
    if (!fabrics.client)
        goto EXIT;

    fabrics.client->n_ifaces = 0;
    fabrics.running = 0;

    memset (&fabrics.queues,0x0,OXF_MAX_QUEUES * sizeof(struct oxf_queue_pair));
    fabrics.n_queues = 0;
    fabrics.running = 1;

    return 0;

EXIT:
    ox_mem_exit();
    return -1;
}

void oxf_host_exit (void)
{
    uint32_t qid = OXF_MAX_QUEUES;

    if (fabrics.running) {
        while (qid) {
            qid--;
            oxf_host_destroy_queue (qid);
        }

        (OXF_PROTOCOL == OXF_UDP) ? oxf_udp_client_exit (fabrics.client) :
                                    oxf_tcp_client_exit (fabrics.client);
    }

    ox_mem_exit();
}