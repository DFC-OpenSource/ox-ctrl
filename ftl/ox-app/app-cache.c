/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX-App Caching Framework
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
#include <sys/queue.h>
#include <libox.h>
#include <ox-app.h>

#define APP_CACHE_ADDR_FLAG   ((1 & AND64) << 63)

static struct app_channel **ch;
extern uint16_t             app_nch;

/* The caching strategy ensures the entry size matches with the NVM pg size */
static uint64_t             map_ent_per_pg;

static int app_cache_evict_pg (struct app_cache *cache)
{
    struct nvm_ppa_addr old_ppa;
    struct app_cache_entry *cache_ent;

    cache_ent = TAILQ_FIRST(&cache->mbu_head);
    if (!cache_ent)
        return -1;

    pthread_mutex_lock (cache_ent->mutex);

    pthread_spin_lock (&cache->mb_spin);
    TAILQ_REMOVE(&cache->mbu_head, cache_ent, u_entry);
    cache->nused--;
    pthread_spin_unlock (&cache->mb_spin);

    old_ppa.ppa = cache_ent->ppa.ppa;

    if (cache_ent->dirty) {
        if (cache->write_fn (cache_ent->buf, cache_ent->up_entry)) {

            pthread_spin_lock (&cache->mb_spin);
            TAILQ_INSERT_HEAD(&cache->mbu_head, cache_ent, u_entry);
            cache->nused++;
            pthread_spin_unlock (&cache->mb_spin);

            pthread_mutex_unlock (cache_ent->mutex);

            return -1;
        }
        
        cache_ent->dirty = 0;

        /* Cache up entry PPA is set after the write completes */

        /* Invalidate old page PPAs */
        if (old_ppa.ppa)
            oxapp()->md->invalidate_fn (ch[old_ppa.g.ch], &old_ppa,
                                                             APP_INVALID_PAGE);
    }

    cache_ent->up_entry->ppa = cache_ent->ppa.ppa;
    cache_ent->ppa.ppa = 0;
    cache_ent->up_entry = NULL;

    pthread_mutex_unlock (cache_ent->mutex);

    pthread_spin_lock (&cache->mb_spin);
    LIST_INSERT_HEAD (&cache->mbf_head, cache_ent, f_entry);
    cache->nfree++;
    pthread_spin_unlock (&cache->mb_spin);

    return 0;
}

static void __app_cache_destroy (struct app_cache *cache)
{
    struct app_cache_entry *ent;

    /* Evict all pages in the cache */
    while (!(TAILQ_EMPTY(&cache->mbu_head))) {
        ent = TAILQ_FIRST(&cache->mbu_head);
        if (ent != NULL)
            if (app_cache_evict_pg (cache))
                log_err ("[appnvm (gl_map): ERROR. Cache entry not persisted "
                                                 "in NVM. Ch %d\n", cache->id);
    }

    /* TODO: Check if any cache entry still remains. Retry I/Os */

    while (!(LIST_EMPTY(&cache->mbf_head))) {
        ent = LIST_FIRST(&cache->mbf_head);
        if (ent != NULL) {
            LIST_REMOVE(ent, f_entry);
            cache->nfree--;
            ox_free (ent->buf, cache->ox_mem_id);
        }
    }

    pthread_spin_destroy (&cache->mb_spin);
    ox_free (cache->pg_buf, cache->ox_mem_id);
}

static int __app_cache_create (struct app_cache *cache,
                        uint16_t ox_mem_id, uint32_t cache_sz, uint32_t ent_sz)
{
    uint32_t pg_i;

    cache->pg_buf = ox_calloc (cache_sz, sizeof(struct app_cache_entry),
                                                                    ox_mem_id);
    if (!cache->pg_buf)
        return -1;

    if (pthread_spin_init(&cache->mb_spin, 0))
        goto FREE_BUF;

    cache->ox_mem_id    = ox_mem_id;
    cache->cache_sz     = cache_sz;
    cache->cache_ent_sz = ent_sz;

    cache->mbf_head.lh_first = NULL;
    LIST_INIT(&cache->mbf_head);
    TAILQ_INIT(&cache->mbu_head);
    cache->nfree = 0;
    cache->nused = 0;

    for (pg_i = 0; pg_i < cache_sz; pg_i++) {
        cache->pg_buf[pg_i].dirty = 0;
        cache->pg_buf[pg_i].buf_sz = ent_sz;
        cache->pg_buf[pg_i].ppa.ppa = 0x0;
        cache->pg_buf[pg_i].up_entry = NULL;
        cache->pg_buf[pg_i].cache = cache;

        cache->pg_buf[pg_i].buf = ox_malloc (ent_sz, ox_mem_id);
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
        ox_free (cache->pg_buf[pg_i].buf, ox_mem_id);
    }
    pthread_spin_destroy (&cache->mb_spin);
FREE_BUF:
    ox_free (cache->pg_buf, ox_mem_id);
    return -1;
}

struct app_cache_entry *app_cache_get (struct app_cache *cache, uint64_t lba)
{
    /* TODO: Not implemented yet */
    return NULL;
}

struct app_cache *app_cache_create (uint16_t ox_mem_id, uint32_t cache_sz,
                            uint32_t ent_sz, uint16_t instances,
                            app_cache_rw_fn *read_fn, app_cache_rw_fn *write_fn)
{
    struct app_cache *cache;
    uint16_t cid;

    cache = ox_malloc (sizeof(struct app_cache) * instances, ox_mem_id);
    if (!cache)
        return NULL;

    cache->instances = instances;
    cache->read_fn   = read_fn;
    cache->write_fn  = write_fn;

    for (cid = 0; cid < instances; cid++) {
        if (__app_cache_create (&cache[cid], ox_mem_id, cache_sz, ent_sz))
            goto FREE;
    }

    return cache;

FREE:
    while (cid) {
        cid--;
        __app_cache_destroy (&cache[cid]);
    }
    ox_free (cache, ox_mem_id);
    return NULL;
}

void app_cache_destroy (struct app_cache *cache)
{
    uint16_t cid;

    for (cid = 0; cid < cache->instances; cid++)
        __app_cache_destroy (&cache[cid]);

    ox_free (cache, cache->ox_mem_id);
}

int app_cache_init (void)
{
    uint32_t nch;

    ch = ox_malloc (sizeof (struct app_channel *) * app_nch, OX_MEM_OXAPP);
    if (!ch)
        return -1;

    nch = oxapp()->channels.get_list_fn (ch, app_nch);
    if (nch != app_nch)
        goto FREE;

    map_ent_per_pg = ch[0]->ch->geometry->pl_pg_size /
                                                 sizeof (struct app_hmap_entry);
    return 0;

FREE:
    ox_free (ch, OX_MEM_OXAPP);
    return -1;
}

void app_cache_exit (void)
{
    ox_free (ch, OX_MEM_OXAPP);
}