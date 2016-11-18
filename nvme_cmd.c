#include <stdint.h>
#include <sys/queue.h>
#include <syslog.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <string.h>
#include <pthread.h>
#include "include/lightnvm.h"
#include "include/nvme.h"

extern struct core_struct core;

inline uint8_t nvme_write_to_host(void *src, uint64_t prp, ssize_t size)
{
    void *host_ptr = NULL;
    if (prp) {
        host_ptr = (core.run_flag & RUN_TESTS) ?
              (void *) prp : (void *) (core.nvm_pcie->host_io_mem->addr + prp);

        if ((!(core.run_flag & RUN_TESTS) && (uint64_t) host_ptr >
                                            core.nvm_pcie->host_io_mem->addr +
                                            core.nvm_pcie->host_io_mem->size) ||
                                            nvm_memcheck(src))
            goto ERR;
        else {
            memcpy (host_ptr, src, size);
            return NVME_SUCCESS;
        }
    }

ERR:
    log_err("[ERR nvme: write_to_host Invalid PRP addr. "
                    "host_ptr: %p, src: %p, prp: %p]\n", host_ptr, src, prp);
    return NVME_INVALID_FIELD;
}

inline uint8_t nvme_read_from_host(void *dest, uint64_t prp, ssize_t size)
{
    void *host_ptr = NULL;
    if (prp) {
        host_ptr = (core.run_flag & RUN_TESTS) ?
              (void *) prp : (void *) (core.nvm_pcie->host_io_mem->addr + prp);

        if ((!(core.run_flag & RUN_TESTS) && (uint64_t) host_ptr >
                                            core.nvm_pcie->host_io_mem->addr +
                                            core.nvm_pcie->host_io_mem->size) ||
                                            nvm_memcheck(dest))
            goto ERR;
        else {
            memcpy (dest, host_ptr, size);
            return NVME_SUCCESS;
        }
    }

ERR:
    log_err("[ERR nvme: read_from_host Invalid PRP addr. "
                    "host_ptr: %p, dest: %p, prp: %p]\n", host_ptr, dest, prp);
    return NVME_INVALID_FIELD;
}

uint16_t nvme_identify (NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeNamespace *ns;
    NvmeIdentify *c = (NvmeIdentify *)cmd;
    NvmeIdCtrl *id = &n->id_ctrl;
    uint32_t cns  = c->cns;
    uint32_t nsid = c->nsid;
    uint64_t prp1 = c->prp1;
    uint32_t ns_list[1024];

    switch (cns) {
        case 1:
            if (prp1)
                return nvme_write_to_host(id, prp1, sizeof (NvmeIdCtrl));
            break;
        case 0:
            if (nsid == 0 || nsid > n->num_namespaces)
                return NVME_INVALID_NSID | NVME_DNR;

            ns = &n->namespaces[nsid - 1];
            if (prp1)
                return nvme_write_to_host(&ns->id_ns, prp1, sizeof(NvmeIdNs));
            break;
        case 2:
            if (nsid == 0xfffffffe || nsid == 0xffffffff)
                return NVME_INVALID_NSID | NVME_DNR;

            /* For now, only 1 valid namespace */
            if (nsid == 0)
                ns_list[0] = 0x1;

            if (prp1)
                return nvme_write_to_host(&ns_list, prp1, 4096);
            break;
        default:
            return NVME_INVALID_NSID | NVME_DNR;
    }

    return NVME_INVALID_NSID | NVME_DNR;
}

uint16_t nvme_del_sq (NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeDeleteQ *c = (NvmeDeleteQ *)cmd;
    NvmeRequest *req;
    NvmeSQ *sq;
    NvmeCQ *cq;
    uint16_t qid = c->qid;

    if (!qid || nvme_check_sqid (n, qid)) {
	return NVME_INVALID_QID | NVME_DNR;
    }

    sq = n->sq[qid];
    TAILQ_FOREACH (req, &sq->out_req_list, entry) {
    	// TODO: handle out_req_list
    }
    if (!nvme_check_cqid (n, sq->cqid)) {
    	cq = n->cq[sq->cqid];
	pthread_mutex_lock(&n->req_mutex);
	TAILQ_REMOVE (&cq->sq_list, sq, entry);
	pthread_mutex_unlock(&n->req_mutex);

	nvme_post_cqes (cq);
	pthread_mutex_lock(&n->req_mutex);
        TAILQ_FOREACH (req, &cq->req_list, entry) {
            if (req->sq == sq) {
                TAILQ_REMOVE (&cq->req_list, req, entry);
                TAILQ_INSERT_TAIL (&sq->req_list, req, entry);
                if (cq->hold_sqs) cq->hold_sqs = 0;
            }
        }
        pthread_mutex_unlock(&n->req_mutex);
    }
    n->qsched.SQID[(qid - 1) >> 5] &= (~(1UL << ((qid - 1) & 31)));
    n->qsched.prio_avail[sq->prio] = n->qsched.prio_avail[sq->prio] - 1;
    n->qsched.mask_regs[sq->prio][(qid - 1) >> 6] &=
                                (~(1UL << ((qid - 1) & (SHADOW_REG_SZ - 1))));
    n->qsched.shadow_regs[sq->prio][(qid -1) >> 6] &=
                                (~(1UL << ((qid - 1) & (SHADOW_REG_SZ - 1))));
    n->qsched.n_active_iosqs--;

    nvme_free_sq (sq, n);
    return NVME_SUCCESS;
}

uint16_t nvme_create_sq (NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeSQ *sq;
    NvmeCreateSq *c = (NvmeCreateSq *)cmd;

    uint16_t cqid = c->cqid;
    uint16_t sqid = c->sqid;
    uint16_t qsize = c->qsize;
    uint16_t qflags = c->sq_flags;
    uint64_t prp1 = c->prp1;

    if (!cqid || nvme_check_cqid (n, cqid)) {
	return NVME_INVALID_CQID | NVME_DNR;
    }
    if (!sqid || (sqid && !nvme_check_sqid (n, sqid))) {
	return NVME_INVALID_QID | NVME_DNR;
    }
    if (!qsize || qsize > NVME_CAP_MQES(n->nvme_regs.vBar.cap)) {
	return NVME_MAX_QSIZE_EXCEEDED | NVME_DNR;
    }
    if (!prp1 || prp1 & (n->page_size - 1)) {
	return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (!(NVME_SQ_FLAGS_PC(qflags)) && NVME_CAP_CQR(n->nvme_regs.vBar.cap)) {
	return NVME_INVALID_FIELD | NVME_DNR;
    }

    sq = calloc (1, sizeof (NvmeSQ));

    if (nvme_init_sq (sq, n, prp1, sqid, cqid, qsize + 1, \
			NVME_SQ_FLAGS_QPRIO(qflags), \
			NVME_SQ_FLAGS_PC(qflags))) {
        FREE_VALID (sq);
	return NVME_INVALID_FIELD | NVME_DNR;
    }

    sq->prio = NVME_SQ_FLAGS_QPRIO(qflags);
    n->qsched.SQID[(sqid - 1) >> 5] |= (1UL << ((sqid - 1) & 31));
    n->qsched.mask_regs[sq->prio][(sqid - 1) >> 6] |=
                            (1UL << ((sqid - 1) & (SHADOW_REG_SZ - 1)));
    n->qsched.n_active_iosqs++;
    n->qsched.prio_avail[sq->prio] = n->qsched.prio_avail[sq->prio] + 1;
    n->stat.num_active_queues += 1;

    return NVME_SUCCESS;
}

uint16_t nvme_del_cq (NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeDeleteQ *c = (NvmeDeleteQ *)cmd;
    NvmeCQ *cq;
    uint16_t qid = c->qid;

    if (!qid || nvme_check_cqid (n, qid)) {
        return NVME_INVALID_CQID | NVME_DNR;
    }

    cq = n->cq[qid];
    if (!TAILQ_EMPTY (&cq->sq_list)) {
	return NVME_INVALID_QUEUE_DEL;
    }
    nvme_free_cq (cq, n);
    return NVME_SUCCESS;
}

uint16_t nvme_create_cq (NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeCQ *cq;
    NvmeCreateCq *c = (NvmeCreateCq *)cmd;

    uint16_t cqid = c->cqid;
    uint16_t vector = c->irq_vector;
    uint16_t qsize = c->qsize;
    uint16_t qflags = c->cq_flags;
    uint64_t prp1 = c->prp1;

    if (!cqid || (cqid && !nvme_check_cqid (n, cqid))) {
	return NVME_INVALID_CQID | NVME_DNR;
    }
    if (!qsize || qsize > NVME_CAP_MQES(n->nvme_regs.vBar.cap)) {
	return NVME_MAX_QSIZE_EXCEEDED | NVME_DNR;
    }
    if (!prp1) {
	return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (vector > n->num_queues) {
	return NVME_INVALID_IRQ_VECTOR | NVME_DNR;
    }
    if (!(NVME_CQ_FLAGS_PC(qflags)) && NVME_CAP_CQR(n->nvme_regs.vBar.cap)) {
	return NVME_INVALID_FIELD | NVME_DNR;
    }

    cq = calloc (1, sizeof (*cq));

    if (nvme_init_cq (cq, n, prp1, cqid, vector, qsize + 1,
		NVME_CQ_FLAGS_IEN(qflags), NVME_CQ_FLAGS_PC(qflags))) {
    	FREE_VALID (cq);
	return NVME_INVALID_FIELD | NVME_DNR;
    }
    return NVME_SUCCESS;
}

uint16_t nvme_set_feature (NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    uint32_t dw10 = cmd->cdw10;
    uint32_t dw11 = cmd->cdw11;
    uint32_t nsid = cmd->nsid;

    switch (dw10) {
        case NVME_ARBITRATION:
            req->cqe.n.result = htole32(n->features.arbitration);
            n->features.arbitration = dw11;
            break;
	case NVME_POWER_MANAGEMENT:
            n->features.power_mgmt = dw11;
            break;
	case NVME_LBA_RANGE_TYPE:
            if (nsid == 0 || nsid > n->num_namespaces) {
                return NVME_INVALID_NSID | NVME_DNR;
            }
            return NVME_SUCCESS;
	case NVME_NUMBER_OF_QUEUES:
            if ((dw11 < 1) || (dw11 > NVME_MAX_QUEUE_ENTRIES)) {
		req->cqe.n.result =
                            htole32 (n->num_queues | (n->num_queues << 16));
            } else {
		req->cqe.n.result = htole32(dw11 | (dw11 << 16));
		n->num_queues = dw11;
            }
            break;
	case NVME_TEMPERATURE_THRESHOLD:
            n->features.temp_thresh = dw11;
            if (n->features.temp_thresh <= n->temperature
                                                    && !n->temp_warn_issued) {
                n->temp_warn_issued = 1;
		nvme_enqueue_event (n, NVME_AER_TYPE_SMART,
                            		NVME_AER_INFO_SMART_TEMP_THRESH,
					NVME_LOG_SMART_INFO);
            } else if (n->features.temp_thresh > n->temperature &&
		!(n->aer_mask & 1 << NVME_AER_TYPE_SMART)) {
		n->temp_warn_issued = 0;
            }
            break;
        case NVME_ERROR_RECOVERY:
            n->features.err_rec = dw11;
            break;
	case NVME_VOLATILE_WRITE_CACHE:
            n->features.volatile_wc = dw11;
            break;
	case NVME_INTERRUPT_COALESCING:
            n->features.int_coalescing = dw11;
            break;
	case NVME_INTERRUPT_VECTOR_CONF:
            if ((dw11 & 0xffff) > n->num_queues) {
                return NVME_INVALID_FIELD | NVME_DNR;
            }
            n->features.int_vector_config[dw11 & 0xffff] = dw11 & 0x1ffff;
            break;
	case NVME_WRITE_ATOMICITY:
            n->features.write_atomicity = dw11;
            break;
	case NVME_ASYNCHRONOUS_EVENT_CONF:
            n->features.async_config = dw11;
            break;
	case NVME_SOFTWARE_PROGRESS_MARKER:
            n->features.sw_prog_marker = dw11;
            break;
	default:
            return NVME_INVALID_FIELD | NVME_DNR;
    }
    return NVME_SUCCESS;
}

uint16_t nvme_get_feature (NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    uint32_t dw10 = cmd->cdw10;
    uint32_t dw11 = cmd->cdw11;
    uint32_t nsid = cmd->nsid;

    switch (dw10) {
	case NVME_ARBITRATION:
            req->cqe.n.result = htole32(n->features.arbitration);
            break;
	case NVME_POWER_MANAGEMENT:
            req->cqe.n.result = htole32(n->features.power_mgmt);
            break;
	case NVME_LBA_RANGE_TYPE:
            if (nsid == 0 || nsid > n->num_namespaces) {
		return NVME_INVALID_NSID | NVME_DNR;
            }
            return NVME_SUCCESS;
	case NVME_NUMBER_OF_QUEUES:
            req->cqe.n.result = htole32(n->num_queues | (n->num_queues << 16));
            break;
        case NVME_TEMPERATURE_THRESHOLD:
            req->cqe.n.result = htole32(n->features.temp_thresh);
            break;
	case NVME_ERROR_RECOVERY:
            req->cqe.n.result = htole32(n->features.err_rec);
            break;
	case NVME_VOLATILE_WRITE_CACHE:
            req->cqe.n.result = htole32(n->features.volatile_wc);
            break;
	case NVME_INTERRUPT_COALESCING:
            req->cqe.n.result = htole32(n->features.int_coalescing);
            break;
	case NVME_INTERRUPT_VECTOR_CONF:
            if ((dw11 & 0xffff) > n->num_queues) {
                return NVME_INVALID_FIELD | NVME_DNR;
            }
            req->cqe.n.result = htole32(
            n->features.int_vector_config[dw11 & 0xffff]);
            break;
	case NVME_WRITE_ATOMICITY:
            req->cqe.n.result = htole32(n->features.write_atomicity);
            break;
        case NVME_ASYNCHRONOUS_EVENT_CONF:
            req->cqe.n.result = htole32(n->features.async_config);
            break;
        case NVME_SOFTWARE_PROGRESS_MARKER:
            req->cqe.n.result = htole32(n->features.sw_prog_marker);
            break;
	default:
            return NVME_INVALID_FIELD | NVME_DNR;
    }
    return NVME_SUCCESS;
}

static uint16_t nvme_error_log_info (NvmeCtrl *n, NvmeCmd *cmd)
{
    uint64_t prp1 = cmd->prp1;
    n->aer_mask &= ~(1 << NVME_AER_TYPE_ERROR);
    if(prp1)
        return nvme_write_to_host(n->elpes, prp1, sizeof (NvmeErrorLog));
    return NVME_SUCCESS;
}

static uint16_t nvme_smart_info (NvmeCtrl *n, NvmeCmd *cmd, uint32_t buf_len)
{
    uint64_t prp1 = cmd->prp1;
    time_t current_seconds;
    int Rtmp = 0,Wtmp = 0;
    int read = 0,write = 0;
    NvmeSmartLog smart;

    memset (&smart, 0x0, sizeof (smart));
    Rtmp = n->stat.nr_bytes_read/1000;
    Wtmp = n->stat.nr_bytes_written/1000;

    read = n->stat.nr_bytes_read%1000;
    write = n->stat.nr_bytes_written%1000;

    read = (read >= 500)?1:0;
    write = (write >=500)?1:0;

    n->stat.nr_bytes_read = Rtmp + read;
    n->stat.nr_bytes_written = Wtmp + write;

    smart.data_units_read[0] = htole64(n->stat.nr_bytes_read);
    smart.data_units_written[0] = htole64(n->stat.nr_bytes_written);
    smart.host_read_commands[0] = htole64(n->stat.tot_num_ReadCmd);
    smart.host_write_commands[0] = htole64(n->stat.tot_num_WriteCmd);
    smart.number_of_error_log_entries[0] = htole64(n->num_errors);
    smart.temperature[0] = n->temperature & 0xff;
    smart.temperature[1] = (n->temperature >> 8) & 0xff;

    current_seconds = time (NULL);
    smart.power_on_hours[0] = htole64(
                                ((current_seconds - n->start_time) / 60) / 60);

    smart.available_spare_threshold = NVME_SPARE_THRESHOLD;
    if (smart.available_spare <= NVME_SPARE_THRESHOLD) {
	smart.critical_warning |= NVME_SMART_SPARE;
    }
    if (n->features.temp_thresh <= n->temperature) {
	smart.critical_warning |= NVME_SMART_TEMPERATURE;
    }

    n->aer_mask &= ~(1 << NVME_AER_TYPE_SMART);
    NvmeSmartLog *smrt = &smart;
    if(prp1)
        return nvme_write_to_host(smrt, prp1, sizeof (NvmeSmartLog));
    return NVME_SUCCESS;
}

static inline uint16_t nvme_fw_log_info (NvmeCtrl *n, NvmeCmd *cmd,
                                                            uint32_t buf_len)
{
    uint32_t trans_len;
    uint64_t prp1 = cmd->prp1;
    uint64_t prp2 = cmd->prp2;
    NvmeFwSlotInfoLog fw_log;

    /* NOT IMPLEMENTED, TODO */
    return NVME_SUCCESS;
}

inline uint16_t nvme_get_log(NvmeCtrl *n, NvmeCmd *cmd)
{
    uint32_t dw10 = cmd->cdw10;
    uint16_t lid = dw10 & 0xffff;
    uint32_t len = ((dw10 >> 16) & 0xff) << 2;

    switch (lid) {
	case NVME_LOG_ERROR_INFO:
            return nvme_error_log_info (n, cmd);
	case NVME_LOG_SMART_INFO:
            return nvme_smart_info (n, cmd, len);
	case NVME_LOG_FW_SLOT_INFO:
            return nvme_fw_log_info (n, cmd, len);
	default:
            return NVME_INVALID_LOG_ID | NVME_DNR;
    }
}

uint16_t nvme_abort_req (NvmeCtrl *n, NvmeCmd *cmd, uint32_t *result)
{
    uint32_t index = 0;
    uint16_t sqid = cmd->cdw10 & 0xffff;
    uint16_t cid = (cmd->cdw10 >> 16) & 0xffff;
    NvmeSQ *sq;
    NvmeRequest *req;

    *result = 1;
    if (nvme_check_sqid (n, sqid)) {
	return NVME_SUCCESS;
    }

    sq = n->sq[sqid];
    TAILQ_FOREACH (req, &sq->out_req_list, entry) {
	if (sq->sqid) {
            if (req->cqe.cid == cid) {
                *result = 0;
		return NVME_SUCCESS;
            }
	}
    }

    while ((sq->head + index) % sq->size != sq->tail) {
        NvmeCmd abort_cmd;
	uint64_t addr;

	if (sq->phys_contig)
            addr = sq->dma_addr + ((sq->head + index) % sq->size) * n->sqe_size;

        nvme_addr_read (n, addr, (void *)&abort_cmd, sizeof (abort_cmd));
	if (abort_cmd.cid == cid) {
            *result = 0;
            pthread_mutex_lock(&n->req_mutex);
            req = (NvmeRequest *)TAILQ_FIRST (&sq->req_list);
            TAILQ_REMOVE (&sq->req_list, req, entry);
            TAILQ_INSERT_TAIL (&sq->out_req_list, req, entry);
            pthread_mutex_unlock(&n->req_mutex);

            memset (&req->cqe, 0, sizeof (req->cqe));
            req->cqe.cid = cid;
            req->status = NVME_CMD_ABORT_REQ;

            abort_cmd.opcode = NVME_OP_ABORTED;
            nvme_addr_write (n, addr, (void *)&abort_cmd, sizeof (abort_cmd));

            nvme_enqueue_req_completion (n->cq[sq->cqid], req);
            return NVME_SUCCESS;
	}

	++index;
    }
    return NVME_SUCCESS;
}

static uint16_t nvme_format_namespace (NvmeNamespace *ns, uint8_t lba_idx,
		uint8_t meta_loc, uint8_t pil, uint8_t pi, uint8_t sec_erase)
{
    uint64_t blks;
    uint16_t ms = ns->id_ns.lbaf[lba_idx].ms;
    NvmeCtrl *n = ns->ctrl;

    if (lba_idx > ns->id_ns.nlbaf) {
	return NVME_INVALID_FORMAT | NVME_DNR;
    }
    if (pi) {
	if (pil && !NVME_ID_NS_DPC_LAST_EIGHT(ns->id_ns.dpc)) {
            return NVME_INVALID_FORMAT | NVME_DNR;
	}
	if (!pil && !NVME_ID_NS_DPC_FIRST_EIGHT(ns->id_ns.dpc)) {
            return NVME_INVALID_FORMAT | NVME_DNR;
	}
	if (!((ns->id_ns.dpc & 0x7) & (1 << (pi - 1)))) {
            return NVME_INVALID_FORMAT | NVME_DNR;
	}
    }
    if (meta_loc && ms && !NVME_ID_NS_MC_EXTENDED(ns->id_ns.mc)) {
	return NVME_INVALID_FORMAT | NVME_DNR;
    }
    if (!meta_loc && ms && !NVME_ID_NS_MC_SEPARATE(ns->id_ns.mc)) {
    	return NVME_INVALID_FORMAT | NVME_DNR;
    }

    FREE_VALID (ns->util);
    FREE_VALID (ns->uncorrectable);
    blks = ns->ctrl->ns_size[ns->id - 1] / ((1 << ns->id_ns.lbaf[lba_idx].ds)
                                                            + ns->ctrl->meta);
    ns->id_ns.flbas = lba_idx | meta_loc;
    ns->id_ns.nsze = htole64(blks);
    ns->id_ns.ncap = ns->id_ns.nsze;
    ns->id_ns.nuse = ns->id_ns.nsze;
    ns->id_ns.dps = pil | pi;

/* TODO: restart lnvm tbls */
/*
    if (lightnvm_dev(n)) {
        ns->tbl_dsk_start_offset = ns->start_block;
        ns->tbl_entries = blks;
        if (ns->tbl) {
            free(ns->tbl);
        }
        ns->tbl = calloc(1, lightnvm_tbl_size(ns));
        lightnvm_tbl_initialize(ns);
    } else {
        ns->tbl = NULL;
        ns->tbl_entries = 0;
    }
 */

    if (sec_erase) {
	/* TODO: write zeros, complete asynchronously */;
    }

    return NVME_SUCCESS;
}

uint16_t nvme_format (NvmeCtrl *n, NvmeCmd *cmd)
{
    /* TODO: Not completely implemented */

    NvmeNamespace *ns;
    uint32_t dw10 = cmd->cdw10;
    uint32_t nsid = cmd->nsid;

    uint8_t lba_idx = dw10 & 0xf;
    uint8_t meta_loc = dw10 & 0x10;
    uint8_t pil = (dw10 >> 5) & 0x8;
    uint8_t pi = (dw10 >> 5) & 0x7;
    uint8_t sec_erase = (dw10 >> 8) & 0x7;

    if (nsid == 0xffffffff) {
	uint32_t i;
	uint16_t ret = NVME_SUCCESS;

    	for (i = 0; i < n->num_namespaces; ++i) {
            ns = &n->namespaces[i];
            ret = nvme_format_namespace (ns,lba_idx,meta_loc,pil,pi,sec_erase);
            if (ret != NVME_SUCCESS) {
		return ret;
            }
	}
	return ret;
    }

    if (nsid == 0 || nsid > n->num_namespaces)
        return NVME_INVALID_NSID | NVME_DNR;

    ns = &n->namespaces[nsid - 1];
    return nvme_format_namespace (ns, lba_idx, meta_loc, pil, pi, sec_erase);
}

uint16_t nvme_async_req (NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
/*
Asynchronous events are used to notify host software of status, error, and
health information as these events occur. To enable asynchronous events to be
reported by the controller, host software needs to submit one or more
Asynchronous Event Request commands to the controller. The controller specifies
an event to the host by completing an Asynchronous Event Request command. Host
software should expect that the controller may not execute the command
immediately; the command should be completed when there is an event to be
reported. The Asynchronous Event Request command is submitted by host software
to enable the reporting of asynchronous events from the controller. This
command has no timeout. The controller posts a completion queue entry for this
command when there is an asynchronous event to report to the host. If
Asynchronous Event Request commands are outstanding when the controller is
reset, the commands are aborted.
*/
    /* TODO: Not implemented yet */

    return NVME_NO_COMPLETE;
}

uint16_t nvme_write_uncor(NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
		NvmeRequest *req)
{
/*
     The Write Uncorrectable command is used to mark a range of logical blocks
     as invalid. When the specified logical block(s) are read after this
     operation, a failure is returned with Unrecovered Read Error status. To
     clear the invalid logical block status, a write operation is performed on
     those logical blocks. The fields used are Command Dword 1
*/
    /* TODO: Not implemented yet */

    return NVME_SUCCESS;
}

uint16_t nvme_dsm (NvmeCtrl *n,NvmeNamespace *ns,NvmeCmd *cmd,NvmeRequest *req)
{
/*
 The Dataset Management command is used by the host to indicate attributes for
 ranges of logical blocks. This includes attributes like frequency that data
 is read or written, access size, and other information that may be used to
 optimize performance and reliability. This command is advisory; a compliant
 controller may choose to take no action based on information provided.
 The command uses Command Dword 10, and Command Dword 11 fields. If the command
 uses PRPs for the data transfer, then the PRP Entry 1 and PRP Entry 2 fields
 are used. If the command uses SGLs for the data transfer, then the SGL Entry 1
 field is used. All other command specific fields are reserved.
 */
    /* TODO: Not implemented yet */

    return NVME_SUCCESS;
}

uint16_t nvme_flush(NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
		NvmeRequest *req)
{
/*
 In case of volatile write cache is enable, flush is used to store the current
 data in the cash to non-volatile memory.
 */
    /* TODO: Not Implemented yet */

    return NVME_SUCCESS;
}

uint16_t nvme_compare(NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
    NvmeRequest *req)
{
/*
The Compare command reads the logical blocks specified by the command from the
medium and compares the data read to a comparison data buffer transferred as
part of the command. If the data read from the controller and the comparison
data buffer are equivalent with no miscompares, then the command completes
successfully. If there is any miscompare, the command completes with an error
of Compare Failure. If metadata is provided, then a comparison is also performed
for the metadata, excluding protection information. Refer to section 8.3. The
command uses Command Dword 10, Command Dword 11, Command Dword 12, Command
Dword 14, and Command Dword 15 fields. If the command uses PRPs for the data
transfer, then the Metadata Pointer, PRP Entry 1, and PRP Entry 2 fields are
used. If the command uses SGLs for the data transfer, then the Metadata SGL
Segment Pointer and SGL Entry 1 fields are used. All other command specific
fields are reserved.
*/
    /* TODO: Not implemented yet */

    return NVME_SUCCESS;
}

uint16_t nvme_write_zeros(NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
    NvmeRequest *req)
{
/*
The Write Zeroes command is used to set a range of logical blocks to zero.
After successful completion of this command, the value returned by subsequent
reads of logical blocks in this range shall be zeroes until a write occurs to
this LBA range. The metadata for this command shall be all zeroes and the
protection information is updated based on CDW12.PRINFO. The fields used are
Command Dword 10, Command Dword 11, Command Dword 12, Command Dword 14, and
Command Dword 15 fields
*/
    /* TODO: Not implemented yet */

    return NVME_SUCCESS;
}

uint16_t nvme_rw (NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                                                             NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    int off_count, i;
    uint16_t ctrl = rw->control;
    uint32_t nlb  = rw->nlb + 1;
    uint64_t slba = rw->slba;
    uint64_t prp1 = rw->prp1;
    uint64_t prp2 = rw->prp2;

    const uint64_t elba = slba + nlb;
    const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint8_t data_shift = ns->id_ns.lbaf[lba_index].ds;
    const uint16_t ms = ns->id_ns.lbaf[lba_index].ms;
    uint64_t data_size = nlb << data_shift;
    uint64_t meta_size = nlb * ms;
    uint64_t aio_slba =
	ns->start_block + (slba << (data_shift - BDRV_SECTOR_BITS));

    req->nvm_io.status.status = NVM_IO_NEW;

    req->is_write = rw->opcode == NVME_CMD_WRITE;
    if (elba > (ns->id_ns.nsze)) {
    	nvme_set_error_page (n, req->sq->sqid, cmd->cid, NVME_LBA_RANGE,
                                	offsetof(NvmeRwCmd, nlb), elba, ns->id);
	return NVME_LBA_RANGE | NVME_DNR;
    }
    if (n->id_ctrl.mdts && data_size > n->page_size * (1 << n->id_ctrl.mdts)) {
	nvme_set_error_page (n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
				offsetof(NvmeRwCmd, nlb), nlb, ns->id);
	return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (meta_size) {
	nvme_set_error_page (n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
				offsetof(NvmeRwCmd, control), ctrl, ns->id);
	return NVME_INVALID_FIELD | NVME_DNR;
    }
    if ((ctrl & NVME_RW_PRINFO_PRACT) && !(ns->id_ns.dps & DPS_TYPE_MASK)) {
	nvme_set_error_page (n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
			offsetof(NvmeRwCmd, control), ctrl, ns->id);
	return NVME_INVALID_FIELD | NVME_DNR;
    }

    /* TODO: Map PRPs for non-sequential prp addresses */
#if 0
    if (nvme_map_prp (req, prp1, prp2, data_size, n)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
#endif /* MAP PRP */

    req->slba = aio_slba;
    req->meta_size = 0;
    req->status = NVME_SUCCESS;
    req->nlb = nlb;
    req->ns = ns;
    req->lba_index = lba_index;
    off_count = (aio_slba % (NVME_KERNEL_PG_SIZE/(1<<data_shift)));

    req->nvm_io.sec_sz = NVME_KERNEL_PG_SIZE;
    req->nvm_io.channel = 0; /* */
    req->nvm_io.cmdtype = (req->is_write) ? MMGR_WRITE_PG : MMGR_READ_PG;
    req->nvm_io.n_sec = nlb;
    req->nvm_io.prp[0] = prp1;
    req->nvm_io.req = (void *) req;
    req->nvm_io.slba = aio_slba;
    req->nvm_io.sec_offset = (off_count * (1<<data_shift));
    req->nvm_io.status.pg_errors = 0;
    req->nvm_io.status.ret_t = 0;
    req->nvm_io.status.total_pgs = 0;

    for (i = 0; i < 8; i++) {
        req->nvm_io.status.pg_map[i] = 0;
    }

    for (i = 0; i < 64; i++) {
        req->nvm_io.mmgr_io[i].pg_index = i;
        req->nvm_io.mmgr_io[i].nvm_io = &req->nvm_io;
        req->nvm_io.mmgr_io[i].pg_sz = NVME_KERNEL_PG_SIZE;
    }

    return nvm_submit_io(&req->nvm_io);
}