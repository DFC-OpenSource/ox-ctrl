/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX-App Flash Translation Layer (Logical Block Address I/O)
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

#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/queue.h>
#include <libox.h>
#include <ox-mq.h>
#include <ox-app.h>

#define LBA_IO_PPA_ENTRIES  1024
#define LBA_IO_PPA_SIZE     64
#define LBA_IO_LBA_ENTRIES  (LBA_IO_PPA_ENTRIES * LBA_IO_PPA_SIZE)
#define LBA_IO_WRITE_Q      0
#define LBA_IO_READ_Q       1
#define LBA_IO_QUEUE_TO     4000000
#define LBA_IO_RETRY        40000
#define LBA_IO_RETRY_DELAY  100

#define LBA_IO_RETRY_S         100
#define LBA_IO_RETRY_DELAY_S   1000

struct lba_io_sec {
    uint32_t                    lba_id;
    uint64_t                    transaction_id;
    uint64_t                    transaction_ts;
    struct app_transaction_log *log;
    struct nvm_io_cmd          *nvme;
    uint64_t                    lba;
    struct nvm_ppa_addr         ppa;
    uint64_t                    prp;
    uint8_t                     type;
    struct app_prov_ppas       *prov;
    struct ox_mq_entry         *mentry;
    STAILQ_ENTRY(lba_io_sec)    fentry;
    TAILQ_ENTRY(lba_io_sec)     uentry;
};

struct lba_io_cmd {
    struct nvm_io_cmd            cmd;
    struct lba_io_sec           *vec[64];

    /* Used to transfer the LBA to the page oob area */
    uint8_t                     *oob_lba;

    struct app_prov_ppas        *prov;
    pthread_spinlock_t           spin;
    STAILQ_ENTRY(lba_io_cmd)     fentry;
    TAILQ_ENTRY(lba_io_cmd)      uentry;
};

struct lba_io_sec_ent {
    uint64_t              lba;
    uint64_t              ppa;
    struct app_prov_ppas *prov;
};

/* PPA I/Os are issued with 64 PPAs. This time in microseconds waits for
 * next command, if time is finished, a smaller PPA I/O command is issued */
#define LBA_IO_EMPTY_US 400

STAILQ_HEAD(flba_q, lba_io_sec) flbahead = STAILQ_HEAD_INITIALIZER(flbahead);
TAILQ_HEAD(ulba_q, lba_io_sec) ulbahead = TAILQ_HEAD_INITIALIZER(ulbahead);
static pthread_spinlock_t sec_spin;

STAILQ_HEAD(fcmd_q, lba_io_cmd) fcmdhead = STAILQ_HEAD_INITIALIZER(fcmdhead);
TAILQ_HEAD(ucmd_q, lba_io_cmd) ucmdhead = TAILQ_HEAD_INITIALIZER(ucmdhead);
static pthread_spinlock_t cmd_spin;

static struct ox_mq        *lba_io_mq;

static uint16_t             sec_pl_pg;
extern uint16_t             app_nch;
static struct app_channel **ch;

/* index 0: write line, index 1: read line */
static struct lba_io_sec   *rw_line[2][64];
static uint8_t              rw_off[2];

extern pthread_mutex_t      user_w_mutex;

static void lba_io_reset_cmd (struct lba_io_cmd *lcmd)
{
    memset (&lcmd->cmd, 0x0, sizeof (struct nvm_io_cmd));
    memset (lcmd->vec, 0x0, sizeof (struct lba_io_sec *) * 64);
    lcmd->prov = NULL;
    memset (lcmd->oob_lba, 0x0, LBA_IO_PPA_SIZE *
                                            ch[0]->ch->geometry->sec_oob_sz);
}

static int lba_io_cmd_amend (struct nvm_io_cmd *cmd)
{
    uint32_t pg_i, ret = 0;
    struct nvm_mmgr_io_cmd *mcmd;

    for (pg_i = 0; pg_i < cmd->status.total_pgs; pg_i++) {
        mcmd = &cmd->mmgr_io[pg_i];

        switch (mcmd->status) {
            case NVM_IO_FAIL:
            case NVM_IO_NEW:
                if (app_transaction_close_blk (&mcmd->ppa, APP_LINE_USER))
                    return -1;
                break;
            case NVM_IO_SUCCESS:
                break;
            case NVM_IO_PROCESS:
                ret++;
            default:
                break;
        }
    }

    return ret;
}

static void __lba_io_callback (struct nvm_io_cmd *cmd)
{
    uint16_t i;
    struct lba_io_cmd *lcmd;
    struct lba_io_sec **lba;
    struct lba_io_sec *last_lba = NULL;
    struct nvm_io_cmd *nvme_cmd = NULL;
    struct lba_io_sec_ent *nvme_lba;

    lcmd = (struct lba_io_cmd *) cmd->req;
    lba = lcmd->vec;

    if (cmd->cmdtype == MMGR_WRITE_PG) {
        if (lba_io_cmd_amend (cmd) < 0)
            log_err ("[appnvm (lba_io callback). Amend for failed write not "
                                                              "completed.]\n");

        for (i = 0; i < cmd->n_sec; i++)
            if (lba[i])
                last_lba = lba[i];
        if (!last_lba)
            goto COMPLETE_CMD;
    }

    for (i = 0; i < cmd->n_sec; i++) {
        if (!lba[i])
            continue;

        nvme_cmd = lba[i]->nvme;

        /* Check if lba has timeout */
        if (nvme_cmd == 0x0)
            goto COMPLETE_LBA;

        if (nvme_cmd->status.status != NVM_IO_FAIL) {
            nvme_cmd->status.status = cmd->status.status;
            nvme_cmd->status.nvme_status = cmd->status.nvme_status;
        }

COMPLETE_LBA:
        if (cmd->cmdtype == MMGR_WRITE_PG && lba[i] == last_lba && lcmd->prov) {

            if (!nvme_cmd) {
                if (lcmd->prov)
                    last_lba->prov = lcmd->prov;
            } else {
                nvme_lba = (struct lba_io_sec_ent *) nvme_cmd->mmgr_io
                                                    [lba[i]->lba_id / 4].rsvd;
                nvme_lba = nvme_lba + (lba[i]->lba_id % 4);
                nvme_lba->prov = lcmd->prov;
            }

        }
        ox_mq_complete_req (lba_io_mq, lba[i]->mentry);
    }

COMPLETE_CMD:
    pthread_spin_lock (&lcmd->spin);
    if (cmd->cmdtype == MMGR_WRITE_PG && !last_lba) {
        log_info ("[appnvm(lba_io): Freeing PPAs. GC not synchronized.]");
        if (lcmd->prov) {
            app_transaction_free_list (lcmd->prov);
            lcmd->prov = NULL;
        }
    }
    pthread_spin_unlock (&lcmd->spin);

    pthread_spin_lock (&cmd_spin);
    TAILQ_REMOVE(&ucmdhead, lcmd, uentry);
    STAILQ_INSERT_TAIL(&fcmdhead, lcmd, fentry);
    pthread_spin_unlock (&cmd_spin);
}

static void lba_io_callback (void *cmd)
{
    __lba_io_callback ((struct nvm_io_cmd *) cmd);
}

static int __lba_io_submit (struct nvm_io_cmd *cmd)
{
    struct app_transaction_t *tr;
    uint32_t sec_i = 0, qtype, ret = 0;
    struct lba_io_sec *lba[256];
    uint32_t retry = LBA_IO_RETRY;

    qtype = (cmd->cmdtype == MMGR_WRITE_PG) ? LBA_IO_WRITE_Q : LBA_IO_READ_Q;

    for (sec_i = 0; sec_i < cmd->n_sec; sec_i++) {
RETRY:
        pthread_spin_lock (&sec_spin);
        if (STAILQ_EMPTY(&flbahead)) {
            pthread_spin_unlock (&sec_spin);
            retry--;
            usleep (LBA_IO_RETRY_DELAY);
            if (retry) {
                goto RETRY;
            } else
                goto REQUEUE;
        }

        lba[sec_i] = STAILQ_FIRST(&flbahead);
        if (!lba[sec_i]) {
            pthread_spin_unlock (&sec_spin);
            retry--;
            usleep (LBA_IO_RETRY_DELAY);
            if (retry) {
                goto RETRY;
            }else
                goto REQUEUE;
        }
        STAILQ_REMOVE_HEAD (&flbahead, fentry);
        TAILQ_INSERT_TAIL(&ulbahead, lba[sec_i], uentry);
        pthread_spin_unlock (&sec_spin);

        lba[sec_i]->lba_id = sec_i;
        lba[sec_i]->nvme = cmd;
        lba[sec_i]->lba = cmd->slba + sec_i;
        lba[sec_i]->type = qtype;
        lba[sec_i]->prov = NULL;
        lba[sec_i]->prp = cmd->prp[sec_i];

        if (qtype == LBA_IO_WRITE_Q) {
            tr = (struct app_transaction_t *) cmd->opaque;
            lba[sec_i]->log = &tr->entries[sec_i];
            lba[sec_i]->transaction_id = tr->tid;
            lba[sec_i]->transaction_ts = tr->ts;

            if ( tr->entries[sec_i].lba != lba[sec_i]->lba )
                log_err ("[appnvm: (lba_io,submit): LBA doesn't match transaction. "
                        "(%lu/%lu)\n", lba[sec_i]->lba, tr->entries[sec_i].lba);
        } else {
            lba[sec_i]->log = 0x0;
            lba[sec_i]->transaction_id = 0x0;
            lba[sec_i]->transaction_ts = 0x0;
        }
    }

    for (sec_i = 0; sec_i < cmd->n_sec; sec_i++) {
        if (ox_mq_submit_req(lba_io_mq, qtype, lba[sec_i]))
            /* MQ_TO and callback take care of aborting submitted lbas */
            goto REQUEUE_UNPROCESSED;
    }

    return 0;

REQUEUE_UNPROCESSED:
    if (APP_DEBUG_LBA_IO)
        printf ("[lba-io: Unprocessed LBAs requeued due failure. Cmd %lu, "
                "LBAs requeued: %d/%d]\n", cmd->cid, cmd->n_sec - sec_i,
                                                                  cmd->n_sec);

    cmd->status.status = NVM_IO_FAIL;
    cmd->status.nvme_status = 0x1000 | NVME_INTERNAL_DEV_ERROR;

    /* If at least 1 lba has been enqueued, let the callback
                                completing the nvme cmd by returning success */
    ret = (sec_i) ? 0 : -1;
    while (sec_i < cmd->n_sec) {
        lba[sec_i]->nvme->status.pgs_p++;
        lba[sec_i]->nvme = 0x0;
        lba[sec_i]->lba = 0x0;
        lba[sec_i]->prp = 0x0;
        lba[sec_i]->log = 0x0;
        lba[sec_i]->transaction_id = 0x0;
        lba[sec_i]->transaction_ts = 0x0;
        pthread_spin_lock (&sec_spin);
        TAILQ_REMOVE(&ulbahead, lba[sec_i], uentry);
        STAILQ_INSERT_TAIL(&flbahead, lba[sec_i], fentry);
        pthread_spin_unlock (&sec_spin);
        sec_i++;
    }
    return ret;

REQUEUE:
    if (APP_DEBUG_LBA_IO)
        printf ("[lba-io: All LBAs requeued due failure. Cmd %lu. LBAs: %d]",
                                                        cmd->cid, cmd->n_sec);

    while (sec_i) {
        sec_i--;
        lba[sec_i]->nvme = 0x0;
        lba[sec_i]->lba = 0x0;
        lba[sec_i]->prp = 0x0;
        lba[sec_i]->log = 0x0;
        lba[sec_i]->transaction_id = 0x0;
        lba[sec_i]->transaction_ts = 0x0;
        pthread_spin_lock (&sec_spin);
        TAILQ_REMOVE(&ulbahead, lba[sec_i], uentry);
        STAILQ_INSERT_TAIL(&flbahead, lba[sec_i], fentry);
        pthread_spin_unlock (&sec_spin);
    }
    cmd->status.status = NVM_IO_FAIL;
    cmd->status.nvme_status = 0x2000 | NVME_INTERNAL_DEV_ERROR;
    return -1;
}

static int lba_io_submit (struct nvm_io_cmd *cmd)
{
    int ret;
    uint32_t lba_i;
    uint64_t lbas[cmd->n_sec];

    if (cmd->cmdtype != MMGR_WRITE_PG)
        goto READ;

    for (lba_i = 0; lba_i < cmd->n_sec; lba_i++)
        lbas[lba_i] = cmd->slba + lba_i;

    cmd->opaque = (void *) app_transaction_new(lbas, cmd->n_sec, APP_TR_LBA_NS);
    if (!cmd->opaque)
        goto ERR;

READ:
    ret = __lba_io_submit (cmd);
    if (ret && cmd->cmdtype == MMGR_WRITE_PG)
        app_transaction_abort ((struct app_transaction_t *) cmd->opaque);

    return ret;

ERR:
    cmd->status.status = NVM_IO_FAIL;
    cmd->status.nvme_status = NVME_INTERNAL_DEV_ERROR;
    return -1;
}

static void lba_io_mq_to (void **opaque, int c)
{
    uint32_t sec_i;
    struct lba_io_sec *lba;

    sec_i = c;
    while (sec_i) {
        sec_i--;
        lba = (struct lba_io_sec *) opaque[sec_i];
        log_err (" [appnvm (lba_io): TIMEOUT LBA-> %lu, cmd-> %p\n",
                                                          lba->lba, lba->nvme);
        lba->nvme->status.status = NVM_IO_FAIL;
        lba->nvme->status.nvme_status = NVME_MEDIA_TIMEOUT;
        lba->nvme->status.pgs_p++;
        lba->nvme = NULL;
        lba->lba = 0x0;
        lba->prp = 0x0;
        lba->ppa.ppa = 0x0;
    }
}

static void lba_io_prepare_cmd (struct lba_io_cmd *lcmd, uint8_t type)
{
    uint32_t i, pg, nsec;
    struct nvm_io_cmd *cmd = &lcmd->cmd;
    uint64_t moff, meta;

    cmd->cid = 0; /* Make some counter */
    cmd->sec_sz = NVME_KERNEL_PG_SIZE;
    cmd->md_sz = 0;
    cmd->cmdtype = (!type) ? MMGR_WRITE_PG : MMGR_READ_PG;
    cmd->req = (void *) lcmd;

    cmd->status.pg_errors = 0;
    cmd->status.ret_t = 0;
    cmd->status.pgs_p = 0;
    cmd->status.pgs_s = 0;
    cmd->status.status = NVM_IO_PROCESS;

    for (i = 0; i < 8; i++)
        cmd->status.pg_map[i] = 0;

    /* Metadata points to command LBA array */
    moff = (uint64_t) lcmd->oob_lba;
    meta = moff;

    pg = 0;
    nsec = 0;
    for (i = 0; i <= cmd->n_sec; i++) {

        /* no write-caching is enabled */
        cmd->mmgr_io[pg].force_sync_data[i] = 0;

        if ((i == cmd->n_sec) ||
            (i && (cmd->ppalist[i].ppa == cmd->ppalist[i - 1].ppa)) ||
            (i && ( cmd->ppalist[i].g.ch != cmd->ppalist[i - 1].g.ch ||
                    cmd->ppalist[i].g.lun != cmd->ppalist[i - 1].g.lun ||
                    cmd->ppalist[i].g.blk != cmd->ppalist[i - 1].g.blk ||
                    cmd->ppalist[i].g.pg != cmd->ppalist[i - 1].g.pg ||
                    cmd->ppalist[i].g.pl != cmd->ppalist[i - 1].g.pl))) {

            cmd->mmgr_io[pg].pg_index = pg;
            cmd->mmgr_io[pg].status = NVM_IO_NEW;
            cmd->mmgr_io[pg].nvm_io = cmd;
            cmd->mmgr_io[pg].pg_sz = nsec * NVME_KERNEL_PG_SIZE;
            cmd->mmgr_io[pg].n_sectors = nsec;
            cmd->mmgr_io[pg].sec_offset = i - nsec;
            cmd->mmgr_io[pg].sync_count = NULL;
            cmd->mmgr_io[pg].sync_mutex = NULL;
            cmd->mmgr_io[pg].force_sync_md = 1;
            cmd->md_prp[pg] = (!type) ? moff : 0;

            pg++;
            nsec = 0;
            moff = meta + (cmd->channel[0]->geometry->sec_oob_sz * i);
        }
        nsec++;
    }

    cmd->status.total_pgs = pg;
}

static int lba_io_write (struct lba_io_cmd *lcmd)
{
    struct lba_io_sec_ent *nvme_lba;
    uint32_t sec_i, pgs, sec_oob, proc, ch_i, ret = 0;
    struct nvm_io_cmd *cmd;
    struct app_prov_ppas *ppas;
    uint32_t nlb = rw_off[LBA_IO_WRITE_Q];
    struct app_sec_oob *oob;
    struct app_transaction_user ent;
    uint32_t retry = LBA_IO_RETRY;

RETRY_CH:
    for (ch_i = 0; ch_i < app_nch; ch_i++)
        if (app_ch_active (ch[ch_i]))
            ret++;
    if (!ret) {
        retry--;
        usleep (LBA_IO_RETRY_DELAY);
        if (retry)
            goto RETRY_CH;
        else
            return -1;
    }

    pgs = nlb / sec_pl_pg;
    if (nlb % sec_pl_pg > 0)
        pgs++;

    cmd = &lcmd->cmd;
    cmd->n_sec = sec_pl_pg * pgs;

    /* The OOB area is used to store the page LBA, for GC reverse mapping */
    sec_oob = ch[0]->ch->geometry->sec_oob_sz;
    cmd->md_sz = cmd->n_sec * sec_oob;

    ent.entries = ox_calloc (cmd->n_sec, sizeof (struct app_transaction_log *),
                                                             OX_MEM_OXBLK_LBA);
    if (!ent.entries)
        return 1;

    ent.count = cmd->n_sec;
    for (sec_i = 0; sec_i < nlb; sec_i++)
        ent.entries[sec_i] = rw_line[LBA_IO_WRITE_Q][sec_i]->log;

    pthread_mutex_lock (&user_w_mutex);

    ppas = app_transaction_alloc_list (&ent, APP_TR_LBA_NS);
    ox_free (ent.entries, OX_MEM_OXBLK_LBA);

    if (!ppas || ppas->nppas < nlb) {
        pthread_mutex_unlock (&user_w_mutex);
        return 1;
    }

    lcmd->prov = ppas;

    for (sec_i = 0; sec_i < nlb; sec_i++) {
        rw_line[LBA_IO_WRITE_Q][sec_i]->ppa.ppa = ppas->ppa[sec_i].ppa;

        cmd->ppalist[sec_i].ppa = ppas->ppa[sec_i].ppa;
        cmd->prp[sec_i]         = rw_line[LBA_IO_WRITE_Q][sec_i]->prp;
        cmd->channel[sec_i]     = ch[ppas->ppa[sec_i].g.ch]->ch;

        lcmd->vec[sec_i]        = rw_line[LBA_IO_WRITE_Q][sec_i];

        oob = (struct app_sec_oob *) (lcmd->oob_lba + (sec_oob * sec_i));
        oob->lba = lcmd->vec[sec_i]->lba;
        oob->pg_type = APP_PG_NAMESPACE;

        /* Keep the LBA/PPAs in the nvme command, in this way, if the command
        fails, no lba is upserted in the mapping table */
        nvme_lba = (struct lba_io_sec_ent *) lcmd->vec[sec_i]->nvme->mmgr_io
                                          [lcmd->vec[sec_i]->lba_id / 4].rsvd;
        nvme_lba = nvme_lba + (lcmd->vec[sec_i]->lba_id % 4);
        nvme_lba->lba = lcmd->vec[sec_i]->lba;
        nvme_lba->ppa = lcmd->vec[sec_i]->ppa.ppa;
        nvme_lba->prov = NULL;
    }

    /* Padding the physical write if needed (same data for now) */
    while (sec_i < cmd->n_sec) {
        cmd->ppalist[sec_i].ppa = ppas->ppa[sec_i].ppa;
        cmd->prp[sec_i] = rw_line[LBA_IO_WRITE_Q][0]->prp;
        cmd->channel[sec_i] = ch[ppas->ppa[sec_i].g.ch]->ch;
        oob = (struct app_sec_oob *) (lcmd->oob_lba + (sec_oob * sec_i));
        oob->lba = AND64;
        oob->pg_type = APP_PG_PADDING;
        sec_i++;
    }

    lba_io_prepare_cmd (lcmd, LBA_IO_WRITE_Q);

    cmd->callback.cb_fn = lba_io_callback;
    cmd->callback.opaque = (void *) cmd;
 
    if (oxapp()->ppa_io->submit_fn (cmd)) {
        pthread_mutex_unlock (&user_w_mutex);
        proc = lba_io_cmd_amend (cmd);

        /* Positive 'proc' means that some LBAs have been submitted */
        if (proc > 0)
            return 0;

        if (proc <= 0)
            goto FREE;
    }
    pthread_mutex_unlock (&user_w_mutex);

    return 0;

FREE:
    pthread_spin_lock (&lcmd->spin);
    if (lcmd->prov) {
        app_transaction_free_list (ppas);
        lcmd->prov = NULL;
    }
    pthread_spin_unlock (&lcmd->spin);
    return -1;
}

static int lba_io_read (struct lba_io_cmd *lcmd)
{
    int ret;
    uint32_t sec_i, sec_oob, pgs;
    struct nvm_io_cmd *cmd;
    uint32_t nlb = rw_off[LBA_IO_READ_Q];
    struct nvm_ppa_addr sec_ppa;

    pgs = nlb / sec_pl_pg;
    if (nlb % sec_pl_pg > 0)
        pgs++;

    cmd = &lcmd->cmd;
    cmd->n_sec = nlb;

    /* The OOB area is used to store the page LBA, for GC reverse mapping */
    sec_oob = ch[0]->ch->geometry->sec_oob_sz;
    cmd->md_sz = sec_pl_pg * pgs * sec_oob;

    for (sec_i = 0; sec_i < nlb; sec_i++) {

        sec_ppa.ppa = oxapp()->gl_map->read_fn
                                          (rw_line[LBA_IO_READ_Q][sec_i]->lba);
        if (sec_ppa.ppa == AND64)
            return 1;

        rw_line[LBA_IO_READ_Q][sec_i]->ppa.ppa = sec_ppa.ppa;
        cmd->ppalist[sec_i].ppa = sec_ppa.ppa;
        cmd->prp[sec_i] = rw_line[LBA_IO_READ_Q][sec_i]->prp;

        cmd->channel[sec_i] = ch[sec_ppa.g.ch]->ch;

        lcmd->vec[sec_i] = rw_line[LBA_IO_READ_Q][sec_i];
    }

    lba_io_prepare_cmd (lcmd, LBA_IO_READ_Q);

    cmd->callback.cb_fn = lba_io_callback;
    cmd->callback.opaque = (void *) cmd;
    ret = oxapp()->ppa_io->submit_fn (cmd);

    return ret;
}

static int lba_io_rw (uint8_t type)
{
    int ret;
    struct lba_io_cmd *lcmd;
    uint32_t retry = LBA_IO_RETRY;

    if (STAILQ_EMPTY(&fcmdhead))
        return -1;

RETRY:
    pthread_spin_lock (&cmd_spin);
    lcmd = STAILQ_FIRST(&fcmdhead);
    if (!lcmd) {
        pthread_spin_unlock (&cmd_spin);
        retry--;
        usleep (LBA_IO_RETRY_DELAY);
        if (retry)
            goto RETRY;
        else
            return -1;
    }
    STAILQ_REMOVE_HEAD (&fcmdhead, fentry);
    TAILQ_INSERT_TAIL(&ucmdhead, lcmd, uentry);
    pthread_spin_unlock (&cmd_spin);

    lba_io_reset_cmd (lcmd);

    ret = (!type) ? lba_io_write (lcmd) : lba_io_read (lcmd);

    if (ret)
        goto REQUEUE;

    return 0;

REQUEUE:
    pthread_spin_lock (&cmd_spin);
    TAILQ_REMOVE(&ucmdhead, lcmd, uentry);
    STAILQ_INSERT_TAIL(&fcmdhead, lcmd, fentry);
    pthread_spin_unlock (&cmd_spin);

    return ret;
}

static void lba_io_complete_failed_lbas (uint8_t type)
{
    uint16_t i;
    struct lba_io_sec *lba;

    for (i = 0; i < rw_off[type]; i++) {
        lba = rw_line[type][i];
        pthread_mutex_lock (&lba->nvme->mutex);
        lba->nvme->status.status = NVM_IO_FAIL;
        lba->nvme->status.nvme_status = NVME_DATA_TRAS_ERROR;
        pthread_mutex_unlock (&lba->nvme->mutex);
        ox_mq_complete_req (lba_io_mq, lba->mentry);
    }
}

static void lba_io_sec_sq (struct ox_mq_entry *req)
{
    int ret, retry = 0;
    struct lba_io_sec *lba = (struct lba_io_sec *) req->opaque;
    lba->mentry = req;

    /* 1 write thread and 1 read thread, so, no lock is needed */
    rw_line[lba->type][rw_off[lba->type]] = lba;
    rw_off[lba->type]++;

    ret = ox_mq_used_count (lba_io_mq, lba->type);

    if (ret < 0) {

        lba_io_complete_failed_lbas (lba->type);
        goto RESET_LINE;

    } else if (ret == 0) {

        /* Prefer larger I/Os for writes, it waits for new entries */
        if (lba->type == LBA_IO_WRITE_Q)
            usleep (LBA_IO_EMPTY_US);

        ret = ox_mq_used_count (lba_io_mq, lba->type);
    }

RETRY:
    if ((rw_off[lba->type] == LBA_IO_PPA_SIZE) || !ret) {
        if (lba_io_rw (lba->type)) {
            usleep (LBA_IO_RETRY_DELAY_S);

            if (retry == LBA_IO_RETRY_S) {
                lba_io_complete_failed_lbas (lba->type);
                log_err ("[lba-io: Line I/O failed. Line is reseted.]");
                goto RESET_LINE;
            }

            retry++;
            goto RETRY;
        }

        goto RESET_LINE;
    }

    return;

RESET_LINE:
    rw_off[lba->type] = 0;
    memset (rw_line[lba->type], 0x0, sizeof (struct lba_io_sec *)
                                                            * LBA_IO_PPA_SIZE);
}

static void lba_io_free_ppas (struct nvm_io_cmd *cmd)
{
    uint32_t sec;
    struct lba_io_sec_ent *ent;

    for (sec = 0; sec < cmd->n_sec; sec++) {
        ent = (struct lba_io_sec_ent *) cmd->mmgr_io[sec / 4].rsvd;
        ent = ent + (sec % 4);

        if (ent->prov) {
            app_transaction_free_list (ent->prov);
            ent->prov = NULL;
        }
    }
}

static void lba_io_commit_callback (void *opaque)
{
    struct nvm_callback *cb = (struct nvm_callback *) opaque;
    ox_ftl_callback ((struct nvm_io_cmd *) cb->opaque);
}

static void lba_io_sec_callback (void *opaque)
{
    struct lba_io_sec *lba = (struct lba_io_sec *) opaque;
    struct nvm_io_cmd *nvme_cmd = lba->nvme;
    struct app_transaction_t *tr;

    /* If cmd is NULL, lba has timeout */
    if ( !nvme_cmd || (nvme_cmd->status.status == NVM_IO_TIMEOUT) ) {
        if (lba->type == LBA_IO_WRITE_Q)
            app_transaction_abort_by_ts (lba->transaction_id,
                                                          lba->transaction_ts);
        goto COMPLETE;
    }

    pthread_mutex_lock (&nvme_cmd->mutex);
    nvme_cmd->status.pgs_p++;

    if (nvme_cmd->status.pgs_p == nvme_cmd->n_sec) {

        pthread_mutex_unlock (&nvme_cmd->mutex);

        if (lba->type == LBA_IO_WRITE_Q) {
            lba_io_free_ppas (nvme_cmd);

            if (nvme_cmd->status.status == NVM_IO_SUCCESS) {

                tr = (struct app_transaction_t *) nvme_cmd->opaque;
                nvme_cmd->callback.cb_fn = lba_io_commit_callback;
                nvme_cmd->callback.opaque = (void *) nvme_cmd;
                nvme_cmd->callback.ts = tr->ts;

                if ( app_transaction_commit (tr, &nvme_cmd->callback,
                                                            APP_T_FLUSH_YES) ) {
                    nvme_cmd->status.status = NVM_IO_FAIL;
                    nvme_cmd->status.nvme_status = NVME_INTERNAL_DEV_ERROR;
                    app_transaction_abort (tr);
                } else {
                    goto COMPLETE;
                }

            } else
                app_transaction_abort ((struct app_transaction_t *)
                                                            nvme_cmd->opaque);
        }

        ox_ftl_callback (nvme_cmd);
        goto COMPLETE;
    }

    pthread_mutex_unlock (&nvme_cmd->mutex);

COMPLETE:
    if (lba->type == LBA_IO_WRITE_Q && lba->prov)
        app_transaction_free_list (lba->prov);

    lba->nvme = NULL;
    lba->prov = NULL;
    lba->lba = lba->ppa.ppa = lba->prp = lba->transaction_id =
                                                     lba->transaction_ts = 0x0;

    pthread_spin_lock (&sec_spin);
    TAILQ_REMOVE(&ulbahead, lba, uentry);
    STAILQ_INSERT_TAIL(&flbahead, lba, fentry);
    pthread_spin_unlock (&sec_spin);
}

static void lba_io_stats_fill_row (struct oxmq_output_row *row, void *opaque)
{
    struct lba_io_sec *sec;

    sec = (struct lba_io_sec *) opaque;

    row->lba = sec->lba;
    row->ch = 0;
    row->lun = 0;
    row->blk = 0;
    row->pg = 0;
    row->pl = 0;
    row->sec = 0;
    row->type = (sec->type == LBA_IO_WRITE_Q)  ? 'W' : 'R';
    row->failed = 0;
    row->datacmp = 0;
    row->size = ch[0]->ch->geometry->sec_size;
}

struct ox_mq_config lba_io_mq_config = {
    /* Queue 0: write, queue 1: read */
    .name       = "LBA_IO",
    .n_queues   = 2,
    .q_size     = LBA_IO_LBA_ENTRIES,
    .sq_fn      = lba_io_sec_sq,
    .cq_fn      = lba_io_sec_callback,
    .to_fn      = lba_io_mq_to,
    .output_fn  = lba_io_stats_fill_row,
    .to_usec    = LBA_IO_QUEUE_TO,
    .flags      = OX_MQ_CPU_AFFINITY
};

static void lba_io_free_cmd (void)
{
    struct lba_io_cmd *cmd;
    struct lba_io_sec *sec;

    while (!STAILQ_EMPTY(&fcmdhead)) {
        cmd = STAILQ_FIRST(&fcmdhead);
        STAILQ_REMOVE_HEAD (&fcmdhead, fentry);
        pthread_spin_destroy (&cmd->spin);
        ox_free (cmd->oob_lba, OX_MEM_OXBLK_LBA);
        ox_free (cmd, OX_MEM_OXBLK_LBA);
    }
    while (!STAILQ_EMPTY(&flbahead)) {
        sec = STAILQ_FIRST(&flbahead);
        STAILQ_REMOVE_HEAD (&flbahead, fentry);
        ox_free (sec, OX_MEM_OXBLK_LBA);
    }
}

static int lba_io_init (void)
{
    uint32_t cmd_i, lba_i, ch_i, ret, qid;
    struct lba_io_cmd *cmd;
    struct lba_io_sec *sec;

    if (!ox_mem_create_type ("OXBLK_LBA", OX_MEM_OXBLK_LBA))
        return -1;

    ch = ox_malloc (sizeof(struct app_channel *) * app_nch, OX_MEM_OXBLK_LBA);
    if (!ch)
        return -1;

    ret = oxapp()->channels.get_list_fn (ch, app_nch);
    if (ret != app_nch)
        goto FREE_CH;

    sec_pl_pg = ch[0]->ch->geometry->sec_per_pl_pg;
    for (ch_i = 0; ch_i < app_nch; ch_i++)
        sec_pl_pg = MIN(ch[ch_i]->ch->geometry->sec_per_pl_pg, sec_pl_pg);

    STAILQ_INIT(&fcmdhead);
    TAILQ_INIT(&ucmdhead);
    STAILQ_INIT(&flbahead);
    TAILQ_INIT(&ulbahead);

    rw_off[0] = 0;
    rw_off[1] = 0;

    if (pthread_spin_init(&cmd_spin, 0))
        goto FREE_CH;

    if (pthread_spin_init(&sec_spin, 0))
        goto CMD_SPIN;

    for (cmd_i = 0; cmd_i < LBA_IO_PPA_ENTRIES; cmd_i++) {
        cmd = ox_calloc (sizeof (struct lba_io_cmd), 1, OX_MEM_OXBLK_LBA);
        if (!cmd)
            goto FREE_CMD;

        if (pthread_spin_init(&cmd->spin, 0)) {
            ox_free (cmd, OX_MEM_OXBLK_LBA);
            goto FREE_CMD;
        }

        cmd->oob_lba = ox_calloc (LBA_IO_PPA_SIZE,
                            ch[0]->ch->geometry->sec_oob_sz, OX_MEM_OXBLK_LBA);
        if (!cmd->oob_lba) {
            pthread_spin_destroy (&cmd->spin);
            ox_free (cmd, OX_MEM_OXBLK_LBA);
            goto FREE_CMD;
        }

        STAILQ_INSERT_TAIL(&fcmdhead, cmd, fentry);

        for (lba_i = 0; lba_i < LBA_IO_PPA_SIZE; lba_i++) {
            sec = ox_calloc (sizeof (struct lba_io_sec), 1, OX_MEM_OXBLK_LBA);
            if (!sec)
                goto FREE_CMD;

            STAILQ_INSERT_TAIL(&flbahead, sec, fentry);
        }
    }

    /* Set thread affinity, if enabled */
    for (qid = 0; qid < lba_io_mq_config.n_queues; qid++) {
        lba_io_mq_config.sq_affinity[qid] = 0;
        lba_io_mq_config.cq_affinity[qid] = 0;

#if OX_TH_AFFINITY
        lba_io_mq_config.sq_affinity[qid] |= ((uint64_t) 1 << 0);
	lba_io_mq_config.sq_affinity[qid] |= ((uint64_t) 1 << 6);

        lba_io_mq_config.cq_affinity[qid] |= ((uint64_t) 1 << 0);
#endif /* OX_TH_AFFINITY */
    }

    lba_io_mq = ox_mq_init(&lba_io_mq_config);
    if (!lba_io_mq)
        goto FREE_CMD;

    log_info("    [appnvm: LBA I/O started.]\n");

    return 0;

FREE_CMD:
    lba_io_free_cmd ();
    pthread_spin_destroy (&sec_spin);
CMD_SPIN:
    pthread_spin_destroy (&cmd_spin);
FREE_CH:
    ox_free (ch, OX_MEM_OXBLK_LBA);
    return -1;
}

static void lba_io_exit (void)
{
    struct lba_io_cmd *cmd;
    struct lba_io_sec *sec;

    while (!TAILQ_EMPTY(&ucmdhead)) {
        cmd = TAILQ_FIRST(&ucmdhead);
        TAILQ_REMOVE(&ucmdhead, cmd, uentry);

        STAILQ_INSERT_TAIL(&fcmdhead, cmd, fentry);
    }

    while (!TAILQ_EMPTY(&ulbahead)) {
        sec = TAILQ_FIRST(&ulbahead);
        TAILQ_REMOVE(&ulbahead, sec, uentry);
        STAILQ_INSERT_TAIL(&flbahead, sec, fentry);
    }

    ox_mq_destroy (lba_io_mq);

    lba_io_free_cmd ();
    pthread_spin_destroy (&sec_spin);
    pthread_spin_destroy (&cmd_spin);
    ox_free (ch, OX_MEM_OXBLK_LBA);

    log_info("    [ox-blk: LBA I/O stopped.]\n");
}

static struct app_lba_io oxblk_lba_io = {
    .mod_id      = OXBLK_LBA_IO,
    .name        = "OX_BLOCK-LBA",
    .init_fn     = lba_io_init,
    .exit_fn     = lba_io_exit,
    .submit_fn   = lba_io_submit,
    .callback_fn = __lba_io_callback
};

void oxb_lba_io_register (void) {
    app_mod_register (APPMOD_LBA_IO, OXBLK_LBA_IO, &oxblk_lba_io);
}