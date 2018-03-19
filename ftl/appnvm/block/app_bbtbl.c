/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Flash Translation Layer (Bad Block Table Management)
 *
 * Copyright 2018 IT University of Copenhagen.
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
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 *
 * Partially supported by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 */

#include "../../../include/ssd.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../appnvm.h"

/* Erases the entire channel, failed erase marks the block as bad
 * bbt -> bad block table pointer to be filled up
 * bbt_sz -> pointer to integer, function will set it up
 * ch  -> channel to be checked
 */

static struct nvm_ppa_addr *app_check_ch_bb (struct nvm_ppa_addr *bbt,
                      uint16_t *bbt_sz,  struct nvm_channel *ch, uint8_t type)
{
    int ret = 0, i, pl, pl2, pg, blk, lun, bb_count = 0;
    struct nvm_mmgr_io_cmd *cmd;
    uint8_t n_pl = ch->geometry->n_of_planes;
    uint8_t *bufw, *bufr;

    log_info("    [appnvm: Checking bad blocks on channel %d...]\n",ch->ch_id);

    /* Prevents channel erasing if not in test mode (disabled in QEMU) */
    /*
    if (!core.tests_init->init) {
        log_info("[appnvm ERR: Please, run in test mode.]\n");
        return NULL;
    }
    */

    cmd = malloc(sizeof(struct nvm_mmgr_io_cmd));
    if (!cmd)
        return NULL;

    bufw = malloc(NVM_PG_SIZE + NVM_OOB_SIZE);
    if (!bufw) {
        free (cmd);
        return NULL;
    }
    memset (bufw, 0xac, NVM_PG_SIZE + NVM_OOB_SIZE);

    bufr = malloc(NVM_PG_SIZE + NVM_OOB_SIZE);
    if (!bufr) {
        free (cmd);
        free (bufw);
        return NULL;
    }

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
                if (nvm_contains_ppa(ch->mmgr_rsv_list, ch->mmgr_rsv *
                                                                n_pl, cmd->ppa))
                    continue;
                if (nvm_contains_ppa(ch->ftl_rsv_list, ch->ftl_rsv *
                                                                n_pl, cmd->ppa))
                    continue;

                if ((ret = nvm_submit_sync_io (ch, cmd, NULL, MMGR_ERASE_BLK)))
                    goto MARK_BLK;

                if ((pl == n_pl - 1) && (type == APP_BBT_FULL)) {
                    for (pg = 0; pg < ch->geometry->pg_per_blk; pg++) {
                        cmd->ppa.g.pg = pg;
                        for (pl2 = 0; pl2 < n_pl; pl2++) {
                            cmd->ppa.g.pl = pl2;
                            if ((ret = nvm_submit_sync_io
                                                (ch, cmd, bufw, MMGR_WRITE_PG)))
                                goto MARK_BLK;
                        }
                    }
                    for (pg = 0; pg < ch->geometry->pg_per_blk; pg++) {
                        cmd->ppa.g.pg = pg;
                        for (pl2 = 0; pl2 < n_pl; pl2++) {
                            cmd->ppa.g.pl = pl2;
                            memset (bufr, 0x0, NVM_PG_SIZE + NVM_OOB_SIZE);
                            if ((ret = nvm_submit_sync_io
                                                 (ch, cmd, bufr, MMGR_READ_PG)))
                                goto MARK_BLK;

                            if ((ret = memcmp (bufw, bufr,
                                                   NVM_PG_SIZE + NVM_OOB_SIZE)))
                                goto MARK_BLK;
                        }
                    }
                }

MARK_BLK:
                if (ret) {
                    /* Avoids adding the same block (multiple plane failure) */
                    if (nvm_contains_ppa(bbt, bb_count, cmd->ppa))
                        continue;

                    bb_count = bb_count + n_pl;
                    bbt = realloc(bbt, sizeof(struct nvm_ppa_addr) * bb_count);

                    /* fill up bb table for all planes */
                    for (i = n_pl; i > 0; i--) {
                        memcpy(&bbt[bb_count - i], &cmd->ppa, sizeof(uint64_t));
                        bbt[bb_count - i].g.pl = n_pl - i;
                    }
                    log_info("      [appnvm: bad block: lun %d, blk %d\n",
                                                                      lun, blk);
                }
            }
            printf("\r");
            printf(" [appnvm: Channel %d. Creating bad block table... (%d/%d)]",
                  ch->ch_id, (lun * ch->geometry->blk_per_lun * n_pl) +
                  ((blk+1) * n_pl),
                  ch->geometry->blk_per_lun * ch->geometry->lun_per_ch * n_pl);
            fflush(stdout);
        }
    }
    free(cmd);
    free(bufw);
    free(bufr);
    *bbt_sz = bb_count;

    return bbt;
}

static uint16_t app_count_bb (struct app_channel *lch)
{
    int i, bb = 0;

    for (i = 0; i < lch->bbtbl->bb_sz; i++)
        if (lch->bbtbl->tbl[i] != 0x0)
            bb++;

    return bb;
}

static int bbt_byte_flush (struct app_channel *lch)
{
    int ret, pg, i;
    struct app_bbtbl nvm_bbt;
    struct nvm_channel *ch = lch->ch;
    struct app_bbtbl *bbt = lch->bbtbl;
    uint8_t n_pl = ch->geometry->n_of_planes;
    uint32_t pg_sz = ch->geometry->pg_size;
    uint8_t *buf;
    uint32_t meta_sz = ch->geometry->sec_oob_sz * ch->geometry->sec_per_pg;
    uint32_t buf_sz = pg_sz + meta_sz;
    void *buf_vec[n_pl];

    if (bbt->bb_sz > pg_sz) {
        log_err("[appnvm ERR: Ch %d -> Maximum Bad block Table size: %d blocks\n",
                                                            ch->ch_id, pg_sz);
        return -1;
    }

    buf = calloc(buf_sz * n_pl, 1);
    if (!buf)
        return EMEM;

    for (i = 0; i < n_pl; i++)
        buf_vec[i] = buf + i * buf_sz;

    pg = 0;
    do {
        memset (buf, 0, buf_sz * n_pl);
        ret = app_io_rsv_blk (lch, MMGR_READ_PG, buf_vec, lch->bbt_blk, pg);

        /* get info from OOB area (64 bytes) in plane 0 */
        memcpy(&nvm_bbt, buf + pg_sz, sizeof(struct app_bbtbl));

        if (ret || nvm_bbt.magic != APP_MAGIC)
            break;

        pg++;
    } while (pg < ch->geometry->pg_per_blk);

    if (ret)
        goto OUT;

    if (pg == ch->geometry->pg_per_blk) {
        if (app_io_rsv_blk (lch, MMGR_ERASE_BLK, NULL, lch->bbt_blk, 0))
            goto OUT;
        pg = 0;
    }

    memset (buf, 0, buf_sz * n_pl);

    /* set info to OOB area */
    bbt->magic = APP_MAGIC;
    bbt->bb_count = app_count_bb (lch);
    memcpy (&buf[pg_sz], bbt, sizeof(struct app_bbtbl));

    /* set bad block table */
    memcpy (buf, bbt->tbl, bbt->bb_sz);

    ret = app_io_rsv_blk (lch, MMGR_WRITE_PG, buf_vec, lch->bbt_blk, pg);

OUT:
    free(buf);
    return ret;
}

static int bbt_byte_create (struct app_channel *lch, uint8_t type)
{
    int i, rsv, l_addr, b_addr, pl_addr, n_pl;
    struct nvm_ppa_addr *bbt_tmp;
    uint16_t bb_count = 0;
    struct nvm_channel *ch = lch->ch;
    struct app_bbtbl *bbt = lch->bbtbl;

    n_pl = ch->geometry->n_of_planes;
    bbt_tmp = malloc (sizeof(struct nvm_ppa_addr));
    if (!bbt_tmp)
        return -1;

    memset (bbt->tbl, 0, bbt->bb_sz);

    /* Set FTL reserved bad blocks */
    for (rsv = 0; rsv < ch->ftl_rsv * n_pl; rsv++){
        l_addr = ch->ftl_rsv_list[rsv].g.lun * ch->geometry->blk_per_lun * n_pl;
        b_addr = ch->ftl_rsv_list[rsv].g.blk * n_pl;
        pl_addr = ch->ftl_rsv_list[rsv].g.pl;
        bbt->tbl[l_addr + b_addr + pl_addr] = NVM_BBT_DMRK;
    }

    /* Set MMGR reserved bad blocks */
    for (rsv = 0; rsv < ch->mmgr_rsv  * n_pl; rsv++){
        l_addr = ch->mmgr_rsv_list[rsv].g.lun * ch->geometry->blk_per_lun*n_pl;
        b_addr = ch->mmgr_rsv_list[rsv].g.blk * n_pl;
        pl_addr = ch->mmgr_rsv_list[rsv].g.pl;
        bbt->tbl[l_addr + b_addr + pl_addr] = NVM_BBT_DMRK;
    }

    printf(" [appnvm: Channel %d. Creating bad block table...]", ch->ch_id);
    fflush(stdout);

    if (type == APP_BBT_FULL || type == APP_BBT_ERASE) {
        /* Check for bad blocks in the whole channel */
        bbt_tmp = app_check_ch_bb (bbt_tmp, &bb_count, ch, type);
        if (!bbt_tmp)
            return -1;
    } else {
        log_info("  [appnvm: Emergency bad block table created on channel %d. "
               "It is recommended the creation using full scan.]\n", ch->ch_id);
        printf ("\n  [WARNING: Emergency bad block table created on channel %d."
                "\n             Use 'ox-ctrl-test admin -t create-bbt'\n",
                                                                     ch->ch_id);
    }

    printf("\n");

    lch->bbtbl->bb_count = bb_count;

    for (i = 0; i < bb_count; i++) {
        l_addr = bbt_tmp[i].g.lun * ch->geometry->blk_per_lun * n_pl;
        b_addr = bbt_tmp[i].g.blk * n_pl;
        pl_addr = bbt_tmp[i].g.pl;
        bbt->tbl[l_addr + b_addr + pl_addr] = NVM_BBT_DMRK;
    }

    return 0;
}

static int bbt_byte_load (struct app_channel *lch)
{
    int ret, pg, i;
    struct app_bbtbl nvm_bbt;
    struct nvm_channel *ch = lch->ch;
    struct app_bbtbl *bbt = lch->bbtbl;
    uint8_t n_pl = ch->geometry->n_of_planes;
    uint32_t pg_sz = ch->geometry->pg_size;
    uint8_t *buf_vec[n_pl];
    uint8_t *buf;
    uint32_t meta_sz = ch->geometry->sec_oob_sz * ch->geometry->sec_per_pg;
    uint32_t buf_sz = pg_sz + meta_sz;

    if (bbt->bb_sz > pg_sz) {
        log_err("[appnvm ERR: Ch %d -> Maximum Bad block Table size: %d blocks\n",
                                                            ch->ch_id, pg_sz);
        return -1;
    }

    buf = calloc(buf_sz * n_pl, 1);
    if (!buf)
        return EMEM;

    for (i = 0; i < n_pl; i++)
        buf_vec[i] = buf + i * buf_sz;

    pg = 0;
    do {
        memset (buf, 0, buf_sz * n_pl);
        ret = app_io_rsv_blk (lch, MMGR_READ_PG, (void **) buf_vec,
                                                            lch->bbt_blk, pg);

        /* get info from OOB area (64 bytes) in plane 0 */
        memcpy(&nvm_bbt, buf + pg_sz, sizeof(struct app_bbtbl));

        if (ret || nvm_bbt.magic != APP_MAGIC)
            break;

        /* copy bad block table to channel */
        memcpy(bbt->tbl, buf, bbt->bb_sz);

        pg++;
    } while (pg < ch->geometry->pg_per_blk);

    if (ret)
        goto OUT;

    if (!pg) {
        ret = app_io_rsv_blk (lch, MMGR_ERASE_BLK, NULL, lch->bbt_blk, 0);

        /* tells the caller that the block is new and must be written */
        bbt->magic = APP_MAGIC;
        goto OUT;
    }

    bbt->magic = 0;
    bbt->bb_count = app_count_bb (lch);

OUT:
    free(buf);
    return ret;
}

static uint8_t *bbt_byte_get (struct app_channel *lch, uint16_t lun)
{
    struct app_bbtbl *bbt = lch->bbtbl;
    size_t lun_sz = sizeof (uint8_t) * lch->ch->geometry->blk_per_lun *
                                                lch->ch->geometry->n_of_planes;

    if (!bbt->tbl)
        return NULL;

    return bbt->tbl + (lun * lun_sz);
}

static struct app_global_bbt appftl_bbt = {
    .mod_id    = APPFTL_BBT,
    .create_fn = bbt_byte_create,
    .flush_fn  = bbt_byte_flush,
    .load_fn   = bbt_byte_load,
    .get_fn    = bbt_byte_get
};

void bbt_byte_register (void) {
    appnvm_mod_register (APPMOD_BBT, APPFTL_BBT, &appftl_bbt);
}