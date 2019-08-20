/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX-Block Channel Mapping
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

#define APP_DEBUG_CH_MAP_E  0

extern uint16_t         app_nch;
extern pthread_mutex_t  user_w_mutex;
uint8_t                 app_map_new;

static int oxb_ch_map_create (struct app_channel *lch)
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

    /* Mark all pages for flushing */
    for (i = 0; i < md->tiny.entries; i++)
        md->tiny.dirty[i] = 1;

    return 0;
}

static int oxb_ch_map_load (struct app_channel *lch)
{
    struct app_rec_entry *cp_entry;
    struct app_tiny_entry *tiny;
    struct nvm_io_data *io = NULL;
    struct nvm_channel *ch = NULL;
    uint32_t ent_i, n_cp_entries, ent_left, loaded = 0;
    uint8_t *md_ents;
    struct app_map_md *md = lch->map_md;
    struct nvm_mmgr *mmgr = ox_get_mmgr_instance ();

    cp_entry = oxapp()->recovery->get_fn (APP_CP_CH_MAP + lch->app_ch_id);
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

            if (((struct app_sec_oob *)io->oob_vec[0])->pg_type != APP_PG_MAP_MD)
                log_err ("[ch-map: SMALL mapping page is corrupted. pg_type: "
                        "%d, ch: %d, index: %d\n",
                        ((struct app_sec_oob *)io->oob_vec[0])->pg_type,
                        lch->app_ch_id, ent_i);

            loaded++;

            if (APP_DEBUG_CH_MAP_E)
                printf ("[ch-map: SMALL load. Index: %d, PPA (%d/%d/%d/%d)]\n",
                        ent_i, tiny[ent_i].ppa.g.ch, tiny[ent_i].ppa.g.lun,
                        tiny[ent_i].ppa.g.blk, tiny[ent_i].ppa.g.pg);

        } else {

            /* if an address is zero, reset the table */
            log_err ("[blk-md: Zero address in tiny table. Reseting table in "
                                                    "ch %d.]", lch->app_ch_id);
            md->byte.magic = APP_MAGIC;
            goto OUT;

        }
    }

    if (APP_DEBUG_CH_MAP)
        printf ("[ch-map: Loaded map SMALL pages: %d, ch %d]\n", loaded,
                                                                lch->app_ch_id);

    md->byte.magic = 0;

OUT:
    if (io)
        ftl_free_pg_io (io);
    return 0;
    
}

static int oxb_ch_map_nvm_write (struct app_channel *lch,
                                    uint8_t *buf, uint64_t index, uint64_t *ppa)
{
    struct app_prov_ppas *prov_ppa;
    struct nvm_ppa_addr *addr;
    struct nvm_io_data *io;
    struct app_map_md *md = lch->map_md;
    struct app_log_entry log;
    int sec, ret;
    uint32_t ent_left;
    uint64_t ns;
    struct timespec ts;

    pthread_mutex_lock (&user_w_mutex);

    /* Mapping table pages are mixed with user data */
    prov_ppa = oxapp()->gl_prov->new_fn (1, APP_LINE_USER);
    if (!prov_ppa) {
        pthread_mutex_unlock (&user_w_mutex);
        log_err ("[ch-map: Write error. No PPAs available.]");
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
        ((struct app_sec_oob *) io->oob_vec[sec])->pg_type = APP_PG_MAP_MD;
    }

    ent_left = ((index % md->tiny.entries) < md->tiny.entries - 1) ?
                                md->ent_per_pg : md->entries % md->ent_per_pg;

    ret = ftl_nvm_seq_transfer (io, addr, buf, 1, md->ent_per_pg,
                            ent_left, md->entry_sz,
                            NVM_TRANS_TO_NVM, NVM_IO_NORMAL);

    if (ret)
        goto FREE_IO;

    pthread_mutex_unlock (&user_w_mutex);

    /* Log the write */
    GET_NANOSECONDS (ns, ts);
    log.ts = ns;
    log.type = APP_LOG_MAP_MD;
    log.write.tid = 0;
    log.write.lba = index;
    log.write.new_ppa = addr->ppa;
    log.write.old_ppa = *ppa;

    if (oxapp()->log->append_fn (&log, 1))
        log_err ("[ch-map: Write log was NOT appended.]");

    *ppa = addr->ppa;

    ftl_free_pg_io (io);
    oxapp()->gl_prov->free_fn (prov_ppa);

    return 0;

FREE_IO:
    ftl_free_pg_io (io);
FREE_PPA:
    pthread_mutex_unlock (&user_w_mutex);

    if (app_transaction_close_blk (addr, APP_LINE_USER))
        log_err ("[ch-map: Block was not closed while write failed.]");

    oxapp()->gl_prov->free_fn (prov_ppa);
    return -1;
}

static int oxb_ch_map_flush (struct app_channel *lch)
{
    uint32_t ent_i, index, flushed = 0, retry = APP_MD_IO_RETRY;
    uint8_t *md_ents;
    uint64_t new_ppa;
    struct app_rec_entry cp_entry;
    struct app_map_md *md = lch->map_md;
    struct app_channel *ch_list[app_nch];

    oxapp()->channels.get_list_fn (ch_list, app_nch);

    for (ent_i = 0; ent_i < md->tiny.entries; ent_i++) {
        /* Flush from immutable table */
        md_ents = md->tbl + (md->entry_sz * md->entries) +
                                        (ent_i * md->entry_sz * md->ent_per_pg);

        /* Only flush dirty pages */
        if (md->tiny.dirty[ent_i]) {

RETRY:
            /* Store the old PPA for logging */
            new_ppa = md->tiny.tbl[ent_i].ppa.ppa;

            /* Use a global index among all channels */
            index = (md->tiny.entries * lch->app_ch_id) + ent_i;

            if (oxb_ch_map_nvm_write (lch, md_ents, index, &new_ppa)) {
                retry--;
                log_err("[ch-map: NVM write failed. Retries: %d]",
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

                pthread_spin_lock (&lch->map_md->entry_spin[ent_i]);
                md->tiny.tbl[ent_i].ppa.ppa = new_ppa;
                md->tiny.dirty[ent_i] = 0;
                pthread_spin_unlock (&lch->map_md->entry_spin[ent_i]);

                flushed++;

                if (APP_DEBUG_CH_MAP_E)
                    printf ("[ch-map: SMALL flush. Index: %d]\n", ent_i);

            } else {
                log_err("[ch-map: NVM write failed. Aborted.]");
                return -1;
            }
        }
    }

    if (APP_DEBUG_CH_MAP)
        printf ("[ch-map: Flushed map SMALL pages: %d, ch %d]\n", flushed,
                                                                lch->app_ch_id);

    ox_stats_add_cp (OX_STATS_CP_MAPMD_EVICT, flushed);

    /* Set new tiny table for checkpoint */
    cp_entry.data = md->tiny.tbl;
    cp_entry.size = md->tiny.entries * md->tiny.entry_sz;
    cp_entry.type = APP_CP_CH_MAP + lch->app_ch_id;

    if (oxapp()->recovery->set_fn (&cp_entry))
        log_err("[ch-map: Recovery SET failed. Checkpoint is not consistent.]");

    return 0;
}

static void oxb_ch_map_mark (struct app_channel *lch, uint64_t index)
{
    if (!lch->map_md->tiny.dirty[index]) {
        pthread_spin_lock (&lch->map_md->entry_spin[index]);

        lch->map_md->tiny.dirty[index] = 1;

        pthread_spin_unlock (&lch->map_md->entry_spin[index]);
    }
}

static struct app_map_entry *oxb_ch_map_get (struct app_channel *lch,
                                                                uint32_t off)
{
    return (off >= lch->map_md->entries) ? NULL :
                             ((struct app_map_entry *) lch->map_md->tbl) + off;
}

static struct app_ch_map oxb_ch_map = {
    .mod_id     = OXBLK_CH_MAP,
    .name       = "OX-BLOCK-UMAP",
    .create_fn  = oxb_ch_map_create,
    .load_fn    = oxb_ch_map_load,
    .flush_fn   = oxb_ch_map_flush,
    .get_fn     = oxb_ch_map_get,
    .mark_fn    = oxb_ch_map_mark
};

void oxb_ch_map_register (void) {
    app_map_new = 0;
    app_mod_register (APPMOD_CH_MAP, OXBLK_CH_MAP, &oxb_ch_map);
}