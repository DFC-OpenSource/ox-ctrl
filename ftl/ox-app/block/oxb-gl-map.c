/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX-App Flash Translation Layer (Global Mapping)
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
#include <sys/queue.h>
#include <libox.h>

#define MAP_BUF_CH_PGS  8192      /* 256 MB per channel */
#define MAP_BUF_PG_SZ   32 * 1024 /* 32 KB */

#define MAP_ADDR_FLAG   ((1 & AND64) << 63)

extern uint8_t             app_map_new;
extern pthread_mutex_t     user_w_mutex;

struct map_cache_entry {
    uint8_t                     dirty;
    uint8_t                    *buf;
    uint32_t                    buf_sz;
    struct nvm_ppa_addr         ppa;    /* Stores the PPA while pg is cached */
    struct app_map_entry       *md_entry;
    struct map_cache           *cache;
    pthread_mutex_t            *mutex;
    pthread_spinlock_t         *spin;
    LIST_ENTRY(map_cache_entry)  f_entry;
    TAILQ_ENTRY(map_cache_entry) u_entry;
};

struct map_cache {
    struct map_cache_entry                 *pg_buf;
    LIST_HEAD(mb_free_l, map_cache_entry)   mbf_head;
    TAILQ_HEAD(mb_used_l, map_cache_entry)  mbu_head;
    pthread_spinlock_t                      mb_spin;
    uint32_t                                nfree;
    uint32_t                                nused;
    uint16_t                                id;
};

struct map_pg_addr {
    union {
        struct {
            uint64_t addr  : 63;
            uint64_t flag : 1;
        } g;
        uint64_t addr;
    };
};

static struct map_cache    *map_ch_cache;
extern uint16_t             app_nch;
static struct app_channel **ch;
static volatile uint8_t     cp_running;

/* The mapping strategy ensures the entry size matches with the NVM pg size */
static uint64_t             map_ent_per_pg;

/**
 * - The mapping table is spread using the global provisioning functions.
 * - The mapping table is recovered by secondary metadata table stored in a
 *    reserved block per channel. Mapping table metadata key/PPA entries are
 *    stored into multi-plane NVM pages in the reserved block using
 *    round-robin distribution among all NVM channels.
 * - Each channel has a separated cache, where only mapping table key/PPA
 *    entries belonging to this channel (previously spread by round-robin)
 *    are cached into.
 * - Cached pages are flushed back to NVM using the global provisioning. This
 *    ensures the mapping table I/Os follow the same provisioning strategy than
 *    the rest of the FTL.
 */

static int map_nvm_write (struct map_cache_entry *ent, uint64_t lba,
                                                                  uint64_t old)
{
    struct app_prov_ppas *prov_ppa;
    struct app_channel *lch;
    struct nvm_ppa_addr *addr;
    struct nvm_io_data *io;
    struct app_log_entry log;
    uint64_t ns;
    struct timespec ts;
    int sec, ret = -1;

    pthread_mutex_lock (&user_w_mutex);

    /* Mapping table pages are mixed with user data */
    prov_ppa = oxapp()->gl_prov->new_fn (1, APP_LINE_USER);
    if (!prov_ppa) {
        log_err ("[appnvm (gl_map): I/O error. No PPAs available.]");
        return -1;
    }

    if (prov_ppa->nppas != ch[0]->ch->geometry->sec_per_pl_pg)
        log_err ("[appnvm (gl_map): NVM write. wrong PPAs. nppas %d]",
                                                              prov_ppa->nppas);

    addr = &prov_ppa->ppa[0];
    lch = ch[addr->g.ch];
    io = ftl_alloc_pg_io(lch->ch);
    if (io == NULL)
        goto FREE_PPA;

    for (sec = 0; sec < io->ch->geometry->sec_per_pl_pg; sec++) {
        ((struct app_sec_oob *) io->oob_vec[sec])->lba = lba;
        ((struct app_sec_oob *) io->oob_vec[sec])->pg_type = APP_PG_MAP;
    }

    ret = ftl_nvm_seq_transfer (io, addr, ent->buf, 1, map_ent_per_pg,
                            map_ent_per_pg, sizeof(struct app_map_entry),
                            NVM_TRANS_TO_NVM, NVM_IO_NORMAL);
    if (ret)
        goto FREE_IO;

    pthread_mutex_unlock (&user_w_mutex);

    /* Log the write */
    GET_NANOSECONDS (ns, ts);
    log.ts = ns;
    log.type = APP_LOG_MAP;
    log.write.tid = 0;
    log.write.lba = lba;
    log.write.new_ppa = addr->ppa;
    log.write.old_ppa = old;

    if (oxapp()->log->append_fn (&log, 1))
        log_err ("[blk-md: Write log was NOT appended.]");

    ent->ppa.ppa = addr->ppa;

    ftl_free_pg_io(io);
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

static int map_nvm_read (struct map_cache_entry *ent)
{
    struct app_channel *lch;
    struct nvm_ppa_addr addr;
    struct nvm_io_data *io;
    int ret = -1;

    addr.ppa = ent->md_entry->ppa;

    /* TODO: Support multiple media managers for mapping the channel ID */
    lch = ch[addr.g.ch];

    io = ftl_alloc_pg_io (lch->ch);
    if (io == NULL)
        return -1;

    ret = ftl_nvm_seq_transfer (io, &addr, ent->buf, 1, map_ent_per_pg,
                            map_ent_per_pg, sizeof(struct app_map_entry),
                            NVM_TRANS_FROM_NVM, NVM_IO_NORMAL);
    if (ret)
        log_err("[appnvm (gl_map): NVM read failed. PPA 0x%016lx]", addr.ppa);

    if (((struct app_sec_oob *)io->oob_vec[0])->pg_type != APP_PG_MAP)
        log_err ("[gl-map: BIG mapping page is corrupted. pg_type: "
                        "%d, ch: %d, index: %lu\n",
                        ((struct app_sec_oob *)io->oob_vec[0])->pg_type,
                        lch->app_ch_id, ent->md_entry->lba);
    
    ent->ppa.ppa = addr.ppa;
    ftl_free_pg_io(io);

    return ret;
}

static int map_evict_process (struct map_cache *cache,
                        struct map_cache_entry *cache_ent, uint8_t set_immut)
{
    struct nvm_ppa_addr old_ppa;
    struct app_map_entry *immutable_ent;
    uint32_t evict_index = 0xffffffff, retry = APP_MD_IO_RETRY;

    if (!cache_ent->md_entry)
        return 0;

    old_ppa.ppa = cache_ent->ppa.ppa;

    if (cache_ent->dirty) {
        
RETRY:
        if (map_nvm_write (cache_ent, cache_ent->md_entry->lba, old_ppa.ppa)) {
            retry--;
            log_err("[gl-map: NVM write failed. Retries: %d]",
                                                      APP_MD_IO_RETRY - retry);
            usleep (APP_MD_IO_RETRY_DELAY);
            if (retry)
                goto RETRY;
            else {
                pthread_spin_lock (&cache->mb_spin);
                TAILQ_INSERT_HEAD(&cache->mbu_head, cache_ent, u_entry);
                cache->nused++;
                pthread_spin_unlock (&cache->mb_spin);

                return -1;
            }
        }
        cache_ent->dirty = 0;

        /* Find the TINY index within a channel from a SMALL index
	 * md_entry lba is an index across all channels and we must get
	 * the evict index related to the tiny table we want to mark */
        evict_index =
	    (cache_ent->md_entry->lba % ch[cache->id]->map_md->entries) /
                                        ch[cache->id]->map_md->ent_per_pg;

        /* Mark SMALL entry as dirty */
        oxapp()->ch_map->mark_fn (ch[cache->id], evict_index);

        /* Cache entry PPA is set after the write completes */

        /* Invalidate old page PPAs */
        if (old_ppa.ppa)
            oxapp()->md->invalidate_fn (ch[old_ppa.g.ch], &old_ppa,
                                                             APP_INVALID_PAGE);
    }

    /* Set immutable table for checkpoint */
    if (set_immut) {
        immutable_ent = cache_ent->md_entry + ch[cache->id]->map_md->entries;
        immutable_ent->ppa = cache_ent->ppa.ppa;
        old_ppa.ppa = immutable_ent->ppa;
    }

    if (APP_DEBUG_GL_MAP)
        printf ("[gl-map: Page cache persisted. PPA (%d/%d/%d/%d). Cache ID: %d, "
             "Tiny index: %d]\n", cache_ent->ppa.g.ch, cache_ent->ppa.g.lun,
             cache_ent->ppa.g.blk, cache_ent->ppa.g.pg, cache->id, evict_index);

    return 0;
}

static int map_evict_pg_cache (struct map_cache *cache, uint8_t is_checkpoint)
{
    struct map_cache_entry *cache_ent;

    pthread_spin_lock (&cache->mb_spin);
    cache_ent = TAILQ_FIRST(&cache->mbu_head);
    if (!cache_ent) {
        pthread_spin_unlock (&cache->mb_spin);
        return -1;
    }

    TAILQ_REMOVE(&cache->mbu_head, cache_ent, u_entry);
    cache->nused--;
    pthread_spin_unlock (&cache->mb_spin);

    if (map_evict_process (cache, cache_ent, is_checkpoint))
        return -1;

    cache_ent->md_entry->ppa = cache_ent->ppa.ppa;
    cache_ent->ppa.ppa = 0;
    cache_ent->md_entry = NULL;

    pthread_spin_lock (&cache->mb_spin);
    LIST_INSERT_HEAD (&cache->mbf_head, cache_ent, f_entry);
    cache->nfree++;
    pthread_spin_unlock (&cache->mb_spin);

    return 0;
}

static int map_load_pg_cache (struct map_cache *cache,
           struct app_map_entry *md_entry, uint64_t first_lba, uint32_t pg_off)
{
    struct map_cache_entry *cache_ent;
    struct app_map_entry *map_ent;
    uint64_t ent_id, wait = 100;

WAIT:
    if (LIST_EMPTY(&cache->mbf_head)) {
        if (cp_running && wait) {
            usleep (200);
            wait--;
            goto WAIT;
        }
        pthread_mutex_lock (&ch[cache->id]->map_md->tbl_mutex);
        if (map_evict_pg_cache (cache, 0)) {
            pthread_mutex_unlock (&ch[cache->id]->map_md->tbl_mutex);
            return -1;
        }
        pthread_mutex_unlock (&ch[cache->id]->map_md->tbl_mutex);
    }

    pthread_spin_lock (&cache->mb_spin);
    cache_ent = LIST_FIRST(&cache->mbf_head);
    if (!cache_ent) {
        pthread_spin_unlock (&cache->mb_spin);
        return -1;
    }

    LIST_REMOVE(cache_ent, f_entry);
    cache->nfree--;
    pthread_spin_unlock (&cache->mb_spin);

    cache_ent->md_entry = md_entry;

    /* If metadata entry PPA is zero, mapping page does not exist yet */
    if (!md_entry->ppa) {
        for (ent_id = 0; ent_id < map_ent_per_pg; ent_id++) {
            map_ent = &((struct app_map_entry *) cache_ent->buf)[ent_id];
            map_ent->lba = first_lba + ent_id;
            map_ent->ppa = 0x0;
        }
        cache_ent->dirty = 1;
    } else {
        if (map_nvm_read (cache_ent)) {
            cache_ent->md_entry = NULL;
            cache_ent->ppa.ppa = 0;

            pthread_spin_lock (&cache->mb_spin);
            LIST_INSERT_HEAD(&cache->mbf_head, cache_ent, f_entry);
            cache->nfree++;
            pthread_spin_unlock (&cache->mb_spin);

            return -1;
        }

        /* Cache entry PPA is set after the read completes */
    }

    cache_ent->mutex = &ch[cache->id]->map_md->entry_mutex[pg_off];
    cache_ent->spin = &ch[cache->id]->map_md->entry_spin[pg_off];

    pthread_spin_lock (&cache->mb_spin);
    md_entry->ppa = (uint64_t) cache_ent;
    md_entry->ppa |= MAP_ADDR_FLAG;

    TAILQ_INSERT_TAIL(&cache->mbu_head, cache_ent, u_entry);
    cache->nused++;
    pthread_spin_unlock (&cache->mb_spin);

    if (APP_DEBUG_GL_MAP)
        printf ("[gl-map: Page cache loaded. PPA (%d/%d/%d/%d)]\n",
                                cache_ent->ppa.g.ch, cache_ent->ppa.g.lun,
                                cache_ent->ppa.g.blk, cache_ent->ppa.g.pg);

    return 0;
}

static int map_init_ch_cache (struct map_cache *cache)
{
    uint32_t pg_i;

    cache->pg_buf = ox_calloc (sizeof(struct map_cache_entry) * MAP_BUF_CH_PGS,
                                                          1, OX_MEM_OXBLK_GMAP);
    if (!cache->pg_buf)
        return -1;

    if (pthread_spin_init(&cache->mb_spin, 0))
        goto FREE_BUF;

    cache->mbf_head.lh_first = NULL;
    LIST_INIT(&cache->mbf_head);
    TAILQ_INIT(&cache->mbu_head);
    cache->nfree = 0;
    cache->nused = 0;

    for (pg_i = 0; pg_i < MAP_BUF_CH_PGS; pg_i++) {
        cache->pg_buf[pg_i].dirty = 0;
        cache->pg_buf[pg_i].buf_sz = MAP_BUF_PG_SZ;
        cache->pg_buf[pg_i].ppa.ppa = 0x0;
        cache->pg_buf[pg_i].md_entry = NULL;
        cache->pg_buf[pg_i].cache = cache;

        cache->pg_buf[pg_i].buf = ox_malloc (MAP_BUF_PG_SZ, OX_MEM_OXBLK_GMAP);
        if (!cache->pg_buf[pg_i].buf)
            goto FREE_PGS;

        LIST_INSERT_HEAD (&cache->mbf_head, &cache->pg_buf[pg_i], f_entry);
        cache->nfree++;
    }

    return 0;

FREE_PGS:
    while (pg_i) {
        pg_i--;
        LIST_REMOVE(&cache->pg_buf[pg_i], f_entry);
        cache->nfree--;
        ox_free (cache->pg_buf[pg_i].buf, OX_MEM_OXBLK_GMAP);
    }
    pthread_spin_destroy (&cache->mb_spin);
FREE_BUF:
    ox_free (cache->pg_buf, OX_MEM_OXBLK_GMAP);
    return -1;
}

static void map_flush_ch_cache (struct map_cache *cache, uint8_t full)
{
    struct map_cache_entry *ent = NULL;
    uint32_t n, ent_i, evicted = 0, persisted = 0;
    struct app_map_md *md = ch[cache->id]->map_md;
    uint8_t *immut;

    pthread_mutex_lock (&ch[cache->id]->map_md->tbl_mutex);
    cp_running = 1;

    /* make an immutable copy of SMALL table */
    immut = md->tbl + (md->entry_sz * md->entries);
    for (ent_i = 0; ent_i < md->entries; ent_i++) {
        pthread_mutex_lock (&md->entry_mutex[ent_i]);
        memcpy (immut + (md->entry_sz * ent_i),
                                md->tbl + (md->entry_sz * ent_i), md->entry_sz);
        pthread_mutex_unlock (&md->entry_mutex[ent_i]);
    }

    pthread_spin_lock (&cache->mb_spin);
    n = cache->nused;
    pthread_spin_unlock (&cache->mb_spin);

    while (n) {
        if (full || LIST_EMPTY(&cache->mbf_head)) {

            if (TAILQ_FIRST(&cache->mbu_head) == ent)
                ent = NULL;

            if (map_evict_pg_cache (cache, 1))
                log_err ("[gl_map: Cache entry not evicted "
                                                 "to NVM. Ch %d\n", cache->id);
            else {
                n--;
                evicted++;
            }

        } else {

            ent = (!ent) ? TAILQ_FIRST(&cache->mbu_head) : TAILQ_NEXT(ent, u_entry);
            if (ent) {
                if (map_evict_process (cache, ent, 1))
                    log_err ("[gl_map: Cache entry not persisted "
                                                 "in NVM. Ch %d\n", cache->id);
                else {
                    n--;
                    persisted++;
                }
            }

        }
    }

    /* Evict all remaining pages in the cache for clean shutdown (full) */
    if (full) {
        while (!(TAILQ_EMPTY(&cache->mbu_head))) {
            ent = TAILQ_FIRST(&cache->mbu_head);
            if (ent != NULL) {
                if (map_evict_pg_cache (cache, 1))
                    log_err ("[gl_map: Cache entry not evicted "
                                                 "in NVM. Ch %d\n", cache->id);
                else
                    evicted++;
            } else
                break;
        }
    }

    cp_running = 0;
    pthread_mutex_unlock (&ch[cache->id]->map_md->tbl_mutex);

    ox_stats_add_cp (OX_STATS_CP_MAP_EVICT, evicted + persisted);

    if (APP_DEBUG_GL_MAP)
        printf ("[gl-map cache %d. persisted: %d, evicted: %d]\n", cache->id,
                                                            persisted, evicted);
}

static void map_flush_all_caches (void)
{
    uint32_t ch_i = app_nch;

    while (ch_i) {
        ch_i--;
        map_flush_ch_cache (&map_ch_cache[ch_i], 0);
    }
}

static void map_exit_ch_cache (struct map_cache *cache)
{
    struct map_cache_entry *ent;

    map_flush_ch_cache (cache, 1);

    while (!(LIST_EMPTY(&cache->mbf_head))) {
        ent = LIST_FIRST(&cache->mbf_head);
        if (ent != NULL) {
            LIST_REMOVE(ent, f_entry);
            cache->nfree--;
            ox_free (ent->buf, OX_MEM_OXBLK_GMAP);
        }
    }

    pthread_spin_destroy (&cache->mb_spin);
    ox_free (cache->pg_buf, OX_MEM_OXBLK_GMAP);
}

static void map_exit_all_caches (void)
{
    uint32_t ch_i = app_nch;

    while (ch_i) {
        ch_i--;
        map_exit_ch_cache (&map_ch_cache[ch_i]);
    }
}

static int map_init (void)
{
    uint32_t nch, ch_i, pg_sz;

    if (!ox_mem_create_type ("OXBLK_GL_MAP", OX_MEM_OXBLK_GMAP))
        return -1;

    ch = ox_malloc (sizeof (struct app_channel *) * app_nch, OX_MEM_OXBLK_GMAP);
    if (!ch)
        return -1;

    nch = oxapp()->channels.get_list_fn (ch, app_nch);
    if (nch != app_nch)
        goto FREE_CH;

    map_ch_cache = ox_malloc (sizeof (struct map_cache) * app_nch,
                                                            OX_MEM_OXBLK_GMAP);
    if (!map_ch_cache)
        goto FREE_CH;

    pg_sz = ch[0]->ch->geometry->pl_pg_size;
    for (ch_i = 0; ch_i < app_nch; ch_i++) {
        pg_sz = MIN(ch[ch_i]->ch->geometry->pl_pg_size, pg_sz);

        if (map_init_ch_cache (&map_ch_cache[ch_i]))
            goto EXIT_BUF_CH;

        map_ch_cache[ch_i].id = ch_i;
    }

    cp_running = 0;
    map_ent_per_pg = pg_sz / sizeof (struct app_map_entry);

    /* Recalculate mapping metadata indexes if the table is new */
    if (app_map_new) {
        for (ch_i = 0; ch_i < app_nch; ch_i++)
            oxapp()->ch_map->create_fn (ch[ch_i]);
        app_map_new = 0;
    }

    log_info("    [appnvm: Global Mapping started.]\n");

    return 0;

EXIT_BUF_CH:
    while (ch_i) {
        ch_i--;
        map_exit_ch_cache (&map_ch_cache[ch_i]);
    }
    ox_free (map_ch_cache, OX_MEM_OXBLK_GMAP);
FREE_CH:
    ox_free (ch, OX_MEM_OXBLK_GMAP);
    return -1;
}

static void map_exit (void)
{
    map_exit_all_caches ();

    ox_free (map_ch_cache, OX_MEM_OXBLK_GMAP);
    ox_free (ch, OX_MEM_OXBLK_GMAP);

    log_info("    [appnvm: Global Mapping stopped.]\n");
}

static struct map_cache_entry *map_get_cache_entry (uint64_t lba)
{
    uint32_t ch_map, pg_off;
    uint64_t first_pg_lba;
    struct app_map_entry *md_ent;
    struct map_cache_entry *cache_ent = NULL;
    struct map_pg_addr *addr;

    /* Mapping metadata pages are spread among channels using round-robin */
    ch_map = (lba / map_ent_per_pg) % app_nch;
    pg_off = (lba / map_ent_per_pg) / app_nch;

    if (APP_DEBUG_GL_MAP)
        printf ("[gl-map: Get page cache. LBA %lu, ch: %d, off: %d]\n",
                                                          lba, ch_map, pg_off);

    md_ent = oxapp()->ch_map->get_fn (ch[ch_map], pg_off);
    if (!md_ent) {
        log_err ("[appnvm (gl_map): Map MD page out of bounds. Ch %d\n",ch_map);
        return NULL;
    }

    addr = (struct map_pg_addr *) &md_ent->ppa;

    /* If the PPA flag is zero, the mapping page is not cached yet */
    /* There is a mutex per metadata page */
    pthread_mutex_lock (&ch[ch_map]->map_md->entry_mutex[pg_off]);
    if (!addr->g.flag) {

        first_pg_lba = (lba / map_ent_per_pg) * map_ent_per_pg;

        if (map_load_pg_cache (&map_ch_cache[ch_map], md_ent, first_pg_lba,
                                                                     pg_off)) {
            pthread_mutex_unlock (&ch[ch_map]->map_md->entry_mutex[pg_off]);
            log_err ("[appnvm(gl_map): Mapping page not loaded ch %d\n",ch_map);
            return NULL;
        }

    } else {

        cache_ent = (struct map_cache_entry *) ((uint64_t) addr->g.addr);

        /* Keep cache entry as hot, in the tail of the queue */
        if (!cp_running) {
            pthread_spin_lock (&map_ch_cache[ch_map].mb_spin);
            TAILQ_REMOVE(&map_ch_cache[ch_map].mbu_head, cache_ent, u_entry);
            TAILQ_INSERT_TAIL(&map_ch_cache[ch_map].mbu_head, cache_ent, u_entry);
            pthread_spin_unlock (&map_ch_cache[ch_map].mb_spin);
        }

    }
    pthread_mutex_unlock (&ch[ch_map]->map_md->entry_mutex[pg_off]);

    /* At this point, the PPA only points to the cache */
    if (cache_ent == NULL)
        cache_ent = (struct map_cache_entry *) ((uint64_t) addr->g.addr);

    return cache_ent;
}

static int map_upsert_md (uint64_t index, uint64_t new_ppa, uint64_t old_ppa)
{
    uint32_t ch_map, pg_off;
    struct app_map_entry *md_ent;
    struct map_cache_entry *cache_ent;
    struct map_pg_addr *addr;
    struct nvm_ppa_addr ppa, old;
    int ret = 0;

    ch_map = index % app_nch;
    pg_off = index / app_nch;

    md_ent = oxapp()->ch_map->get_fn (ch[ch_map], pg_off);
    if (!md_ent) {
        log_err ("[ox-blk (gl_map): MD page out of bounds. Index %lu\n", index);
        return -1;
    }

    if (md_ent->lba != index) {
        ppa.ppa = md_ent->ppa;
        log_err ("[ox-blk (gl_map): UPD MD. LBA does not match entry. lba: %lu, "
            "md lba: %lu, md ppa: (%d/%d/%d/%d/%d/%d), Ch %d, pg_off %d\n",
            index, md_ent->lba, ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl,
            ppa.g.pg, ppa.g.sec, ch_map, pg_off);
        return -1;
    }

    addr = (struct map_pg_addr *) &md_ent->ppa;

    /* If the PPA flag is zero, the mapping page is not cached */
    pthread_mutex_lock (&ch[ch_map]->map_md->entry_mutex[pg_off]);
    if (!addr->g.flag) {

        if (addr->addr != old_ppa)
            ret = 1;
        else {
            /* Mark SMALL entry as dirty */
            oxapp()->ch_map->mark_fn (ch[ch_map],
				(index % ch[ch_map]->map_md->entries) /
				 ch[ch_map]->map_md->ent_per_pg);
            addr->addr = new_ppa;
        }

    } else {

        cache_ent = (struct map_cache_entry *) ((uint64_t) addr->g.addr);
        if (cache_ent->ppa.ppa != old_ppa)
            ret = 2;
        else
            cache_ent->ppa.ppa = new_ppa;

    }
    pthread_mutex_unlock (&ch[ch_map]->map_md->entry_mutex[pg_off]);

    ppa.ppa = (!addr->g.flag) ? addr->addr : cache_ent->ppa.ppa;
    old.ppa = old_ppa;
    if (old_ppa)
        oxapp()->md->invalidate_fn (ch[old.g.ch], &old, APP_INVALID_PAGE);

    if (APP_DEBUG_GL_MAP)
        printf("[gl-map: Upsert MD. Index: %lu, ch: %d, off: %d, cached: %d,"
                " ppa: (%d/%d/%d/%d), old: (%d/%d/%d/%d)]\n", index, ch_map,
                pg_off, addr->g.flag, ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pg,
                old.g.ch, old.g.lun, old.g.blk, old.g.pg);

    return ret;
}

static int map_upsert (uint64_t lba, uint64_t ppa, uint64_t *old,
                                                        uint64_t old_caller)
{
    uint32_t ch_map, ent_off;
    struct app_map_entry *map_ent;
    struct map_cache_entry *cache_ent;
    struct nvm_ppa_addr addr;

    ch_map = (lba / map_ent_per_pg) % app_nch;
    ent_off = lba % map_ent_per_pg;
    if (ent_off >= map_ent_per_pg) {
        log_err ("[ox-blk (gl_map): Entry offset out of bounds. Ch %d\n",ch_map);
        return -1;
    }

    if (APP_DEBUG_GL_MAP)
        printf("[gl-map: Upsert. LBA: %lu, ch: %d, off: %d]\n",
                                                        lba, ch_map, ent_off);

    cache_ent = map_get_cache_entry (lba);
    if (!cache_ent)
        return -1;

    map_ent = &((struct app_map_entry *) cache_ent->buf)[ent_off];

    if (map_ent->lba != lba) {
        addr.ppa = map_ent->ppa;
        log_err ("[ox-blk (gl_map): WRITE LBA does not match entry. lba: %lu, "
            "map lba: %lu, map ppa: (%d/%d/%d/%d/%d/%d), Ch %d, ent_off %d\n",
            lba, map_ent->lba, addr.g.ch, addr.g.lun, addr.g.blk, addr.g.pl,
            addr.g.pg, addr.g.sec, ch_map, ent_off);
        return -1;
    }

    pthread_spin_lock (cache_ent->spin);

    /* Fill old PPA pointer, caller may use to invalidate the sector for GC */
    *old = map_ent->ppa;

    /* User writes have priority and always update the mapping by setting 
       'old_caller' as 0. GC, for example, sets 'old_caller' with the old
       sector address. If other thread has updated it, keep the current value.*/
    if (old_caller && map_ent->ppa != old_caller) {
        pthread_spin_unlock (cache_ent->spin);
        return 1;
    }

    map_ent->ppa = ppa;
    cache_ent->dirty = 1;

    pthread_spin_unlock (cache_ent->spin);

    return 0;
}

static uint64_t map_read (uint64_t lba)
{
    struct map_cache_entry *cache_ent;
    struct app_map_entry *map_ent;
    struct nvm_ppa_addr ppa;
    uint32_t ent_off;
    uint64_t ret;

    ent_off = lba % map_ent_per_pg;
    if (ent_off >= map_ent_per_pg) {
        log_err ("[ox-blk (gl_map): Entry offset out of bounds. lba %lu\n", lba);
        return AND64;
    }

    if (APP_DEBUG_GL_MAP)
        printf("[gl-map: read. LBA: %lu, off: %d]\n", lba, ent_off);

    cache_ent = map_get_cache_entry (lba);
    if (!cache_ent)
        return AND64;

//  pthread_spin_lock (cache_ent->spin);
    map_ent = &((struct app_map_entry *) cache_ent->buf)[ent_off];

    if (map_ent->lba != lba) {
        ppa.ppa = map_ent->ppa;
        log_err ("[ox-blk (gl_map): READ LBA does not match entry. lba: %lu, "
            "map lba: %lu, map ppa: (%d/%d/%d/%d/%d/%d), ent_off %d\n",
                lba, map_ent->lba, ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl,
                ppa.g.pg, ppa.g.sec, ent_off);
//	pthread_spin_unlock (cache_ent->spin);
        return -1;
    }
    ret = map_ent->ppa;
//  pthread_spin_unlock (cache_ent->spin);

    return ret;
}

static struct app_gl_map oxblk_gl_map = {
    .mod_id         = OXBLK_GL_MAP,
    .name           = "OX-BLOCK-GMAP",
    .init_fn        = map_init,
    .exit_fn        = map_exit,
    .clear_fn       = map_flush_all_caches,
    .upsert_md_fn   = map_upsert_md,
    .upsert_fn      = map_upsert,
    .read_fn        = map_read
};

void oxb_gl_map_register (void) {
    app_mod_register (APPMOD_GL_MAP, OXBLK_GL_MAP, &oxblk_gl_map);
}
