/* OX: Open-Channel NVM Express SSD Controller
 * 
 *  - LightNVM Flash Translation Layer
 *
 * Copyright 2017 IT University of Copenhagen
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
#include <lnvm_ftl.h>
#include <libox.h>
#include <nvme.h>
#include <ox-lightnvm.h>

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
    return ox_submit_mmgr(cmd);
}

static int lnvm_pg_write (struct nvm_mmgr_io_cmd *cmd)
{
    return ox_submit_mmgr(cmd);
}

static int lnvm_erase_blk (struct nvm_mmgr_io_cmd *cmd)
{
    return ox_submit_mmgr(cmd);
}

/* If all pages were processed, it checks for errors. If all succeed, finish
 * cmd. Otherwise, retry only pages with error.
 *
 * Calls to this fn come from submit_io or from end_pg_io
 */
static int lnvm_check_end (struct nvm_io_cmd *cmd)
{
    if (cmd->status.pgs_p == cmd->status.total_pgs) {

        pthread_mutex_lock (&cmd->mutex);
        /* if true, some pages failed */
        if ( lnvm_check_pgmap_complete (cmd->status.pg_map,
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
    pthread_mutex_unlock (&cmd->mutex);
    lnvm_submit_io (cmd);
    goto RETURN;

COMPLETE:
    pthread_mutex_unlock (&cmd->mutex);
    ox_ftl_callback (cmd);

RETURN:
    return 0;
}

static int lnvm_check_pg (struct nvm_io_cmd *cmd, uint8_t index)
{
    int c;
    struct nvm_ppa_addr *ppa;
    struct nvm_mmgr_geometry *g = cmd->mmgr_io[index].ch->geometry;
    struct nvm_mmgr_io_cmd *mio = &cmd->mmgr_io[index];

    mio->nvm_io = cmd;

    if (cmd->cmdtype == MMGR_ERASE_BLK) {
        mio->ppa = cmd->ppalist[index];
        mio->ch = cmd->channel[index];
        return 0;
    }

    mio->ppa = cmd->ppalist[mio->sec_offset];
    mio->ch = cmd->channel[mio->sec_offset];

    /* Writes must follow a correct page ppa sequence, while reads are allowed
     * at sector granularity */
    if (cmd->cmdtype == MMGR_WRITE_PG) {
        for (c = 1; c < g->sec_per_pg; c++) {
            ppa = &cmd->ppalist[mio->sec_offset + c];
            if (ppa->g.ch != mio->ppa.g.ch || ppa->g.lun != mio->ppa.g.lun ||
                    ppa->g.blk != mio->ppa.g.blk ||
                    ppa->g.pg != mio->ppa.g.pg   ||
                    ppa->g.pl != mio->ppa.g.pl   ||
                    ppa->g.sec != mio->ppa.g.sec + c) {
                log_err ("[ERROR ftl_lnvm: Wrong write ppa sequence. "
                                                         "Aborting IO cmd.\n");
                return -1;
            }
        }
    }

    if (cmd->sec_sz != g->sec_size)
        return -1;

    /* Build the MMGR command at page granularity, but PRP for empty sectors
     * are kept 0. The empty PRPs are checked in the MMGR for DMA. */
    mio->sec_sz = cmd->sec_sz;
    mio->md_sz = g->pg_oob_sz;

    memset(mio->prp, 0x0, sizeof(uint64_t) * g->sec_per_pg);
    for (c = 0; c < mio->n_sectors; c++)
        mio->prp[cmd->ppalist[mio->sec_offset + c].g.sec] =
                                                cmd->prp[mio->sec_offset + c];

    mio->pg_sz = g->pg_size;
    mio->n_sectors = mio->pg_sz / mio->sec_sz;

    mio->md_prp = cmd->md_prp[index];

    return 0;
}

static int lnvm_check_io (struct nvm_io_cmd *cmd)
{
    int i;

    if (cmd->status.pgs_p == 0) {
        for (i = 0; i < cmd->status.total_pgs; i++)
            lnvm_set_pgmap(cmd->status.pg_map, i, FTL_PGMAP_ON);
    }

    cmd->status.pgs_p = cmd->status.pgs_s;

    if (cmd->cmdtype == MMGR_ERASE_BLK)
        return 0;

    if (cmd->status.total_pgs > 64 || cmd->status.total_pgs == 0){
        cmd->status.status = NVM_IO_FAIL;
        return cmd->status.nvme_status = NVME_INVALID_FORMAT;
    }

    return 0;
}

static void lnvm_decompact_write (struct nvm_mmgr_io_cmd *cmd)
{
    struct nvm_mmgr_geometry *g = cmd->ch->geometry;

    for (int pl = 1; pl < g->n_of_planes; pl++) {
        for (int sec = 0; sec < g->sec_per_pg; sec++)
            cmd->prp[g->sec_per_pg * pl + sec] = 0x0;

        if (cmd->status == NVM_IO_SUCCESS) {
            lnvm_set_pgmap(cmd->nvm_io->status.pg_map,
                                            cmd->pg_index + pl, FTL_PGMAP_OFF);
            pthread_mutex_lock(&cmd->nvm_io->mutex);
            cmd->nvm_io->status.pgs_s++;
        } else {
            pthread_mutex_lock(&cmd->nvm_io->mutex);
            cmd->status = NVM_IO_FAIL;
            cmd->nvm_io->status.pg_errors++;
        }

        cmd->nvm_io->status.pgs_p++;
        pthread_mutex_unlock(&cmd->nvm_io->mutex);
    }

    cmd->n_sectors = g->sec_per_pg;
}

static void lnvm_callback (struct nvm_mmgr_io_cmd *cmd)
{
    if (cmd->cmdtype == MMGR_WRITE_PG &&
                                    (cmd->ch->mmgr->flags & MMGR_FLAG_PL_CMD))
        lnvm_decompact_write (cmd);

    if (cmd->status == NVM_IO_SUCCESS) {
        lnvm_set_pgmap (cmd->nvm_io->status.pg_map,
                                                cmd->pg_index, FTL_PGMAP_OFF);
        pthread_mutex_lock (&cmd->nvm_io->mutex);
        cmd->nvm_io->status.pgs_s++;
    } else {
        pthread_mutex_lock (&cmd->nvm_io->mutex);
        cmd->nvm_io->status.pg_errors++;
    }

    cmd->nvm_io->status.pgs_p++;
    pthread_mutex_unlock (&cmd->nvm_io->mutex);

    lnvm_check_end (cmd->nvm_io);
}

static void lnvm_compact_write (struct nvm_io_cmd *cmd, uint16_t i)
{
    struct nvm_mmgr_geometry *g = cmd->mmgr_io[i].ch->geometry;

    for (int pl = 1; pl < g->n_of_planes; pl++) {
        for (int sec = 0; sec < g->sec_per_pg; sec++)
            cmd->mmgr_io[i].prp[g->sec_per_pg* pl + sec] =
                                                cmd->mmgr_io[i + pl].prp[sec];
        cmd->mmgr_io[i + pl].status = NVM_IO_PROCESS;
    }
    cmd->mmgr_io[i].n_sectors = g->sec_per_pl_pg;
}

static int lnvm_submit_io (struct nvm_io_cmd *cmd)
{
    uint8_t compact;
    int ret, i, pl;
    struct nvm_mmgr_geometry *g;

    ret = lnvm_check_io (cmd);
    if (ret) return ret;

    for (i = 0; i < cmd->status.total_pgs; i++) {

        /* if true, page not processed yet */
        if ( cmd->status.pg_map[i / 8] & (1 << (i % 8)) ) {
            if (lnvm_check_pg (cmd, i)) {
                cmd->status.status = NVM_IO_FAIL;
                cmd->status.nvme_status = NVME_INVALID_FORMAT;
                return -1;
            }
        }
    }

    for (i = 0; i < cmd->status.total_pgs; i++) {
        g = cmd->mmgr_io[i].ch->geometry;

        /* if true, page not processed yet */
        if ( cmd->status.pg_map[i / 8] & (1 << (i % 8)) ) {

            compact = (cmd->mmgr_io[i].ch->mmgr->flags & MMGR_FLAG_PL_CMD) ?
                                                                         1 : 0;
            cmd->mmgr_io[i].status = NVM_IO_PROCESS;
            switch (cmd->cmdtype) {
                case MMGR_WRITE_PG:

                    if (compact)
                        lnvm_compact_write(cmd, i);

                    ret = lnvm_pg_write(&cmd->mmgr_io[i]);

                    if (compact && ret)
                        for (pl = 1; pl < g->n_of_planes; pl++)
                            lnvm_decompact_write (&cmd->mmgr_io[i + pl]);

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
                pthread_mutex_lock (&cmd->mutex);
                cmd->status.pg_errors++;
                cmd->status.pgs_p++;
                pthread_mutex_unlock (&cmd->mutex);
                cmd->mmgr_io[i].status = NVM_IO_FAIL;
                lnvm_check_end (cmd);
            }

            if (compact && (cmd->cmdtype == MMGR_WRITE_PG))
                i += g->n_of_planes - 1;
        }
    }

    return 0;
}

static int lnvm_init_channel (struct nvm_channel *ch)
{
    uint32_t tblks;
    int ret, trsv, n, pl, n_pl;
    struct lnvm_channel *lch;
    struct lnvm_bbtbl *bbt;
    struct nvm_ppa_addr *ppa;

    n_pl = ch->geometry->n_of_planes;
    ch->ftl_rsv = FTL_LNVM_RSV_BLK_COUNT;
    trsv = ch->ftl_rsv * n_pl;
    ch->ftl_rsv_list = ox_realloc (ch->ftl_rsv_list,
                          trsv * sizeof(struct nvm_ppa_addr), OX_MEM_MMGR);

    if (!ch->ftl_rsv_list)
        return EMEM;

    memset (ch->ftl_rsv_list, 0, trsv * sizeof(struct nvm_ppa_addr));

    for (n = 0; n < ch->ftl_rsv; n++) {
        for (pl = 0; pl < n_pl; pl++) {
            ppa = &ch->ftl_rsv_list[n_pl * n + pl];
            ppa->g.ch = ch->ch_mmgr_id;
            ppa->g.lun = 0;
            ppa->g.blk = n + ch->mmgr_rsv;
            ppa->g.pl = pl;
        }
    }

    lch = ox_malloc (sizeof(struct lnvm_channel), OX_MEM_FTL_LNVM);
    if (!lch)
        return EMEM;

    tblks = ch->geometry->blk_per_lun * ch->geometry->lun_per_ch * n_pl;

    lch->ch = ch;

    ret = EMEM;
    lch->bbtbl = ox_malloc (sizeof(struct lnvm_bbtbl), OX_MEM_FTL_LNVM);
    if (!lch->bbtbl)
        goto FREE_LCH;

    bbt = lch->bbtbl;
    bbt->tbl = ox_malloc (sizeof(uint8_t) * tblks, OX_MEM_FTL_LNVM);
    if (!bbt->tbl)
        goto FREE_BBTBL;

    memset (bbt->tbl, 0, tblks);
    bbt->magic = 0;
    bbt->bb_sz = tblks;

    ret = lnvm_get_bbt_nvm(lch);
    if (ret) goto ERR;

    /* create and flush bad block table if it does not exist */
    /* this procedure will erase the entire device (only in test mode) */
    if (bbt->magic == FTL_LNVM_MAGIC) {
        printf("\n [lnvm: Channel %d. Creating bad block table...]", ch->ch_id);
        fflush(stdout);
        ret = lnvm_bbt_create (lch, bbt, LNVM_BBT_EMERGENCY);
        if (ret) goto ERR;
        ret = lnvm_flush_bbt (lch);
        if (ret) goto ERR;
    }

    LIST_INSERT_HEAD(&ch_head, lch, entry);
    log_info("    [lnvm: channel %d started with %d bad blocks.]\n",ch->ch_id,
                                                                bbt->bb_count);
    return 0;

ERR:
    ox_free (bbt->tbl, OX_MEM_FTL_LNVM);
FREE_BBTBL:
    ox_free (bbt, OX_MEM_FTL_LNVM);
FREE_LCH:
    ox_free (lch, OX_MEM_FTL_LNVM);
    log_err("[lnvm ERR: Ch %d -> Not possible to read/create bad block "
                                                        "table.]\n", ch->ch_id);
    return ret;
}

static int lnvm_ftl_get_bbtbl (struct nvm_ppa_addr *ppa, uint8_t *bbtbl,
                                                                    uint32_t nb)
{
    struct lnvm_channel *lch = lnvm_get_ch_instance(ppa->g.ch);
    struct nvm_channel *ch = lch->ch;
    int l_addr = ppa->g.lun * ch->geometry->blk_per_lun *
                                                     ch->geometry->n_of_planes;

    if (nb != ch->geometry->blk_per_lun * (ch->geometry->n_of_planes & 0xffff))
        return -1;

    memcpy(bbtbl, &lch->bbtbl->tbl[l_addr], nb);

    return 0;
}

static int lnvm_ftl_set_bbtbl (struct nvm_ppa_addr *ppa, uint8_t value)
{
    int l_addr, n_pl, flush, ret;
    struct lnvm_channel *lch = lnvm_get_ch_instance(ppa->g.ch);

    n_pl = lch->ch->geometry->n_of_planes;

    if ((ppa->g.blk * n_pl + ppa->g.pl) >
                                   (lch->ch->geometry->blk_per_lun * n_pl - 1))
        return -1;

    l_addr = ppa->g.lun * lch->ch->geometry->blk_per_lun * n_pl;

    /* flush the table if the value changes */
    flush = (lch->bbtbl->tbl[l_addr+(ppa->g.blk * n_pl + ppa->g.pl)] == value)
                                                                        ? 0 : 1;
    lch->bbtbl->tbl[l_addr + (ppa->g.blk * n_pl + ppa->g.pl)] = value;

    if (flush) {
        ret = lnvm_flush_bbt (lch);
        if (ret)
            log_info("[ftl WARNING: Error flushing bad block table to NVM!]");
    }

    return 0;
}

static void lnvm_exit (void)
{
    struct lnvm_channel *lch;

    LIST_FOREACH(lch, &ch_head, entry){
        ox_free(lch->bbtbl->tbl, OX_MEM_FTL_LNVM);
        ox_free(lch->bbtbl, OX_MEM_FTL_LNVM);
    }
    while (!LIST_EMPTY(&ch_head)) {
        lch = LIST_FIRST(&ch_head);
        LIST_REMOVE (lch, entry);
        ox_free(lch, OX_MEM_FTL_LNVM);
    }
    pthread_mutex_destroy (&endio_mutex);
}

struct nvm_ftl_ops lnvm_ops = {
    .init_ch     = lnvm_init_channel,
    .submit_io   = lnvm_submit_io,
    .callback_io = lnvm_callback,
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

int ftl_lnvm_init (void)
{
    if (!ox_mem_create_type ("FTL_LNVM", OX_MEM_FTL_LNVM))
        return -1;

    LIST_INIT(&ch_head);
    pthread_mutex_init (&endio_mutex, NULL);
    lnvm.cap |= 1 << FTL_CAP_GET_BBTBL;
    lnvm.cap |= 1 << FTL_CAP_SET_BBTBL;
    lnvm.bbtbl_format = FTL_BBTBL_BYTE;
    return ox_register_ftl(&lnvm);
}