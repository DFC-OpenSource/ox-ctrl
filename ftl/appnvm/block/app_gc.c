/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Garbage Collecting
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
#include "../appnvm.h"
#include <sys/queue.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include "../../../include/ssd.h"

#define APP_GC_PARALLEL_CH   1
#define APP_GC_DELAY_US      10000
#define APP_GC_DELAY_CH_BUSY 1000

extern uint16_t              app_nch;
static struct app_channel  **ch;
static pthread_t             check_th;
static uint8_t               stop;
static struct app_io_data ***gc_buf;
static uint16_t              buf_pg_sz, buf_oob_sz, buf_npg;

static uint32_t gc_recycled_blks;
static uint64_t gc_moved_sec, gc_pad_sec, gc_err_sec, gc_wro_sec, gc_map_pgs;

pthread_mutex_t *gc_cond_mutex;
pthread_cond_t  *gc_cond;

struct gc_th_arg {
    uint16_t            tid;
    uint16_t            bufid;
    struct app_channel *lch;
};

static int gc_bucket_sort (struct app_blk_md_entry **list,
                    uint32_t list_sz, uint32_t n_buckets, uint32_t min_invalid)
{
    uint32_t stop, bi, j, k, ip, bucketc[++n_buckets];
    struct app_blk_md_entry **bucket[n_buckets];
    int ret = -1;

    memset (bucketc, 0, n_buckets * sizeof (uint32_t));
    memset (bucket, 0, n_buckets * sizeof (struct app_blk_md_entry *));

    for(j = 0; j < list_sz; j++) {;
        ip = list[j]->invalid_sec;
        if (ip) {
            bucketc[ip]++;
            bucket[ip] = realloc (bucket[ip],
                              bucketc[ip] * sizeof (struct app_blk_md_entry *));
            if (!bucket[ip])
                goto FREE;
            bucket[ip][bucketc[ip] - 1] = list[j];
        }
    }

    k = 0;
    stop = 0;
    for (bi = n_buckets - 1; bi > 0; bi--) {
        for (j = 0; j < bucketc[bi]; j++) {
            if (((float) bi / (float) n_buckets < APPNVM_GC_TARGET_RATE &&
                            (k >= APPNVM_GC_MAX_BLKS)) || (bi < min_invalid)) {
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
            free (bucket[bi]);
    return ret;
}

static struct app_blk_md_entry **gc_get_target_blks (struct app_channel *lch,
                                                                   uint32_t *c)
{
    int nblks;
    struct app_blk_md_entry *lun;
    struct app_blk_md_entry **list = NULL;
    uint32_t lun_i, blk_i, min_inv;
    float count = 0, avlb = 0, inv_rate;

    for (lun_i = 0; lun_i < lch->ch->geometry->lun_per_ch; lun_i++) {

        lun = appnvm()->md->get_fn (lch, lun_i);
        if (!lun) {
            *c = 0;
            return NULL;
        }

        for (blk_i = 0; blk_i < lch->ch->geometry->blk_per_lun; blk_i++) {

            if (!(lun[blk_i].flags & APP_BLK_MD_AVLB))
                continue;

            if (    (lun[blk_i].flags & APP_BLK_MD_USED) &&
                   !(lun[blk_i].flags & APP_BLK_MD_OPEN) &&
                     lun[blk_i].current_pg == lch->ch->geometry->pg_per_blk &&
                     lun[blk_i].invalid_sec > 0) {

                count += 1;
                list = realloc(list, sizeof(struct app_blk_md_entry *) *
                                                                   (int) count);
                if (!list)
                    goto FREE;
                list[(int)count - 1] = &lun[blk_i];
            }
            avlb++;
        }
    }

    /* Compute minimum of invalid pages for targeting a block */
    min_inv = lch->ch->geometry->pg_per_blk * APPNVM_GC_TARGET_RATE;
    inv_rate = 1.0 - ((count / avlb - APPNVM_GC_THRESD) /
                                                     (1.0 - APPNVM_GC_THRESD));
    if ((count / avlb) >= APPNVM_GC_THRESD)
        min_inv *= inv_rate;

    nblks = gc_bucket_sort(list, count, lch->ch->geometry->sec_per_blk,min_inv);
    if (nblks <= 0)
        goto FREE;

    *c = nblks;
    return list;

FREE:
    if (count > 1)
        free (list);
    *c = 0;
    return NULL;
}

static int gc_check_valid_pg (struct app_channel *lch,
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

static int gc_proc_mapping_pg (struct app_channel *lch, struct app_io_data *io,
                           struct nvm_ppa_addr *old_ppa, struct app_pg_oob *oob)
{
    struct nvm_ppa_addr ppa_list[lch->ch->geometry->sec_per_pl_pg];

    if (appnvm()->ch_prov->get_ppas_fn (lch, ppa_list, 1))
        return -1;

    if (app_pg_io (lch, MMGR_WRITE_PG, (void **) io->pl_vec, ppa_list))
        goto ERR;

        /* TODO: If write fails, tell provisioning to recycle
         * current block and abort any write to the blocks,
         * otherwise we loose the blk sequential writes guarantee */

    if (appnvm()->gl_map->upsert_md_fn (oob->lba, ppa_list[0].ppa, old_ppa->ppa))
        goto ERR;

    return 0;

ERR:
    appnvm()->md->invalidate_fn (lch, ppa_list, APP_INVALID_PAGE);
    return -1;
}

static int gc_read_blk (struct app_channel *lch,
                                 struct app_blk_md_entry *blk_md, uint16_t tid)
{
    uint32_t pg_i, nsec = 0;
    struct nvm_ppa_addr ppa;
    struct app_io_data *io;
    struct app_pg_oob *oob;

    for (pg_i = 0; pg_i < lch->ch->geometry->pg_per_blk; pg_i++) {

        if (gc_check_valid_pg (lch, blk_md, pg_i)) {
            ppa.ppa = 0x0;
            ppa.ppa = blk_md->ppa.ppa;
            ppa.g.pg = pg_i;

            io = (struct app_io_data *) gc_buf[tid][pg_i];

            if (app_pg_io (lch, MMGR_READ_PG, (void **) io->pl_vec, &ppa))
                return -1;

            oob = (struct app_pg_oob *) io->oob_vec[0];
            if (oob->pg_type == APP_PG_MAP) {
                if (gc_proc_mapping_pg (lch, io, &ppa, oob)) {
                    gc_err_sec += lch->ch->geometry->sec_per_pl_pg;
                    return -1;
                }
                nsec += lch->ch->geometry->sec_per_pl_pg;
                gc_map_pgs++;
            }
        }
    }

    return nsec;
}

static uint8_t gc_process_pg (struct app_channel *lch, struct app_io_data *io,
                                                                uint16_t n_sec)
{
    struct app_pg_oob *oob;
    struct nvm_mmgr_geometry *geo = lch->ch->geometry;
    struct nvm_ppa_addr ppa_list[geo->sec_per_pl_pg];
    uint16_t sec_i = 0, ret = 0;

    /* Write page to the same channel to keep the parallelism */
    if (appnvm()->ch_prov->get_ppas_fn (lch, ppa_list, 1))
        return ret;

    if (app_pg_io (lch, MMGR_WRITE_PG, (void **) io->pl_vec, ppa_list)) {

        /* TODO: If write fails, tell provisioning to recycle
         * current block and abort any write to the blocks,
         * otherwise we loose the blk sequential writes guarantee */

        goto INV_PG;
    }

    for (sec_i = 0; sec_i < geo->sec_per_pl_pg; sec_i++) {

        /* Invalidate padded sectors */
        if (sec_i >= n_sec) {
            appnvm()->md->invalidate_fn
                                   (lch, &ppa_list[sec_i], APP_INVALID_SECTOR);
            continue;
        }

        oob = (struct app_pg_oob *) io->oob_vec[sec_i];
        if (appnvm()->gl_map->upsert_fn (oob->lba, ppa_list[sec_i].ppa)) {
            appnvm()->md->invalidate_fn
                                   (lch, &ppa_list[sec_i], APP_INVALID_SECTOR);
            continue;
        }
        ret++;
    }

    return ret;

INV_PG:
    appnvm()->md->invalidate_fn (lch, ppa_list, APP_INVALID_PAGE);
    return 0;
}

static int gc_move_sector (struct app_channel *lch, struct app_io_data *w_io,
        struct nvm_ppa_addr *old_ppa, uint32_t pg_i, uint32_t sec_i,
        uint32_t ppa_off, uint16_t tid)
{
    uint32_t w_pl, w_sec;
    uint8_t *data, *w_data, *oob, *w_oob;
    struct app_io_data *io;
    struct app_pg_oob *sec_oob;
    struct nvm_mmgr_geometry *geo = lch->ch->geometry;

    io = (struct app_io_data *) gc_buf[tid][pg_i];
    sec_oob = (struct app_pg_oob *) io->oob_vec[sec_i];

    switch (sec_oob->pg_type) {
        case APP_PG_NAMESPACE:
            break;
        case APP_PG_PADDING:
            appnvm()->md->invalidate_fn (lch, old_ppa, APP_INVALID_SECTOR);
            gc_pad_sec++;
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
    log_info ("[gc: Suspicious data type (%d). LBA %lu, PPA "
            "(%d/%d/%d/%d/%d/%d)\n", sec_oob->pg_type, sec_oob->lba,
            old_ppa->g.ch, old_ppa->g.lun, old_ppa->g.blk,
            old_ppa->g.pl, old_ppa->g.pg, old_ppa->g.sec);
            appnvm()->md->invalidate_fn (lch, old_ppa, APP_INVALID_SECTOR);
    return -1;
}

static int gc_process_blk (struct app_channel *lch, struct app_blk_md_entry *md,
                                  uint16_t tid, uint32_t *failed)
{
    uint32_t pg_i, sec_i, pl, sec, ppa_off;
    uint8_t *pg_map;
    struct app_io_data *w_io;
    struct app_pg_oob *sec_oob;
    struct nvm_ppa_addr old_ppa;
    struct nvm_mmgr_geometry *geo = lch->ch->geometry;
    uint8_t nsec, failed_sec = 0, sec_ok = 0;

    w_io = app_alloc_pg_io (lch);
    if (!w_io)
        return -1;

    ppa_off = 0;
    for (pg_i = 0; pg_i < geo->pg_per_blk; pg_i++) {

        if (gc_check_valid_pg (lch, md, pg_i)) {

            pg_map = &md->pg_state[pg_i * geo->n_of_planes];

            for (sec_i = 0; sec_i < geo->sec_per_pl_pg; sec_i++) {

                pl  = sec_i / geo->sec_per_pg;
                sec = sec_i % geo->sec_per_pg;

                if (!(pg_map[pl] & (1 << sec))) {

                    old_ppa.ppa   = 0x0;
                    old_ppa.ppa   = md->ppa.ppa;
                    old_ppa.g.pg  = pg_i;
                    old_ppa.g.pl  = pl;
                    old_ppa.g.sec = sec;

                    if (gc_move_sector
                              (lch, w_io, &old_ppa, pg_i, sec_i, ppa_off, tid))
                        continue;

                    ppa_off++;

                    if (ppa_off == geo->sec_per_pl_pg) {
                        nsec = gc_process_pg (lch, w_io, ppa_off);
                        failed_sec += ppa_off - nsec;
                        sec_ok += nsec;
                        ppa_off = 0;
                    }
                }
            }
        }
    }

    if (ppa_off) {
        for (sec_i = ppa_off; sec_i < geo->sec_per_pl_pg; sec_i++) {
            sec_oob  = (struct app_pg_oob *) w_io->oob_vec[sec_i];
            sec_oob->lba = 0;
            sec_oob->pg_type = APP_PG_PADDING;
        }

        nsec = gc_process_pg (lch, w_io, ppa_off);
        failed_sec += ppa_off - nsec;
        sec_ok += nsec;
    }

    app_free_pg_io (w_io);
    *failed = failed_sec;

    return sec_ok;
}

static int gc_recycle_blks (struct app_channel *lch,
                            struct app_blk_md_entry **list, uint32_t count,
                            uint16_t tid, uint32_t *ch_sec)
{
    int blk_sec;
    uint32_t blk_i, recycled = 0, count_sec = 0, failed_sec;

    for (blk_i = 0; blk_i < count; blk_i++) {

        /* Read all valid pages in the blk from NVM to the buffer */
        /* Also move mapping table pages separately */
        blk_sec = gc_read_blk (lch, list[blk_i], tid);
        if (blk_sec < 0) {
            log_err ("[appnvm (gc): Read block / move mapping failed.]");
            continue;
        }
        gc_moved_sec += blk_sec;
        count_sec += blk_sec;

        blk_sec = appnvm()->gc->recycle_fn (lch, list[blk_i], tid, &failed_sec);
        if (blk_sec < 0) {
            log_err ("[appnvm (gc): Process block failed.]");
            continue;
        }

        if (!failed_sec) {
            /* Put the block back in the channel provisioning */
            if (appnvm()->ch_prov->put_blk_fn (lch, list[blk_i]->ppa.g.lun,
                                                     list[blk_i]->ppa.g.blk)) {
                log_err ("[appnvm (gc): Put block failed (%d/%d/%d)]",
                        list[blk_i]->ppa.g.ch, list[blk_i]->ppa.g.lun,
                        list[blk_i]->ppa.g.blk);
                goto COUNT;
            }
            recycled++;
            gc_recycled_blks++;
        }

COUNT:
        gc_moved_sec += blk_sec;
        gc_err_sec   += failed_sec;
        count_sec    += blk_sec;
    }

    *ch_sec = count_sec;
    return recycled;
}

static void gc_print_stats (struct app_channel *lch, uint32_t recycled,
                                                              uint32_t blk_sec)
{
    printf (" GC (%d): (%d/%d) %.2f MB, T: (%d/%lu) %.2f MB, "
            "(M%lu/P%lu/F%lu/W%lu) \n", lch->app_ch_id, recycled, blk_sec,
            (4.0 * (double) blk_sec) / (double) 1024,
            gc_recycled_blks, gc_moved_sec,
            (4.0 * (double) gc_moved_sec) / (double) 1024,
            gc_map_pgs, gc_pad_sec, gc_err_sec, gc_wro_sec);
}

static void *gc_run_ch (void *arg)
{
    uint8_t  loop = 1;
    uint32_t victims, recycled, blk_sec;

    struct gc_th_arg         *th_arg = (struct gc_th_arg *) arg;
    struct app_channel       *lch = th_arg->lch;
    struct app_blk_md_entry **list;

    while (!stop) {

        pthread_mutex_lock(&gc_cond_mutex[th_arg->tid]);
        pthread_cond_signal(&gc_cond[th_arg->tid]);
        pthread_cond_wait(&gc_cond[th_arg->tid], &gc_cond_mutex[th_arg->tid]);
        pthread_mutex_unlock(&gc_cond_mutex[th_arg->tid]);
        if (stop)
            break;

        appnvm_ch_active_unset (lch);

        do {
            if (!appnvm_ch_nthreads (lch)) {
                loop = 0;

                list = appnvm()->gc->target_fn (lch, &victims);
                if (!list || !victims) {
                    usleep (APP_GC_DELAY_US);
                    break;
                }

                recycled = gc_recycle_blks (lch, list, victims,
                                                      th_arg->bufid, &blk_sec);
                free (list);

                if (recycled != victims)
                    log_info ("[appnvm (gc): %d recycled, %d with errors.]",
                                                 recycled, victims - recycled);
                if (APPNVM_DEBUG_GC)
                    gc_print_stats (lch, recycled, blk_sec);
            } else {
                usleep (APP_GC_DELAY_CH_BUSY);
            }
        } while (loop);

        appnvm_ch_need_gc_unset (lch);
        if (victims)
            appnvm()->ch_prov->check_gc_fn (lch);
        appnvm_ch_active_set (lch);
    };

    return NULL;
}

static void *gc_check_fn (void *arg)
{
    uint8_t wait[app_nch];
    uint16_t cch = 0, sleep = 0, n_run = 0, ch_i, th_i;
    pthread_t run_th[app_nch];
    struct gc_th_arg *th_arg;

    memset (wait, 0x0, sizeof (uint8_t) * app_nch);

    th_arg = malloc (sizeof (struct gc_th_arg) * app_nch);
    if (!th_arg)
        return NULL;

    for (th_i = 0; th_i < app_nch; th_i++) {
        th_arg[th_i].tid = th_i;
        th_arg[th_i].lch = ch[th_i];

        if (pthread_create (&run_th[th_i], NULL, gc_run_ch,
                                                       (void *) &th_arg[th_i]))
            goto STOP;
    }

    while (!stop) {
        if (appnvm_ch_need_gc (ch[cch])) {

            th_arg[cch].bufid = n_run;
            n_run++;
            sleep++;
            wait[cch]++;

            pthread_mutex_lock (&gc_cond_mutex[cch]);
            pthread_cond_signal(&gc_cond[cch]);
            pthread_mutex_unlock (&gc_cond_mutex[cch]);

            if (n_run == APP_GC_PARALLEL_CH || (cch == app_nch - 1)) {
                for (ch_i = 0; ch_i < app_nch; ch_i++) {
                    if (wait[ch_i]) {
                        pthread_mutex_lock (&gc_cond_mutex[cch]);
                        pthread_cond_signal(&gc_cond[cch]);
                        pthread_cond_wait(&gc_cond[cch], &gc_cond_mutex[cch]);
                        pthread_mutex_unlock (&gc_cond_mutex[cch]);
                        wait[cch] = 0x0;
                        n_run--;
                    }
                    if (!n_run)
                        break;
                }

            }
        }

        cch = (cch == app_nch - 1) ? 0 : cch + 1;
        if (!cch) {
            if (!sleep)
                usleep (APP_GC_DELAY_US);
            sleep = 0;
        }
    }

STOP:
    while (th_i) {
        th_i--;
        pthread_mutex_lock (&gc_cond_mutex[th_i]);
        pthread_cond_signal(&gc_cond[th_i]);
        pthread_mutex_unlock (&gc_cond_mutex[th_i]);
        pthread_join (run_th[th_i], NULL);
    }

    free (th_arg);
    return NULL;
}

static int gc_alloc_buf (void)
{
    uint32_t th_i, pg_i;

    gc_buf = malloc (sizeof (void *) * APP_GC_PARALLEL_CH);
    if (!gc_buf)
        return -1;

    for (th_i = 0; th_i < APP_GC_PARALLEL_CH; th_i++) {
        gc_buf[th_i] = malloc (sizeof (uint8_t *) * buf_npg);
        if (!gc_buf[th_i])
            goto FREE_BUF;

        for (pg_i = 0; pg_i < buf_npg; pg_i++) {
            gc_buf[th_i][pg_i] = app_alloc_pg_io (ch[0]);
            if (!gc_buf[th_i][pg_i])
                goto FREE_BUF_PG;
        }
    }

    return 0;

FREE_BUF_PG:
    while (pg_i) {
        pg_i--;
        app_free_pg_io (gc_buf[th_i][pg_i]);
    }
    free (gc_buf[th_i]);
FREE_BUF:
    while (th_i) {
        th_i--;
        for (pg_i = 0; pg_i < buf_npg; pg_i++)
            app_free_pg_io (gc_buf[th_i][pg_i]);
        free (gc_buf[th_i]);
    }
    free (gc_buf);
    return -1;
}

static void gc_free_buf (void)
{
    uint32_t pg_i, th_i = APP_GC_PARALLEL_CH;

    while (th_i) {
        th_i--;
        for (pg_i = 0; pg_i < buf_npg; pg_i++)
            app_free_pg_io (gc_buf[th_i][pg_i]);
        free (gc_buf[th_i]);
    }
    free (gc_buf);
}

static int gc_init (void)
{
    uint16_t nch, ch_i;
    struct nvm_mmgr_geometry *geo;

    ch = malloc (sizeof (struct app_channel *) * app_nch);
    if (!ch)
        return -1;

    nch = appnvm()->channels.get_list_fn (ch, app_nch);
    if (nch != app_nch)
        goto FREE_CH;

    gc_recycled_blks = 0;
    gc_moved_sec = gc_pad_sec = gc_err_sec = gc_wro_sec = gc_map_pgs = 0;

    geo = ch[0]->ch->geometry;
    buf_pg_sz = geo->pg_size;
    buf_oob_sz = geo->pg_oob_sz;
    buf_npg = geo->pg_per_blk;

    gc_cond = malloc (sizeof (pthread_cond_t) * app_nch);
    if (!gc_cond)
        goto FREE_CH;

    gc_cond_mutex = malloc (sizeof (pthread_mutex_t) * app_nch);
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

    if (gc_alloc_buf ())
        goto COND_MUTEX;

    stop = 0;
    if (pthread_create (&check_th, NULL, gc_check_fn, NULL))
        goto FREE_BUF;

    return 0;

FREE_BUF:
    gc_free_buf ();
COND_MUTEX:
    while (ch_i) {
        ch_i--;
        pthread_cond_destroy (&gc_cond[ch_i]);
        pthread_mutex_destroy (&gc_cond_mutex[ch_i]);
    }
    free (gc_cond_mutex);
FREE_COND:
    free (gc_cond);
FREE_CH:
    free (ch);
    return -1;
}

static void gc_exit (void)
{
    uint16_t ch_i;

    stop++;
    pthread_join (check_th, NULL);
    gc_free_buf ();

    for (ch_i = 0; ch_i < app_nch; ch_i++) {
        pthread_cond_destroy (&gc_cond[ch_i]);
        pthread_mutex_destroy (&gc_cond_mutex[ch_i]);
    }

    free (gc_cond_mutex);
    free (gc_cond);
    free (ch);
}

static struct app_gc appftl_gc = {
    .mod_id     = APPFTL_GC,
    .init_fn    = gc_init,
    .exit_fn    = gc_exit,
    .target_fn  = gc_get_target_blks,
    .recycle_fn = gc_process_blk
};

void gc_register (void)
{
    appnvm_mod_register (APPMOD_GC, APPFTL_GC, &appftl_gc);
}