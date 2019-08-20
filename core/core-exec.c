/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX Controller Core Execution
 *
 * Copyright 2016 IT University of Copenhagen
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
 * The controller core is responsible to handle:
 *
 *      - Media managers: NAND, DDR, OCSSD, etc.
 *      - Flash Translation Layers + LightNVM raw FTL support
 *      - NVMe Transports: PCIe, network fabric, etc.
 *      - NVMe, NVMe over Fabrics and LightNVM Standards
 *      - RDMA handler: TCP, RoCE, InfiniBand, etc.
 */

#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/queue.h>
#include <syslog.h>
#include <mqueue.h>
#include <pthread.h>
#include <sys/time.h>
#include <nvme.h>
#include <libox.h>
#include <ox-mq.h>

extern struct core_struct core;

int ox_contains_ppa (struct nvm_ppa_addr *list, uint32_t list_sz,
                                                        struct nvm_ppa_addr ppa)
{
    int i;

    if (!list)
        return 0;

    for (i = 0; i < list_sz; i++)
        if (ppa.ppa == list[i].ppa)
            return 1;

    return 0;
}

inline static void ox_complete_request (NvmeRequest *req)
{
    if (core.std_transport == NVM_TRANSP_FABRICS)
        nvmef_complete_request (req);
    else
        nvme_rw_cb (req);
}

void ox_ftl_process_sq (struct ox_mq_entry *req)
{
    struct nvm_io_cmd *cmd = (struct nvm_io_cmd *) req->opaque;
    struct nvm_ftl *ftl = cmd->channel[0]->ftl;
    int ret, retry;

    cmd->mq_req = (void *) req;

    retry = 1;//NVM_QUEUE_RETRY;
    do {
        ret = ftl->ops->submit_io(cmd);
        if (ret) {
            //retry--;
            //usleep (NVM_QUEUE_RETRY_SLEEP);
            if (retry) {
                cmd->status.nvme_status = NVME_SUCCESS;
                cmd->status.status = NVM_IO_PROCESS;
            }
        }
    } while (ret && retry);

    if (ret) {
        if (core.debug)
            log_err ("[ftl: Cmd %lu (0x%x) NOT completed. ret %d]\n",
                                                   cmd->cid,cmd->cmdtype, ret);
        ox_ftl_callback (cmd);
    }
}

void ox_ftl_process_cq (void *opaque)
{
    struct nvm_io_cmd *cmd = (struct nvm_io_cmd *) opaque;
    NvmeRequest *req = (NvmeRequest *) cmd->req;

    req->status = (cmd->status.status == NVM_IO_SUCCESS) ?
                NVME_SUCCESS : (cmd->status.nvme_status) ?
                      cmd->status.nvme_status : NVME_CMD_ABORT_REQ;

    if (core.debug)
        printf(" [NVMe cmd 0x%x. cid: %d completed. Status: %x]\n",
                                   req->cmd.opcode, req->cmd.cid, req->status);

    ox_complete_request (req);
}

void ox_ftl_process_to (void **opaque, int counter)
{
    struct nvm_io_cmd *cmd;

    while (counter) {
        counter--;
        cmd = (struct nvm_io_cmd *) opaque[counter];
        cmd->status.status = NVM_IO_TIMEOUT;
        cmd->status.nvme_status = NVME_MEDIA_TIMEOUT;
    }
}

static void nvm_sync_io_free (uint8_t flags, void *buf,
                                                   struct nvm_mmgr_io_cmd *cmd)
{
    if (flags & NVM_SYNCIO_FLAG_DEC) {
        pthread_mutex_lock(cmd->sync_mutex);
        u_atomic_dec(cmd->sync_count);
        pthread_mutex_unlock(cmd->sync_mutex);
    }

    if (flags & NVM_SYNCIO_FLAG_BUF) {
        ox_free (buf, OX_MEM_CORE_EXEC);
    }

    if (flags & NVM_SYNCIO_FLAG_SYNC) {
        pthread_mutex_destroy (cmd->sync_mutex);
        ox_free (cmd->sync_count, OX_MEM_CORE_EXEC);
        ox_free (cmd->sync_mutex, OX_MEM_CORE_EXEC);
    }
}

static void nvm_debug_print_mmgr_io (struct nvm_mmgr_io_cmd *cmd)
{
    printf (" [IO CALLBK. CMD 0x%x. mmgr_ch: %d, lun: %d, blk: %d, pl: %d, "
        "pg: %d -> Status: %d]\n", cmd->cmdtype, cmd->ppa.g.ch, cmd->ppa.g.lun,
        cmd->ppa.g.blk, cmd->ppa.g.pl, cmd->ppa.g.pg, cmd->status);
}

void ox_mmgr_callback (struct nvm_mmgr_io_cmd *cmd)
{
    gettimeofday(&cmd->tend,NULL);

    if (core.debug)
        nvm_debug_print_mmgr_io (cmd);

    if (cmd->status != NVM_IO_SUCCESS)
        log_err (" [FAILED 0x%x CMD. mmgr_ch: %d, lun: %d, blk: %d, pl: %d, "
                "pg: %d]\n", cmd->cmdtype, cmd->ppa.g.ch, cmd->ppa.g.lun,
                cmd->ppa.g.blk, cmd->ppa.g.pl, cmd->ppa.g.pg);

    if (cmd->sync_count) {

        pthread_mutex_lock(cmd->sync_mutex);
        if (cmd->status != NVM_IO_TIMEOUT) {
            u_atomic_dec(cmd->sync_count);
        }
        pthread_mutex_unlock(cmd->sync_mutex);

    } else {

        if (cmd->cmdtype == MMGR_READ_PG)
            ox_stats_add_io (cmd, 0, (cmd->ch->geometry->lun_per_ch *
                                            cmd->ppa.g.ch) + cmd->ppa.g.lun);

        if (cmd->callback.cb_fn)
            cmd->callback.cb_fn (cmd->callback.opaque);
        else
            cmd->ch->ftl->ops->callback_io(cmd);
    }
}

void ox_ftl_callback (struct nvm_io_cmd *cmd)
{
    int retry, ret;
    struct ox_mq_entry *req = (struct ox_mq_entry *) cmd->mq_req;

    retry = NVM_QUEUE_RETRY;
    do {
        ret = ox_mq_complete_req(cmd->channel[0]->ftl->mq, req);
        if (ret) {
            retry--;
            usleep (NVM_QUEUE_RETRY_SLEEP);
        }
    } while (ret && retry);
}

static int nvm_sync_io_prepare (struct nvm_channel *ch,
                        struct nvm_mmgr_io_cmd *cmd, void *buf, uint8_t *flags)
{
    int i, mpl;

    if (!ch)
        return -1;

    cmd->ch = ch;

    mpl = (*flags & NVM_SYNCIO_FLAG_MPL) ? ch->geometry->n_of_planes : 1;
    if (!cmd->sync_count || !cmd->sync_mutex) {
        cmd->sync_count = ox_malloc (sizeof (u_atomic_t), OX_MEM_CORE_EXEC);
        if (!cmd->sync_count)
            return -1;
        cmd->sync_mutex = ox_malloc (sizeof (pthread_mutex_t),OX_MEM_CORE_EXEC);
        if (!cmd->sync_mutex) {
            ox_free (cmd->sync_count, OX_MEM_CORE_EXEC);
            return -1;
        }
        cmd->sync_count->counter = U_ATOMIC_INIT_RUNTIME(0);
        pthread_mutex_init (cmd->sync_mutex, NULL);
        *flags |= NVM_SYNCIO_FLAG_SYNC;
    }

    cmd->ppa.g.ch = ch->ch_mmgr_id;

    if (cmd->cmdtype == MMGR_ERASE_BLK)
        goto OUT;

    if (cmd->pg_sz == 0)
        cmd->pg_sz = ch->geometry->pg_size;
    if (cmd->sec_sz == 0)
        cmd->sec_sz = ch->geometry->pg_size / ch->geometry->sec_per_pg;
    if (cmd->md_sz == 0)
        cmd->md_sz = ch->geometry->sec_oob_sz * ch->geometry->sec_per_pg * mpl;
    if (cmd->n_sectors == 0)
        cmd->n_sectors = ch->geometry->sec_per_pg * mpl;

    if (!buf) {
        buf = ox_malloc((ch->geometry->pg_size + ch->geometry->sec_oob_sz) * mpl,
                                                            OX_MEM_CORE_EXEC);
        if (!buf) {
            nvm_sync_io_free (*flags, NULL, cmd);
            return -1;
        }
        *flags |= NVM_SYNCIO_FLAG_BUF;
    }

    if (cmd->cmdtype == MMGR_READ_SGL || cmd->cmdtype == MMGR_WRITE_SGL) {

        for (i = 0; i < cmd->n_sectors; i++)
            cmd->prp[i] = (uint64_t) ((uint8_t **) buf)[i];
        cmd->md_prp = (uint64_t) ((uint8_t **) buf)[cmd->n_sectors];
        cmd->cmdtype = (cmd->cmdtype == MMGR_READ_SGL) ?
                                                  MMGR_READ_PG : MMGR_WRITE_PG;
    } else {

        for (i = 0; i < cmd->n_sectors; i++)
            cmd->prp[i] = (uint64_t) buf + cmd->sec_sz * i;
        cmd->md_prp = (uint64_t) buf + cmd->sec_sz * cmd->n_sectors;

    }

OUT:
    return 0;
}

/**
 * Submit an IO to a specific channel and wait for completion to return.
 * Please, use nvm_submit_multi_plane_sync_io if you have planes > 1
 *
 * BE CAREFUL WHEN MULTIPLE THREADS USE THE SAME ATOMIC INT: If cmd->sync_count
 * and cmd->sync_mutex are not NULL, multiple threads can share the same
 * waiting loop. If so, all threads will return when all IOs are completed.
 *
 * @param ch - nvm_channel pointer
 * @param cmd - Media manager command pointer
 * @param buf - Pointer to dma data
 * @param cmdtype - MMGR_READ_PG, MMGR_WRITE_PG, MMGR_ERASE_BLK
 * @return 0 on success
 */
int ox_submit_sync_io (struct nvm_channel *ch, struct nvm_mmgr_io_cmd *cmd,
                                                    void *buf, uint8_t cmdtype)
{
    int ret = 0, i;
    uint8_t flags = 0;
    time_t start;
    struct nvm_mmgr *mmgr;
    char err[64];

    if (cmdtype == MMGR_WRITE_PL_PG || cmdtype == MMGR_READ_PL_PG) {
        flags |= NVM_SYNCIO_FLAG_MPL;
        cmdtype = (cmdtype == MMGR_WRITE_PL_PG) ? MMGR_WRITE_PG : MMGR_READ_PG;
        cmd->n_sectors = ch->geometry->sec_per_pg * ch->geometry->n_of_planes;
    }

    cmd->cmdtype = cmdtype;
    if (nvm_sync_io_prepare (ch, cmd, buf, &flags))
        goto ERR;

    mmgr = ch->mmgr;
    if (!mmgr)
        goto ERR;

    pthread_mutex_lock(cmd->sync_mutex);
    u_atomic_inc(cmd->sync_count);
    pthread_mutex_unlock(cmd->sync_mutex);
    flags |= NVM_SYNCIO_FLAG_DEC;

    cmd->status = NVM_IO_PROCESS;

    gettimeofday(&cmd->tstart,NULL);
    switch (cmd->cmdtype) {
        case MMGR_READ_PG:
            ret = mmgr->ops->read_pg(cmd);
            break;
        case MMGR_WRITE_PG:
            ox_stats_add_io (cmd, 1, 0);
            ret = mmgr->ops->write_pg(cmd);
            break;
        case MMGR_ERASE_BLK:
            ox_stats_add_io (cmd, 1, 0);
            ret = mmgr->ops->erase_blk(cmd);
            break;
        default:
            ret = -1;
    }

    if (core.debug)
        printf("[Sync IO: 0x%x ppa: ch %d, lun %d, blk %d, pl %d, pg %d]\n",
                cmd->cmdtype, cmd->ppa.g.ch, cmd->ppa.g.lun, cmd->ppa.g.blk,
                cmd->ppa.g.pl, cmd->ppa.g.pg);

    if (ret) {
        nvm_sync_io_free (flags, buf, cmd);
        goto ERR;
    }

    /* Concurrent threads with the same sync_counter wait others to complete */
    start = time(NULL);
    i = 1;
    do {
        if (time(NULL) > start + NVM_SYNCIO_TO) {
            cmd->status = NVM_IO_TIMEOUT;
            nvm_sync_io_free (flags, buf, cmd);
            log_err ("[nvm: Sync IO cmd 0x%x TIMEOUT. Aborted.]\n",cmd->cmdtype);
            return -1;
        }
        pthread_mutex_lock(cmd->sync_mutex);
        i = u_atomic_read(cmd->sync_count);
        pthread_mutex_unlock(cmd->sync_mutex);
        if (i)
            usleep(1);
    } while (i || cmd->status == NVM_IO_PROCESS);

    if (cmdtype == MMGR_READ_PG)
        ox_stats_add_io (cmd, 1, 0);

    ret = (cmd->status == NVM_IO_SUCCESS) ? 0 : -1;

    flags ^= NVM_SYNCIO_FLAG_DEC;
    nvm_sync_io_free (flags, buf, cmd);

    if (!ret) return ret;
ERR:
    sprintf(err, "[ERROR: Sync IO cmd 0x%x with errors. Aborted.]\n",
                                                                  cmd->cmdtype);
    log_err ("%s",err);
    if (core.debug) printf ("%s",err);

    return -1;
}

int ox_submit_mmgr (struct nvm_mmgr_io_cmd *cmd)
{
    gettimeofday(&cmd->tstart,NULL);
    cmd->cmdtype = cmd->nvm_io->cmdtype;
    int ret;

    switch (cmd->nvm_io->cmdtype) {
        case MMGR_WRITE_PG:
            ox_stats_add_io (cmd, 0, 0);
            return cmd->ch->mmgr->ops->write_pg(cmd);
        case MMGR_READ_PG:
            ret = cmd->ch->mmgr->ops->read_pg(cmd);
            return ret;
        case MMGR_ERASE_BLK:
            ox_stats_add_io (cmd, 0, 0);
            return cmd->ch->mmgr->ops->erase_blk(cmd);
        default:
            cmd->status = NVM_IO_FAIL;
            return -1;
    }
}

int ox_dma (void *ptr, uint64_t prp, ssize_t size, uint8_t direction)
{
    if (!size || !prp)
        return 0;

    switch (direction) {
        case NVM_DMA_TO_HOST:
        case NVM_DMA_FROM_HOST:

            if (core.std_transport == NVM_TRANSP_FABRICS)
                return core.nvm_fabrics->ops->rdma (ptr, size, prp, direction);
            else
                return core.nvm_pcie->ops->dma (ptr, size, prp, direction);

        case NVM_DMA_SYNC_READ:
            memcpy ((void *) prp, ptr, size);
            break;
        case NVM_DMA_SYNC_WRITE:
            memcpy (ptr, (void *) prp, size);
            break;
        default:
            return -1;
    }

    return 0;
}

static uint16_t ox_ftl_q_schedule (struct nvm_ftl *ftl,
                                      struct nvm_io_cmd *cmd, uint8_t multi_ch)
{
    uint16_t qid, ret;

    /* Separate writes and reads in different queues for OX-App FTL */
    if (ftl->ftl_id == FTL_ID_OXAPP) {
        qid = (cmd->cmdtype == MMGR_WRITE_PG) ? 0 : 1;

        pthread_spin_lock (&ftl->next_queue_spin[qid]);
        ftl->next_queue[qid] = (ftl->next_queue[qid] + 1 == ftl->nq / 2) ?
                                                  0 : ftl->next_queue[qid] + 1;
        ret = ftl->next_queue[qid] + (qid * (ftl->nq / 2));
        pthread_spin_unlock (&ftl->next_queue_spin[qid]);

        return ret;
    }

    if (!multi_ch)
        return cmd->channel[0]->ch_id % ftl->nq;
    else {
        pthread_spin_lock (&ftl->next_queue_spin[0]);
        qid = ftl->next_queue[0];
        ftl->next_queue[0] = (ftl->next_queue[0] + 1 == ftl->nq) ?
                                                    0 : ftl->next_queue[0] + 1;
        pthread_spin_unlock (&ftl->next_queue_spin[0]);
        return qid;
    }
}

int ox_submit_ftl (struct nvm_io_cmd *cmd)
{
    struct nvm_ftl *ftl;
    int ret, retry, qid, i;
    uint8_t ch_ppa[core.nvm_ch_count];

    uint8_t multi_ch = 0;
    NvmeRequest *req = (NvmeRequest *) cmd->req;

    cmd->status.nvme_status = NVME_SUCCESS;

    switch (cmd->status.status) {
        case NVM_IO_NEW:
            break;
        case NVM_IO_PROCESS:
            break;
        case NVM_IO_FAIL:
            req->status = NVME_CMD_ABORT_REQ;
            ox_complete_request (req);
            return NVME_CMD_ABORT_REQ;
        case NVM_IO_SUCCESS:
            req->status = NVME_SUCCESS;
            ox_complete_request (req);
            return NVME_SUCCESS;
        default:
            req->status = NVME_CMD_ABORT_REQ;
            ox_complete_request (req);
            return NVME_CMD_ABORT_REQ;
    }

    switch (core.std_ftl) {

        case FTL_ID_LNVM:
            /*For now, the host ppa channel must be aligned with core.nvm_ch[]*/
            /*PPAs in the vector must address channels managed by the same FTL*/
            if (cmd->ppalist[0].g.ch >= core.nvm_ch_count)
                goto CH_ERR;

            ftl = core.nvm_ch[cmd->ppalist[0].g.ch]->ftl;

            memset (ch_ppa, 0, sizeof (uint8_t) * core.nvm_ch_count);

            /* Per PPA checking */
            for (i = 0; i < cmd->n_sec; i++) {
                if (cmd->ppalist[i].g.ch >= core.nvm_ch_count)
                    goto CH_ERR;

                ch_ppa[cmd->ppalist[i].g.ch]++;

                if (!multi_ch && cmd->ppalist[i].g.ch != cmd->ppalist[0].g.ch)
                    multi_ch++;

                if (core.nvm_ch[cmd->ppalist[i].g.ch]->ftl != ftl)
                    goto FTL_ERR;

                /* Set channel per PPA */
                cmd->channel[i] = core.nvm_ch[cmd->ppalist[i].g.ch];
            }
            break;

        case FTL_ID_OXAPP:
        default:

            switch (core.std_oxapp) {
            case FTL_ID_ELEOS:
                cmd->channel[0] = core.nvm_ch[0];
                multi_ch++;
                ftl = ox_get_ftl_instance(core.std_ftl);
                break;

            case FTL_ID_BLOCK:
            default:
                /* All channels must be included in the global namespace */
                if ((cmd->slba > (core.nvm_ns_size / cmd->sec_sz)) ||
                    (cmd->slba + (cmd->sec_sz * cmd->n_sec) > core.nvm_ns_size))
                        goto RANGE_ERR;

                cmd->channel[0] = core.nvm_ch[cmd->slba /
                                      (core.nvm_ns_size / core.nvm_ch_count)];
                multi_ch++;
                ftl = ox_get_ftl_instance(core.std_ftl);
            }
    }

    cmd->status.status = NVM_IO_PROCESS;
    retry = NVM_QUEUE_RETRY;

    qid = ox_ftl_q_schedule (ftl, cmd, multi_ch);
    do {
        ret = ox_mq_submit_req(ftl->mq, qid, cmd);

        if (ret) {
            retry--;
            usleep (NVM_QUEUE_RETRY_SLEEP);
        }
        else if (core.debug) {
            printf(" CMD cid: %lu, type: 0x%x submitted to FTL. "
                               "FTL queue: %d\n", cmd->cid, cmd->cmdtype, qid);
            if (core.std_ftl == FTL_ID_LNVM) {
                for (i = 0; i < core.nvm_ch_count; i++)
                    if (ch_ppa[i] > 0)
                        printf("  Channel: %d, PPAs: %d\n", i, ch_ppa[i]);
            }
        }
    } while (ret && retry);

    return (retry) ? NVME_NO_COMPLETE : NVME_CMD_ABORT_REQ;

RANGE_ERR:
    syslog(LOG_INFO,"[ox ERROR: IO out of bounds.]\n");
    req->status = NVME_LBA_RANGE;
    ox_complete_request (req);
    return NVME_LBA_RANGE;

CH_ERR:
    syslog(LOG_INFO,"[ox ERROR: IO failed, channel not found.]\n");
    req->status = NVME_INVALID_FIELD;
    ox_complete_request (req);
    return NVME_INVALID_FIELD;

FTL_ERR:
    syslog(LOG_INFO,"[ox ERROR: IO failed, channels do not match FTL.]\n");
    req->status = NVME_INVALID_FIELD;
    ox_complete_request (req);
    return NVME_INVALID_FIELD;
}

static int nvm_ftl_cap_get_bbtbl (struct nvm_channel *ch,
                                          struct nvm_ftl_cap_get_bbtbl_st *arg)
{
    if (arg->nblk < 1)
        return -1;

    if (!ch->ftl->ops->get_bbtbl)
        return -1;

    if (ch->ftl->bbtbl_format == arg->bb_format)
        return ch->ftl->ops->get_bbtbl(&arg->ppa, arg->bbtbl, arg->nblk);

    return -1;
}

static int nvm_ftl_cap_set_bbtbl (struct nvm_channel *ch,
                                          struct nvm_ftl_cap_set_bbtbl_st *arg)
{
    if (!ch->ftl->ops->set_bbtbl)
        return -1;

    if (ch->ftl->bbtbl_format == arg->bb_format)
        return ch->ftl->ops->set_bbtbl(&arg->ppa, arg->value);

    return -1;
}

static int nvm_ftl_cap_init_fn (struct nvm_ftl *ftl,
                                                 struct nvm_ftl_cap_gl_fn *arg)
{
    if (!ftl->ops->init_fn)
        return -1;

    return ftl->ops->init_fn (arg->fn_id, arg->arg);
}

static int nvm_ftl_cap_exit_fn (struct nvm_ftl *ftl,
                                                 struct nvm_ftl_cap_gl_fn *arg)
{
    if (!ftl->ops->exit_fn)
        return -1;

    ftl->ops->exit_fn (arg->fn_id);

    return 0;
}

int ox_ftl_cap_exec (uint8_t cap, void *arg)
{
    struct nvm_channel *ch;
    struct nvm_ftl_cap_set_bbtbl_st *set_bbtbl;
    struct nvm_ftl_cap_get_bbtbl_st *get_bbtbl;
    struct nvm_ftl_cap_gl_fn        *gl_fn;
    struct nvm_ftl                  *ftl;

    if (!arg)
        return -1;

    switch (cap) {
        case FTL_CAP_GET_BBTBL:

            get_bbtbl = (struct nvm_ftl_cap_get_bbtbl_st *) arg;
            if (get_bbtbl->ppa.g.ch >= core.nvm_ch_count)
                goto OUT;
            ch = core.nvm_ch[get_bbtbl->ppa.g.ch];
            if (ch->ftl->cap & 1 << FTL_CAP_GET_BBTBL) {
                if (nvm_ftl_cap_get_bbtbl(ch, arg))
                    goto OUT;
                return 0;
            }
            break;

        case FTL_CAP_SET_BBTBL:

            set_bbtbl = (struct nvm_ftl_cap_set_bbtbl_st *) arg;
            if (set_bbtbl->ppa.g.ch >= core.nvm_ch_count)
                goto OUT;
            ch = core.nvm_ch[set_bbtbl->ppa.g.ch];
            if (ch->ftl->cap & 1 << FTL_CAP_SET_BBTBL) {
                if (nvm_ftl_cap_set_bbtbl(ch, arg))
                    goto OUT;
                return 0;
            }
            break;

        case FTL_CAP_GET_L2PTBL:
        case FTL_CAP_SET_L2PTBL:
        case FTL_CAP_INIT_FN:

            gl_fn = (struct nvm_ftl_cap_gl_fn *) arg;
            ftl = ox_get_ftl_instance(gl_fn->ftl_id);
            if (!ftl)
                goto OUT;
            if (ftl->cap & 1 << FTL_CAP_INIT_FN) {
                if (nvm_ftl_cap_init_fn(ftl, gl_fn))
                    goto OUT;
                return 0;
            }
            break;

        case FTL_CAP_EXIT_FN:

            gl_fn = (struct nvm_ftl_cap_gl_fn *) arg;
            ftl = ox_get_ftl_instance(gl_fn->ftl_id);
            if (!ftl)
                goto OUT;
            if (ftl->cap & 1 << FTL_CAP_EXIT_FN) {
                if (nvm_ftl_cap_exit_fn(ftl, gl_fn))
                    goto OUT;
                return 0;
            }
            break;

        default:
            goto OUT;
    }

OUT:
    log_err ("[ERROR: FTL capability: %x. Aborted.]\n", cap);
    return -1;
}

void ox_execute_opcode (uint16_t qid, NvmeRequest *req)
{
    struct nvm_parser_cmd *parser;
    NvmeCmd *cmd = &req->cmd;
    uint16_t status;
    char err[100];
    uint64_t cid;

    if (qid)
        core.nvme_ctrl->stat.tot_num_IOCmd += 1;
    else
        core.nvme_ctrl->stat.tot_num_AdminCmd += 1;

    cid = (qid) ? core.nvme_ctrl->stat.tot_num_IOCmd :
                  core.nvme_ctrl->stat.tot_num_AdminCmd;
    if (core.debug)
        printf ("\n[%lu: Q-%03d / CID-%05d / OP-0x%02x ]\n",
                                            cid, qid, cmd->cid, cmd->opcode);

    parser = (!qid) ? core.parser_admin : core.parser_io;

    if (!parser[cmd->opcode].opcode_fn) {
        sprintf(err, "[ox: Opcode 0x%02x not registered. Aborting command.]",
                                                                  cmd->opcode);
        log_err ("%s",err);
        if (core.debug) printf("%s\n",err);
        req->status = NVME_INVALID_OPCODE;
        ox_complete_request (req);
        return;
    }

    req->nvm_io.status.status = NVM_IO_NEW;

    status = parser[cmd->opcode].opcode_fn (req, cmd);

    if (status != NVME_NO_COMPLETE && status != NVME_SUCCESS) {
        sprintf(err, "[ox: opcode 0x%02x, with cid: %d returned an "
                          "error status: %x\n", cmd->opcode, cmd->cid, status);
        log_err ("%s",err);
        if (core.debug) printf("%s\n",err);
    }

    /* Enqueue completion for flush command and flush everything to NVM */
    if (qid && cmd->opcode == NVME_CMD_FLUSH) {
        req->status = status;
        ox_complete_request (req);
        //nvm_restart();
        return;
    }

    /* Enqueue completion in case of admin command */
    if (!qid && status != NVME_NO_COMPLETE) {
        req->status = status;
        ox_complete_request (req);
    }

    /* Enqueue in case of failed IO cmd that hasn't been enqueued */
    if (qid && status != NVME_NO_COMPLETE &&
                (req->nvm_io.status.status == NVM_IO_PROCESS ||
                 req->nvm_io.status.status == NVM_IO_NEW)) {
        req->status = status;
        ox_complete_request (req);
    }
}