/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Flash Translation Layer (Channel Management)
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
#include "appnvm.h"
#include <sys/queue.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include "../../include/ssd.h"

LIST_HEAD(app_ch, app_channel) app_ch_head = LIST_HEAD_INITIALIZER(app_ch_head);

extern struct core_struct core;
extern pthread_spinlock_t *md_ch_spin;

static int app_reserve_blks (struct app_channel *lch)
{
    int trsv, n, pl, n_pl;
    struct nvm_ppa_addr *ppa;
    struct nvm_channel *ch = lch->ch;

    n_pl = ch->geometry->n_of_planes;

    ch->ftl_rsv = APP_RSV_BLK_COUNT;
    trsv = ch->ftl_rsv * n_pl;

    /* ftl_rsv_list is first allocated by the MMGR that also frees the memory */
    ch->ftl_rsv_list = realloc (ch->ftl_rsv_list,
                                           trsv * sizeof(struct nvm_ppa_addr));
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

    lch->bbtbl = malloc (sizeof(struct app_bbtbl));
    if (!lch->bbtbl)
        return -1;

    bbt = lch->bbtbl;
    bbt->tbl = malloc (sizeof(uint8_t) * tblks);
    if (!bbt->tbl)
        goto FREE_BBTBL;

    memset (bbt->tbl, 0, tblks);
    bbt->magic = 0;
    bbt->bb_sz = tblks;

    ret = appnvm()->bbt->load_fn (lch);
    if (ret) goto ERR;

    /* create and flush bad block table if it does not exist */
    /* this procedure will erase the entire device (only in test mode) */
    if (bbt->magic == APP_MAGIC) {
        ret = appnvm()->bbt->create_fn (lch, APP_BBT_EMERGENCY);
        if (ret) goto ERR;
        ret = appnvm()->bbt->flush_fn (lch);
        if (ret) goto ERR;
    }

    log_info("    [appnvm: Bad block table started. Ch %d]\n", ch->ch_id);

    return 0;

ERR:
    free(lch->bbtbl->tbl);
FREE_BBTBL:
    free(lch->bbtbl);

    log_err("[appnvm ERR: Ch %d -> Not possible to read/create bad block "
                                                        "table.]\n", ch->ch_id);
    return -1;
}

static void app_exit_bbt (struct app_channel *lch)
{
    free(lch->bbtbl->tbl);
    free(lch->bbtbl);
}

static int app_init_blk_md (struct app_channel *lch)
{
    int ret = -1;
    uint32_t tblks;
    struct app_blk_md *md;
    struct nvm_channel *ch = lch->ch;

    /* For block metadata, we consider multi-plane blocks */
    tblks = ch->geometry->blk_per_lun * ch->geometry->lun_per_ch;

    lch->blk_md = malloc (sizeof(struct app_blk_md));
    if (!lch->blk_md)
        return -1;

    md = lch->blk_md;
    md->entry_sz = sizeof(struct app_blk_md_entry);
    md->tbl = malloc (md->entry_sz * tblks);
    if (!md->tbl)
        goto FREE_MD;

    memset (md->tbl, 0, md->entry_sz * tblks);
    md->magic = 0;
    md->entries = tblks;

    ret = appnvm()->md->load_fn (lch);
    if (ret) goto ERR;

    /* create and flush block metadata table if it does not exist */
    if (md->magic == APP_MAGIC) {
        ret = appnvm()->md->create_fn (lch);
        if (ret) goto ERR;
        ret = appnvm()->md->flush_fn (lch);
        if (ret) goto ERR;
    }

    log_info("    [appnvm: Block metadata started. Ch %d]\n", ch->ch_id);

    return 0;

ERR:
    free (lch->blk_md->tbl);
FREE_MD:
    free (lch->blk_md);

    log_err("[appnvm ERR: Ch %d -> Not possible to read/create block metadata "
                                                        "table.]\n", ch->ch_id);
    return -1;
}

static void app_exit_blk_md (struct app_channel *lch)
{
    free (lch->blk_md->tbl);
    free (lch->blk_md);
}

static int app_init_map (struct app_channel *lch)
{
    int ret = -1;
    uint32_t ch_map_md_ent, ent_i;
    uint64_t ch_map_sz;
    struct app_map_md *md;
    struct nvm_channel *ch = lch->ch;
    struct nvm_mmgr_geometry *g = ch->geometry;

    /* sector granularity mapping table */
    ch_map_sz = g->sec_per_ch * sizeof(struct app_map_entry);

    /* each map_md entry maps to a mapping table physical page */
    ch_map_md_ent = ch_map_sz / g->pl_pg_size;

    lch->map_md = malloc (sizeof(struct app_map_md));
    if (!lch->map_md)
        return -1;

    md = lch->map_md;
    md->entry_sz = sizeof(struct app_map_entry);
    md->tbl = malloc (md->entry_sz * ch_map_md_ent);
    if (!md->tbl)
        goto FREE_MD;

    memset (md->tbl, 0, md->entry_sz * ch_map_md_ent);
    md->magic = 0;
    md->entries = ch_map_md_ent;

    ret = appnvm()->ch_map->load_fn (lch);
    if (ret) goto FREE_TBL;

    /* create and flush mapping metadata table if it does not exist */
    if (md->magic == APP_MAGIC) {
        ret = appnvm()->ch_map->create_fn (lch);
        if (ret) goto FREE_TBL;
        ret = appnvm()->ch_map->flush_fn (lch);
        if (ret) goto FREE_TBL;
    }

    md->entry_mutex = malloc (sizeof(pthread_mutex_t) * md->entries);
    if (!md->entry_mutex)
        goto FREE_TBL;

    for (ent_i = 0; ent_i < md->entries; ent_i++) {
        if (pthread_mutex_init (&md->entry_mutex[ent_i], NULL))
            goto ENT_MUTEX;
    }


    log_info("    [appnvm: Mapping metadata started. Ch %d]\n", ch->ch_id);

    return 0;

ENT_MUTEX:
    while (ent_i) {
        ent_i--;
        pthread_mutex_destroy (&md->entry_mutex[ent_i]);
    }
    free (md->entry_mutex);
FREE_TBL:
    free (lch->map_md->tbl);
FREE_MD:
    free (lch->map_md);

    log_err("[appnvm ERR: Ch %d -> Not possible to read/create mapping "
                                              "metadata table.]\n", ch->ch_id);
    return -1;
}

static void app_exit_map (struct app_channel *lch)
{
    uint32_t ent_i = lch->map_md->entries;

    while (ent_i) {
        ent_i--;
        pthread_mutex_destroy (&lch->map_md->entry_mutex[ent_i]);
    }

    free (lch->map_md->entry_mutex);
    free (lch->map_md->tbl);
    free (lch->map_md);
}

static int channels_init (struct nvm_channel *ch, uint16_t id)
{
    struct app_channel *lch;
    uint32_t blk_sz;

    md_ch_spin = realloc
                 ((void *) md_ch_spin, sizeof (pthread_spinlock_t) * (id + 1));
    if (!md_ch_spin)
        return -1;

    if (pthread_spin_init (&md_ch_spin[id], 0))
        return -1;

    lch = malloc (sizeof(struct app_channel));
    if (!lch)
        goto CH_SPIN;

    lch->ch = ch;

    LIST_INSERT_HEAD(&app_ch_head, lch, entry);
    lch->app_ch_id = id;

    lch->flags.busy.counter = ATOMIC_INIT_RUNTIME(0);

    if (pthread_spin_init (&lch->flags.busy_spin, 0))    goto FREE_LCH;
    if (pthread_spin_init (&lch->flags.active_spin, 0))  goto BUSY_SPIN;
    if (pthread_spin_init (&lch->flags.need_gc_spin, 0)) goto ACT_SPIN;

    /* Enabled channel and no need for GC */
    appnvm_ch_active_set (lch);
    appnvm_ch_need_gc_unset (lch);

    if (app_reserve_blks (lch))
        goto GC_SPIN;

    if (app_init_bbt (lch))
        goto FREE_LCH;

    if (app_init_blk_md (lch))
        goto FREE_BBT;

    if (appnvm()->ch_prov->init_fn (lch))
        goto FREE_BLK_MD;

    if (app_init_map (lch))
        goto EXIT_CH_PROV;

    /* Remove reserved blocks from namespace */
    blk_sz = lch->ch->geometry->pg_per_blk * lch->ch->geometry->pg_size;
    core.nvm_ns_size -= lch->ch->mmgr_rsv * blk_sz;
    core.nvm_ns_size -= lch->ch->ftl_rsv * blk_sz;

    log_info("    [appnvm: channel %d started with %d bad blocks.]\n",ch->ch_id,
                                                          lch->bbtbl->bb_count);
    return 0;

EXIT_CH_PROV:
    appnvm()->ch_prov->exit_fn (lch);
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
    free(lch);
CH_SPIN:
    pthread_spin_destroy (&md_ch_spin[id]);
    return -1;
}

static void channels_exit (struct app_channel *lch)
{
    int ret, retry;

    retry = 0;
    do {
        retry++;
        ret = appnvm()->ch_map->flush_fn (lch);
    } while (ret && retry < APPNVM_FLUSH_RETRY);

    /* TODO: Recover from last checkpoint (make a checkpoint) */
    if (ret)
        log_err(" [appnvm: ERROR. Mapping metadata not flushed to NVM. "
                                          "Channel %d]", lch->ch->ch_id);
    else
        log_info(" [appnvm: Mapping metadata persisted into NVM. "
                                          "Channel %d]", lch->ch->ch_id);

    retry = 0;
    do {
        retry++;
        ret = appnvm()->md->flush_fn (lch);
    } while (ret && retry < APPNVM_FLUSH_RETRY);

    /* TODO: Recover from last checkpoint (make a checkpoint) */
    if (ret)
        log_err(" [appnvm: ERROR. Block metadata not flushed to NVM. "
                                          "Channel %d]", lch->ch->ch_id);
    else
        log_info(" [appnvm: Block metadata persisted into NVM. "
                                          "Channel %d]", lch->ch->ch_id);

    app_exit_map (lch);
    appnvm()->ch_prov->exit_fn (lch);
    app_exit_blk_md (lch);
    app_exit_bbt (lch);

    pthread_spin_destroy (&lch->flags.busy_spin);
    pthread_spin_destroy (&lch->flags.active_spin);
    pthread_spin_destroy (&lch->flags.need_gc_spin);
    pthread_spin_destroy (&md_ch_spin[lch->app_ch_id]);

    LIST_REMOVE (lch, entry);
    free(lch);
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

void channels_register (void) {
    appnvm()->channels.init_fn = channels_init;
    appnvm()->channels.exit_fn = channels_exit;
    appnvm()->channels.get_fn = channels_get;
    appnvm()->channels.get_list_fn = channels_get_list;

    LIST_INIT(&app_ch_head);
}