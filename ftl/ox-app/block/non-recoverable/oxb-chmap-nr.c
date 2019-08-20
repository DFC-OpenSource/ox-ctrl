/* OX: Open-Channel NVM Express SSD Controller
 *  - Non-recoverable Channel Mapping (deprecated)
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

#include <stdlib.h>
#include <stdio.h>
#include <ox-app.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <libox.h>

extern uint16_t app_nch;
uint8_t         app_map_new;

static int ch_map_create (struct app_channel *lch)
{
    int i;
    struct app_map_entry *ent;
    struct app_map_md *md = lch->map_md;

    for (i = 0; i < md->entries; i++) {
        ent = ((struct app_map_entry *) md->tbl) + i;
        memset (ent, 0x0, sizeof (struct app_map_entry));
        ent->lba = (i * app_nch) + lch->app_ch_id;
    }

    app_map_new = 1;

    return 0;
}

static int ch_map_load (struct app_channel *lch)
{
    int pg;
    struct app_map_md *md = lch->map_md;
    struct nvm_ppa_addr ppa;

    struct nvm_io_data *io = ftl_alloc_pg_io (lch->ch);
    if (io == NULL)
        return -1;

    /* single page planes might be padded to avoid broken entries */
    uint16_t ent_per_pg = (io->pg_sz / sizeof(struct app_map_entry)) * io->n_pl;
    uint16_t md_pgs = md->entries / ent_per_pg;
    if (md->entries % ent_per_pg > 0)
        md_pgs++;

    if (md_pgs > io->ch->geometry->pg_per_blk) {
        log_err("[appnvm ERR: Ch %d -> Maximum Mapping Metadata: %d bytes\n",
                       io->ch->ch_id, io->pg_sz * io->ch->geometry->pg_per_blk);
        goto ERR;
    }

    pg =ftl_blk_current_page (lch->ch, io, lch->map_blk, md_pgs);
    if (pg < 0)
        goto ERR;

    if (!pg) {
        if (ftl_io_rsv_blk (lch->ch, MMGR_ERASE_BLK, NULL, lch->map_blk, 0))
            goto ERR;

        /* tells the caller that the block is new and must be written */
        md->byte.magic = APP_MAGIC;
        goto OUT;

    } else {

        /* load mapping metadata table from nvm */
        pg -= md_pgs;
        ppa.g.pg = pg;
        ppa.g.blk = lch->map_blk;

        if (ftl_nvm_seq_transfer (io, &ppa, md->tbl, md_pgs, ent_per_pg,
                            md->entries, sizeof(struct app_map_entry),
                            NVM_TRANS_FROM_NVM, NVM_IO_RESERVED))
                goto ERR;
    }

    md->byte.magic = 0;

OUT:
    ftl_free_pg_io(io);
    return 0;

ERR:
    ftl_free_pg_io(io);
    return -1;
}

static int ch_map_flush (struct app_channel *lch)
{
    int pg, retry = 0;
    struct app_map_md *md = lch->map_md;
    struct nvm_ppa_addr ppa;

    struct nvm_io_data *io = ftl_alloc_pg_io(lch->ch);
    if (io == NULL)
        return -1;

    /* single page planes might be padded to avoid broken entries */
    uint16_t ent_per_pg = (io->pg_sz / sizeof(struct app_map_entry)) * io->n_pl;
    uint16_t md_pgs = md->entries / ent_per_pg;
    if (md->entries % ent_per_pg > 0)
        md_pgs++;

    if (md_pgs > io->ch->geometry->pg_per_blk) {
        log_err("[appnvm ERR: Ch %d -> Maximum Mapping Metadata: %d bytes\n",
                       io->ch->ch_id, io->pg_sz * io->ch->geometry->pg_per_blk);
        goto ERR;
    }

    pg = ftl_blk_current_page (lch->ch, io, lch->map_blk, md_pgs);
    if (pg < 0)
        goto ERR;

ERASE:
    if (pg >= io->ch->geometry->pg_per_blk - md_pgs) {
        if (ftl_io_rsv_blk (lch->ch, MMGR_ERASE_BLK, NULL, lch->map_blk, 0))
            goto ERR;
        pg = 0;
    }

    md->byte.magic = APP_MAGIC;
    memset (io->buf, 0, io->buf_sz);

    /* set info to OOB area */
    memcpy (io->oob_vec[0], md, sizeof(struct app_map_md));

    /* flush the mapping metadata table to nvm */
    ppa.g.pg = pg;
    ppa.g.blk = lch->map_blk;

    if (ftl_nvm_seq_transfer (io, &ppa, md->tbl, md_pgs, ent_per_pg,
                            md->entries, sizeof(struct app_map_entry),
                            NVM_TRANS_TO_NVM, NVM_IO_RESERVED)) {
        retry++;

        if (retry > 3) {
            goto ERR;
        } else {
            pg = io->ch->geometry->pg_per_blk;
            goto ERASE;
        }
    }

    ftl_free_pg_io(io);
    return 0;

ERR:
    ftl_free_pg_io(io);
    return -1;
}

static void ch_map_mark (struct app_channel *lch, uint64_t addr)
{
    
}

static struct app_map_entry *ch_map_get (struct app_channel *lch, uint32_t off)
{
    return (off >= lch->map_md->entries) ? NULL :
                             ((struct app_map_entry *) lch->map_md->tbl) + off;
}

static struct app_ch_map nr_ch_map = {
    .mod_id     = NR_CH_MAP,
    .name       = "NON-RECOVERABLE-UMAP",
    .create_fn  = ch_map_create,
    .load_fn    = ch_map_load,
    .flush_fn   = ch_map_flush,
    .get_fn     = ch_map_get,
    .mark_fn    = ch_map_mark
};

void nr_ch_map_register (void) {
    app_map_new = 0;
    app_mod_register (APPMOD_CH_MAP, NR_CH_MAP, &nr_ch_map);
}