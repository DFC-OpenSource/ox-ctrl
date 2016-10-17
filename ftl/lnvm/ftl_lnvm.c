/* OX: OpenChannel NVM Express SSD Controller
 *
 * Copyright (C) 2016, IT University of Copenhagen. All rights reserved.
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 *
 * Funding support provided by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#include "../../include/ssd.h"

#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/queue.h>
#include "ftl_lnvm.h"

LIST_HEAD(lnvm_ch, nvm_channel) ch_head = LIST_HEAD_INITIALIZER(ch_head);

static pthread_mutex_t endio_mutex;

static struct nvm_channel *lnvm_get_ch_instance(uint16_t ch_id)
{
    struct nvm_channel *ch;
    LIST_FOREACH(ch, &ch_head, entry){
        if(ch->ch_mmgr_id == ch_id)
            return ch;
    }

    return NULL;
}

static void lnvm_set_pgmap(uint8_t *pgmap, uint8_t index)
{
    pthread_mutex_lock(&endio_mutex);
    pgmap[index / 8] |= 1 << (index % 8);
    pthread_mutex_unlock(&endio_mutex);
}

static int lnvm_check_pgmap_complete (uint8_t *pgmap) {
    int sum, i;
    for (i = 0; i < 8; i++) {
        sum += pgmap[i] ^ 0xff;
    }

    return sum;
}

static int lnvm_pg_read (struct nvm_mmgr_io_cmd *cmd)
{
    return nvm_submit_mmgr_io(cmd);
}

static int lnvm_pg_write (struct nvm_mmgr_io_cmd *cmd)
{
    return nvm_submit_mmgr_io(cmd);
}

static int lnvm_erase_blk (struct nvm_mmgr_io_cmd *cmd)
{
    return nvm_submit_mmgr_io(cmd);
}

/* If all pages were processed, it checks for errors. If all succeed, finish
 * cmd. Otherwise, retry only pages with error.
 *
 * Calls to this fn come from submit_io or from end_pg_io
 */
static void lnvm_check_end_io (struct nvm_io_cmd *cmd)
{
    if (cmd->status.pgs_p == cmd->status.total_pgs) {
        pthread_mutex_lock(&endio_mutex);
        if ( lnvm_check_pgmap_complete(cmd->status.pg_map) ) { //IO not ended
            cmd->status.ret_t++;
            if (cmd->status.ret_t <= FTL_LNVM_IO_RETRY) {
                log_err ("[FTL WARNING: Cmd resubmitted due failed pages]\n");
                nvm_submit_io(cmd);
            } else {
                cmd->status.status = NVM_IO_FAIL;
                cmd->status.nvme_status = NVME_DATA_TRAS_ERROR;
                nvm_complete_io(cmd);
            }
        } else {
            cmd->status.status = NVM_IO_SUCCESS;
            cmd->status.nvme_status = NVME_SUCCESS;
            nvm_complete_io(cmd);
        }
        pthread_mutex_unlock(&endio_mutex);
    }
}

/* TODO: For now, we assume all the pages will be back from MMGR,
 * we dont define a timeout to finish the IO.
 */
void lnvm_callback_io (struct nvm_mmgr_io_cmd *cmd)
{
    if (cmd->status == NVM_IO_SUCCESS) {
        lnvm_set_pgmap(cmd->nvm_io->status.pg_map, cmd->pg_index);
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

    if (cmd->sec_offset && (cmd->cmdtype != MMGR_ERASE_BLK)) {
        cmd->status.total_pgs == (cmd->n_sec / LNVM_SEC_PG) + 1;
    }

    for (i = 64; i > cmd->status.total_pgs; i--){
        lnvm_set_pgmap(cmd->status.pg_map, i-1);
    }

    cmd->status.pgs_p = cmd->status.pgs_s;

    // TODO: if erase, wait for the ongoing W/R to complete before erase blk;
    // During erasing, block all r/w to the target block until erase completion
    if (cmd->cmdtype == MMGR_ERASE_BLK)
        return 0;

    if (cmd->status.total_pgs * LNVM_SEC_PG > 64
                                                || cmd->status.total_pgs == 0){
        cmd->status.status = NVM_IO_FAIL;
        return cmd->status.nvme_status = NVME_INVALID_FORMAT;
    }

    // TODO: check page/NAND constrains before r/w

    return 0;
}

int lnvm_submit_io (struct nvm_io_cmd *cmd)
{
    int ret, i;
    ret = lnvm_check_io(cmd);
    if (ret) return ret;

    for (i = 0; i < cmd->status.total_pgs; i++) {
        if ((cmd->status.pg_map[i / 8] & 1 << (i % 8)) == 0) {
            if (lnvm_check_pg_io(cmd, i)) {
                cmd->status.status = NVM_IO_FAIL;
                cmd->status.nvme_status = NVME_INVALID_FORMAT;
                return -1;
            }
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

int lnvm_init_channel (struct nvm_channel *ch)
{
    /* Blks 2 and 3 (block 1 for dual-plane) in the lun 0 belong to lnvm */
    ch->ftl_rsv = 2;
    ch->ftl_rsv_list = realloc(ch->ftl_rsv_list, ch->ftl_rsv *
                                                sizeof(struct nvm_ppa_addr));
    if (nvm_memcheck(ch->ftl_rsv_list))
        return EMEM;

    ch->ftl_rsv_list[0].ppa = (uint64_t) (2 & AND64);
    ch->ftl_rsv_list[1].ppa = (uint64_t) (3 & AND64);

    LIST_INSERT_HEAD(&ch_head, ch, entry);
    log_info("    [lnvm: channel %d started with %d bad blocks.\n",ch->ch_id,
                                                    ch->mmgr_rsv+ch->ftl_rsv);
    return 0;
}

int lnvm_ftl_get_bbtbl (struct nvm_ppa_addr *ppa, uint8_t *bbtbl, uint32_t nb)
{
    int j;
    struct nvm_channel *ch = lnvm_get_ch_instance(ppa->g.ch);

    if (nvm_memcheck(bbtbl) || nvm_memcheck(ch) ||
                                        nb != ch->geometry->blk_per_lun *
                                        (ch->geometry->n_of_planes & 0xffff))
        return -1;

    memset(bbtbl, 0, nb);

    /* TODO: get bbtbl from non-volatile storage */
    /* TODO: set rsv bad blks when start ch and flush to nvm */

    /* Set FTL reserved bad blocks */
    for (j = 0; j < ch->ftl_rsv; j++){
        if (ch->ftl_rsv_list[j].g.lun == ppa->g.lun)
            bbtbl[ch->ftl_rsv_list[j].g.blk] = 0x1;
    }

    /* Set MMGR reserved bad blocks */
    for (j = 0; j < ch->mmgr_rsv; j++){
        if (ch->mmgr_rsv_list[j].g.lun == ppa->g.lun)
            bbtbl[ch->mmgr_rsv_list[j].g.blk] = 0x1;
    }

    return 0;
}

int lnvm_ftl_set_bbtbl (struct nvm_ppa_addr *ppa, uint32_t nb)
{
    return 0;
}

void lnvm_exit (struct nvm_ftl *ftl)
{
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