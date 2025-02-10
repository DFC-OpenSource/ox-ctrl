/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Flash Translation Layer (Bad Block Table Management)
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

#include <libox.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ox-app.h>

/* Erases the entire channel, failed erase marks the block as bad
 * bbt -> bad block table pointer to be filled up
 * bbt_sz -> pointer to integer, function will set it up
 * ch  -> channel to be checked
 */

extern struct core_struct core;

static int oxb_check_ch_bb (struct nvm_ppa_addr *bbt,
                      uint16_t *bbt_sz,  struct nvm_channel *ch, uint8_t type)
{
    int ret = 0, i, pl, plc, pg, blk, lun, bb_count = 0;
    struct nvm_mmgr_io_cmd *cmd;
    struct nvm_mmgr_geometry *g = ch->geometry;
    struct nvm_io_data *bufw, *bufr;
    uint8_t n_pl = g->n_of_planes;

    log_info("    [ox-blk: Checking bad blocks on channel %d...]\n",ch->ch_id);

    bufw = ftl_alloc_pg_io (ch);
    if (!bufw)
        return -1;

    bufr = ftl_alloc_pg_io (ch);
    if (!bufr) {
        ftl_free_pg_io (bufw);
        return -1;
    }

    cmd = ox_malloc(sizeof(struct nvm_mmgr_io_cmd), OX_MEM_OXAPP);
    if (!cmd) {
        ftl_free_pg_io (bufr);
        ftl_free_pg_io (bufw);
        return -1;
    }

    memset (bufw->buf, NVM_MAGIC, bufw->buf_sz);

    for (lun = 0; lun < ch->geometry->lun_per_ch; lun++) {
        for (blk = 0; blk < ch->geometry->blk_per_lun; blk++) {
            for (pl = 0; pl < n_pl; pl++) {
                memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
                cmd->ppa.g.blk = blk;
                cmd->ppa.g.pl = pl;
                cmd->ppa.g.ch = ch->ch_mmgr_id;
                cmd->ppa.g.lun = lun;
                cmd->ppa.g.pg = 0;

                /* Prevents erasing reserved blocks */
                if (ox_contains_ppa(ch->mmgr_rsv_list, ch->mmgr_rsv *
                                                                n_pl, cmd->ppa))
                    continue;
                if (ox_contains_ppa(ch->ftl_rsv_list, ch->ftl_rsv *
                                                                n_pl, cmd->ppa))
                    continue;

                if ((ret = ox_submit_sync_io (ch, cmd, NULL, MMGR_ERASE_BLK)))
                    goto MARK_BLK;

                if ((pl == n_pl - 1) && (type == APP_BBT_FULL)) {
                    for (pg = 0; pg < ch->geometry->pg_per_blk; pg++) {
                        cmd->ppa.g.pg = pg;
                        
                        if ( (ret = ftl_pg_io_switch (ch, MMGR_WRITE_PG,
                            (void **)bufw->pl_vec, &cmd->ppa, NVM_IO_NORMAL)))
                            goto MARK_BLK;
                    }
                    for (pg = 0; pg < ch->geometry->pg_per_blk; pg++) {
                        cmd->ppa.g.pg = pg;
                        memset (bufr->buf, 0x0, bufr->buf_sz);
                        if ( (ret = ftl_pg_io_switch (ch, MMGR_READ_PG,
                            (void **)bufr->pl_vec, &cmd->ppa, NVM_IO_NORMAL)))
                            goto MARK_BLK;

                        /* Compare only data, not OOB */
                        for (plc = 0; plc < n_pl; plc++)
                            if ((ret = memcmp (bufw->pl_vec[plc],
                                             bufr->pl_vec[plc], bufw->pg_sz)))
                                goto MARK_BLK;
                    }
                }

MARK_BLK:
                if (ret) {
                    /* Avoids adding the same block (multiple plane failure) */
                    if (ox_contains_ppa(bbt, bb_count, cmd->ppa))
                        continue;

                    bb_count = bb_count + n_pl;
                    bbt = ox_realloc(bbt, sizeof(struct nvm_ppa_addr) *
                                                       bb_count, OX_MEM_OXAPP);

                    /* fill up bb table for all planes */
                    for (i = n_pl; i > 0; i--) {
                        memcpy(&bbt[bb_count - i], &cmd->ppa, sizeof(uint64_t));
                        bbt[bb_count - i].g.pl = n_pl - i;
                    }
                    log_info("      [ox-blk: bad block: lun %d, blk %d\n",
                                                                      lun, blk);
                }
            }
            printf("\r");
            printf(" [ox-blk: Channel %d. Creating bad block table (type %d)... (%d/%d)]",
                  ch->ch_id, type, (lun * ch->geometry->blk_per_lun * n_pl) +
                  ((blk+1) * n_pl),
                  ch->geometry->blk_per_lun * ch->geometry->lun_per_ch * n_pl);
            fflush(stdout);
        }
    }
    ox_free(cmd, OX_MEM_OXAPP);
    ftl_free_pg_io (bufr);
    ftl_free_pg_io (bufw);
    *bbt_sz = bb_count;

    return 0;
}

static uint16_t oxb_count_bb (struct app_channel *lch)
{
    int i, bb = 0;

    for (i = 0; i < lch->bbtbl->bb_sz; i++)
        if (lch->bbtbl->tbl[i] != 0x0)
            bb++;

    return bb;
}

static int oxb_bbt_byte_flush (struct app_channel *lch)
{
    int ret, pg;
    struct app_bbtbl nvm_bbt;
    struct nvm_io_data *io;
    struct nvm_ppa_addr ppa;
    struct nvm_channel *ch = lch->ch;
    struct app_bbtbl *bbt = lch->bbtbl;
    uint32_t pg_sz = ch->geometry->pg_size;

    if (bbt->bb_sz > pg_sz) {
        log_err("[ox-blk ERR: Ch %d -> Maximum Bad block Table size: %d blocks. Current: %d\n",
                                                            ch->ch_id, pg_sz, bbt->bb_sz);
        return -1;
    }

    io = ftl_alloc_pg_io (lch->ch);
    if (io == NULL)
        return -1;

    pg = 0;
    do {
        ppa.ppa = 0;
        ppa.g.blk = lch->bbt_blk;
        ppa.g.pg = pg;
        ret = ftl_pg_io_switch (lch->ch, MMGR_READ_PG,
                                  (void **) io->pl_vec, &ppa, NVM_IO_RESERVED);

        /* get info from OOB area (16 bytes - header of struct ftl_bbtbl) */
        memcpy(&nvm_bbt, io->oob_vec[0], 16);

        if (ret || nvm_bbt.byte.magic != NVM_MAGIC)
            break;

        pg++;
    } while (pg < ch->geometry->pg_per_blk);

    if (ret)
        goto OUT;

    if (pg == ch->geometry->pg_per_blk) {
        if (ftl_io_rsv_blk (lch->ch, MMGR_ERASE_BLK, NULL, lch->bbt_blk, 0))
            goto OUT;
        pg = 0;
    }

    /* Max of 8 * 1024 blocks per channel */
    memset (io->buf, 0, pg_sz);

    /* set info to OOB area */
    bbt->byte.magic = NVM_MAGIC;
    bbt->bb_count = oxb_count_bb (lch);
    memcpy (io->oob_vec[0], bbt, 16);

    /* set bad block table */
    memcpy (io->buf, bbt->tbl, bbt->bb_sz);

    ppa.ppa = 0;
    ppa.g.blk = lch->bbt_blk;
    ppa.g.pg = pg;
    ret = ftl_pg_io_switch (lch->ch, MMGR_WRITE_PG,
                                  (void **) io->pl_vec, &ppa, NVM_IO_RESERVED);
    if (ret) {
        pg = 0;
        if (ftl_io_rsv_blk (lch->ch, MMGR_ERASE_BLK, NULL, lch->bbt_blk, 0))
            goto OUT;
        ppa.g.pg = pg;
        ret = ftl_pg_io_switch (lch->ch, MMGR_WRITE_PG,
                                  (void **) io->pl_vec, &ppa, NVM_IO_RESERVED);
    }

OUT:
    ftl_free_pg_io (io);
    return ret;
}

static int oxb_bbt_byte_create (struct app_channel *lch, uint8_t type)
{
    int i, rsv, l_addr, b_addr, pl_addr, n_pl;
    struct nvm_ppa_addr *bbt_tmp;
    uint16_t bb_count = 0;
    struct nvm_channel *ch = lch->ch;
    struct app_bbtbl *bbt = lch->bbtbl;

    n_pl = ch->geometry->n_of_planes;
    bbt_tmp = ox_malloc (sizeof(struct nvm_ppa_addr), OX_MEM_OXAPP);
    if (!bbt_tmp)
        return -1;

    memset (bbt->tbl, 0, bbt->bb_sz);

    /* Set FTL reserved bad blocks */
    for (rsv = 0; rsv < ch->ftl_rsv * n_pl; rsv++){
        l_addr = ch->ftl_rsv_list[rsv].g.lun * ch->geometry->blk_per_lun * n_pl;
        b_addr = ch->ftl_rsv_list[rsv].g.blk * n_pl;
        pl_addr = ch->ftl_rsv_list[rsv].g.pl;
        bbt->tbl[l_addr + b_addr + pl_addr] = LNVM_BBT_DMRK;
    }

    /* Set MMGR reserved bad blocks */
    for (rsv = 0; rsv < ch->mmgr_rsv  * n_pl; rsv++){
        l_addr = ch->mmgr_rsv_list[rsv].g.lun * ch->geometry->blk_per_lun*n_pl;
        b_addr = ch->mmgr_rsv_list[rsv].g.blk * n_pl;
        pl_addr = ch->mmgr_rsv_list[rsv].g.pl;
        bbt->tbl[l_addr + b_addr + pl_addr] = LNVM_BBT_DMRK;
    }

    if (type == APP_BBT_FULL || type == APP_BBT_ERASE) {
        /* Check for bad blocks in the whole channel */
        if (oxb_check_ch_bb (bbt_tmp, &bb_count, ch, type)) {
            ox_free (bbt_tmp, OX_MEM_OXAPP);
            return -1;
        }

    } else {
        log_info("  [ox-blk: Emergency bad block table created on channel %d. "
               "A FAST or FULL scan is recommended.]\n", ch->ch_id);
        if (core.debug)
            printf ("\n  [WARNING: Emergency bad block table created on channel %d."
                "\n   ! FAST or FULL scan is recommended. Use 'admin create-bbt'\n",
                                                                     ch->ch_id);
    }

    lch->bbtbl->bb_count = bb_count;

    for (i = 0; i < bb_count; i++) {
        l_addr = bbt_tmp[i].g.lun * ch->geometry->blk_per_lun * n_pl;
        b_addr = bbt_tmp[i].g.blk * n_pl;
        pl_addr = bbt_tmp[i].g.pl;
        bbt->tbl[l_addr + b_addr + pl_addr] = APP_BBT_DMRK;
    }

    ox_free (bbt_tmp, OX_MEM_OXAPP);
    return 0;
}

static int oxb_bbt_byte_load (struct app_channel *lch)
{
    int ret, pg;
    struct app_bbtbl nvm_bbt;
    struct nvm_io_data *io;
    struct nvm_ppa_addr ppa;
    struct nvm_channel *ch = lch->ch;
    struct app_bbtbl *bbt = lch->bbtbl;
    uint32_t pg_sz = ch->geometry->pg_size;

    if (bbt->bb_sz > pg_sz) {
        log_err("[ox-blk ERR: Ch %d -> Maximum Bad block Table size: %d blocks. Current: %d\n",
                                                            ch->ch_id, pg_sz, bbt->bb_sz);
        return -1;
    }

    io = ftl_alloc_pg_io (lch->ch);
    if (io == NULL)
        return -1;

    pg = 0;
    do {
        ppa.ppa = 0;
        ppa.g.blk = lch->bbt_blk;
        ppa.g.pg = pg;
        ret = ftl_pg_io_switch (lch->ch, MMGR_READ_PG,
                                  (void **) io->pl_vec, &ppa, NVM_IO_RESERVED);

        /* get info from OOB area (16 bytes - header of struct ftl_bbtbl) */
        memcpy(&nvm_bbt, io->oob_vec[0], 16);

        if (ret || nvm_bbt.byte.magic != NVM_MAGIC)
            break;

        /* copy bad block table to channel */
        memcpy(bbt->tbl, io->buf, bbt->bb_sz);

        pg++;
    } while (pg < ch->geometry->pg_per_blk);

    if (ret)
        goto OUT;

    if (!pg) {
        ret = ftl_io_rsv_blk (lch->ch, MMGR_ERASE_BLK, NULL, lch->bbt_blk, 0);

        /* tells the caller that the block is new and must be written */
        bbt->byte.magic = NVM_MAGIC;
        goto OUT;
    }

    bbt->byte.magic = 0;
    bbt->bb_count = oxb_count_bb (lch);

OUT:
    ftl_free_pg_io (io);
    return ret;
}

static uint8_t *oxb_bbt_byte_get (struct app_channel *lch, uint16_t lun)
{
    struct app_bbtbl *bbt = lch->bbtbl;
    size_t lun_sz = sizeof (uint8_t) * lch->ch->geometry->blk_per_lun *
                                                lch->ch->geometry->n_of_planes;

    if (!bbt->tbl)
        return NULL;

    return bbt->tbl + (lun * lun_sz);
}

static struct app_global_bbt oxblk_bbt = {
    .mod_id    = OXBLK_BBT,
    .name      = "OX-BLOCK-BBT",
    .create_fn = oxb_bbt_byte_create,
    .flush_fn  = oxb_bbt_byte_flush,
    .load_fn   = oxb_bbt_byte_load,
    .get_fn    = oxb_bbt_byte_get
};

void oxb_bbt_byte_register (void) {
    app_mod_register (APPMOD_BBT, OXBLK_BBT, &oxblk_bbt);
}
