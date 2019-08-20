/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Garbage Collecting
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
#include <sys/queue.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include <libox.h>

#define BLOCK_GC_PARALLEL_CH   4
#define BLOCK_GC_DELAY_US      10000
#define BLOCK_GC_DELAY_CH_BUSY 1000

extern uint16_t              app_nch;
static struct app_channel  **ch;
static pthread_t             check_th;
static uint8_t               stop;
static struct nvm_io_data ***gc_buf;
static uint16_t              buf_pg_sz, buf_oob_sz, buf_npg;

static uint32_t gc_recycled_blks;
static volatile uint64_t gc_moved_sec, gc_pad_sec, gc_err_sec, gc_wro_sec,
                                       gc_map_pgs, gc_blk_pgs, gc_mapmd_pgs,
                                       gc_log_pgs, gc_race_sma, gc_race_big;

static pthread_mutex_t *gc_cond_mutex;
static pthread_cond_t  *gc_cond;

extern pthread_mutex_t gc_w_mutex;
extern pthread_mutex_t user_w_mutex;

struct egc_th_arg {
    uint16_t            tid;
    uint16_t            bufid;
    uint8_t             waiting;
    struct app_channel *lch;
};

static int block_gc_bucket_sort (struct app_blk_md_entry **list,
                    uint32_t list_sz, uint32_t n_buckets, uint32_t min_invalid)
{
    uint32_t stop, bi, j, k, ip, bucketc[++n_buckets];
    struct app_blk_md_entry **bucket[n_buckets];
    uint16_t sec_per_blk = ch[0]->ch->geometry->sec_per_blk;
    int ret = -1;

    memset (bucketc, 0, n_buckets * sizeof (uint32_t));
    memset (bucket, 0, n_buckets * sizeof (struct app_blk_md_entry *));

    for(j = 0; j < list_sz; j++) {;
        ip = list[j]->invalid_sec;
        if (ip) {
            if (ip > sec_per_blk) {
                log_err ("[gc: Block (%d/%d/%d) has %d of a maximum of %d "
                    "invalid sectors.]", list[j]->ppa.g.ch, list[j]->ppa.g.lun,
                    list[j]->ppa.g.blk, ip, sec_per_blk);
                goto FREE;
            }
            bucketc[ip]++;
            bucket[ip] = ox_realloc (bucket[ip], bucketc[ip] *
                        sizeof (struct app_blk_md_entry *), OX_MEM_OXBLK_GC);
            if (!bucket[ip])
                goto FREE;
            bucket[ip][bucketc[ip] - 1] = list[j];
        }
    }

    k = 0;
    stop = 0;
    for (bi = n_buckets - 1; bi > 0; bi--) {
        for (j = 0; j < bucketc[bi]; j++) {
            if (((float) bi / (float) n_buckets < APP_GC_TARGET_RATE &&
                            (k >= APP_GC_MAX_BLKS)) || (bi < min_invalid)) {
                stop++;
                break;
            }
            list[k] = bucket[bi][j];
            k++;
        }
        if (stop) break;
    }
    ret = k;

FREE:
    for(bi = 0; bi < n_buckets; bi++)
        if (bucket[bi])
            ox_free (bucket[bi], OX_MEM_OXBLK_GC);
    return ret;
}

static struct app_blk_md_entry **block_gc_get_target_blks
                                    (struct app_channel *lch, uint32_t *c)
{
    int nblks;
    struct app_rec_entry *cp_entry;
    struct app_blk_md_entry *lun;
    struct app_blk_md_entry **list = NULL;
    uint32_t lun_i, blk_i, min_inv, ch_blk_i;
    float count = 0, avlb = 0, inv_rate;
    uint64_t blk_ts, cp_ns, cp_ts = 0;

    cp_entry = oxapp()->recovery->get_fn (APP_CP_TIMESTAMP);
    if (cp_entry)
        cp_ts = *((uint64_t *) cp_entry->data);

    cp_ns = (uint64_t) APP_CP_INTERVAL * (uint64_t) 1000000000;
    cp_ts -= (cp_ts >= cp_ns * 2) ? cp_ns * 2 : 0;

    for (lun_i = 0; lun_i < lch->ch->geometry->lun_per_ch; lun_i++) {

        lun = oxapp()->md->get_fn (lch, lun_i);
        if (!lun) {
            *c = 0;
            return NULL;
        }

        for (blk_i = 0; blk_i < lch->ch->geometry->blk_per_lun; blk_i++) {

            if (!(lun[blk_i].flags & APP_BLK_MD_AVLB))
                continue;

            ch_blk_i = (lun_i * lch->ch->geometry->blk_per_lun) + blk_i;
            blk_ts = lch->blk_md->closed_ts[ch_blk_i];

            /* Do not GC log blocks closed after last 3 checkpoints */
            if ((lun[blk_i].flags & APP_BLK_MD_META) && (blk_ts > cp_ts))
                continue;

            if (    (lun[blk_i].flags & APP_BLK_MD_USED) &&
                   !(lun[blk_i].flags & APP_BLK_MD_OPEN) &&
                     lun[blk_i].current_pg == lch->ch->geometry->pg_per_blk &&
                     lun[blk_i].invalid_sec > 0) {

                count += 1;
                list = ox_realloc(list, sizeof(struct app_blk_md_entry *) *
                                                (int) count, OX_MEM_OXBLK_GC);
                if (!list)
                    goto FREE;
                list[(int)count - 1] = &lun[blk_i];
            }
            avlb++;
        }
    }

    /* Compute minimum of invalid pages for targeting a block */
    min_inv = lch->ch->geometry->sec_per_blk * APP_GC_TARGET_RATE;
    inv_rate = 1.0 - ((count / avlb - APP_GC_THRESD) /
                                                     (1.0 - APP_GC_THRESD));
    if ((count / avlb) >= APP_GC_THRESD)
        min_inv *= inv_rate;

    nblks = block_gc_bucket_sort(list, count, lch->ch->geometry->sec_per_blk,
                                                                    min_inv);
    if (nblks <= 0)
        goto FREE;

    *c = nblks;
    return list;

FREE:
    if (count > 1)
        ox_free (list, OX_MEM_OXBLK_GC);
    *c = 0;
    return NULL;
}

static int block_gc_check_valid_pg (struct app_channel *lch,
                                  struct app_blk_md_entry *blk_md, uint16_t pg)
{
    uint8_t *pg_map;
    uint8_t map, off, pl;

    pg_map = &blk_md->pg_state[pg * lch->ch->geometry->n_of_planes];
    map    = lch->ch->geometry->n_of_planes;
    off    = (1 << lch->ch->geometry->sec_per_pg) - 1;

    for (pl = 0; pl < lch->ch->geometry->n_of_planes; pl++)
        if ((pg_map[pl] & off) == off)
            map--;

    return map;
}

static int block_gc_proc_mapping_pg (struct app_channel *lch,
                        struct nvm_io_data *io, struct nvm_ppa_addr *old_ppa,
                        struct app_sec_oob *oob)
{
    struct app_transaction_t *tr;
    struct app_prov_ppas *ppas;
    struct app_transaction_user ent;
    uint32_t sec_pl_pg = lch->ch->geometry->sec_per_pl_pg;
    uint64_t lba;
    int ret;

    lba = oob->lba;
    tr = app_transaction_new (&lba, 1, APP_TR_GC_MAP);
    if (!tr)
        return -1;

    ent.entries = ox_calloc (sec_pl_pg, sizeof (struct app_transaction_log *),
                                                            OX_MEM_OXBLK_GC);
    if (!ent.entries)
        goto ABORT;

    ent.tid = lch->app_ch_id;
    ent.count = sec_pl_pg;
    ent.entries[0] = &tr->entries[0];

    pthread_mutex_lock (&user_w_mutex);

    ppas = app_transaction_alloc_list (&ent, APP_TR_GC_MAP);
    ox_free (ent.entries, OX_MEM_OXBLK_GC);

    if (!ppas || ppas->nppas < sec_pl_pg) {
        pthread_mutex_unlock (&user_w_mutex);
        goto FREE_PPA;
    }

    if (ftl_pg_io_switch (lch->ch, MMGR_WRITE_PG,
                        (void **) io->pl_vec, &ppas->ppa[0], NVM_IO_NORMAL)) {

        pthread_mutex_unlock (&user_w_mutex);
        app_transaction_close_blk (&ppas->ppa[0], APP_LINE_USER);
        goto FREE_PPA;
    }

    pthread_mutex_unlock (&user_w_mutex);

    ox_stats_add_gc (APP_PG_MAP, 1, sec_pl_pg);

    ret = oxapp()->gl_map->upsert_md_fn(oob->lba, ppas->ppa[0].ppa,old_ppa->ppa);
    if (ret > 0) {

        oxapp()->md->invalidate_fn (lch, &ppas->ppa[0], APP_INVALID_PAGE);

        /* Other write thread had already updated the metadata.
         * Mark in the commit log sectors that shouldn't be replayed */
        ox_stats_add_event (OX_GC_EVENT_RACE_SMALL, 1);
        tr->entries[0].abort = 1;
        gc_race_sma++;
        ret = 0;
        goto FREE_PPA;

    } else if (ret < 0)
        goto FREE_PPA;

    app_transaction_free_list (ppas);
    app_transaction_commit (tr, NULL, APP_T_FLUSH_NO);

    return 0;

FREE_PPA:
    if (ppas)
        app_transaction_free_list (ppas);
ABORT:
    app_transaction_abort (tr);
    return ret;
}

static void block_gc_stats_read (struct nvm_io_data *io)
{
    struct app_sec_oob *sec_oob;
    uint32_t sec_i;

    for (sec_i = 0; sec_i < io->ch->geometry->sec_per_pl_pg; sec_i++) {
        sec_oob = (struct app_sec_oob *) io->oob_vec[sec_i];

        switch (sec_oob->pg_type) {

            case APP_PG_NAMESPACE:
                ox_stats_add_gc (APP_PG_NAMESPACE, 0, 1);
                break;
            case APP_PG_PADDING:
                ox_stats_add_gc (APP_PG_PADDING, 0, 1);
                break;
            case APP_PG_BLK_MD:
                ox_stats_add_gc (APP_PG_BLK_MD, 0, 1);
                break;
            case APP_PG_MAP_MD:
                ox_stats_add_gc (APP_PG_MAP_MD, 0, 1);
                break;
            case APP_PG_LOG:
                ox_stats_add_gc (APP_PG_LOG, 0, 1);
                break;
            case APP_PG_MAP:
                ox_stats_add_gc (APP_PG_MAP, 0, 1);
                break;
            case APP_PG_RESERVED:
            default:
                ox_stats_add_gc (APP_PG_UNKNOWN, 0, 1);
        }
    }
}

static void block_gc_proc_blkmd_pg (uint64_t lba)
{
    uint32_t ch_i;
    uint64_t tiny_index;

    ch_i = lba / ch[0]->blk_md->entries;
    tiny_index = lba % ch[0]->blk_md->entries;
    tiny_index = tiny_index / ch[0]->blk_md->ent_per_pg;
    oxapp()->md->mark_fn (ch[ch_i], tiny_index);
}

static void block_gc_proc_mapmd_pg (uint64_t lba)
{
    uint32_t ch_i;
    uint64_t index;

    ch_i = lba / ch[0]->blk_md->entries;
    index = lba % ch[0]->blk_md->entries;
    index = index / ch[0]->blk_md->ent_per_pg;
    oxapp()->ch_map->mark_fn (ch[ch_i], index);
}

static int block_gc_read_blk (struct app_channel *lch,
                                 struct app_blk_md_entry *blk_md, uint16_t tid)
{
    uint32_t pg_i, nsec = 0;
    struct nvm_ppa_addr ppa;
    struct nvm_io_data *io;
    struct app_sec_oob *oob;
    struct nvm_mmgr_geometry *g = lch->ch->geometry;

    for (pg_i = 0; pg_i < g->pg_per_blk; pg_i++) {

        if (block_gc_check_valid_pg (lch, blk_md, pg_i)) {
            ppa.ppa = 0x0;
            ppa.ppa = blk_md->ppa.ppa;
            ppa.g.pg = pg_i;

            io = (struct nvm_io_data *) gc_buf[tid][pg_i];

            if (ftl_pg_io_switch (lch->ch, MMGR_READ_PG,
                                    (void **) io->pl_vec, &ppa, NVM_IO_NORMAL))
                return -1;

            block_gc_stats_read (io);

            oob = (struct app_sec_oob *) io->oob_vec[0];
            switch (oob->pg_type) {
                case APP_PG_MAP:

                    if (block_gc_proc_mapping_pg (lch, io, &ppa, oob)) {
                        gc_err_sec += g->sec_per_pl_pg;
                        return -1;
                    }
                    nsec += g->sec_per_pl_pg;
                    gc_map_pgs++;
                    break;

                case APP_PG_BLK_MD:

                    block_gc_proc_blkmd_pg (oob->lba);
                    gc_blk_pgs++;
                    break;

                case APP_PG_MAP_MD:

                    block_gc_proc_mapmd_pg (oob->lba);
                    gc_mapmd_pgs++;
                    break;

                default:
                    break;
            }
        }
    }

    return nsec;
}

static uint8_t block_gc_process_pg (struct app_channel *lch,
        struct nvm_io_data *io, uint16_t n_sec, struct nvm_ppa_addr *gc_old_ppa)
{
    struct app_transaction_t *tr;
    struct app_transaction_user ent;
    struct app_prov_ppas *ppas;
    struct nvm_ppa_addr old_ppa;
    struct nvm_mmgr_geometry *geo = lch->ch->geometry;
    uint16_t sec_i = 0, nsec = 0;
    uint64_t lbas[n_sec];
    int ret;

    for (sec_i = 0; sec_i < n_sec; sec_i++)
        lbas[sec_i] = ((struct app_sec_oob *) io->oob_vec[sec_i])->lba;

    tr = app_transaction_new (lbas, n_sec, APP_TR_GC_NS);
    if (!tr)
        return 0;

    ent.entries = ox_calloc (geo->sec_per_pl_pg,
                        sizeof (struct app_transaction_log *), OX_MEM_OXBLK_GC);
    if (!ent.entries)
        goto ABORT;

    ent.tid = lch->app_ch_id;
    ent.count = geo->sec_per_pl_pg;
    for (sec_i = 0; sec_i < n_sec; sec_i++)
        ent.entries[sec_i] = &tr->entries[sec_i];

    pthread_mutex_lock (&gc_w_mutex);

    ppas = app_transaction_alloc_list (&ent, APP_TR_GC_NS);
    ox_free (ent.entries, OX_MEM_OXBLK_GC);

    if (!ppas || ppas->nppas < geo->sec_per_pl_pg) {
        pthread_mutex_unlock (&gc_w_mutex);
        goto FREE_PPA;
    }

    if (ftl_pg_io_switch (lch->ch, MMGR_WRITE_PG,
                        (void **) io->pl_vec, &ppas->ppa[0], NVM_IO_NORMAL)){

        pthread_mutex_unlock (&gc_w_mutex);
        app_transaction_close_blk (&ppas->ppa[0], APP_LINE_COLD);

        goto FREE_PPA;
    }

    pthread_mutex_unlock (&gc_w_mutex);

    for (sec_i = 0; sec_i < geo->sec_per_pl_pg; sec_i++) {

        gc_old_ppa->g.pl  = sec_i / geo->sec_per_pg;
        gc_old_ppa->g.sec = sec_i % geo->sec_per_pg;

        /* Invalidate padded sectors */
        if (sec_i >= n_sec) {
            oxapp()->md->invalidate_fn
                                (lch, &ppas->ppa[sec_i], APP_INVALID_SECTOR);
            continue;
        } else {
            ret = oxapp()->gl_map->upsert_fn (lbas[sec_i],
                                        ppas->ppa[sec_i].ppa,
                                        (uint64_t *) &old_ppa, gc_old_ppa->ppa);
            if (ret) {

                oxapp()->md->invalidate_fn
                                (lch, &ppas->ppa[sec_i], APP_INVALID_SECTOR);

                /* Other write thread had already updated the metadata.
                 * Mark in the commit log sectors that shouldn't be replayed */
                ox_stats_add_event (OX_GC_EVENT_RACE_BIG, 1);
                tr->entries[sec_i].abort = 1;
                gc_race_big++;

            } else {
                if (old_ppa.ppa)
                    oxapp()->md->invalidate_fn (ch[old_ppa.g.ch],
                                                &old_ppa, APP_INVALID_SECTOR);
            }
        }

        nsec++;
    }

    app_transaction_free_list (ppas);
    app_transaction_commit (tr, NULL, APP_T_FLUSH_NO);

    return nsec;

FREE_PPA:
    if (ppas)
        app_transaction_free_list (ppas);
ABORT:
    app_transaction_abort (tr);
    return 0;
}

static int block_gc_move_sector (struct app_channel *lch,
        struct nvm_io_data *w_io, struct nvm_ppa_addr *old_ppa, uint32_t pg_i,
        uint32_t sec_i, uint32_t ppa_off, uint16_t tid)
{
    uint32_t w_pl, w_sec;
    uint8_t *data, *w_data, *oob, *w_oob;
    struct nvm_io_data *io;
    struct app_sec_oob *sec_oob;
    struct nvm_mmgr_geometry *geo = lch->ch->geometry;

    io = (struct nvm_io_data *) gc_buf[tid][pg_i];
    sec_oob = (struct app_sec_oob *) io->oob_vec[sec_i];

    switch (sec_oob->pg_type) {

        /* We do NOT garbage collect logs. If we find logs here, they should
         * be already invalidated during checkpoint (truncate) */

        case APP_PG_NAMESPACE:
            break;
        case APP_PG_PADDING:
            oxapp()->md->invalidate_fn (lch, old_ppa, APP_INVALID_SECTOR);
            gc_pad_sec++;
            return -1;
        case APP_PG_BLK_MD:
        case APP_PG_MAP_MD:
            return -1;
        case APP_PG_LOG:
            gc_log_pgs++;
            return -1;
        case APP_PG_MAP:
        case APP_PG_RESERVED:
        default:
            goto ERR;
    }

    w_pl  = ppa_off / geo->sec_per_pg;
    w_sec = ppa_off % geo->sec_per_pg;

    /* Move sector data and oob to write position */
    data = io->sec_vec[sec_i / geo->sec_per_pg][sec_i % geo->sec_per_pg];
    oob  = io->oob_vec[sec_i];

    w_data = w_io->sec_vec[w_pl][w_sec];
    w_oob  = w_io->oob_vec[ppa_off];

    memcpy (w_data, data, geo->sec_size);
    memcpy (w_oob, oob, geo->sec_oob_sz);

    return 0;

ERR:
    gc_wro_sec++;
    log_info ("[gc: Suspicious data type (%x). LBA %lu, PPA "
            "(%d/%d/%d/%d/%d/%d)\n", sec_oob->pg_type, sec_oob->lba,
            old_ppa->g.ch, old_ppa->g.lun, old_ppa->g.blk,
            old_ppa->g.pl, old_ppa->g.pg, old_ppa->g.sec);
            oxapp()->md->invalidate_fn (lch, old_ppa, APP_INVALID_SECTOR);
    return -1;
}

static int block_gc_process_blk (struct app_channel *lch,
                struct app_blk_md_entry *md, uint16_t tid, uint32_t *failed)
{
    uint32_t pg_i, sec_i, pl, sec, ppa_off;
    uint8_t *pg_map;
    struct nvm_io_data *w_io;
    struct app_sec_oob *sec_oob;
    struct nvm_ppa_addr old_ppa;
    struct nvm_mmgr_geometry *geo = lch->ch->geometry;
    uint8_t nsec, failed_sec = 0, sec_ok = 0;

    w_io = ftl_alloc_pg_io (lch->ch);
    if (!w_io)
        return -1;

    ppa_off = 0;
    for (pg_i = 0; pg_i < geo->pg_per_blk; pg_i++) {

        old_ppa.ppa   = 0x0;
        old_ppa.ppa   = md->ppa.ppa;
        old_ppa.g.pg  = pg_i;

        if (block_gc_check_valid_pg (lch, md, pg_i)) {

            pg_map = &md->pg_state[pg_i * geo->n_of_planes];

            for (sec_i = 0; sec_i < geo->sec_per_pl_pg; sec_i++) {

                pl  = sec_i / geo->sec_per_pg;
                sec = sec_i % geo->sec_per_pg;

                if (!(pg_map[pl] & (1 << sec))) {

                    old_ppa.g.pl  = pl;
                    old_ppa.g.sec = sec;

                    if (block_gc_move_sector
                              (lch, w_io, &old_ppa, pg_i, sec_i, ppa_off, tid))
                        continue;

                    ppa_off++;

                    if (ppa_off == geo->sec_per_pl_pg) {
                        nsec = block_gc_process_pg (lch, w_io, ppa_off,
                                                                    &old_ppa);
                        failed_sec += ppa_off - nsec;
                        sec_ok += nsec;

                        ox_stats_add_gc (APP_PG_NAMESPACE, 1, nsec);

                        ppa_off = 0;
                    }
                }
            }
        }
    }

    if (ppa_off) {
        for (sec_i = ppa_off; sec_i < geo->sec_per_pl_pg; sec_i++) {
            sec_oob  = (struct app_sec_oob *) w_io->oob_vec[sec_i];
            sec_oob->lba = 0;
            sec_oob->pg_type = APP_PG_PADDING;
        }

        nsec = block_gc_process_pg (lch, w_io, ppa_off, &old_ppa);

        if (nsec) {
            ox_stats_add_gc (APP_PG_NAMESPACE, 1, nsec);
            ox_stats_add_gc (APP_PG_PADDING, 1, geo->sec_per_pl_pg - nsec);
        }

        failed_sec += ppa_off - nsec;
        sec_ok += nsec;
    }

    ftl_free_pg_io (w_io);
    *failed = failed_sec;

    return sec_ok;
}

static int block_gc_recycle_blks (struct app_channel *lch,
                            struct app_blk_md_entry **list, uint32_t count,
                            uint16_t tid, uint32_t *ch_sec)
{
    int blk_sec;
    struct timespec ts;
    struct app_log_entry log;
    uint64_t ns;
    uint32_t blk_i, recycled = 0, count_sec = 0, failed_sec;

    for (blk_i = 0; blk_i < count; blk_i++) {

        /* Read all valid pages in the blk from NVM to the buffer */
        /* Also move mapping table pages separately */
        blk_sec = block_gc_read_blk (lch, list[blk_i], tid);
        if (blk_sec < 0) {
            log_err ("[ox-blk (gc): Read block / move mapping failed.]");
            continue;
        }
        gc_moved_sec += blk_sec;
        count_sec += blk_sec;

        blk_sec = oxapp()->gc->recycle_fn (lch, list[blk_i], tid, &failed_sec);
        if (blk_sec < 0) {
            log_err ("[ox-blk (gc): Process block failed.]");
            continue;
        }

        if (!failed_sec) {
            /* Put the block back in the channel provisioning */
            if (oxapp()->ch_prov->put_blk_fn (lch, list[blk_i]->ppa.g.lun,
                                                     list[blk_i]->ppa.g.blk)) {
                log_err ("[ox-blk (gc): Put block failed (%d/%d/%d)]",
                        list[blk_i]->ppa.g.ch, list[blk_i]->ppa.g.lun,
                        list[blk_i]->ppa.g.blk);
                goto COUNT;
            }

            /* Log the PUT block */
            GET_NANOSECONDS (ns, ts);
            memset (&log, 0x0, sizeof (struct app_log_entry));
            log.type = APP_LOG_PUT_BLK;
            log.ts = ns;
            log.amend.ppa = list[blk_i]->ppa.ppa;

            if (oxapp()->log->append_fn (&log, 1))
                log_err ("[gc: Put block log NOT appended during shutdown.]");

            recycled++;
            gc_recycled_blks++;
            ox_stats_add_event (OX_GC_EVENT_BLOCK_REC, 1);
            ox_stats_add_event (OX_GC_EVENT_SPACE_REC,
                                                lch->ch->geometry->blk_size);
        }

COUNT:
        gc_moved_sec += blk_sec;
        gc_err_sec   += failed_sec;
        count_sec    += blk_sec;

        if (failed_sec)
            ox_stats_add_event (OX_GC_EVENT_FAILED, failed_sec);
    }

    *ch_sec = count_sec;
    return recycled;
}

static void block_gc_print_stats (struct app_channel *lch, uint32_t recycled,
                                                              uint32_t blk_sec)
{
    printf (" GC (%d): (B%d/S%d) %.2f MB, T: (B%d/S%lu) %.2f MB, "
            "(M%lu/MM%lu/B%lu/L%lu/P%lu/F%lu/W%lu/RS%lu/RB%lu) \n",
            lch->app_ch_id, recycled, blk_sec,
            (4.0 * (double) blk_sec) / (double) 1024, gc_recycled_blks,
            gc_moved_sec, (4.0 * (double) gc_moved_sec) / (double) 1024,
            gc_map_pgs, gc_mapmd_pgs, gc_blk_pgs, gc_log_pgs, gc_pad_sec,
            gc_err_sec, gc_wro_sec, gc_race_sma, gc_race_big);
}

static void *block_gc_run_ch (void *arg)
{
    uint8_t  loop = 1;
    uint32_t victims, recycled, blk_sec;

    struct egc_th_arg         *th_arg = (struct egc_th_arg *) arg;
    struct app_channel       *lch = th_arg->lch;
    struct app_blk_md_entry **list;

    while (!stop) {

        pthread_mutex_lock(&gc_cond_mutex[th_arg->tid]);
        th_arg->waiting = 1;
        pthread_cond_wait(&gc_cond[th_arg->tid], &gc_cond_mutex[th_arg->tid]);
        pthread_mutex_unlock(&gc_cond_mutex[th_arg->tid]);
        if (stop)
            break;

        app_ch_active_unset (lch);

        do {
            if (!app_ch_nthreads (lch)) {
                loop = 0;

                list = oxapp()->gc->target_fn (lch, &victims);
                if (!list || !victims) {
                    usleep (BLOCK_GC_DELAY_US);
                    break;
                }

                recycled = block_gc_recycle_blks (lch, list, victims,
                                                      th_arg->bufid, &blk_sec);
                ox_free (list, OX_MEM_OXBLK_GC);

                if (recycled != victims)
                    log_info ("[ox-blk (gc): %d recycled, %d with errors.]",
                                                 recycled, victims - recycled);
                if (APP_DEBUG_GC)
                    block_gc_print_stats (lch, recycled, blk_sec);
            } else {
                usleep (BLOCK_GC_DELAY_CH_BUSY);
            }
        } while (loop);

        app_ch_need_gc_unset (lch);
        if (victims)
            oxapp()->ch_prov->check_gc_fn (lch);
        app_ch_active_set (lch);
    };

    return NULL;
}

static void *block_gc_check_fn (void *arg)
{
    uint16_t cch = 0, sleep = 0, slot_i, th_i;
    pthread_t run_th[app_nch];
    struct egc_th_arg *th_arg;
    uint8_t slot[BLOCK_GC_PARALLEL_CH];

    memset (slot, 0x0, sizeof (uint8_t) * BLOCK_GC_PARALLEL_CH);

    th_arg = ox_malloc (sizeof (struct egc_th_arg) * app_nch, OX_MEM_OXBLK_GC);
    if (!th_arg)
        return NULL;

    for (th_i = 0; th_i < app_nch; th_i++) {
        th_arg[th_i].tid = th_i;
        th_arg[th_i].lch = ch[th_i];
        th_arg[th_i].waiting = 0;

        if (pthread_create (&run_th[th_i], NULL, block_gc_run_ch,
                                                       (void *) &th_arg[th_i]))
            goto STOP;
    }

    while (!stop) {
        if (!th_arg[cch].waiting)
            goto NEXT;

        if (app_ch_need_gc (ch[cch])) {
            do {
                for (slot_i = 0; slot_i < BLOCK_GC_PARALLEL_CH; slot_i++)
                    if (!slot[slot_i] || th_arg[slot[slot_i] - 1].waiting)
                        break;
                if (slot_i < BLOCK_GC_PARALLEL_CH)
                    break;

                usleep (BLOCK_GC_DELAY_US);
            } while (!stop);

            if (stop)
                break;

            slot[slot_i] = cch + 1;
            th_arg[cch].bufid = slot_i;
            sleep++;

            pthread_mutex_lock (&gc_cond_mutex[cch]);
            pthread_cond_signal(&gc_cond[cch]);
            th_arg[cch].waiting = 0;
            pthread_mutex_unlock (&gc_cond_mutex[cch]);
        }

NEXT:
        cch = (cch == app_nch - 1) ? 0 : cch + 1;
        if (!cch) {
            if (!sleep)
                usleep (BLOCK_GC_DELAY_US);
            sleep = 0;
        }
    }

STOP:
    stop = 1;
    while (th_i) {
        th_i--;
        usleep (BLOCK_GC_DELAY_US);
        pthread_mutex_lock (&gc_cond_mutex[th_i]);
        pthread_cond_signal(&gc_cond[th_i]);
        pthread_mutex_unlock (&gc_cond_mutex[th_i]);
        pthread_join (run_th[th_i], NULL);
    }

    ox_free (th_arg, OX_MEM_OXBLK_GC);
    log_info(" [ox-blk: GC stopped.]\n");

    return NULL;
}

static int block_gc_alloc_buf (void)
{
    uint32_t th_i, pg_i;

    gc_buf = ox_malloc(sizeof (void *) * BLOCK_GC_PARALLEL_CH, OX_MEM_OXBLK_GC);
    if (!gc_buf)
        return -1;

    for (th_i = 0; th_i < BLOCK_GC_PARALLEL_CH; th_i++) {
        gc_buf[th_i] = ox_malloc(sizeof (uint8_t *) * buf_npg, OX_MEM_OXBLK_GC);
        if (!gc_buf[th_i])
            goto FREE_BUF;

        for (pg_i = 0; pg_i < buf_npg; pg_i++) {
            gc_buf[th_i][pg_i] = ftl_alloc_pg_io (ch[0]->ch);
            if (!gc_buf[th_i][pg_i])
                goto FREE_BUF_PG;
        }
    }

    return 0;

FREE_BUF_PG:
    while (pg_i) {
        pg_i--;
        ftl_free_pg_io (gc_buf[th_i][pg_i]);
    }
    ox_free (gc_buf[th_i], OX_MEM_OXBLK_GC);
FREE_BUF:
    while (th_i) {
        th_i--;
        for (pg_i = 0; pg_i < buf_npg; pg_i++)
            ftl_free_pg_io (gc_buf[th_i][pg_i]);
        ox_free (gc_buf[th_i], OX_MEM_OXBLK_GC);
    }
    ox_free (gc_buf, OX_MEM_OXBLK_GC);
    return -1;
}

static void block_gc_free_buf (void)
{
    uint32_t pg_i, th_i = BLOCK_GC_PARALLEL_CH;

    while (th_i) {
        th_i--;
        for (pg_i = 0; pg_i < buf_npg; pg_i++)
            ftl_free_pg_io (gc_buf[th_i][pg_i]);
        ox_free (gc_buf[th_i], OX_MEM_OXBLK_GC);
    }
    ox_free (gc_buf, OX_MEM_OXBLK_GC);
}

static int block_gc_init (void)
{
    uint16_t nch, ch_i;
    struct nvm_mmgr_geometry *geo;

    if (!ox_mem_create_type ("OXBLK_GC", OX_MEM_OXBLK_GC))
        return -1;

    ch = ox_malloc (sizeof (struct app_channel *) * app_nch, OX_MEM_OXBLK_GC);
    if (!ch)
        return -1;

    nch = oxapp()->channels.get_list_fn (ch, app_nch);
    if (nch != app_nch)
        goto FREE_CH;

    gc_recycled_blks = 0;
    gc_moved_sec = gc_pad_sec = gc_err_sec = gc_wro_sec = gc_map_pgs =
                   gc_mapmd_pgs = gc_blk_pgs = gc_log_pgs = gc_race_sma =
                   gc_race_big = 0;

    geo = ch[0]->ch->geometry;
    buf_pg_sz = geo->pg_size;
    buf_oob_sz = geo->pg_oob_sz;
    buf_npg = geo->pg_per_blk;

    gc_cond = ox_malloc (sizeof (pthread_cond_t) * app_nch, OX_MEM_OXBLK_GC);
    if (!gc_cond)
        goto FREE_CH;

    gc_cond_mutex = ox_malloc (sizeof (pthread_mutex_t) * app_nch,
                                                            OX_MEM_OXBLK_GC);
    if (!gc_cond_mutex)
        goto FREE_COND;

    for (ch_i = 0; ch_i < app_nch; ch_i++) {
        if (pthread_cond_init (&gc_cond[ch_i], NULL))
            goto COND_MUTEX;
        if (pthread_mutex_init (&gc_cond_mutex[ch_i], NULL)) {
            pthread_cond_destroy (&gc_cond[ch_i]);
            goto COND_MUTEX;
        }
    }

    if (block_gc_alloc_buf ())
        goto COND_MUTEX;

    stop = 0;
    if (pthread_create (&check_th, NULL, block_gc_check_fn, NULL))
        goto FREE_BUF;

    log_info("    [ox-blk: GC started. Slots: %d]\n", BLOCK_GC_PARALLEL_CH);

    return 0;

FREE_BUF:
    block_gc_free_buf ();
COND_MUTEX:
    while (ch_i) {
        ch_i--;
        pthread_cond_destroy (&gc_cond[ch_i]);
        pthread_mutex_destroy (&gc_cond_mutex[ch_i]);
    }
    ox_free (gc_cond_mutex, OX_MEM_OXBLK_GC);
FREE_COND:
    ox_free (gc_cond, OX_MEM_OXBLK_GC);
FREE_CH:
    ox_free (ch, OX_MEM_OXBLK_GC);
    return -1;
}

static void block_gc_exit (void)
{
    uint16_t ch_i;

    stop++;
    pthread_join (check_th, NULL);
    block_gc_free_buf ();

    for (ch_i = 0; ch_i < app_nch; ch_i++) {
        pthread_cond_destroy (&gc_cond[ch_i]);
        pthread_mutex_destroy (&gc_cond_mutex[ch_i]);
    }

    ox_free (gc_cond_mutex, OX_MEM_OXBLK_GC);
    ox_free (gc_cond, OX_MEM_OXBLK_GC);
    ox_free (ch, OX_MEM_OXBLK_GC);
}

static struct app_gc block_gc = {
    .mod_id     = OXBLK_GC,
    .name       = "OX-BLOCK-GC",
    .init_fn    = block_gc_init,
    .exit_fn    = block_gc_exit,
    .target_fn  = block_gc_get_target_blks,
    .recycle_fn = block_gc_process_blk
};

void oxb_gc_register (void)
{
    app_mod_register (APPMOD_GC, OXBLK_GC, &block_gc);
}