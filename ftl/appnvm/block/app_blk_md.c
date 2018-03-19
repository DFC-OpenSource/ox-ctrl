/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Flash Translation Layer (Block Metadata Management)
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../../include/ssd.h"
#include "../appnvm.h"

extern pthread_spinlock_t *md_ch_spin;

static int blk_md_create (struct app_channel *lch)
{
    int i;
    struct app_blk_md_entry *ent;
    struct app_blk_md *md = lch->blk_md;

    for (i = 0; i < md->entries; i++) {
        ent = ((struct app_blk_md_entry *) md->tbl) + i;
        memset (ent, 0x0, sizeof (struct app_blk_md_entry));
        ent->ppa.ppa = 0x0;
        ent->ppa.g.ch = lch->ch->ch_id;
        ent->ppa.g.lun = i / lch->ch->geometry->blk_per_lun;
        ent->ppa.g.blk = i % lch->ch->geometry->blk_per_lun;
    }

    return 0;
}

static int blk_md_load (struct app_channel *lch)
{
    int pg;
    struct app_blk_md *md = lch->blk_md;
    struct nvm_ppa_addr ppa;

    struct app_io_data *io = app_alloc_pg_io(lch);
    if (io == NULL)
        return -1;

    /* single page planes might be padded to avoid broken entries */
    uint16_t ent_per_pg = (io->pg_sz / sizeof (struct app_blk_md_entry)) *
                                                                      io->n_pl;
    uint16_t md_pgs = md->entries / ent_per_pg;
    if (md->entries % ent_per_pg > 0)
        md_pgs++;

    if (md_pgs > io->ch->geometry->pg_per_blk) {
        log_err("[appnvm ERR: Ch %d -> Maximum Block Metadata: %d bytes\n",
                       io->ch->ch_id, io->pg_sz * io->ch->geometry->pg_per_blk);
        goto ERR;
    }

    pg = app_blk_current_page (lch, io, lch->meta_blk, md_pgs);
    if (pg < 0)
        goto ERR;

    if (!pg) {
        if (app_io_rsv_blk (lch, MMGR_ERASE_BLK, NULL, lch->meta_blk, 0))
            goto ERR;

        /* tells the caller that the block is new and must be written */
        md->magic = APP_MAGIC;
        goto OUT;

    } else {

        /* load block metadata table from nvm */
        pg -= md_pgs;
        ppa.g.pg = pg;
        ppa.g.blk = lch->meta_blk;

        if (app_nvm_seq_transfer (io, &ppa, md->tbl, md_pgs, ent_per_pg,
                            md->entries, sizeof(struct app_blk_md_entry),
                            APP_TRANS_FROM_NVM, APP_IO_RESERVED))
                goto ERR;
    }

    md->magic = 0;

OUT:
    app_free_pg_io(io);
    return 0;

ERR:
    app_free_pg_io(io);
    return -1;
}

static int blk_md_flush (struct app_channel *lch)
{
    int pg;
    struct app_blk_md *md = lch->blk_md;
    struct nvm_ppa_addr ppa;

    struct app_io_data *io = app_alloc_pg_io(lch);
    if (io == NULL)
        return -1;

    /* single page planes might be padded to avoid broken entries */
    uint16_t ent_per_pg = (io->pg_sz /
                                   sizeof (struct app_blk_md_entry)) * io->n_pl;
    uint16_t md_pgs = md->entries / ent_per_pg;
    if (md->entries % ent_per_pg > 0)
        md_pgs++;

    if (md_pgs > io->ch->geometry->pg_per_blk) {
        log_err("[appnvm ERR: Ch %d -> Maximum Block Metadata: %d bytes\n",
                       io->ch->ch_id, io->pg_sz * io->ch->geometry->pg_per_blk);
        goto ERR;
    }

    pg = app_blk_current_page (lch, io, lch->meta_blk, md_pgs);
    if (pg < 0)
        goto ERR;

    if (pg >= io->ch->geometry->pg_per_blk - md_pgs) {
        if (app_io_rsv_blk (lch, MMGR_ERASE_BLK, NULL, lch->meta_blk, 0))
            goto ERR;
        pg = 0;
    }

    md->magic = APP_MAGIC;
    memset (io->buf, 0, io->buf_sz);

    /* set info to OOB area */
    memcpy (&io->buf[io->pg_sz], md, sizeof(struct app_blk_md));

    /* flush the block metadata table to nvm */
    ppa.g.pg = pg;
    ppa.g.blk = lch->meta_blk;

    if (app_nvm_seq_transfer (io, &ppa, md->tbl, md_pgs, ent_per_pg,
                            md->entries, sizeof(struct app_blk_md_entry),
                            APP_TRANS_TO_NVM, APP_IO_RESERVED))
        goto ERR;

    app_free_pg_io(io);
    return 0;

ERR:
    app_free_pg_io(io);
    return -1;
}

static struct app_blk_md_entry *blk_md_get (struct app_channel *lch,
                                                                  uint16_t lun)
{
    struct app_blk_md *md = lch->blk_md;
    size_t lun_sz = md->entry_sz * lch->ch->geometry->blk_per_lun;

    if (!md->tbl)
        return NULL;

    return (struct app_blk_md_entry *) (md->tbl + (lun * lun_sz));
}

static void blk_md_invalidate (struct app_channel *lch,
                                        struct nvm_ppa_addr *ppa, uint8_t full)
{
    uint16_t pl_i;
    uint8_t *pg_map, off;
    struct app_blk_md_entry *lun;
    struct nvm_mmgr_geometry *g = lch->ch->geometry;

    off = (1 << g->sec_per_pg) - 1;

    lun = appnvm()->md->get_fn (lch, ppa->g.lun);
    pg_map = &lun[ppa->g.blk].pg_state[ppa->g.pg * g->n_of_planes];

    pthread_spin_lock (&md_ch_spin[ppa->g.ch]);

    /* If full is > 0, invalidate all sectors in the page */
    if (full) {
        for (pl_i = 0; pl_i < g->n_of_planes; pl_i++)
            pg_map[pl_i] = off;
        lun[ppa->g.blk].invalid_sec += g->sec_per_pl_pg;
    } else {
        pg_map[ppa->g.pl] |= 1 << ppa->g.sec;
        lun[ppa->g.blk].invalid_sec++;
    }

    pthread_spin_unlock (&md_ch_spin[ppa->g.ch]);
}

static struct app_global_md appftl_md = {
    .mod_id         = APPFTL_BLK_MD,
    .create_fn      = blk_md_create,
    .flush_fn       = blk_md_flush,
    .load_fn        = blk_md_load,
    .get_fn         = blk_md_get,
    .invalidate_fn  = blk_md_invalidate
};

void blk_md_register (void) {
    appnvm_mod_register (APPMOD_BLK_MD, APPFTL_BLK_MD, &appftl_md);
}