/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX-App Flash Translation Layer (Channel Management)
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
#include <stdint.h>
#include <string.h>
#include <libox.h>

LIST_HEAD(app_ch, app_channel) app_ch_head = LIST_HEAD_INITIALIZER(app_ch_head);

extern struct core_struct core;

static int app_reserve_blks (struct app_channel *lch)
{
    int trsv, n, pl, n_pl;
    struct nvm_ppa_addr *ppa;
    struct nvm_channel *ch = lch->ch;

    n_pl = ch->geometry->n_of_planes;

    ch->ftl_rsv = APP_RSV_BLK_COUNT;
    trsv = ch->ftl_rsv * n_pl;

    /* ftl_rsv_list is first allocated by the MMGR that also frees the memory */
    ch->ftl_rsv_list = ox_realloc (ch->ftl_rsv_list,
                            trsv * sizeof(struct nvm_ppa_addr), OX_MEM_MMGR);
    if (!ch->ftl_rsv_list)
        return EMEM;

    memset (ch->ftl_rsv_list, 0, trsv * sizeof(struct nvm_ppa_addr));

    for (n = 0; n < ch->ftl_rsv; n++) {
        for (pl = 0; pl < n_pl; pl++) {
            ppa = &ch->ftl_rsv_list[n_pl * n + pl];
            ppa->g.ch = ch->ch_mmgr_id;
            ppa->g.lun = 0;
            ppa->g.blk = n + ch->mmgr_rsv;
            ppa->g.pl = pl;
        }
    }

    /* Set reserved blocks */
    lch->bbt_blk  = ch->mmgr_rsv + APP_RSV_BBT_OFF;
    lch->meta_blk = ch->mmgr_rsv + APP_RSV_META_OFF;
    lch->map_blk  = ch->mmgr_rsv + APP_RSV_MAP_OFF;
    lch->cp_blk   = ch->mmgr_rsv + APP_RSV_CP_OFF;

    return 0;
}

static int app_init_bbt (struct app_channel *lch)
{
    int ret = -1;
    uint32_t tblks;
    struct app_bbtbl *bbt;
    struct nvm_channel *ch = lch->ch;
    int n_pl = ch->geometry->n_of_planes;

    /* For bad block table we consider blocks in a single plane */
    tblks = ch->geometry->blk_per_lun * ch->geometry->lun_per_ch * n_pl;

    lch->bbtbl = ox_malloc (sizeof(struct app_bbtbl), OX_MEM_APP_CH);
    if (!lch->bbtbl)
        return -1;

    bbt = lch->bbtbl;
    bbt->tbl = ox_malloc (sizeof(uint8_t) * tblks, OX_MEM_APP_CH);
    if (!bbt->tbl)
        goto FREE_BBTBL;

    memset (bbt->tbl, 0, tblks);
    bbt->byte.magic = 0;
    bbt->bb_sz = tblks;

    ret = oxapp()->bbt->load_fn (lch);
    if (ret) goto ERR;

    /* create and flush bad block table if it does not exist */
    /* this procedure will erase the entire device (only in test mode) */
    if (bbt->byte.magic == APP_MAGIC /*|| core.reset*/) {
        ret = oxapp()->bbt->create_fn (lch, APP_BBT_EMERGENCY);
        if (ret) goto ERR;
        ret = oxapp()->bbt->flush_fn (lch);
        if (ret) goto ERR;
    }

    log_info("    [ox-app: Bad block table started. Ch %d]\n", ch->ch_id);

    return 0;

ERR:
    ox_free (lch->bbtbl->tbl, OX_MEM_APP_CH);
FREE_BBTBL:
    ox_free(lch->bbtbl, OX_MEM_APP_CH);

    log_err("[ox-app ERR: Ch %d -> Not possible to read/create bad block "
                                                        "table.]\n", ch->ch_id);
    return -1;
}

static void app_exit_bbt (struct app_channel *lch)
{
    ox_free(lch->bbtbl->tbl, OX_MEM_APP_CH);
    ox_free(lch->bbtbl, OX_MEM_APP_CH);
}

static int app_init_blk_md (struct app_channel *lch)
{
    int ret = -1;
    uint32_t tblks, ent_i, tiny_sz;
    struct app_blk_md *md;
    struct nvm_channel *ch = lch->ch;

    /* For block metadata, we consider multi-plane blocks */
    tblks = ch->geometry->blk_per_lun * ch->geometry->lun_per_ch;

    lch->blk_md = ox_malloc (sizeof(struct app_blk_md), OX_MEM_APP_CH);
    if (!lch->blk_md)
        return -1;

    md = lch->blk_md;
    md->entry_sz = sizeof(struct app_blk_md_entry);
    md->ent_per_pg = (ch->geometry->pg_size / md->entry_sz) *
                                                lch->ch->geometry->n_of_planes;
    tiny_sz = tblks / md->ent_per_pg;
    tiny_sz += (tblks % md->ent_per_pg != 0) ? 1 : 0;

    md->entry_spin = ox_malloc (sizeof (pthread_spinlock_t) *
                                                        tiny_sz, OX_MEM_APP_CH);
    if (!md->entry_spin)
        goto FREE_MD;

    for (ent_i = 0; ent_i < tiny_sz; ent_i++) {
        if (pthread_spin_init (&md->entry_spin[ent_i], 0))
            goto DESTROY_SPIN;
    }

    md->tbl = ox_calloc (md->entry_sz, tblks, OX_MEM_APP_CH);
    if (!md->tbl)
        goto DESTROY_SPIN;

    ox_stats_add_cp (OX_STATS_CP_BLK_SMALL_SZ, tblks * md->entry_sz);

    md->closed_ts = ox_calloc (tblks, sizeof (uint64_t), OX_MEM_APP_CH);
    if (!md->closed_ts)
        goto FREE_TBL;

    /* Setup tiny table */
    md->tiny.entry_sz = sizeof(struct app_tiny_entry);
    md->tiny.entries = tiny_sz;
    md->tiny.tbl = ox_calloc (md->tiny.entries, md->tiny.entry_sz,
                                                                OX_MEM_APP_CH);
    if (!md->tiny.tbl)
        goto FREE_TS;

    ox_stats_add_cp (OX_STATS_CP_BLK_TINY_SZ, tiny_sz * md->tiny.entry_sz);

    md->tiny.dirty = ox_calloc (1, md->tiny.entries, OX_MEM_APP_CH);
    if (!md->tiny.dirty)
        goto FREE_TINY;

    md->byte.magic = 0;
    md->entries = tblks;

    ret = oxapp()->md->load_fn (lch);
    if (ret) goto FREE_DIRTY;

    /* create and flush block metadata table if it does not exist */
    if ((md->byte.magic == APP_MAGIC) || core.reset) {
        ret = oxapp()->md->create_fn (lch);
        if (ret) goto FREE_DIRTY;
    }

    log_info("    [ox-app: Block metadata started. Ch %d]\n", ch->ch_id);

    return 0;

FREE_DIRTY:
    ox_free (md->tiny.dirty, OX_MEM_APP_CH);
FREE_TINY:
    ox_free (md->tiny.tbl, OX_MEM_APP_CH);
FREE_TS:
    ox_free (md->closed_ts, OX_MEM_APP_CH);
FREE_TBL:
    ox_free (md->tbl, OX_MEM_APP_CH);
DESTROY_SPIN:
    while (ent_i) {
        ent_i--;
        pthread_spin_destroy (&lch->blk_md->entry_spin[ent_i]);
    }
    ox_free ((void *)lch->blk_md->entry_spin, OX_MEM_APP_CH);
FREE_MD:
    ox_free (lch->blk_md, OX_MEM_APP_CH);

    log_err("[ox-app ERR: Ch %d -> Not possible to read/create block metadata "
                                                        "table.]\n", ch->ch_id);
    return -1;
}

static void app_exit_blk_md (struct app_channel *lch)
{
    uint32_t ent_i;

    ox_free (lch->blk_md->tiny.dirty, OX_MEM_APP_CH);
    ox_free (lch->blk_md->tiny.tbl, OX_MEM_APP_CH);
    ox_free (lch->blk_md->closed_ts, OX_MEM_APP_CH);
    ox_free (lch->blk_md->tbl, OX_MEM_APP_CH);

    for (ent_i = 0; ent_i < lch->blk_md->tiny.entries; ent_i++)
        pthread_spin_destroy (&lch->blk_md->entry_spin[ent_i]);

    ox_free ((void *)lch->blk_md->entry_spin, OX_MEM_APP_CH);
    ox_free (lch->blk_md, OX_MEM_APP_CH);
}

static int app_init_map_lock (struct app_map_md *md)
{
    uint32_t ent_i, ent_s;

    if (pthread_mutex_init (&md->tbl_mutex, NULL))
        return -1;

    md->entry_mutex = ox_malloc (sizeof(pthread_mutex_t) * md->entries,
                                                                OX_MEM_APP_CH);
    if (!md->entry_mutex)
        goto TBL_MUTEX;

    for (ent_i = 0; ent_i < md->entries; ent_i++) {
        if (pthread_mutex_init (&md->entry_mutex[ent_i], NULL))
            goto ENT_MUTEX;
    }

    md->entry_spin = ox_malloc (sizeof(pthread_spinlock_t) * md->entries,
                                                                OX_MEM_APP_CH);
    if (!md->entry_spin)
        goto ENT_MUTEX;

    for (ent_s = 0; ent_s < md->entries; ent_s++) {
        if (pthread_spin_init (&md->entry_spin[ent_s], 0))
            goto ENT_SPIN;
    }

    return 0;

ENT_SPIN:
    while (ent_s) {
        ent_s--;
        pthread_spin_destroy (&md->entry_spin[ent_s]);
    }
    ox_free ((void *)md->entry_spin, OX_MEM_APP_CH);
ENT_MUTEX:
    while (ent_i) {
        ent_i--;
        pthread_mutex_destroy (&md->entry_mutex[ent_i]);
    }
    ox_free (md->entry_mutex, OX_MEM_APP_CH);
TBL_MUTEX:
    pthread_mutex_destroy (&md->tbl_mutex);

    return -1;
}

static int app_init_map (struct app_channel *lch)
{
    int ret = -1;
    uint32_t ch_map_md_ents;
    uint64_t ch_map_sz;
    struct app_map_md *md;
    struct nvm_channel *ch = lch->ch;
    struct nvm_mmgr_geometry *g = ch->geometry;

    /* sector granularity mapping table */
    ch_map_sz = g->sec_per_ch * sizeof(struct app_map_entry);
    ox_stats_add_cp (OX_STATS_CP_MAP_BIG_SZ, ch_map_sz);

    /* each map_md entry maps to a mapping table physical page */
    ch_map_md_ents = ch_map_sz / g->pl_pg_size;

    lch->map_md = ox_calloc (1, sizeof(struct app_map_md), OX_MEM_APP_CH);
    if (!lch->map_md)
        return -1;

    md = lch->map_md;
    md->entry_sz = sizeof(struct app_map_entry);
    md->ent_per_pg = (ch->geometry->pg_size / md->entry_sz) *
                                                lch->ch->geometry->n_of_planes;

    /* Allocate double space for a immutable copy needed for checkpoint */
    md->tbl = ox_calloc (md->entry_sz, ch_map_md_ents * 2, OX_MEM_APP_CH);
    if (!md->tbl)
        goto FREE_MD;

    ox_stats_add_cp (OX_STATS_CP_MAP_SMALL_SZ, ch_map_md_ents * md->entry_sz);

    /* Setup tiny table */
    md->tiny.entry_sz = sizeof(struct app_tiny_entry);
    md->tiny.entries = ch_map_md_ents / md->ent_per_pg;
    md->tiny.entries += (ch_map_md_ents % md->ent_per_pg != 0) ? 1 : 0;
    md->tiny.tbl = ox_calloc (md->tiny.entries, md->tiny.entry_sz,
                                                                OX_MEM_APP_CH);
    if (!md->tiny.tbl)
        goto FREE_TBL;

    ox_stats_add_cp (OX_STATS_CP_MAP_TINY_SZ, md->tiny.entries *
                                                            md->tiny.entry_sz);

    md->tiny.dirty = ox_calloc (1, md->tiny.entries, OX_MEM_APP_CH);
    if (!md->tiny.dirty)
        goto FREE_TINY;

    memset (md->tbl, 0, md->entry_sz * ch_map_md_ents);
    md->byte.magic = 0;
    md->entries = ch_map_md_ents;

    ret = oxapp()->ch_map->load_fn (lch);
    if (ret) goto FREE_DIRTY;

    /* create and flush mapping metadata table if it does not exist */
    if (md->byte.magic == APP_MAGIC || core.reset) {
        ret = oxapp()->ch_map->create_fn (lch);
        if (ret) goto FREE_DIRTY;
    }

    if (app_init_map_lock (md))
        goto FREE_DIRTY;

    log_info("    [ox-app: Mapping metadata started. Ch %d]\n", ch->ch_id);

    return 0;

FREE_DIRTY:
    ox_free (md->tiny.dirty, OX_MEM_APP_CH);
FREE_TINY:
    ox_free (md->tiny.tbl, OX_MEM_APP_CH);
FREE_TBL:
    ox_free (lch->map_md->tbl, OX_MEM_APP_CH);
FREE_MD:
    ox_free (lch->map_md, OX_MEM_APP_CH);

    log_err("[ox-app ERR: Ch %d -> Not possible to read/create mapping "
                                              "metadata table.]\n", ch->ch_id);
    return -1;
}

static void app_exit_map (struct app_channel *lch)
{
    uint32_t ent_i = lch->map_md->entries;

    while (ent_i) {
        ent_i--;
        pthread_mutex_destroy (&lch->map_md->entry_mutex[ent_i]);
        pthread_spin_destroy (&lch->map_md->entry_spin[ent_i]);
    }
    pthread_mutex_destroy (&lch->map_md->tbl_mutex);

    ox_free (lch->map_md->entry_mutex, OX_MEM_APP_CH);
    ox_free ((void *)lch->map_md->entry_spin, OX_MEM_APP_CH);
    ox_free (lch->map_md->tiny.dirty, OX_MEM_APP_CH);
    ox_free (lch->map_md->tiny.tbl, OX_MEM_APP_CH);
    ox_free (lch->map_md->tbl, OX_MEM_APP_CH);
    ox_free (lch->map_md, OX_MEM_APP_CH);
}

static int channels_init (struct nvm_channel *ch, uint16_t id)
{
    struct app_channel *lch;
    uint32_t blk_sz;
    struct timespec ts;
    uint64_t start, end;
    
    lch = ox_malloc (sizeof(struct app_channel), OX_MEM_APP_CH);
    if (!lch)
        return -1;

    lch->ch = ch;

    LIST_INSERT_HEAD(&app_ch_head, lch, entry);
    lch->app_ch_id = id;

    lch->flags.busy.counter = U_ATOMIC_INIT_RUNTIME(0);

    if (pthread_spin_init (&lch->flags.busy_spin, 0))    goto FREE_LCH;
    if (pthread_spin_init (&lch->flags.active_spin, 0))  goto BUSY_SPIN;
    if (pthread_spin_init (&lch->flags.need_gc_spin, 0)) goto ACT_SPIN;

    /* Enabled channel and no need for GC */
    app_ch_active_set (lch);
    app_ch_need_gc_unset (lch);

    if (app_reserve_blks (lch))
        goto GC_SPIN;

    GET_MICROSECONDS (start, ts);
    if (app_init_bbt (lch))
        goto FREE_LCH;
    GET_MICROSECONDS (end, ts);
    ox_stats_add_rec (OX_STATS_REC_BBT_US, end - start);

    GET_MICROSECONDS (start, ts);
    if (app_init_blk_md (lch))
        goto FREE_BBT;
    GET_MICROSECONDS (end, ts);
    ox_stats_add_rec (OX_STATS_REC_BLK_US, end - start);

    if (oxapp()->ch_prov->init_fn (lch))
        goto FREE_BLK_MD;

    GET_MICROSECONDS (start, ts);
    if (app_init_map (lch))
        goto EXIT_CH_PROV;
    GET_MICROSECONDS (end, ts);
    ox_stats_add_rec (OX_STATS_REC_CH_MAP_US, end - start);

    /* Remove reserved blocks from namespace */
    blk_sz = lch->ch->geometry->pg_per_blk * lch->ch->geometry->pg_size;
    core.nvm_ns_size -= lch->ch->mmgr_rsv * blk_sz;
    core.nvm_ns_size -= lch->ch->ftl_rsv * blk_sz;

    log_info("    [ox-app: channel %d started with %d bad blocks.]\n",ch->ch_id,
                                                          lch->bbtbl->bb_count);
    return 0;

EXIT_CH_PROV:
    oxapp()->ch_prov->exit_fn (lch);
FREE_BLK_MD:
    app_exit_blk_md (lch);
FREE_BBT:
    app_exit_bbt (lch);
GC_SPIN:
    pthread_spin_destroy (&lch->flags.need_gc_spin);
ACT_SPIN:
    pthread_spin_destroy (&lch->flags.active_spin);
BUSY_SPIN:
    pthread_spin_destroy (&lch->flags.busy_spin);
FREE_LCH:
    LIST_REMOVE (lch, entry);
    ox_free(lch, OX_MEM_APP_CH);
    return -1;
}

static void channels_exit (struct app_channel *lch)
{
    app_exit_map (lch);
    oxapp()->ch_prov->exit_fn (lch);
    app_exit_blk_md (lch);
    app_exit_bbt (lch);

    pthread_spin_destroy (&lch->flags.busy_spin);
    pthread_spin_destroy (&lch->flags.active_spin);
    pthread_spin_destroy (&lch->flags.need_gc_spin);

    LIST_REMOVE (lch, entry);
    ox_free (lch, OX_MEM_APP_CH);
}

static struct app_channel *channels_get(uint16_t ch_id)
{
    struct app_channel *lch;
    LIST_FOREACH(lch, &app_ch_head, entry){
        if(lch->ch->ch_id == ch_id)
            return lch;
    }

    return NULL;
}

static int channels_get_list (struct app_channel **list, uint16_t nch)
{
    int n = 0, i = nch - 1;
    struct app_channel *lch;

    LIST_FOREACH(lch, &app_ch_head, entry){
        if (i < 0)
            break;
        list[i] = lch;
        i--;
        n++;
    }

    return n;
}

void app_channels_register (void) {
    oxapp()->channels.init_fn = channels_init;
    oxapp()->channels.exit_fn = channels_exit;
    oxapp()->channels.get_fn = channels_get;
    oxapp()->channels.get_list_fn = channels_get_list;

    if (!ox_mem_create_type ("OXAPP_CHANNELS", OX_MEM_APP_CH))
        return;

    LIST_INIT(&app_ch_head);
}
