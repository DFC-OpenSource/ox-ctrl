/* OX: Open-Channel NVM Express SSD Controller
 * 
 *  - OX-App Hierarchical Mapping
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

#include <stdio.h>
#include <string.h>
#include <libox.h>
#include <ox-app.h>

/* Maximum static table size: 32 MB */
#define HMAP_MAX_STATIC_SZ  1024 * 1024 * 32

#define HMAP_CACHE_CH_SZ    1024  /* 32 MB of cache per channel */

#define HMAP_DEBUG  0

static struct app_channel **ch;
extern uint16_t             app_nch;

struct hmap_pg_addr {
    union {
        struct {
            uint64_t addr  : 63;
            uint64_t flag : 1;
        } g;
        uint64_t addr;
    };
};

struct app_hmap *app_hmap_create (uint8_t id, uint64_t entries,
                                                 struct nvm_mmgr_geometry *geo)
{
    uint64_t lvl_entries, lvl_sz;
    uint16_t l;
    struct app_hmap *map;
    char name[16];

    sprintf (name, "OXAPP_HMAP-%d", id);
    if (!ox_mem_create_type (name, OX_MEM_APP_HMAP + id))
        return NULL;

    map = ox_malloc (sizeof (struct app_hmap), OX_MEM_APP_HMAP + id);
    if (!map)
        return NULL;

    map->id         = id;
    map->n_levels   = 0;
    map->geo        = geo;
    map->ent_per_pg = geo->pl_pg_size / sizeof (struct app_hmap_entry);

    /* Create n levels until the table size fits in a physical page */
    do {
        lvl_entries = (!map->n_levels) ? entries :
                                    lvl_entries / (uint64_t) map->ent_per_pg;
        lvl_sz      = lvl_entries * (uint64_t) sizeof (struct app_hmap_entry);

        l = map->n_levels;
        map->level[l].hmap      = map;
        map->level[l].level     = l;
        map->level[l].tbl_sz    = lvl_sz;
        map->level[l].n_entries = lvl_entries;
        map->level[l].ent_sz    = sizeof (struct app_hmap_entry);

        map->level[l].use_cache = (lvl_sz > HMAP_MAX_STATIC_SZ) ? 1 : 0;

        if (!map->level[l].use_cache) {
            map->level[l].buf = ox_calloc (1, lvl_sz, OX_MEM_APP_HMAP + id);
            if (!map->level[l].buf)
                goto FREE;

            map->level[l].static_pgs = lvl_sz / map->ent_per_pg;
            map->level[l].static_flag = ox_calloc (lvl_sz / map->ent_per_pg, 1,
                                                        OX_MEM_APP_HMAP + id);
            if (!map->level[l].static_flag) {
                ox_free (map->level[l].buf, OX_MEM_APP_HMAP + id);
                goto FREE;
            }
        }

        map->n_levels++;
    } while (lvl_sz > geo->pl_pg_size);

    if (HMAP_DEBUG) {
        for (int i = 0; i < map->n_levels; i++) {
            printf ("Level %d:\n", i);
            printf ("  Size:    %lu bytes\n", map->level[i].tbl_sz);
            printf ("  Entries: %lu\n", map->level[i].n_entries);
            printf ("  Ent sz:  %d bytes\n", map->level[i].ent_sz);
            printf ("  Cached:  %d\n", map->level[i].use_cache);
        }
    }

    return map;

FREE:
    while (map->n_levels) {
        map->n_levels--;
        if (!map->level[l].use_cache) {
            ox_free (map->level[l].static_flag, OX_MEM_APP_HMAP + id);
            ox_free (map->level[l].buf, OX_MEM_APP_HMAP + id);
        }
    }
    ox_free (map, OX_MEM_APP_HMAP + id);
    return NULL;
}

static int app_hmap_read (uint8_t *buf, struct app_hmap_entry *ent)
{ 
    uint32_t ent_per_pg;
    struct nvm_ppa_addr addr;
    struct nvm_io_data *io;
    int ret = -1;

    addr.ppa = ent->ppa;

    io = ftl_alloc_pg_io (ch[0]->ch);
    if (io == NULL)
        return -1;

    ent_per_pg = ch[0]->ch->geometry->pl_pg_size /
                                                sizeof (struct app_hmap_entry);

    ret = ftl_nvm_seq_transfer (io, &addr, buf, 1, ent_per_pg,
                            ent_per_pg, sizeof(struct app_hmap_entry),
                            NVM_TRANS_FROM_NVM, NVM_IO_NORMAL);
    if (ret)
        log_err("[hmap: NVM read failed. PPA 0x%016lx]", addr.ppa);

    ftl_free_pg_io(io);

    return ret;
}

/* Returns 0 if it fails and a PPA addr if it succeed */
static int app_hmap_write (uint8_t *buf, struct app_hmap_entry *ent)
{
    struct app_prov_ppas *prov_ppa;
    struct app_channel *lch;
    struct nvm_ppa_addr *addr;
    struct nvm_io_data *io;
    uint32_t ent_per_pg;
    int sec, ret = -1;

    /* Mapping table pages are mixed with user data */
    prov_ppa = oxapp()->gl_prov->new_fn (1, APP_LINE_USER);
    if (!prov_ppa) {
        log_err ("[cache: I/O error. No PPAs available.]");
        return -1;
    }

    if (prov_ppa->nppas != ch[0]->ch->geometry->sec_per_pl_pg)
        log_err ("[cache: NVM write. wrong PPAs. nppas %d]", prov_ppa->nppas);

    addr = &prov_ppa->ppa[0];
    lch = ch[addr->g.ch];
    io = ftl_alloc_pg_io(lch->ch);
    if (io == NULL)
        goto FREE_PPA;

    for (sec = 0; sec < io->ch->geometry->sec_per_pl_pg; sec++) {
        ((struct app_sec_oob *) io->oob_vec[sec])->lba = ent->lba;
        ((struct app_sec_oob *) io->oob_vec[sec])->pg_type = APP_PG_MAP;
    }

    ent_per_pg = ch[0]->ch->geometry->pl_pg_size /
                                                sizeof (struct app_hmap_entry);

    ret = ftl_nvm_seq_transfer (io, addr, buf, 1, ent_per_pg,
                            ent_per_pg, sizeof(struct app_hmap_entry),
                            NVM_TRANS_TO_NVM, NVM_IO_NORMAL);
    if (ret) {
        if (app_transaction_close_blk (addr, APP_LINE_USER))
            log_err ("[cache: Amend for failed write not completed.]\n");
        log_err("[cache: NVM write failed. PPA 0x%016lx]", addr->ppa);
        goto FREE_PG;
    }

    /* Set the written PPA */
    ent->ppa = addr->ppa;

FREE_PG:
    ftl_free_pg_io(io);
FREE_PPA:
    oxapp()->gl_prov->free_fn (prov_ppa);
    return ret;
}

static int hmap_build_level (struct app_hmap_level *parent,
                                                    struct app_hmap_level *lvl)
{
    uint32_t ent_i;
    void *offset;

    if (lvl->use_cache) {

        lvl->cache = app_cache_create (OX_MEM_APP_HMAP + lvl->hmap->id,
                    HMAP_CACHE_CH_SZ, ch[0]->ch->geometry->pl_pg_size,
                    app_nch, app_hmap_read, app_hmap_write);
        if (!lvl->cache) {
            log_err ("[hmap: Failed to creating cache %d]", lvl->level);
            return -1;
        }
        
        if (HMAP_DEBUG)
            printf ("[hmap: Created cache for lvl %d]\n", lvl->level);

    } else {

        for (ent_i = 0; ent_i < parent->n_entries; ent_i++) {
            offset = &lvl->buf[ent_i * lvl->hmap->ent_per_pg];

            if (parent->buf[ent_i].ppa != 0) {
                if (app_hmap_read (offset, &parent->buf[ent_i])) {
                    log_err ("[hmap: Failed to reading static page.]");
                    return -1;
                }
                if (HMAP_DEBUG)
                    printf ("[hmap: Lvl %d, read PPA %d -> %lx]\n", lvl->level,
                                                ent_i, parent->buf[ent_i].ppa);
            }

        }
        if (HMAP_DEBUG)
            printf ("[hmap: Loaded pages for lvl %d: %d]\n", lvl->level, ent_i);

    }

    return 0;
}

static void hmap_destroy_level (struct app_hmap_level *parent,
                                                    struct app_hmap_level *lvl)
{
    uint32_t pg_i;
    uint8_t *offset;

    if (lvl->use_cache) {

        app_cache_destroy (lvl->cache);

    } else {

        for (pg_i = 0; pg_i < lvl->static_pgs; pg_i++) {
            offset = (uint8_t *) &lvl->buf[pg_i * lvl->hmap->ent_per_pg];

            /* Check if static page is dirty */
            if (lvl->static_flag[pg_i]) {

                /* The up level is updated with new PPA by the write */
                if (app_hmap_write (offset, &parent->buf[pg_i])) {
                    log_err ("[hmap: Failed to flushing static page.]");
                    // RETRY ?
                }
                if (HMAP_DEBUG)
                    printf ("[hmap: Lvl %d, written PPA %d -> %lx]\n", lvl->level,
                                                   pg_i, parent->buf[pg_i].ppa);
            }
        }

    }
    log_info ("[hmap: Level destroyed: %d]", lvl->level);
}

inline static uint64_t app_hmap_lba_up (struct app_hmap *map, uint64_t lba) {
    return lba / map->ent_per_pg;
}

uint64_t app_hmap_get (struct app_hmap *map, uint64_t lba)
{
    uint32_t lid = 0;
    uint64_t up_lba = lba;
    struct app_hmap_entry *entry = NULL;

    if (lba > map->level[lid].n_entries - 1) {
        printf ("[hmap (get): LBA out of bounds -> %lu]\n", lba);
        return -1;
    }

    /* Find the first level in a static table */
    while ((!entry || !entry->ppa) && lid < map->n_levels) {

        if (!map->level[lid].use_cache)
            entry = &map->level[lid].buf[up_lba];

        if (entry && entry->ppa)
            break;

        up_lba = app_hmap_lba_up (map, up_lba);
        lid++;
    }

    if (!entry) {
        printf ("[hmap (get): Entry is NULL.]");
        return 0;
    }

    /* Return if this is already the bottom level or if ppa is zero */
    if (!lid || !entry->ppa)
        return entry->ppa;

    /* TODO: NOT implemented yet
             Check if the page is cached
             Loop the caching levels until finding the entry */

    return 0;
}

int app_hmap_upsert (struct app_hmap *map, uint64_t lba, uint64_t ppa)
{
    uint32_t lid;

    /* TODO: NOT implemented yet */

    /* Start the search from biggest level */
    for (lid = 0; lid < map->n_levels; lid++) {
        if (map->level[lid].use_cache) {
            // retrive from cache
        } else {
            // retrieve from static
        }
        //update entry
    }

    return -1;
}

int app_hmap_build (struct app_hmap *map, uint8_t *tiny)
{
    uint32_t lvl_i;
    uint8_t free = 0;
    struct app_hmap_level *lvl = &map->level[map->n_levels - 1];

    /* Build tiny table */
    if (!tiny) {
        tiny = ox_calloc (1, lvl->tbl_sz, OX_MEM_APP_HMAP + map->id);
        if (!tiny)
            return -1;
        free++;
    }
    memcpy (lvl->buf, tiny, lvl->tbl_sz);
    if (free)
        ox_free (tiny, OX_MEM_APP_HMAP + map->id);

    /* Build levels from smallest to bigger */
    for (lvl_i = map->n_levels - 1; lvl_i > 0; lvl_i--) {
        lvl = &map->level[lvl_i];
        if (hmap_build_level (lvl, lvl - 1))
            goto ERR;
    }

    return 0;

ERR:
    while (lvl_i < map->n_levels) {
        lvl_i++;
        hmap_destroy_level (&map->level[lvl_i], &map->level[lvl_i] - 1);
    }
    return -1;
}

void app_hmap_destroy (struct app_hmap *map)
{
    uint32_t lvl_i;

    /* Destroy levels from biggest to smallest */
    for (lvl_i = 0; lvl_i < map->n_levels; lvl_i++) {

        if (lvl_i < map->n_levels - 1)
            hmap_destroy_level (&map->level[lvl_i] + 1, &map->level[lvl_i]);

        if (!map->level[lvl_i].use_cache) {
            ox_free (map->level[lvl_i].static_flag, OX_MEM_APP_HMAP + map->id);
            ox_free (map->level[lvl_i].buf, OX_MEM_APP_HMAP + map->id);
        }
    }
    ox_free (map, OX_MEM_APP_HMAP + map->id);

    log_info ("[hmap: Hmap destroyed.]");
}

int app_hmap_init (void)
{
    uint32_t nch;

    ch = ox_malloc (sizeof (struct app_channel *) * app_nch, OX_MEM_OXAPP);
    if (!ch)
        return -1;

    nch = oxapp()->channels.get_list_fn (ch, app_nch);
    if (nch != app_nch)
        goto FREE;

    return 0;

FREE:
    ox_free (ch, OX_MEM_OXAPP);
    return -1;
}

void app_hmap_exit (void)
{
    ox_free (ch, OX_MEM_OXAPP);
}