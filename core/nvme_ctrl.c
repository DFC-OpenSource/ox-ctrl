/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - NVM Express Standard
 *
 * Copyright 2016 IT University of Copenhagen
 * 
 * Modified by Ivan Luiz Picoli <ivpi@itu.dk>
 * Modified from QEMU Project
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

#include <syslog.h>
#include <endian.h>
#include <stdlib.h>
#include <stddef.h>
#include <pthread.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <sys/queue.h>
#include <string.h>
#include <libox.h>
#include <nvme.h>
#include <ox-lightnvm.h>

extern struct core_struct core;
static uint64_t           nvm_ns_size;
static NvmeCtrl           *nvm_nvme_ctrl;
static struct nvm_pcie    *nvm_pcie;

//static void nvme_process_sq (void *);

void nvme_addr_read (NvmeCtrl *n, uint64_t addr, void *buf, int size)
{
    /* TODO: Use the DMA module */
    /*if (n->cmbsz && addr >= core.qemu->ctrl_mem.addr &&
                addr < (core.qemu->ctrl_mem.addr +
                int128_get64(core.qemu->ctrl_mem.size))) {
        memcpy(buf, (void *)&n->cmbuf[addr - core.qemu->ctrl_mem.addr], size);
    } else {
        pci_dma_read(&core.qemu->parent_obj, addr, buf, size);
    }*/
}

static void nvme_set_default (NvmeCtrl *n)
{
    n->num_namespaces = 1;
    n->num_queues = 64;
    n->max_q_ents = 0x7ff;
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
    n->vid = PCI_VENDOR_ID_INTEL;
    n->did = PCI_DEVICE_ID_LS2085;

    if (core.std_ftl == FTL_ID_LNVM) {
        n->vid = PCI_VENDOR_ID_LNVM;
        n->did = PCI_DEVICE_ID_LNVM;
        lnvm_set_default(&n->lightnvm_ctrl);
    }
}

static void nvme_regs_setup (NvmeCtrl *n)
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

static int nvme_init_ctrl (NvmeCtrl *n)
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

    n->features.int_vector_config = calloc (1, n->num_queues *
			sizeof (*n->features.int_vector_config));
    for (i = 0; i < n->num_queues; i++) {
	n->features.int_vector_config[i] = i | (n->intc << 16);
    }

    nvme_regs_setup (n);

    if ((core.std_ftl == FTL_ID_LNVM) && lnvm_dev(n)) {
        NVME_CAP_SET_LIGHTNVM(n->nvme_regs.vBar.cap, 1);
        lnvm_init_id_ctrl(&n->lightnvm_ctrl.id_ctrl);
    }

    n->temperature = NVME_TEMPERATURE;

    n->sq = calloc (n->features.num_queues, sizeof (void *));
    n->cq = calloc (n->features.num_queues, sizeof (void *));
    if (!n->sq || !n->cq)
        return EMEM;

    n->elpes = calloc (1, (n->id_ctrl.elpe + 1) * sizeof (*n->elpes));
    n->aer_reqs = calloc (1, (n->id_ctrl.aerl + 1) * sizeof (*n->aer_reqs));
    if (!n->elpes || !n->aer_reqs)
        return EMEM;

    memset(&n->stat, 0, sizeof(NvmeStats));

    return 0;
}

static int nvme_init_namespaces (NvmeCtrl *n)
{
    int i, j, k, lba_index;
    uint16_t oob_sz, ch_oobsz, sec_sz;
    NvmeNamespace *ns;
    NvmeIdNs *id_ns;
    uint64_t blks;

    n->namespaces = calloc (1, sizeof (NvmeNamespace) * n->num_namespaces);
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

        if ((core.std_ftl == FTL_ID_LNVM) && lnvm_dev(n)) {
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

static int nvme_check_constraints (NvmeCtrl *n)
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

static void nvme_isr_notify(void *opaque)
{
    NvmeCQ *cq = opaque;
    core.nvm_pcie->ops->isr_notify(cq);
}

uint16_t nvme_init_cq (NvmeCQ *cq, NvmeCtrl *n, uint64_t dma_addr,
                    uint16_t cqid, uint16_t vector, uint16_t size,
                    uint16_t irq_enabled, int contig)
{
    cq->ctrl = n;
    cq->cqid = cqid;
    cq->size = size;
    cq->phase = 1;
    cq->irq_enabled = irq_enabled;
    cq->vector = vector;
    cq->head = cq->tail = 0;
    cq->phys_contig = contig;
    if (cq->phys_contig) {
        cq->dma_addr = dma_addr;
    } else {
        cq->prp_list = NULL;
        if (!cq->prp_list) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
    }

    TAILQ_INIT(&cq->req_list);
    TAILQ_INIT(&cq->sq_list);
    cq->db_addr = 0;
    cq->eventidx_addr = 0;
    //msix_vector_use(&core.qemu->parent_obj, cq->vector);
    n->cq[cqid] = cq;
    //cq->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, nvme_isr_notify, cq);

    log_info("\n[nvme: init CQ qid: %d irq_vector: %d\n", cqid, vector);
    return NVME_SUCCESS;
}

uint16_t nvme_init_sq (NvmeSQ *sq, NvmeCtrl *n, uint64_t dma_addr,
                    uint16_t sqid, uint16_t cqid, uint16_t size,
                    enum NvmeQFlags prio, int contig)
{
    int i;
    NvmeCQ *cq;

    sq->ctrl = n;
    sq->sqid = sqid;
    sq->size = size;
    sq->cqid = cqid;
    sq->head = sq->tail = 0;
    sq->phys_contig = contig;
    if (sq->phys_contig) {
        sq->dma_addr = dma_addr;
    } else {
        sq->prp_list = NULL;
        if (!sq->prp_list) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
    }

    sq->io_req = malloc(sq->size * sizeof(*sq->io_req));
    TAILQ_INIT(&sq->req_list);
    TAILQ_INIT(&sq->out_req_list);
    for (i = 0; i < sq->size; i++) {
        sq->io_req[i].sq = sq;
        pthread_mutex_init (&sq->io_req[i].nvm_io.mutex, NULL);
        TAILQ_INSERT_TAIL(&(sq->req_list), &sq->io_req[i], entry);
    }

    switch (prio) {
    case NVME_Q_PRIO_URGENT:
        sq->arb_burst = (1 << NVME_ARB_AB(n->features.arbitration));
        break;
    case NVME_Q_PRIO_HIGH:
        sq->arb_burst = NVME_ARB_HPW(n->features.arbitration) + 1;
        break;
    case NVME_Q_PRIO_NORMAL:
        sq->arb_burst = NVME_ARB_MPW(n->features.arbitration) + 1;
        break;
    case NVME_Q_PRIO_LOW:
    default:
        sq->arb_burst = NVME_ARB_LPW(n->features.arbitration) + 1;
        break;
    }
    //sq->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, nvme_process_sq, sq);
    sq->db_addr = 0;
    sq->eventidx_addr = 0;

    assert(n->cq[cqid]);
    cq = n->cq[cqid];
    TAILQ_INSERT_TAIL(&(cq->sq_list), sq, entry);
    n->sq[sqid] = sq;

    log_info("\n[nvme: init SQ qid: %d\n", sqid);

    return NVME_SUCCESS;
}

static int nvme_start_ctrl (NvmeCtrl *n)
{
    uint32_t page_bits = NVME_CC_MPS(n->nvme_regs.vBar.cc) + 12;
    uint32_t page_size = 1 << page_bits;
    syslog(LOG_INFO,"[nvme: nvme starting ctrl]\n");

    if (n->cq[0] || n->sq[0] || !n->nvme_regs.vBar.asq ||
                        !n->nvme_regs.vBar.acq ||
                        n->nvme_regs.vBar.asq & (page_size - 1) ||
                        n->nvme_regs.vBar.acq & (page_size - 1) ||
                        NVME_CC_MPS(n->nvme_regs.vBar.cc) <
                            NVME_CAP_MPSMIN(n->nvme_regs.vBar.cap) ||
                        NVME_CC_MPS(n->nvme_regs.vBar.cc) >
                            NVME_CAP_MPSMAX(n->nvme_regs.vBar.cap) ||
                        NVME_CC_IOCQES(n->nvme_regs.vBar.cc) <
                            NVME_CTRL_CQES_MIN(n->id_ctrl.cqes) ||
                        NVME_CC_IOCQES(n->nvme_regs.vBar.cc) >
                            NVME_CTRL_CQES_MAX(n->id_ctrl.cqes) ||
                        NVME_CC_IOSQES(n->nvme_regs.vBar.cc) <
                            NVME_CTRL_SQES_MIN(n->id_ctrl.sqes) ||
                        NVME_CC_IOSQES(n->nvme_regs.vBar.cc) >
                            NVME_CTRL_SQES_MAX(n->id_ctrl.sqes) ||
                        !NVME_AQA_ASQS(n->nvme_regs.vBar.aqa) ||
                        NVME_AQA_ASQS(n->nvme_regs.vBar.aqa) > 4095 ||
                        !NVME_AQA_ACQS(n->nvme_regs.vBar.aqa) ||
                        NVME_AQA_ACQS(n->nvme_regs.vBar.aqa) > 4095) {
        syslog (LOG_ERR,"[ERROR nvme: init values went bad]\n");
	return -1;
    }

    n->page_bits = page_bits;
    n->page_size = 1 << n->page_bits;
    n->max_prp_ents = n->page_size / sizeof (uint64_t);
    n->cqe_size = 1 << NVME_CC_IOCQES(n->nvme_regs.vBar.cc);
    n->sqe_size = 1 << NVME_CC_IOSQES(n->nvme_regs.vBar.cc);

    nvme_init_cq (&n->admin_cq, n, n->nvme_regs.vBar.acq, 0, 0, \
		NVME_AQA_ACQS(n->nvme_regs.vBar.aqa) + 1, 1, 1);

    nvme_init_sq (&n->admin_sq, n, n->nvme_regs.vBar.asq, 0, 0, \
		NVME_AQA_ASQS(n->nvme_regs.vBar.aqa) + 1, NVME_Q_PRIO_HIGH, 1);

    n->aer_queue.tqh_first = NULL;
    n->aer_queue.tqh_last = &(n->aer_queue).tqh_first;
   // n->aer_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, nvme_aer_process_cb, n);
    TAILQ_INIT (&n->aer_queue);

    return 0;
}

void nvme_free_sq (NvmeSQ *sq, NvmeCtrl *n)
{
    uint32_t i;

    for (i = 0; i < sq->size; i++)
        pthread_mutex_destroy (&sq->io_req[i].nvm_io.mutex);

    n->sq[sq->sqid] = NULL;
    FREE_VALID (sq->io_req);
    FREE_VALID (sq->prp_list);

    if (sq->dma_addr)
        sq->dma_addr = 0;

    SAFE_CLOSE (sq->fd_qmem);
    if (sq->sqid)
	FREE_VALID (sq);
}

void nvme_free_cq (NvmeCQ *cq, NvmeCtrl *n)
{
    n->cq[cq->cqid] = NULL;
    if (cq->prp_list)
	FREE_VALID (cq->prp_list);

    if (cq->dma_addr)
	cq->dma_addr = 0;

    SAFE_CLOSE (cq->fd_qmem);
    if (cq->cqid)
    	FREE_VALID (cq);
}

static void nvme_clear_ctrl (NvmeCtrl *n)
{
    NvmeAsyncEvent *event;
    int i;

    if (!n->running) {
    	/*nvme not in action.. so nothing to stop*/
	return;
    }

    n->running = 0;
    if (n->sq)
        for (i = 0; i < n->num_queues; i++)
            if (n->sq[i] != NULL)
		nvme_free_sq (n->sq[i], n);

    if (n->cq)
        for (i = 0; i < n->num_queues; i++)
            if (n->cq[i] != NULL)
		nvme_free_cq (n->cq[i], n);

    pthread_mutex_lock(&n->aer_req_mutex);
    while((event = (NvmeAsyncEvent *)TAILQ_FIRST(&n->aer_queue)) != NULL) {
        TAILQ_REMOVE(&n->aer_queue, event, entry);
        FREE_VALID(event);
    }
    pthread_mutex_unlock(&n->aer_req_mutex);

    n->nvme_regs.vBar.cc = 0;
    n->nvme_regs.vBar.csts = 0;
    n->features.temp_thresh = 0x14d;
    n->temp_warn_issued = 0;
    n->outstanding_aers = 0;
}

void nvme_process_reg (NvmeCtrl *n, uint64_t offset, uint64_t data)
{
    switch (offset) {
	case  0x0c:
            log_info("[nvme: INTMS: %lx]\n", data);
            n->nvme_regs.vBar.intms |= data & 0xffffffff;
            n->nvme_regs.vBar.intmc = n->nvme_regs.vBar.intms;
            break;
        case  0x10:
            log_info("[nvme: INTMC: %lx]\n", data);
            n->nvme_regs.vBar.intms &= ~(data & 0xffffffff);
            n->nvme_regs.vBar.intmc = n->nvme_regs.vBar.intms;
	case  0x14:
            log_info("[nvme: CC: %lx]\n", data);
            if (NVME_CC_EN(data) && !NVME_CC_EN(n->nvme_regs.vBar.cc)) {
		n->nvme_regs.vBar.cc = data;
		syslog(LOG_DEBUG,"[nvme: Nvme EN!]\n");
                if (nvme_start_ctrl(n)) {
                    n->nvme_regs.vBar.csts = NVME_CSTS_FAILED;
                } else {
                    n->nvme_regs.vBar.csts = NVME_CSTS_READY;
                }
                n->qsched.WRR = n->nvme_regs.vBar.cc & 0x3800;
            } else if (!NVME_CC_EN(data) && \
		NVME_CC_EN(n->nvme_regs.vBar.cc)) {
		syslog(LOG_DEBUG,"[nvme: Nvme !EN]\n");
		ox_restart ();
		n->nvme_regs.vBar.cc = 0;
		n->nvme_regs.vBar.csts &= ~NVME_CSTS_READY;
            }
            if (NVME_CC_SHN(data) && !(NVME_CC_SHN(n->nvme_regs.vBar.cc))) {
		syslog(LOG_DEBUG,"[nvme: Nvme SHN!]\n");
		n->nvme_regs.vBar.cc = data;
		n->running = 1;
		n->nvme_regs.vBar.csts |= NVME_CSTS_SHST_COMPLETE;
		n->nvme_regs.vBar.csts &= ~NVME_CSTS_READY;
		n->nvme_regs.vBar.cc = 0;
            } else if (!NVME_CC_SHN(data) && NVME_CC_SHN(n->nvme_regs.vBar.cc)){
		syslog(LOG_DEBUG,"[nvme: Nvme !SHN]\n");
		n->nvme_regs.vBar.csts &= ~NVME_CSTS_SHST_COMPLETE;
		n->nvme_regs.vBar.cc = data;
            }
            break;
        case 0x20:
            n->nvme_regs.vBar.nssrc = data & 0xffffffff;
            break;
        case  0x24:
            log_info("[nvme: AQA: %lx]\n", data);
            n->nvme_regs.vBar.aqa = data;
            break;
        case  0x28:
            log_info("[nvme: ASQ: %lx]\n", data);
            n->nvme_regs.vBar.asq = data;
            break;
        case 0x2c:
            n->nvme_regs.vBar.asq |= data << 32;
            break;
        case 0x30:
            log_info("[nvme: ACQ: %lx]\n", data);
            n->nvme_regs.vBar.acq = data;
            break;
        case 0x34:
            n->nvme_regs.vBar.acq |= data << 32;
            break;
        default:
            log_info("[nvme: %x?]\n", (uint32_t)offset);
    }
}

static inline uint8_t nvme_sq_empty (NvmeSQ *sq)
{
    return sq->head == sq->tail;
}

inline uint8_t nvme_write_to_host(void *src, uint64_t prp, ssize_t size)
{
    if (prp) {
    /* TODO: Use the DMA module */
    /*    if (core.run_flag & RUN_TESTS)
            memcpy ((void *) prp, src, size);
        else
            pci_dma_write(&core.qemu->parent_obj, prp, src, size);

        return NVME_SUCCESS;*/
    }
    return NVME_INVALID_FIELD;
}

inline uint8_t nvme_read_from_host(void *dest, uint64_t prp, ssize_t size)
{
    if (prp) {
    /* TODO Use the DMA module */
    /*    if (core.run_flag & RUN_TESTS)
            memcpy (dest, (void *) prp, size);
        else
            pci_dma_read(&core.qemu->parent_obj, prp, dest, size);

        return NVME_SUCCESS;*/
    }
    return NVME_INVALID_FIELD;
}

void nvme_addr_write (NvmeCtrl *n, uint64_t addr, void *buf, int size)
{
    if (n->cmb && addr >= n->ctrl_mem.addr && \
                    addr < (n->ctrl_mem.addr + n->ctrl_mem.size)) {
	memcpy ((void *)&n->cmbuf[addr - n->ctrl_mem.addr], buf, size);
        return;
    } else {
	memcpy ((void *)addr , buf, size);
    }
}

static inline void nvme_update_sq_tail (NvmeSQ *sq)
{
    if (sq->db_addr) {
    	nvme_addr_read (sq->ctrl, sq->db_addr, &sq->tail, sizeof (sq->tail));
    }
}

static inline void nvme_inc_sq_head (NvmeSQ *sq)
{
    sq->head = (sq->head + 1) % sq->size;
}

static inline void nvme_inc_cq_tail (NvmeCQ *cq)
{
    cq->tail++;
    if (cq->tail >= cq->size) {
	cq->tail = 0;
	cq->phase = !cq->phase;
    }
}

static inline void nvme_update_cq_head (NvmeCQ *cq)
{
    if (cq->db_addr) {
    	nvme_addr_read (cq->ctrl, cq->db_addr, &cq->head, sizeof (cq->head));
    }
}

static inline uint8_t nvme_cq_full (NvmeCQ *cq)
{
    nvme_update_cq_head (cq);
    return (cq->tail + 1) % cq->size == cq->head;
}

static inline int nvme_cqes_pending (NvmeCQ *cq)
{
    return cq->tail > cq->head ?
	cq->head + (cq->size - cq->tail) :
	cq->head - cq->tail;
}

int nvme_check_cqid (NvmeCtrl *n, uint16_t cqid)
{
    return cqid < n->num_queues && n->cq[cqid] != NULL ? 0 : -1;
}

int nvme_check_sqid (NvmeCtrl *n, uint16_t sqid)
{
    return sqid < n->num_queues && n->sq[sqid] != NULL ? 0 : -1;
}

static void nvme_post_cqe (NvmeCQ *cq, NvmeRequest *req)
{
    NvmeCtrl *n = cq->ctrl;
    NvmeSQ *sq = req->sq;
    NvmeCqe *cqe = &req->cqe;
    uint8_t phase = cq->phase;
    uint64_t addr;

    if (core.std_ftl == FTL_ID_LNVM) {
        LnvmCtrl *ln = &n->lightnvm_ctrl;
        if (ln->err_write && req->is_write) {
            if ((ln->err_write_cnt + req->nlb + 1) > ln->err_write) {
                int bit = ln->err_write - ln->err_write_cnt;
                cqe->u.res64 = 1ULL << bit; // kill first sector in ppa list
                req->status = 0x40ff; // FAIL WRITE status code
                ln->err_write_cnt = 0;
                log_info("[lnvm: injected error: %u]\n", bit);
            }
            ln->err_write_cnt += req->nlb + 1;
        }
    }

    if (cq->phys_contig)
	addr = cq->dma_addr + cq->tail * n->cqe_size;
    else
	addr = 0;

    cqe->status = htole16((req->status << 1) | phase);
    cqe->sq_id = sq->sqid;
    cqe->sq_head = htole16(sq->head);
    nvme_addr_write (n, addr, (void *)cqe, sizeof (*cqe));

    nvme_inc_cq_tail (cq);

    /* In case of timeout request, we have to avoid reusing the same structure
     * TODO: Replace structures in case of timeout */

    TAILQ_INSERT_TAIL (&sq->req_list, req, entry);
    if (cq->hold_sqs) cq->hold_sqs = 0;
}

void nvme_enqueue_req_completion (NvmeCQ *cq, NvmeRequest *req)
{
    NvmeCtrl *n = cq->ctrl;
    uint64_t time_ns = NVME_INTC_TIME(n->features.int_coalescing) * 100000;
    uint8_t thresh = NVME_INTC_THR(n->features.int_coalescing) + 1;
    uint8_t coalesce_disabled =
        		(n->features.int_vector_config[cq->vector] >> 16) & 1;
    uint8_t notify;

    assert (cq->cqid == req->sq->cqid);
    pthread_mutex_lock(&n->req_mutex);
    TAILQ_REMOVE (&req->sq->out_req_list, req, entry);
    pthread_mutex_unlock(&n->req_mutex);

    if (nvme_cq_full (cq) || !TAILQ_EMPTY (&cq->req_list)) {
    	pthread_mutex_lock(&n->req_mutex);
	TAILQ_INSERT_TAIL (&cq->req_list, req, entry);
	pthread_mutex_unlock(&n->req_mutex);
	return;
    }

    notify = coalesce_disabled || !req->sq->sqid || !time_ns ||
	req->status != NVME_SUCCESS || nvme_cqes_pending(cq) >= thresh;

    pthread_mutex_lock(&n->req_mutex);
    nvme_post_cqe (cq, req);
    pthread_mutex_unlock(&n->req_mutex);

    if (notify)
	nvm_pcie->ops->isr_notify(cq);
}

void nvme_enqueue_event (NvmeCtrl *n, uint8_t event_type,
                                        uint8_t event_info, uint8_t log_page)
{
    NvmeAsyncEvent *event;
    if (!(n->nvme_regs.vBar.csts & NVME_CSTS_READY))
		return;
    event = (NvmeAsyncEvent *)calloc (1, sizeof (*event));
    event->result.event_type = event_type;
    event->result.event_info = event_info;
    event->result.log_page   = log_page;

    pthread_mutex_lock(&n->aer_req_mutex);
    TAILQ_INSERT_TAIL (&n->aer_queue, event, entry);
    pthread_mutex_unlock(&n->aer_req_mutex);
}

void nvme_post_cqes (void *opaque)
{
    NvmeCtrl *n = nvm_nvme_ctrl;
    NvmeCQ *cq = opaque;
    NvmeRequest *req;

    pthread_mutex_lock(&n->req_mutex);
    TAILQ_FOREACH (req, &cq->req_list, entry) {
        if (nvme_cq_full (cq)) {
            break;
	}
	TAILQ_REMOVE (&cq->req_list, req, entry);
            nvme_post_cqe (cq, req);
    }
    pthread_mutex_unlock(&n->req_mutex);
    nvme_isr_notify(cq);
}

uint16_t nvme_admin_cmd (NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
   /* n->stat.tot_num_AdminCmd += 1;

    if (core.debug)
        printf("\n[%lu] ADMIN CMD 0x%x, nsid: %d, cid: %d\n",
                   n->stat.tot_num_AdminCmd, cmd->opcode, cmd->nsid, cmd->cid);

    switch (cmd->opcode) {
    	case NVME_ADM_CMD_DELETE_SQ:
            return nvme_del_sq (n, cmd);
	case NVME_ADM_CMD_CREATE_SQ:
            return nvme_create_sq (n, cmd);
	case NVME_ADM_CMD_DELETE_CQ:
            return nvme_del_cq (n, cmd);
        case NVME_ADM_CMD_CREATE_CQ:
            return nvme_create_cq (n, cmd);
        case NVME_ADM_CMD_IDENTIFY:
            return nvme_identify (n, cmd);
	case NVME_ADM_CMD_SET_FEATURES:
            return nvme_set_feature (n, cmd, req);
        case NVME_ADM_CMD_GET_FEATURES:
            return nvme_get_feature (n, cmd, req);
	case NVME_ADM_CMD_GET_LOG_PAGE:
            return nvme_get_log(n, cmd);
	case NVME_ADM_CMD_ASYNC_EV_REQ:
            return nvme_async_req (n, cmd, req);
	case NVME_ADM_CMD_ABORT:
            return nvme_abort_req (n, cmd, &req->cqe.u.n.result);
        case NVME_ADM_CMD_FORMAT_NVM:
            if (NVME_OACS_FORMAT & n->id_ctrl.oacs)
                  return nvme_format (n, cmd);
            return NVME_INVALID_OPCODE | NVME_DNR;
*/
        /* Near-data processing */
   /*     case NDP_ADM_CMD_INFO:
            return NVME_SUCCESS;
        case NDP_ADM_CMD_INST_DAEM:
            return NVME_SUCCESS;
        case NDP_ADM_CMD_DEL_DAEM:
            return NVME_SUCCESS;

        case LNVM_ADM_CMD_IDENTITY:
            return lnvm_identity(n, cmd);
        case LNVM_ADM_CMD_GET_L2P_TBL:
            return lnvm_get_l2p_tbl(n, cmd, req);
        case LNVM_ADM_CMD_GET_BB_TBL:
            return lnvm_get_bb_tbl(n, cmd, req);
        case LNVM_ADM_CMD_SET_BB_TBL:
            return lnvm_set_bb_tbl(n, cmd, req);

        case NVME_ADM_CMD_ACTIVATE_FW:
	case NVME_ADM_CMD_DOWNLOAD_FW:
	case NVME_ADM_CMD_SECURITY_SEND:
	case NVME_ADM_CMD_SECURITY_RECV:
	default:
            n->stat.tot_num_AdminCmd -= 1;
            return NVME_INVALID_OPCODE | NVME_DNR;
    }*/
    return 0;
}

/*static uint16_t nvme_io_cmd (NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    NvmeNamespace *ns;
    uint32_t nsid = cmd->nsid;

    if (nsid == 0 || nsid > n->num_namespaces) {
	log_err("[ERROR nvme: io cmd, bad nsid %d]\n", nsid);
	return NVME_INVALID_NSID | NVME_DNR;
    }

    ns = &n->namespaces[nsid - 1];
    n->stat.tot_num_IOCmd += 1;

    if (core.debug)
        printf("\n[%lu] IO CMD 0x%x, nsid: %d, cid: %d\n",
                   n->stat.tot_num_IOCmd, cmd->opcode, cmd->nsid, cmd->cid);

    switch (cmd->opcode) {
        case LNVM_CMD_PHYS_READ:
            n->stat.tot_num_ReadCmd += 1;
            return (core.std_appnvm == FTL_ID_ELEOS) ?
                                    nvme_eleos_read (n, ns, cmd, req) :
                                    lnvm_rw(n, ns, cmd, req);
        case LNVM_CMD_HYBRID_WRITE:
        case LNVM_CMD_PHYS_WRITE:
            n->stat.tot_num_WriteCmd += 1;
            return lnvm_rw(n, ns, cmd, req);

    	case NVME_CMD_READ:
	    n->stat.tot_num_ReadCmd += 1;
            return nvme_rw(n, ns, cmd, req);

        case NVME_CMD_WRITE:
            n->stat.tot_num_WriteCmd += 1;
            return nvme_rw(n, ns, cmd, req);

        case LNVM_CMD_ERASE_SYNC:
            if (lnvm_dev(n))
                return lnvm_erase_sync(n, ns, cmd, req);
            return NVME_INVALID_OPCODE | NVME_DNR;
*/
        /* Near-data processing */
/*        case NDP_ELEOS_FLUSH:
            return nvme_eleos_flush (n, ns, cmd, req);
        case NDP_ELEOS_READ:
            return nvme_eleos_read (n, ns, cmd, req);
*/
        /* Commands not supported yet */

/*	case NVME_CMD_FLUSH:
            if (!n->id_ctrl.vwc || !n->features.volatile_wc) {
                return NVME_SUCCESS;
            }
            return nvme_flush(n, ns, cmd, req);

	case NVME_CMD_DSM:
            if (NVME_ONCS_DSM & n->id_ctrl.oncs) {
                return nvme_dsm(n, ns, cmd, req);
            }

	case NVME_CMD_COMPARE:
            if (NVME_ONCS_COMPARE & n->id_ctrl.oncs) {
		return nvme_compare (n, ns, cmd, req);
            }
            return NVME_INVALID_OPCODE | NVME_DNR;

	case NVME_CMD_WRITE_ZEROS:
            if (NVME_ONCS_WRITE_ZEROS & n->id_ctrl.oncs) {
		return nvme_write_zeros (n, ns, cmd, req);
            }
            return NVME_INVALID_OPCODE | NVME_DNR;

	case NVME_CMD_WRITE_UNCOR:
            if (NVME_ONCS_WRITE_UNCORR & n->id_ctrl.oncs) {
            	return nvme_write_uncor(n, ns, cmd, req);
	}
	return NVME_INVALID_OPCODE | NVME_DNR;

	default:
            n->stat.tot_num_IOCmd -= 1;
            return NVME_INVALID_OPCODE | NVME_DNR;
    }
    return 0;
}*/

void nvme_rw_cb (void *opaque)
{
    NvmeRequest *req = (NvmeRequest *) opaque;
    NvmeSQ *sq = req->sq;
    NvmeCtrl *n = sq->ctrl;
    NvmeCQ *cq = n->cq[sq->cqid];

    /* TODO: Calculate here n->stats like bytes read/written */

    nvme_enqueue_req_completion (cq, req);
}

/*static void nvme_process_sq (void *opaque)
{
    NvmeSQ *sq = (NvmeSQ *) opaque;

    NvmeCtrl *n = sq->ctrl;
    NvmeCQ *cq = n->cq[sq->cqid];
    char err[100];

    if (cq->hold_sqs || TAILQ_EMPTY (&sq->req_list)) {
        cq->hold_sqs = 1;
	log_info("[nvme: Process-SQ %d with CQ %d delayed]\n",
                                                           sq->sqid, sq->cqid);
	return;
    }

    uint16_t status;
    uint64_t addr = 0;
    NvmeCmd cmd;
    NvmeRequest *req;
    int processed = 0;

    nvme_update_sq_tail (sq);

    while (!(nvme_sq_empty(sq) || TAILQ_EMPTY (&sq->req_list))
			&&	processed < sq->arb_burst) {
	++sq->posted;
	if (sq->phys_contig) {
            addr = sq->dma_addr + sq->head * n->sqe_size;
	} else {
            // TODO
        }

        if (addr == 0) continue;

        nvme_addr_read (n, addr, (void *)&cmd, sizeof (NvmeCmd));
	nvme_inc_sq_head (sq);

        if (cmd.opcode == NVME_OP_ABORTED) {
            continue;
	}

	pthread_mutex_lock(&n->req_mutex);
	req = (NvmeRequest *) TAILQ_FIRST(&sq->req_list);
	TAILQ_REMOVE (&sq->req_list, req, entry);
	TAILQ_INSERT_TAIL (&sq->out_req_list, req, entry);
	pthread_mutex_unlock(&n->req_mutex);

	memset (&req->cqe, 0, sizeof (req->cqe));
	req->cqe.cid = cmd.cid;

        memcpy (&req->cmd, &cmd, sizeof(NvmeCmd));

	status = sq->sqid ?
            nvme_io_cmd (n, &cmd, req) : nvme_admin_cmd (n, &cmd, req);
*/
        /* JUMP */
        /*
        if (sq->sqid) {
            req->status = NVME_SUCCESS;
            nvme_enqueue_req_completion (cq, req);
            goto JUMP;
        }
        */
        /* JUMP */
/*
        if (status != NVME_NO_COMPLETE && status != NVME_SUCCESS) {
            sprintf(err, " [ERROR nvme: cmd 0x%x, with cid: %d returned an "
                          "error status: %x\n", cmd.opcode, cmd.cid, status);
            log_err ("%s",err);
            if (core.debug) printf("%s",err);
        }
*/
        /* Enqueue completion for flush command and flush everything to NVM */
/*        if (sq->sqid && cmd.opcode == NVME_CMD_FLUSH) {
            req->status = status;
            nvme_enqueue_req_completion (cq, req);
            ox_restart();
            return;
        }
*/
        /* Enqueue completion in case of admin command */
/*        if (!sq->sqid && status != NVME_NO_COMPLETE) {
            req->status = status;
            nvme_enqueue_req_completion (cq, req);
        }
*/
        /* Enqueue in case of failed IO cmd that hasn't been enqueued */
/*	if (status != NVME_NO_COMPLETE && sq->sqid &&
                (req->nvm_io.status.status == NVM_IO_PROCESS ||
                req->nvm_io.status.status == NVM_IO_NEW)) {
            req->status = status;
            nvme_enqueue_req_completion (cq, req);
	}

//JUMP:
	processed++;
    }

    nvme_update_sq_tail (sq);

    sq->completed += processed;
}*/

void nvme_process_db (NvmeCtrl *n, uint64_t addr, uint64_t val)
{
    uint32_t qid;
    uint16_t new_val = val & 0xffff;
    NvmeSQ *sq;

    if (addr & ((1 << (2 + n->db_stride)) - 1)) {
        nvme_enqueue_event(n, NVME_AER_TYPE_ERROR,
            NVME_AER_INFO_ERR_INVALID_DB, NVME_LOG_ERROR_INFO);
        return;
    }

    if (((addr - 0x1000) >> (2 + n->db_stride)) & 1) {
        NvmeCQ *cq;
        uint8_t start_sqs;

        qid = (addr - (0x1000 + (1 << (2 + n->db_stride)))) >>
            (3 + n->db_stride);
        if (nvme_check_cqid(n, qid)) {
            nvme_enqueue_event(n, NVME_AER_TYPE_ERROR,
                NVME_AER_INFO_ERR_INVALID_DB, NVME_LOG_ERROR_INFO);
            return;
        }

        cq = n->cq[qid];
        if (new_val >= cq->size) {
            nvme_enqueue_event(n, NVME_AER_TYPE_ERROR,
                NVME_AER_INFO_ERR_INVALID_DB, NVME_LOG_ERROR_INFO);
            return;
        }

        start_sqs = nvme_cq_full(cq) ? 1 : 0;

        /* When the mapped pointer memory area is setup, we don't rely on
         * the MMIO written values to update the head pointer. */
        if (!cq->db_addr) {
            cq->head = new_val;
        }
        if (start_sqs) {
            nvme_post_cqes(cq);
        } else if (cq->tail != cq->head) {
            nvme_isr_notify(cq);
        }
    } else {
        qid = (addr - 0x1000) >> (3 + n->db_stride);
        if (nvme_check_sqid(n, qid)) {
            nvme_enqueue_event(n, NVME_AER_TYPE_ERROR,
                NVME_AER_INFO_ERR_INVALID_SQ, NVME_LOG_ERROR_INFO);
            return;
        }
        sq = n->sq[qid];
        if (new_val >= sq->size) {
            nvme_enqueue_event(n, NVME_AER_TYPE_ERROR,
                NVME_AER_INFO_ERR_INVALID_DB, NVME_LOG_ERROR_INFO);
            return;
        }

        /* When the mapped pointer memory area is setup, we don't rely on
         * the MMIO written values to update the tail pointer. */
        if (!sq->db_addr) {
            sq->tail = new_val;
        }
    }
}

void nvme_exit(void)
{
    NvmeCtrl *n = nvm_nvme_ctrl;
    nvme_clear_ctrl (n);
    FREE_VALID (n->sq);
    FREE_VALID (n->cq);
    FREE_VALID (n->aer_reqs);
    FREE_VALID (n->elpes);
    FREE_VALID (n->features.int_vector_config);
    FREE_VALID (n->namespaces);

    if ((core.std_ftl == FTL_ID_LNVM) && lnvm_dev(n))
        lightnvm_exit(n);

    pthread_mutex_destroy(&n->req_mutex);
    pthread_mutex_destroy(&n->qs_req_mutex);
    pthread_mutex_destroy(&n->aer_req_mutex);

    log_info(" [nvm: NVME standard unregistered.]\n");
}

int nvme_init(NvmeCtrl *n)
{
    nvm_ns_size = core.nvm_ns_size;
    nvm_nvme_ctrl = core.nvme_ctrl;
    nvme_set_default (n);
    nvm_pcie = core.nvm_pcie;
    n->start_time = time (NULL);

    /* For now only 1 namespace */
    n->ns_size = (uint64_t *)calloc(1, sizeof(uint64_t));
    if (!n->ns_size)
        return EMEM;

    n->ns_size[0] = nvm_ns_size;

    if(nvme_init_ctrl(n))
        return ENVME_REGISTER;

    if(nvme_check_constraints(n) || nvme_init_namespaces(n) /*||
                                                    nvme_init_q_scheduler (n)*/)
        return ENVME_REGISTER;

    if ((core.std_ftl == FTL_ID_LNVM) && lnvm_dev(n) && lnvm_init(n))
        return ENVME_REGISTER;

    log_info("  [nvm: NVME standard registered]\n");

    return 0;
}