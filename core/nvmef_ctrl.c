/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - NVM over Fabrics Controller
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
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <libox.h>
#include <nvme.h>
#include <nvmef.h>
#include <ox-mq.h>
#include <ox-lightnvm.h>

extern struct core_struct core;

struct nvmef_queue_pair {
    struct ox_mq                       *mq;
    NvmeRequest                        *requests;
    TAILQ_HEAD(req_free, NvmeRequest)   req_fh;
    TAILQ_HEAD(req_used, NvmeRequest)   req_uh;
    pthread_spinlock_t                  req_spin;
};

static struct nvmef_queue_pair *nvmef_queues[NVME_NUM_QUEUES];

/* SGL size is limited to 16 descriptors, 'keys' must have at least 16 slots */
int nvmef_sgl_to_prp (uint32_t nlb, NvmeSGLDesc *desc, uint64_t *prp_buf,
                                                                uint64_t *keys)
{
    uint32_t nlb_count = 0, key_count = 0, desc_sz, desc_i = 0;
    uint64_t desc_addr, offset = 0;
    uint8_t last = 0;
    NvmeSGLDesc next_desc[NVMEF_SGL_SZ / 16];

    while (nlb_count < nlb) {

        if (desc_i >= 16) {
            log_err ("[nvme: SGL out of bounds: %d of %d]", desc_i + 1, 16);
            return -1;
        }

        if (desc->subtype != NVME_SGL_SUB_ADDR) {
            log_err ("[nvme: SGL Subtype not supported: 0x%x]", desc->subtype);
            return -1;
        }

        switch (desc->type) {
            case NVME_SGL_DATA_BLOCK:
                desc_addr = desc->data.addr;
                desc_sz   = desc->data.length;
                break;
            case NVME_SGL_KEYED_DATA:
                desc_addr = desc->keyed.addr;
                desc_sz = 0;
                memcpy (&desc_sz, desc->keyed.length, 3);
                keys[key_count] = (uint64_t) desc->keyed.key;
                key_count++;
                break;
            case NVME_SGL_5BYTE_KEYS:
                desc_addr = desc->keyed5.addr;
                desc_sz   = desc->keyed5.length;
                keys[key_count] = 0;
                memcpy (&keys[key_count], desc->keyed5.key, 5);
                key_count++;
                break;
            case NVME_SGL_SEGMENT:
            case NVME_SGL_LAST_SEGMENT:

                if (last || !desc->data.length ||
                            (desc->data.length % 16 != 0)) {
                    log_err ("[nvme: Invalid SGL descriptor in Last Segment: "
                            "0x%x, length: %d]", desc->type, desc->data.length);
                    return -1;
                }
                if (desc->data.length > NVMEF_SGL_SZ) {
                    log_err ("[nvme: SGL next segment size not supported: %d. "
                            "Max: %d bytes]", desc->data.length, NVMEF_SGL_SZ);
                    return -1;
                }
                if (ox_dma (next_desc, desc->data.addr, desc->data.length,
                                                          NVM_DMA_FROM_HOST)) {
                    log_err ("[nvme: DMA error, next SGL segment not loaded]");
                    return -1;
                }
                desc = next_desc;
                desc_i = 0;
                continue;

            case NVME_SGL_BIT_BUCKET:
            default:
                log_err ("[nvme: SGL Type not supported: 0x%x]", desc->type);
                return -1;
        }

        while ( offset < desc_sz ) {
            prp_buf[nlb_count] = desc_addr + offset;
            offset += NVME_KERNEL_PG_SIZE;
            nlb_count++;
        }
        offset = 0;
        desc++;
        desc_i++;
    }

    return 0;
}

int nvmef_process_capsule (NvmeCmd *cmd, void *ctx)
{
    NvmeRequest *req;
    uint16_t sq_id, retry = NVMEF_RETRY;

    sq_id = cmd->cid / core.nvme_ctrl->max_q_ents;

    while (retry) {
        pthread_spin_lock (&nvmef_queues[sq_id]->req_spin);
        req = TAILQ_FIRST(&nvmef_queues[sq_id]->req_fh);
        if (!req) {
            pthread_spin_unlock (&nvmef_queues[sq_id]->req_spin);
            usleep (NVMEF_RETRY_DELAY);
            retry--;
            continue;
        }
        TAILQ_REMOVE(&nvmef_queues[sq_id]->req_fh, req, entry);
        TAILQ_INSERT_TAIL(&nvmef_queues[sq_id]->req_uh, req, entry);
        pthread_spin_unlock (&nvmef_queues[sq_id]->req_spin);
        break;
    }

    if (!retry) {
        log_err ("[nvmef (process capsule-1): Command not submitted. "
                                                        "Queue %d]\n", sq_id);
        return -1;
    }

    memcpy (&req->cmd, cmd, sizeof (NvmeCmd));

    memset (&req->nvm_io, 0x0, sizeof (struct nvm_io_cmd));
    memset (&req->cqe, 0x0, sizeof (NvmeCqe));
    req->cqe.cid = cmd->cid;
    req->cqe.sq_id = sq_id;

    req->ctx = ctx;
    req->status = NVME_SUCCESS;
    req->ns = &core.nvme_ctrl->namespaces[cmd->nsid - 1];

    if (ox_mq_submit_req (nvmef_queues[sq_id]->mq, 0, req)) {
        log_err ("[nvmef (process capsule-2): Command not submitted. "
                                                        "Queue %d]\n", sq_id);
        pthread_spin_lock (&nvmef_queues[sq_id]->req_spin);
        TAILQ_REMOVE(&nvmef_queues[sq_id]->req_uh, req, entry);
        TAILQ_INSERT_TAIL(&nvmef_queues[sq_id]->req_fh, req, entry);
        pthread_spin_unlock (&nvmef_queues[sq_id]->req_spin);
        return -1;
    }

    return 0;
}

static void nvmef_set_default (NvmeCtrl *n)
{
    n->num_namespaces = 1;
    n->num_queues = NVME_NUM_QUEUES;
    n->max_q_ents = NVME_MAX_QS;
    n->max_cqes = 0x4;
    n->max_sqes = 0x6;
    n->db_stride = 0;

    n->cqr = 1; /* Contiguous Queues Required */
    n->intc = 0;
    n->intc_thresh = 0;
    n->intc_time = 0;
    n->mpsmin = 0;
    n->mpsmax = 0;
    n->nlbaf = 4; /* Number of LBA Formats,   For LBA size 512B:1 4KB: 4*/
    n->lba_index = 3;                       /*For LBA size 512B:0 4KB: 3*/
    n->extended = 0;
    n->dpc = 0; /* End-to-end Data Protection Capabilities */
    n->dps = 0; /* End-to-end Data Protection Type Settings */
    n->mc = 0x2; /* Metadata Capabilities */
    n->meta = log2 (core.nvm_ch[0]->geometry->pg_oob_sz);
    n->cmb = 0; /* Controller Memory Buffer NOT IMPLEMENTED, MUST BE 0*/
    n->cmbloc = 0;
    n->cmbsz = 0;
    n->vid = PCI_VENDOR_ID_OX;
    n->did = PCI_DEVICE_ID_OX;

    if (core.std_ftl == FTL_ID_LNVM) {
        n->vid = PCI_VENDOR_ID_LNVM;
        n->did = PCI_DEVICE_ID_LNVM;
        lnvm_set_default(&n->lightnvm_ctrl);
    }
}

static void nvmef_regs_setup (NvmeCtrl *n)
{
    n->nvme_regs.vBar.cap = 0;
    NVME_CAP_SET_MQES(n->nvme_regs.vBar.cap, n->max_q_ents);
    NVME_CAP_SET_CQR(n->nvme_regs.vBar.cap, n->cqr);
    NVME_CAP_SET_AMS(n->nvme_regs.vBar.cap, 1);
    NVME_CAP_SET_TO(n->nvme_regs.vBar.cap, 0xf);
    NVME_CAP_SET_DSTRD(n->nvme_regs.vBar.cap, n->db_stride);
    NVME_CAP_SET_NSSRS(n->nvme_regs.vBar.cap, 0);
    NVME_CAP_SET_CSS(n->nvme_regs.vBar.cap, 1);

    if (core.std_ftl == FTL_ID_LNVM) {
        NVME_CAP_SET_LIGHTNVM(n->nvme_regs.vBar.cap, 1);
        NVME_CAP_SET_LIGHTNVM(n->nvme_regs.bBar.cap.lnvm, 1);
    }

    NVME_CAP_SET_MPSMIN(n->nvme_regs.vBar.cap, n->mpsmin);
    NVME_CAP_SET_MPSMAX(n->nvme_regs.vBar.cap, n->mpsmax);

    if (n->cmbsz)
        n->nvme_regs.vBar.vs = 0x00010200;
    else
        n->nvme_regs.vBar.vs = 0x00010100;
    n->nvme_regs.vBar.intmc = n->nvme_regs.vBar.intms = 0;
}

static int nvmef_init_ctrl (NvmeCtrl *n)
{
    int i;
    NvmeIdCtrl *id = &n->id_ctrl;

    memset (id, 0, 4096);

    /* Identify Data Structure definition */
    id->vid = htole16(n->vid);
    id->ssvid = htole16(n->did);
    id->rab = 6;
    id->ieee[0] = 0x00;
    id->ieee[1] = 0x02;
    id->ieee[2] = 0xb3;
    id->cmic = 0;
    id->mdts = 8; /* 4k * (1 << 8) = 1 MB max transfer per NVMe I/O */
    id->oacs = htole16(NVME_OACS_FORMAT);
    id->acl = 3;
    id->aerl = 3;
    id->frmw = 7 << 1 | 1;
    id->lpa = 1 << 1;
    id->elpe = 3;
    id->npss = 0;
    id->sqes = (n->max_sqes << 4) | 0x6;
    id->cqes = (n->max_cqes << 4) | 0x4;
    id->nn = htole32(n->num_namespaces);
    id->oncs = htole16(NVME_ONCS_FEATURES);
    id->fuses = htole16(0);
    id->fna = 0;
    id->vwc = 0;
    id->awun = htole16(0);
    id->awupf = htole16(0);
    id->psd[0].mp = htole16(0x9c4);
    id->psd[0].enlat = htole32(0x10);
    id->psd[0].exlat = htole32(0x4);
    id->oaes = 0;

    /* To be checked */
    memcpy (id->sn, "---OX-CONTROLLER---\0", 20);
    memcpy (id->mn, "---------------DFC-CARD-OX-------------\0", 40);
    memcpy (id->subnqn, "2016-09-ox.ctrl.dfc.nvme\0", 25);
    memcpy (id->fr, "180916\0", 7);
    id->cntlid = htole16(0xaaac);

    /* Fields not defined yet */

    id->ver = htole32(0);
    id->rtd3r = 0;
    id->rtd3e = 0;
    id->ctratt = 0;
    id->avscc = 0;
    id->apsta = 0;
    id->wctemp = htole16(0);
    id->cctemp = htole16(0);
    id->mtfa = 0;
    id->hmpre = 0;
    id->hmmin = 0;
    id->tnvmcap[0] = 0;
    id->unvmcap[0] = 0;
    id->rpmbs = 0;
    id->kas = 0;
    id->maxcmd = 0;
    id->nvscc = 0;
    id->acwu = htole16(0);
    id->sgls = htole32(0);
    id->vs[0] = 0;

    /* Controller features */
    n->features.arbitration     = 0x1f0f0706;
    n->features.power_mgmt      = 0;
    n->features.temp_thresh     = 0x14d;
    n->features.err_rec         = 0;
    n->features.volatile_wc     = n->id_ctrl.vwc;
    n->features.num_queues      = n->num_queues;
    n->features.int_coalescing  = n->intc_thresh | (n->intc_time << 8);
    n->features.write_atomicity = 0;
    n->features.async_config    = 0x0;
    n->features.sw_prog_marker  = 0;

    n->features.int_vector_config = ox_calloc (1, n->num_queues *
			sizeof (*n->features.int_vector_config), OX_MEM_NVMEF);
    for (i = 0; i < n->num_queues; i++) {
	n->features.int_vector_config[i] = i | (n->intc << 16);
    }

    nvmef_regs_setup (n);

    if ( (core.std_ftl == FTL_ID_LNVM ) && lnvm_dev(n)) {
        NVME_CAP_SET_LIGHTNVM(n->nvme_regs.vBar.cap, 1);
        lnvm_init_id_ctrl(&n->lightnvm_ctrl.id_ctrl);
    }

    n->temperature = NVME_TEMPERATURE;

    n->sq = ox_calloc (n->features.num_queues, sizeof (void *), OX_MEM_NVMEF);
    n->cq = ox_calloc (n->features.num_queues, sizeof (void *), OX_MEM_NVMEF);
    if (!n->sq || !n->cq)
        return EMEM;

    n->elpes = ox_calloc (1, (n->id_ctrl.elpe + 1) * sizeof (*n->elpes),
                                                                OX_MEM_NVMEF);
    n->aer_reqs = ox_calloc (1, (n->id_ctrl.aerl + 1) * sizeof (*n->aer_reqs),
                                                                OX_MEM_NVMEF);
    if (!n->elpes || !n->aer_reqs)
        return EMEM;

    memset (&n->stat, 0, sizeof(NvmeStats));
    memset (nvmef_queues, 0x0, NVME_NUM_QUEUES *
                                           sizeof (struct nvmef_queue_pair *));

    n->page_bits = NVME_CC_MPS(n->nvme_regs.vBar.cc) + 12;
    n->page_size = 1 << n->page_bits;
    n->max_prp_ents = n->page_size / sizeof (uint64_t);

    return 0;
}

static int nvmef_check_constraints (NvmeCtrl *n)
{
    if ((n->num_namespaces == 0 || n->num_namespaces>NVME_MAX_NUM_NAMESPACES) ||
	(n->num_queues < 1 || n->num_queues > NVME_MAX_QS) ||
	(n->db_stride > NVME_MAX_STRIDE) ||
	(n->max_q_ents < 1) ||
	(n->max_sqes > NVME_MAX_QUEUE_ES || n->max_cqes > NVME_MAX_QUEUE_ES ||
	 n->max_sqes < NVME_MIN_SQUEUE_ES || n->max_cqes<NVME_MIN_CQUEUE_ES) ||
	(n->id_ctrl.vwc > 1 || n->intc > 1 || n->cqr > 1 || n->extended > 1) ||
	(n->nlbaf > 16) ||
	(n->lba_index >= n->nlbaf) ||
	(n->meta && !n->mc) ||
	(n->extended && !(NVME_ID_NS_MC_EXTENDED(n->mc))) ||
	(!n->extended && n->meta && !(NVME_ID_NS_MC_SEPARATE(n->mc))) ||
	(n->dps && n->meta < 8) ||
	(n->dps && ((n->dps & DPS_FIRST_EIGHT) &&
		    !NVME_ID_NS_DPC_FIRST_EIGHT(n->dpc))) ||
	(n->dps && !(n->dps & DPS_FIRST_EIGHT) &&
	 !NVME_ID_NS_DPC_LAST_EIGHT(n->dpc)) ||
	(n->dps & DPS_TYPE_MASK && !((n->dpc & NVME_ID_NS_DPC_TYPE_MASK) &
				     (1 << ((n->dps & DPS_TYPE_MASK) - 1)))) ||
	(n->mpsmax > 0xf || n->mpsmax < n->mpsmin) ||
	(n->id_ctrl.oacs & ~(NVME_OACS_FORMAT)) ||
	(n->id_ctrl.oncs & ~(NVME_ONCS_FEATURES))) {
        return -1;
    }
    return 0;
}

static int nvmef_init_namespaces (NvmeCtrl *n)
{
    int i, j, k, lba_index;
    uint16_t oob_sz, ch_oobsz, sec_sz;
    NvmeNamespace *ns;
    NvmeIdNs *id_ns;
    uint64_t blks;

    n->namespaces = ox_calloc (1, sizeof (NvmeNamespace) * n->num_namespaces,
                                                                  OX_MEM_NVMEF);
    if (!n->namespaces)
        return EMEM;

    for (i = 0; i < n->num_namespaces; i++) {
	ns = &n->namespaces[i];
	id_ns = &ns->id_ns;

        memset (id_ns, 0, 4096);

        /* Identify Namespace Data Structure definition */
        id_ns->nsfeat = 0;
        id_ns->nlbaf = n->nlbaf - 1;
        id_ns->flbas = n->lba_index | (n->extended << 4);
        id_ns->mc = n->mc;
	id_ns->dpc = n->dpc;
	id_ns->dps = n->dps;

        /* TODO: if we have more than 1 namespace, the metadata size
         per sector must be the lower size among all channels related
         to the namespace. */
        oob_sz = 1 << (BDRV_SECTOR_BITS + n->nlbaf - 1);
        for (k = 0; k < core.nvm_ch_count; k++) {
            ch_oobsz = core.nvm_ch[k]->geometry->sec_oob_sz;
            if (ch_oobsz < oob_sz)
                oob_sz = ch_oobsz;
        }

	for (j = 0; j < n->nlbaf; j++) {
            id_ns->lbaf[j].ds = BDRV_SECTOR_BITS + j;
            sec_sz = 1 << id_ns->lbaf[j].ds;
            id_ns->lbaf[j].ms = (oob_sz > sec_sz) ?
                                            htole16(sec_sz) : htole16(oob_sz);
	}

        lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
	blks = n->ns_size[0] / ((1 << id_ns->lbaf[lba_index].ds));

	id_ns->nuse = id_ns->ncap = id_ns->nsze = htole64(blks);

        if ( (core.std_ftl == FTL_ID_LNVM) && lnvm_dev(n)) {
            id_ns->vs[0] = 0x1;
            id_ns->nsze = 0;
        }

        ns->id = i + 1;
	ns->ctrl = n;
	ns->start_block = 0;

        /* To be checked */
        memcpy (id_ns->eui64, "ox-ns\0", 6);
        memcpy (id_ns->nguid, "ox-ctrl-lnvm-ns\0", 16);

        /* Field not defined yet */
        id_ns->nmic = 0;
        id_ns->rescap = 0;
        id_ns->fpi = 0;
        id_ns->nawun = htole16(0);
        id_ns->nawupf = htole16(0);
        id_ns->nacwu = htole16(0);
        id_ns->nabsn = htole16(0);
        id_ns->nabo = htole16(0);
        id_ns->nabspf = htole16(0);
        id_ns->nvmcap[0] = htole16(0);
    }

    return 0;
}

static void nvmef_process_sq (struct ox_mq_entry *mq_req)
{
    NvmeRequest *req;

    req = (NvmeRequest *) mq_req->opaque;
    req->mq_req = mq_req;

    ox_execute_opcode (req->cqe.sq_id, req);
}

static void nvmef_process_cq (void *opaque)
{
    NvmeRequest *req = (NvmeRequest *) opaque;

    if (core.nvm_fabrics->ops->complete (&req->cqe, req->ctx))
        log_err ("[ox-fabrics (cq): Command not completed to host.]\n");

    pthread_spin_lock (&nvmef_queues[req->cqe.sq_id]->req_spin);
    TAILQ_REMOVE(&nvmef_queues[req->cqe.sq_id]->req_uh, req, entry);
    TAILQ_INSERT_TAIL(&nvmef_queues[req->cqe.sq_id]->req_fh, req, entry);
    pthread_spin_unlock (&nvmef_queues[req->cqe.sq_id]->req_spin);
}

void nvmef_complete_request (NvmeRequest *req)
{
    NvmeCqe *cqe = &req->cqe;

    cqe->status = req->status;

    if (ox_mq_complete_req (nvmef_queues[cqe->sq_id]->mq,
                                        (struct ox_mq_entry *) req->mq_req))
        log_err ("[ox-fabrics (complete): Command not completed]\n");
}

static void nvmef_process_to (void **opaque, int counter)
{

}

int nvmef_create_queue (uint16_t qid)
{
    uint32_t ent_i;
    struct ox_mq_config mq_config;

    if (qid >= NVME_NUM_QUEUES)
        return -1;

    if (nvmef_queues[qid]) {
        log_info ("[nvmef: Queue %d already created.]", qid);
        return core.nvm_fabrics->ops->create (qid);
    }

    nvmef_queues[qid] = ox_calloc (1, sizeof (struct nvmef_queue_pair),
                                                                 OX_MEM_NVMEF);
    if (!nvmef_queues[qid])
        return -1;

    sprintf(mq_config.name, "%s-%d", "NVME_QUEUE", qid);
    mq_config.n_queues = 1;
    mq_config.q_size = core.nvme_ctrl->max_q_ents;
    mq_config.sq_fn = nvmef_process_sq;
    mq_config.cq_fn = nvmef_process_cq;
    mq_config.to_fn = nvmef_process_to;
    mq_config.output_fn = NULL;
    mq_config.to_usec = 0;
    mq_config.flags = OX_MQ_CPU_AFFINITY;

    /* Set thread affinity, if enabled */
    mq_config.sq_affinity[0] = 0;
    mq_config.cq_affinity[0] = 0;

#if OX_TH_AFFINITY
    mq_config.sq_affinity[0] |= ((uint64_t) 1 << 0);
    mq_config.sq_affinity[0] |= ((uint64_t) 1 << 2);
    mq_config.sq_affinity[0] |= ((uint64_t) 1 << 5);

    mq_config.cq_affinity[0] |= ((uint64_t) 1 << 0);
    mq_config.cq_affinity[0] |= ((uint64_t) 1 << 1);
#endif /* OX_TH_AFFINITY */

    nvmef_queues[qid]->mq = ox_mq_init(&mq_config);
    if (!nvmef_queues[qid]->mq)
        return -1;

    nvmef_queues[qid]->requests = ox_calloc (core.nvme_ctrl->max_q_ents * 2,
                                            sizeof (NvmeRequest), OX_MEM_NVMEF);
    if (!nvmef_queues[qid]->requests)
        goto DESTROY_MQ;

    if (pthread_spin_init (&nvmef_queues[qid]->req_spin, 0))
        goto FREE_REQ;

    if (core.nvm_fabrics->ops->create (qid))
        goto DESTROY_SPIN;

    TAILQ_INIT(&nvmef_queues[qid]->req_fh);
    TAILQ_INIT(&nvmef_queues[qid]->req_uh);
    for (ent_i = 0; ent_i < core.nvme_ctrl->max_q_ents; ent_i++) {

        if (pthread_mutex_init
                    (&nvmef_queues[qid]->requests[ent_i].nvm_io.mutex, NULL))
            goto FREE_ENT;

        TAILQ_INSERT_TAIL(&nvmef_queues[qid]->req_fh,
                                    &nvmef_queues[qid]->requests[ent_i], entry);
    }

    return 0;

FREE_ENT:
    while (ent_i) {
        ent_i--;
        pthread_mutex_destroy (&nvmef_queues[qid]->requests[ent_i].nvm_io.mutex);
    }
DESTROY_SPIN:
    pthread_spin_destroy (&nvmef_queues[qid]->req_spin);
FREE_REQ:
    ox_free (nvmef_queues[qid]->requests, OX_MEM_NVMEF);
DESTROY_MQ:
    ox_mq_destroy (nvmef_queues[qid]->mq);
    return -1;
}

void nvmef_destroy_queue (uint16_t qid)
{
    uint32_t ent_i;

    if ((qid >= NVME_NUM_QUEUES) || !nvmef_queues[qid])
        return;

    if (core.nvm_fabrics)
        core.nvm_fabrics->ops->destroy (qid);

    for (ent_i = 0; ent_i < core.nvme_ctrl->max_q_ents; ent_i++)
        pthread_mutex_destroy (&nvmef_queues[qid]->requests[ent_i].nvm_io.mutex);

    TAILQ_INIT(&nvmef_queues[qid]->req_fh);
    TAILQ_INIT(&nvmef_queues[qid]->req_uh);

    pthread_spin_destroy (&nvmef_queues[qid]->req_spin);
    ox_free (nvmef_queues[qid]->requests, OX_MEM_NVMEF);
    ox_mq_destroy (nvmef_queues[qid]->mq);

    ox_free (nvmef_queues[qid], OX_MEM_NVMEF);
    nvmef_queues[qid] = NULL;
}

void nvmef_exit(void)
{
    NvmeCtrl *n = core.nvme_ctrl;
    uint32_t ent_i;

    for (ent_i = 0; ent_i < NVME_NUM_QUEUES; ent_i++) {
        if (nvmef_queues[ent_i])
            nvmef_destroy_queue (ent_i);
    }

    //nvme_clear_ctrl (n);
    ox_free (n->sq, OX_MEM_NVMEF);
    ox_free (n->cq, OX_MEM_NVMEF);
    ox_free (n->aer_reqs, OX_MEM_NVMEF);
    ox_free (n->elpes, OX_MEM_NVMEF);
    ox_free (n->features.int_vector_config, OX_MEM_NVMEF);
    ox_free (n->namespaces, OX_MEM_NVMEF);
    ox_free (n->ns_size, OX_MEM_NVMEF);

    if ( (core.std_ftl == FTL_ID_LNVM ) && lnvm_dev(n) )
        lightnvm_exit(n);

    pthread_mutex_destroy(&n->req_mutex);
   // pthread_mutex_destroy(&n->qs_req_mutex);
   // pthread_mutex_destroy(&n->aer_req_mutex);

    log_info(" [nvm: NVMe over Fabrics unregistered.]\n");
}

int nvmef_init (NvmeCtrl *n)
{
    if (!ox_mem_create_type ("NVME_FABRICS", OX_MEM_NVMEF))
        return -1;

    nvmef_set_default (n);
    n->start_time = time (NULL);

    if (pthread_mutex_init (&n->req_mutex, NULL))
        return -1;

    /* For now only 1 namespace */
    n->ns_size = (uint64_t *) ox_calloc(1, sizeof(uint64_t), OX_MEM_NVMEF);
    if (!n->ns_size)
        return EMEM;

    n->ns_size[0] = core.nvm_ns_size;

    if(nvmef_init_ctrl(n))
        goto FREE_NS;

    if(nvmef_check_constraints(n) || nvmef_init_namespaces(n))
        return ENVME_REGISTER;

    if ( (core.std_ftl == FTL_ID_LNVM) && lnvm_dev(n) && lnvm_init(n) )
        return ENVME_REGISTER;

    /* Start admin queue */
    if (nvmef_create_queue (0))
        goto FREE_NS;

    log_info("  [nvm: NVME over Fabrics registered.]\n");

    return 0;

FREE_NS:
    ox_free (n->ns_size, OX_MEM_NVMEF);
    return ENVME_REGISTER;
}
