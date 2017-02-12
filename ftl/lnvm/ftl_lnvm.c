/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - LightNVM Flash Translation Layer (NVMe to MMGR translation only)
 *
 * Copyright (C) 2016, IT University of Copenhagen. All rights reserved.
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 *
 * Funding support provided by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *  this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "../../include/ssd.h"

#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/queue.h>
#include "ftl_lnvm.h"

LIST_HEAD(lnvm_ch, lnvm_channel) ch_head = LIST_HEAD_INITIALIZER(ch_head);

static pthread_mutex_t endio_mutex;
static int lnvm_submit_io (struct nvm_io_cmd *);

static struct lnvm_channel *lnvm_get_ch_instance(uint16_t ch_id)
{
    struct lnvm_channel *lch;
    LIST_FOREACH(lch, &ch_head, entry){
        if(lch->ch->ch_mmgr_id == ch_id)
            return lch;
    }

    return NULL;
}

static void lnvm_set_pgmap(uint8_t *pgmap, uint8_t index, uint8_t flag)
{
    pthread_mutex_lock(&endio_mutex);
    pgmap[index / 8] = (flag)
            ? pgmap[index / 8] | (1 << (index % 8))
            : pgmap[index / 8] ^ (1 << (index % 8));
    pthread_mutex_unlock(&endio_mutex);
}

static int lnvm_check_pgmap_complete (uint8_t *pgmap, uint8_t ni) {
    int sum = 0, i;
    for (i = 0; i < ni; i++)
        sum += pgmap[i];
    return sum;
}

static int lnvm_pg_read (struct nvm_mmgr_io_cmd *cmd)
{
    return nvm_submit_mmgr(cmd);
}

static int lnvm_pg_write (struct nvm_mmgr_io_cmd *cmd)
{
    return nvm_submit_mmgr(cmd);
}

static int lnvm_erase_blk (struct nvm_mmgr_io_cmd *cmd)
{
    return nvm_submit_mmgr(cmd);
}

/* If all pages were processed, it checks for errors. If all succeed, finish
 * cmd. Otherwise, retry only pages with error.
 *
 * Calls to this fn come from submit_io or from end_pg_io
 */
static int lnvm_check_end_io (struct nvm_io_cmd *cmd)
{
    if (cmd->status.pgs_p == cmd->status.total_pgs) {

        pthread_mutex_lock(&endio_mutex);
        /* if true, some pages failed */
        if ( lnvm_check_pgmap_complete(cmd->status.pg_map,
                                      ((cmd->status.total_pgs - 1) / 8) + 1)) {

            cmd->status.ret_t++;
            if (cmd->status.ret_t <= FTL_LNVM_IO_RETRY) {
                log_err ("[FTL WARNING: Cmd resubmitted due failed pages]\n");
                goto SUBMIT;
            } else {
                log_err ("[FTL WARNING: Completing FAILED command]\n");
                cmd->status.status = NVM_IO_FAIL;
                cmd->status.nvme_status = NVME_DATA_TRAS_ERROR;
                goto COMPLETE;
            }

        } else {
            cmd->status.status = NVM_IO_SUCCESS;
            cmd->status.nvme_status = NVME_SUCCESS;
            goto COMPLETE;
        }
    }
    goto RETURN;

SUBMIT:
    pthread_mutex_unlock(&endio_mutex);
    lnvm_submit_io(cmd);
    goto RETURN;

COMPLETE:
    pthread_mutex_unlock(&endio_mutex);
    nvm_complete_ftl(cmd);

RETURN:
    return 0;
}

static void lnvm_callback_io (struct nvm_mmgr_io_cmd *cmd)
{
    if (cmd->status == NVM_IO_SUCCESS) {
        lnvm_set_pgmap(cmd->nvm_io->status.pg_map, cmd->pg_index,FTL_PGMAP_OFF);
        pthread_mutex_lock(&endio_mutex);
        cmd->nvm_io->status.pgs_s++;
    } else {
        pthread_mutex_lock(&endio_mutex);
        cmd->nvm_io->status.pg_errors++;
    }

    cmd->nvm_io->status.pgs_p++;
    pthread_mutex_unlock(&endio_mutex);

    lnvm_check_end_io(cmd->nvm_io);
}

static int lnvm_check_pg_io (struct nvm_io_cmd *cmd, uint8_t index)
{
    int c;
    uint8_t plane;
    struct nvm_ppa_addr *ppa;
    struct nvm_mmgr_io_cmd *mio = &cmd->mmgr_io[index];

    mio->pg_index = index;
    mio->status = NVM_IO_SUCCESS;
    mio->nvm_io = cmd;

    if (cmd->cmdtype == MMGR_ERASE_BLK) {
        mio->ppa = cmd->ppalist[index];
        return 0;
    }

    mio->ppa = cmd->ppalist[index * LNVM_SEC_PG];

    /* We put planes manually, for now */
    plane = index % LNVM_PLANES;
    mio->ppa.g.pl = plane;

    /* We check ppa addresses for only for multiple sector page IOs */
    if (!cmd->sec_offset || (cmd->sec_offset &&
                                        (index+1 < cmd->status.total_pgs))) {
        for (c = 1; c < LNVM_SEC_PG; c++) {
            ppa = &cmd->ppalist[index * LNVM_SEC_PG + c];
            if (ppa->g.ch != mio->ppa.g.ch || ppa->g.lun != mio->ppa.g.lun ||
                    ppa->g.blk != mio->ppa.g.blk ||
                    ppa->g.pg != mio->ppa.g.pg   ||
                    ppa->g.sec != mio->ppa.g.sec + c) {
                log_err ("[ERROR ftl_lnvm: Wrong multi-sector ppa sequence. "
                                                         "Aborting IO cmd.\n");
                return -1;
            }
        }
    }

    if (cmd->sec_sz != LNVM_SECSZ)
        return -1;

    /* If offset is positive, last pg_size is smaller */
    mio->pg_sz = (index+1 == cmd->status.total_pgs && cmd->sec_offset) ?
                                cmd->sec_sz * cmd->sec_offset : LNVM_PG_SIZE;
    mio->sec_sz = cmd->sec_sz;
    mio->n_sectors = mio->pg_sz / mio->sec_sz;
    mio->md_sz = cmd->md_sz / cmd->status.total_pgs;

    for (c = 0; c < mio->n_sectors; c++)
        mio->prp[c] = cmd->prp[index * LNVM_SEC_PG + c];

    mio->md_prp = cmd->md_prp[index];

    return 0;
}

static int lnvm_check_io (struct nvm_io_cmd *cmd)
{
    int i;

    if (cmd->sec_offset && (cmd->cmdtype != MMGR_ERASE_BLK))
        cmd->status.total_pgs = (cmd->n_sec / LNVM_SEC_PG) + 1;

    if (cmd->status.pgs_p == 0) {
        for (i = 0; i < cmd->status.total_pgs; i++)
            lnvm_set_pgmap(cmd->status.pg_map, i, FTL_PGMAP_ON);
    }

    cmd->status.pgs_p = cmd->status.pgs_s;

    if (cmd->cmdtype == MMGR_ERASE_BLK)
        return 0;

    if (cmd->status.total_pgs * LNVM_SEC_PG > 64
                                                || cmd->status.total_pgs == 0){
        cmd->status.status = NVM_IO_FAIL;
        return cmd->status.nvme_status = NVME_INVALID_FORMAT;
    }

    return 0;
}

static int lnvm_submit_io (struct nvm_io_cmd *cmd)
{
    /* DEBUG: Force timeout for testing */
    /*
    if (cmd->ppalist[0].g.pg > 64 && cmd->ppalist[0].g.pg < 68)
        return 0;
    */

    int ret, i;
    ret = lnvm_check_io(cmd);
    if (ret) return ret;

    for (i = 0; i < cmd->status.total_pgs; i++) {

        /* if true, page not processed yet */
        if ( cmd->status.pg_map[i / 8] & (1 << (i % 8)) ) {
            if (lnvm_check_pg_io(cmd, i)) {
                cmd->status.status = NVM_IO_FAIL;
                cmd->status.nvme_status = NVME_INVALID_FORMAT;
                return -1;
            }
        }
    }

    for (i = 0; i < cmd->status.total_pgs; i++) {
        /* if true, page not processed yet */
        if ( cmd->status.pg_map[i / 8] & (1 << (i % 8)) ) {
            switch (cmd->cmdtype) {
                case MMGR_WRITE_PG:
                    ret = lnvm_pg_write(&cmd->mmgr_io[i]);
                    break;
                case MMGR_READ_PG:
                    ret = lnvm_pg_read(&cmd->mmgr_io[i]);
                    break;
                case MMGR_ERASE_BLK:
                    ret = lnvm_erase_blk(&cmd->mmgr_io[i]);
                    break;
                default:
                    ret = -1;
            }
            if (ret) {
                pthread_mutex_lock(&endio_mutex);
                cmd->status.pg_errors++;
                cmd->status.pgs_p++;
                pthread_mutex_unlock(&endio_mutex);
                lnvm_check_end_io(cmd);
            }
        }
    }

    return 0;
}

static int lnvm_init_channel (struct nvm_channel *ch)
{
    struct lnvm_channel *lch;
    uint32_t tblks;
    int rsv, l_addr, b_addr, pl_addr, trsv, n, pl, n_pl;

    n_pl = ch->geometry->n_of_planes;
    ch->ftl_rsv = FTL_LNVM_RSV_BLK;
    trsv = ch->ftl_rsv * n_pl;
    ch->ftl_rsv_list = malloc (trsv * sizeof(struct nvm_ppa_addr));

    if (!ch->ftl_rsv_list)
        return EMEM;

    memset (ch->ftl_rsv_list, 0, trsv * sizeof(struct nvm_ppa_addr));

    for (n = 0; n < ch->ftl_rsv; n++) {
        for (pl = 0; pl < n_pl; pl++) {
            ch->ftl_rsv_list[n_pl * n + pl].g.blk = n + ch->mmgr_rsv;
            ch->ftl_rsv_list[n_pl * n + pl].g.pl = pl;
        }
    }

    lch = malloc (sizeof(struct lnvm_channel));
    if (!lch)
        return EMEM;

    tblks = ch->geometry->blk_per_lun * ch->geometry->lun_per_ch * n_pl;

    lch->ch = ch;
    lch->bbtbl = malloc (sizeof(uint8_t) * tblks);
    if (!lch->bbtbl)
        return EMEM;

    memset (lch->bbtbl, 0, tblks);

    /* TODO: Verify if bbtbl exists in non-volatile storage
             if not, create it and flush to nvm
             get bbtbl from non-volatile storage
             flush bbtbl to nvm when get a set_bb_tbl with ppa sector > 0
             move the loopings below to a bbtbl create fn */

    /* Set FTL reserved bad blocks */
    for (rsv = 0; rsv < ch->ftl_rsv * n_pl; rsv++){
        l_addr = ch->ftl_rsv_list[rsv].g.lun * ch->geometry->blk_per_lun * n_pl;
        b_addr = ch->ftl_rsv_list[rsv].g.blk * n_pl;
        pl_addr = ch->ftl_rsv_list[rsv].g.pl;
        lch->bbtbl[l_addr + b_addr + pl_addr] = 0x1;
    }

    /* Set MMGR reserved bad blocks */
    for (rsv = 0; rsv < ch->mmgr_rsv  * n_pl; rsv++){
        l_addr = ch->mmgr_rsv_list[rsv].g.lun * ch->geometry->blk_per_lun*n_pl;
        b_addr = ch->mmgr_rsv_list[rsv].g.blk * n_pl;
        pl_addr = ch->mmgr_rsv_list[rsv].g.pl;
        lch->bbtbl[l_addr + b_addr + pl_addr] = 0x1;
    }

    LIST_INSERT_HEAD(&ch_head, lch, entry);
    log_info("    [lnvm: channel %d started with %d bad blocks.\n",ch->ch_id,
                                                    ch->mmgr_rsv+ch->ftl_rsv);
    return 0;
}

static int lnvm_ftl_get_bbtbl (struct nvm_ppa_addr *ppa, uint8_t *bbtbl,
                                                                    uint32_t nb)
{
    struct lnvm_channel *lch = lnvm_get_ch_instance(ppa->g.ch);
    struct nvm_channel *ch = lch->ch;
    int l_addr = ppa->g.lun * ch->geometry->blk_per_lun;

    if (nvm_memcheck(bbtbl) || nvm_memcheck(ch) ||
                                        nb != ch->geometry->blk_per_lun *
                                        (ch->geometry->n_of_planes & 0xffff))
        return -1;

    memcpy(bbtbl, &lch->bbtbl[l_addr], nb);

    return 0;
}

static int lnvm_ftl_set_bbtbl (struct nvm_ppa_addr *ppa, uint8_t value)
{
    int l_addr, n_pl;
    struct lnvm_channel *lch = lnvm_get_ch_instance(ppa->g.ch);

    n_pl = lch->ch->geometry->n_of_planes;

    if ((ppa->g.blk * n_pl + ppa->g.pl) >
                                   (lch->ch->geometry->blk_per_lun * n_pl - 1))
        return -1;

    l_addr = ppa->g.lun * lch->ch->geometry->blk_per_lun * n_pl;
    lch->bbtbl[l_addr + (ppa->g.blk * n_pl + ppa->g.pl)] = value;

    return 0;
}

static void lnvm_exit (struct nvm_ftl *ftl)
{
    struct lnvm_channel *lch;
    LIST_FOREACH(lch, &ch_head, entry){
        free(lch->bbtbl);
    }
    do {
        lch = LIST_FIRST(&ch_head);
        LIST_REMOVE (lch, entry);
        free (lch);
    } while (!LIST_EMPTY(&ch_head));

    pthread_mutex_destroy (&endio_mutex);
}

struct nvm_ftl_ops lnvm_ops = {
    .init_ch     = lnvm_init_channel,
    .submit_io   = lnvm_submit_io,
    .callback_io = lnvm_callback_io,
    .exit        = lnvm_exit,
    .get_bbtbl   = lnvm_ftl_get_bbtbl,
    .set_bbtbl   = lnvm_ftl_set_bbtbl,
};

struct nvm_ftl lnvm = {
    .ftl_id         = FTL_ID_LNVM,
    .name           = "FTL_LNVM",
    .nq             = 8,
    .ops            = &lnvm_ops,
    .cap            = ZERO_32FLAG,
};

int ftl_lnvm_init ()
{
    LIST_INIT(&ch_head);
    pthread_mutex_init (&endio_mutex, NULL);
    lnvm.cap |= 1 << FTL_CAP_GET_BBTBL;
    lnvm.cap |= 1 << FTL_CAP_SET_BBTBL;
    lnvm.bbtbl_format = FTL_BBTBL_BYTE;
    return nvm_register_ftl(&lnvm);
}