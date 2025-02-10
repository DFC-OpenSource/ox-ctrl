/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX-Block Block Metadata Management
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
#include <string.h>
#include <libox.h>
#include <ox-app.h>


extern pthread_mutex_t user_w_mutex;
extern uint16_t app_nch;

static int oxb_blk_md_create (struct app_channel *lch)
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

    /* Mark all pages for flushing */
    for (i = 0; i < md->tiny.entries; i++)
        md->tiny.dirty[i] = 1;

    return 0;
}

static int oxb_blk_md_load (struct app_channel *lch)
{
    struct app_rec_entry *cp_entry;
    struct app_tiny_entry *tiny;
    struct nvm_io_data *io = NULL;
    struct nvm_channel *ch = NULL;
    uint32_t ent_i, n_cp_entries, ent_left;
    uint8_t *md_ents;
    struct app_blk_md *md = lch->blk_md;
    struct nvm_mmgr *mmgr = ox_get_mmgr_instance ();

    cp_entry = oxapp()->recovery->get_fn (APP_CP_BLK_MD + lch->app_ch_id);
    if (!cp_entry) {
        md->byte.magic = APP_MAGIC;
        return 0;
    }

    tiny = (struct app_tiny_entry *) cp_entry->data;
    n_cp_entries = cp_entry->size / sizeof (struct app_tiny_entry);

    memcpy (md->tiny.tbl, tiny, cp_entry->size);

    /* Rebuild table from checkpoint */
    for (ent_i = 0; ent_i < n_cp_entries; ent_i++) {
        md_ents = md->tbl + (ent_i * md->entry_sz * md->ent_per_pg);

        if (tiny[ent_i].ppa.ppa) {

            if (!ch || (ch->ch_id != tiny[ent_i].ppa.g.ch)) {
                if (io)
                    ftl_free_pg_io (io);

                /* Get correct channel from media manager */
                ch = &mmgr->ch_info[tiny[ent_i].ppa.g.ch];
                if (!ch)
                    return -1;

                /* Initialize basic channel info. Channels are not ready yet */
                ch->ch_id       = tiny[ent_i].ppa.g.ch;
                ch->geometry    = mmgr->geometry;
                ch->mmgr        = mmgr;

                io = ftl_alloc_pg_io (ch);
                if (io == NULL)
                    return -1;
            }

            ent_left = (ent_i < n_cp_entries - 1) ?
                                md->ent_per_pg : md->entries % md->ent_per_pg;

            if (ftl_nvm_seq_transfer (io, &tiny[ent_i].ppa, md_ents, 1,
                                md->ent_per_pg, ent_left, md->entry_sz,
                                NVM_TRANS_FROM_NVM, NVM_IO_NORMAL)) {
                ftl_free_pg_io (io);
                return -1;
            }
            if (((struct app_sec_oob *)io->oob_vec[0])->pg_type != APP_PG_BLK_MD)
                log_err ("[blk-md: Metadata page is corrupted. pg_type: "
                        "%d, ch: %d, index: %d\n",
                        ((struct app_sec_oob *)io->oob_vec[0])->pg_type,
                        lch->app_ch_id, ent_i);

        } else {

            /* if an address is zero, reset the table */
            log_err ("[blk-md: Zero address in tiny table. Reseting table in "
                                                    "ch %d.]", lch->app_ch_id);
            md->byte.magic = APP_MAGIC;
            goto OUT;

        }
    }

    md->byte.magic = 0;

OUT:
    if (io)
        ftl_free_pg_io (io);
    return 0;
}

static int oxb_blk_md_nvm_write (struct app_channel *lch,
            uint8_t *buf, uint64_t index, uint32_t entries, uint64_t *ppa)
{
    struct app_prov_ppas *prov_ppa;
    struct nvm_ppa_addr *addr;
    struct nvm_io_data *io;
    struct app_blk_md *md = lch->blk_md;
    struct app_log_entry log;
    int sec, ret;
    uint64_t ns;
    struct timespec ts;

    pthread_mutex_lock (&user_w_mutex);

    /* Block metadata pages are mixed with user data */
    prov_ppa = oxapp()->gl_prov->new_fn (1, APP_LINE_USER);
    if (!prov_ppa) {
        pthread_mutex_unlock (&user_w_mutex);
        log_err ("[blk-md: Write error. No PPAs available.]");
        return -1;
    }

    if (prov_ppa->nppas != lch->ch->geometry->sec_per_pl_pg)
        log_err ("[blk-md: NVM write. wrong PPAs. nppas %d]", prov_ppa->nppas);

    addr = &prov_ppa->ppa[0];
    io = ftl_alloc_pg_io(prov_ppa->ch[addr->g.ch]->ch);
    if (io == NULL)
        goto FREE_PPA;

    for (sec = 0; sec < io->ch->geometry->sec_per_pl_pg; sec++) {
        ((struct app_sec_oob *) io->oob_vec[sec])->lba = index;
        ((struct app_sec_oob *) io->oob_vec[sec])->pg_type = APP_PG_BLK_MD;
    }

    ret = ftl_nvm_seq_transfer (io, addr, buf, 1, md->ent_per_pg,
                            entries, md->entry_sz,
                            NVM_TRANS_TO_NVM, NVM_IO_NORMAL);

    if (ret)
        goto FREE_IO;

    pthread_mutex_unlock (&user_w_mutex);

    /* Log the write */
    GET_NANOSECONDS (ns, ts);
    log.ts = ns;
    log.type = APP_LOG_BLK_MD;
    log.write.tid = 0;
    log.write.lba = index;
    log.write.new_ppa = addr->ppa;
    log.write.old_ppa = *ppa;

    if (oxapp()->log->append_fn (&log, 1))
        log_err ("[blk-md: Write log was NOT appended.]");

    *ppa = addr->ppa;

    ftl_free_pg_io (io);
    oxapp()->gl_prov->free_fn (prov_ppa);

    return 0;

FREE_IO:
    ftl_free_pg_io (io);
FREE_PPA:
    pthread_mutex_unlock (&user_w_mutex);

    if (app_transaction_close_blk (addr, APP_LINE_USER))
        log_err ("[blk-md: Block was not closed while write failed.]");

    oxapp()->gl_prov->free_fn (prov_ppa);
    return -1;
}

static int oxb_blk_md_flush (struct app_channel *lch)
{
    uint32_t ent_i, index, entries, flushed = 0, retry = APP_MD_IO_RETRY;
    uint8_t *md_ents;
    uint64_t new_ppa;
    struct app_rec_entry cp_entry;
    struct app_blk_md *md = lch->blk_md;
    struct app_channel *ch_list[app_nch];
    uint8_t pg_buf[lch->ch->geometry->pl_pg_size];

    oxapp()->channels.get_list_fn (ch_list, app_nch);

    memset (pg_buf, 0x0, lch->ch->geometry->pl_pg_size);

    for (ent_i = 0; ent_i < md->tiny.entries; ent_i++) {

        /* Only flush dirty pages */
        if (md->tiny.dirty[ent_i]) {
            md_ents = md->tbl + (ent_i * md->entry_sz * md->ent_per_pg);
            entries = (ent_i == md->tiny.entries - 1) ?
                                            md->entries % md->ent_per_pg :
                                            md->ent_per_pg;

            /* Copy the page to an immutable buffer */
            pthread_spin_lock (&lch->blk_md->entry_spin[ent_i]);
            memcpy (pg_buf, md_ents, entries * md->entry_sz);
            pthread_spin_unlock (&lch->blk_md->entry_spin[ent_i]);

RETRY:
            /* Store the old PPA for logging */
            new_ppa = md->tiny.tbl[ent_i].ppa.ppa;

            /* Use a global index among all channels */
            index = (md->tiny.entries * lch->app_ch_id) + ent_i;

            if (oxb_blk_md_nvm_write (lch, pg_buf, index, entries, &new_ppa)){
                retry--;
                log_err("[blk-md: NVM write failed. Retries: %d]",
                                                      APP_MD_IO_RETRY - retry);
                usleep (APP_MD_IO_RETRY_DELAY);
                if (retry)
                    goto RETRY;
            }

            if (retry) {

                /* Invalidate old page PPA */
                if (md->tiny.tbl[ent_i].ppa.ppa) {
                    oxapp()->md->invalidate_fn (
                            ch_list[md->tiny.tbl[ent_i].ppa.g.ch],
                            &md->tiny.tbl[ent_i].ppa, APP_INVALID_PAGE);
                }

                pthread_spin_lock (&lch->blk_md->entry_spin[ent_i]);
                md->tiny.tbl[ent_i].ppa.ppa = new_ppa;
                md->tiny.dirty[ent_i] = 0;
                pthread_spin_unlock (&lch->blk_md->entry_spin[ent_i]);

                flushed++;

            } else {
                log_err("[blk-md: NVM write failed. Aborted.]");
                return -1;
            }
        }
    }

    if (APP_DEBUG_BLK_MD)
        printf ("[blk_md: Flushed blk-md SMALL pages: %d, ch %d\n]", flushed,
                                                                lch->app_ch_id);

    ox_stats_add_cp (OX_STATS_CP_BLK_EVICT, flushed);

    /* Set new tiny table for checkpoint */
    cp_entry.data = md->tiny.tbl;
    cp_entry.size = md->tiny.entries * md->tiny.entry_sz;
    cp_entry.type = APP_CP_BLK_MD + lch->app_ch_id;

    if (oxapp()->recovery->set_fn (&cp_entry))
        log_err("[blk-md: Recovery SET failed. Checkpoint is not consistent.]");

    return 0;
}

static struct app_blk_md_entry *oxb_blk_md_get (struct app_channel *lch,
                                                                  uint16_t lun)
{
    struct app_blk_md *md = lch->blk_md;
    struct app_blk_md_entry *blk;
    uint64_t lun_sz = md->entry_sz * lch->ch->geometry->blk_per_lun;

    if (!md->tbl)
        return NULL;

    blk = (struct app_blk_md_entry *) (md->tbl + ((uint64_t)lun * lun_sz));

    return blk;
}

/* Marks a blk_md page as dirty, to be flushed to storage.
 * Index: Index of the dirty blk_md page in the tiny dirty array
 * 	  Generally, it is the sequenctial blk id within a channel
 * 	  divided by the number of blk_md entries in a flash page */
static void oxb_blk_md_mark (struct app_channel *lch, uint64_t index)
{
    if (!lch->blk_md->tiny.dirty[index]) {
        pthread_spin_lock (&lch->blk_md->entry_spin[index]);

        lch->blk_md->tiny.dirty[index] = 1;

        pthread_spin_unlock (&lch->blk_md->entry_spin[index]);
    }
}

static void oxb_blk_md_invalidate (struct app_channel *lch,
                                        struct nvm_ppa_addr *ppa, uint8_t full)
{
    uint16_t pl_i;
    uint8_t *pg_map, off;
    struct app_blk_md_entry *lun;
    struct nvm_mmgr_geometry *g = lch->ch->geometry;
    uint32_t blk_i;
    uint64_t index;

    off = (uint8_t)((1 << g->sec_per_pg) - 1);
    blk_i = (g->blk_per_lun * ppa->g.lun) + ppa->g.blk;

    lun = oxapp()->md->get_fn (lch, ppa->g.lun);
    pg_map = &lun[ppa->g.blk].pg_state[ppa->g.pg * g->n_of_planes];

    index = blk_i / lch->blk_md->ent_per_pg;
    pthread_spin_lock (&lch->blk_md->entry_spin[index]);

    /* If full is > 0, invalidate all sectors in the page */
    if (full) {

        for (pl_i = 0; pl_i < g->n_of_planes; pl_i++) {
            if (pg_map[pl_i] == off) {
                if (APP_DEBUG_BLK_MD)
                    printf ("[blk-md: Duplicated page invalidation "
                            "(%d/%d/%d/%d/%d)]\n", ppa->g.ch, ppa->g.lun,
                                                  ppa->g.blk, ppa->g.pg, pl_i);

                pthread_spin_unlock (&lch->blk_md->entry_spin[index]);
                return;
            }
            pg_map[pl_i] = off;
        }
        lun[ppa->g.blk].invalid_sec += g->sec_per_pl_pg;

    } else {

        if (pg_map[ppa->g.pl] & (1 << ppa->g.sec)) {
            if (APP_DEBUG_BLK_MD)
                printf ("[blk-md: Duplicated sector invalidation "
                        "(%d/%d/%d/%d/%d/%d)]\n", ppa->g.ch, ppa->g.lun,
                        ppa->g.blk, ppa->g.pg, ppa->g.pl, ppa->g.sec);

            pthread_spin_unlock (&lch->blk_md->entry_spin[index]);
            return;
        }
        pg_map[ppa->g.pl] |= 1 << ppa->g.sec;
        lun[ppa->g.blk].invalid_sec++;
    }

    pthread_spin_unlock (&lch->blk_md->entry_spin[index]);

    /* mark page for flushing */
    oxapp()->md->mark_fn (lch, index);
}

static struct app_global_md oxb_md = {
    .mod_id         = OXBLK_BLK_MD,
    .name           = "OX-BLOCK-BLKMD",
    .create_fn      = oxb_blk_md_create,
    .flush_fn       = oxb_blk_md_flush,
    .load_fn        = oxb_blk_md_load,
    .get_fn         = oxb_blk_md_get,
    .invalidate_fn  = oxb_blk_md_invalidate,
    .mark_fn        = oxb_blk_md_mark
};

void oxb_blk_md_register (void) {
    app_mod_register (APPMOD_BLK_MD, OXBLK_BLK_MD, &oxb_md);
}
