/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Flash Translation Layer (core)
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

#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/queue.h>
#include "../../include/ssd.h"
#include "../../include/uatomic.h"
#include "appnvm.h"

extern struct core_struct core;
struct app_global __appnvm;
static uint8_t gl_fn; /* Positive if global function has been called */
uint16_t app_nch;

pthread_mutex_t     gc_ns_mutex;
pthread_spinlock_t *md_ch_spin;

static int app_submit_io (struct nvm_io_cmd *);

struct app_global *appnvm (void) {
    return &__appnvm;
}

void app_pg_io_prepare (struct app_channel *lch, struct app_io_data *data)
{
    uint16_t sec, pl;
    struct nvm_mmgr_geometry *g = lch->ch->geometry;

    for (pl = 0; pl < data->n_pl; pl++) {
        data->pl_vec[pl] = data->buf +
                      ((uint32_t) pl * (data->buf_sz / (uint32_t) data->n_pl));

        for (sec = 0; sec < g->sec_per_pg; sec++) {
            data->oob_vec[g->sec_per_pg * pl + sec] =
                        data->pl_vec[pl] + data->pg_sz + (g->sec_oob_sz * sec);
            data->sec_vec[pl][sec] = data->pl_vec[pl] + (g->sec_size * sec);
        }

        data->sec_vec[pl][g->sec_per_pg] = data->oob_vec[g->sec_per_pg * pl];
    }
}

struct app_io_data *app_alloc_pg_io (struct app_channel *lch)
{
    struct app_io_data *data;
    struct nvm_mmgr_geometry *g;
    uint16_t pl;

    data = malloc (sizeof (struct app_io_data));
    if (!data)
        return NULL;

    data->lch = lch;
    data->ch = lch->ch;
    g = data->ch->geometry;
    data->n_pl = g->n_of_planes;
    data->pg_sz = g->pg_size;
    data->meta_sz = g->sec_oob_sz * g->sec_per_pg;
    data->buf_sz = (data->pg_sz + data->meta_sz) * data->n_pl;

    data->buf = calloc(data->buf_sz, 1);
    if (!data->buf)
        goto FREE;

    data->pl_vec = malloc (sizeof (uint8_t *) * data->n_pl);
    if (!data->pl_vec)
        goto FREE_BUF;

    data->oob_vec = malloc (sizeof (uint8_t *) * g->sec_per_pl_pg);
    if (!data->oob_vec)
        goto FREE_VEC;

    data->sec_vec = malloc (sizeof (void *) * data->n_pl);
    if (!data->sec_vec)
        goto FREE_OOB;

    for (pl = 0; pl < data->n_pl; pl++) {
        data->sec_vec[pl] = malloc (sizeof (uint8_t *) * (g->sec_per_pg + 1));
        if (!data->sec_vec[pl])
            goto FREE_SEC;
    }

    data->mod_oob = malloc (data->meta_sz * data->n_pl);
    if (!data->mod_oob)
        goto FREE_SEC;

    app_pg_io_prepare (lch, data);

    return data;

FREE_SEC:
    while (pl) {
        pl--;
        free (data->sec_vec[pl]);
    }
    free (data->sec_vec);
FREE_OOB:
    free (data->oob_vec);
FREE_VEC:
    free (data->pl_vec);
FREE_BUF:
    free (data->buf);
FREE:
    free (data);
    return NULL;
}

void app_free_pg_io (struct app_io_data *data)
{
    uint32_t pl;

    for (pl = 0; pl < data->n_pl; pl++)
        free (data->sec_vec[pl]);

    free (data->sec_vec);
    free (data->mod_oob);
    free (data->oob_vec);
    free (data->pl_vec);
    free (data->buf);
    free (data);
}

int app_blk_current_page (struct app_channel *lch, struct app_io_data *io,
                                              uint16_t blk_id, uint16_t offset)
{
    int pg, ret, fr = 0;
    struct app_magic oob;

    if (io == NULL) {
        io = app_alloc_pg_io(lch);
        if (io == NULL)
            return -1;
        fr++;
    }

    /* Finds the location of the newest data by checking APP_MAGIC */
    pg = 0;
    do {
        memset (io->buf, 0, io->buf_sz);
        ret = app_io_rsv_blk (lch, MMGR_READ_PG,
                                            (void **) io->pl_vec, blk_id, pg);

        /* get info from OOB area in plane 0 */
        memcpy(&oob, io->buf + io->pg_sz, sizeof(struct app_magic));

        if (ret || oob.magic != APP_MAGIC)
            break;

        pg += offset;
    } while (pg < io->ch->geometry->pg_per_blk - offset);

    if (fr)
        app_free_pg_io (io);

    return (!ret) ? pg : -1;
}

static inline int app_pg_io_switch (struct app_channel *lch, uint8_t cmdtype,
                    void **pl_vec, struct nvm_ppa_addr *ppa, uint8_t type)
{
    switch (type) {
        case APP_IO_NORMAL:
            return app_pg_io (lch, cmdtype, pl_vec, ppa);
        case APP_IO_RESERVED:
            return app_io_rsv_blk(lch, cmdtype, pl_vec, ppa->g.blk, ppa->g.pg);
        default:
            return -1;
    }
}

/**
 *  Transfers a table of user specific entries to/from a NVM unique block
 *  This function mount/unmount the multi-plane I/O buffers to a flat table
 *
 *  The table can cross multiple pages in the block
 *  For now, the maximum table size is the flash block
 *
 * @param io - struct created by alloc_alloc_pg_io
 * @param ppa - lun, block and first page number
 * @param user_buf - table buffer to be transfered
 * @param pgs - number of flash pages the table crosses (multi-plane pages)
 * @param ent_per_pg - number of entries per flash page (multi-plane pages)
 * @param ent_left - number of entries to be transfered
 * @param entry_sz - size of an entry
 * @param direction - APP_TRANS_FROM_NVM or APP_TRANS_TO_NVM
 * @param reserved - APP_IO_NORMAL or APP_IO_RESERVED
 * @return 0 on success, -1 on failure
 */
int app_nvm_seq_transfer (struct app_io_data *io, struct nvm_ppa_addr *ppa,
        uint8_t *user_buf, uint16_t pgs, uint16_t ent_per_pg, uint32_t ent_left,
        size_t entry_sz, uint8_t direction, uint8_t reserved)
{
    uint32_t i, pl, start_pg;
    size_t trf_sz, pg_ent_sz;
    uint8_t *from, *to;
    struct nvm_ppa_addr ppa_io;

    pg_ent_sz = ent_per_pg * entry_sz;
    memcpy (&ppa_io, ppa, sizeof(struct nvm_ppa_addr));
    start_pg = ppa_io.g.pg;

    /* Transfer page by page from/to NVM */
    for (i = 0; i < pgs; i++) {
        ppa_io.g.pg = start_pg + i;
        if (direction == APP_TRANS_FROM_NVM)
            if (app_pg_io_switch (io->lch, MMGR_READ_PG, (void **) io->pl_vec,
                                                            &ppa_io, reserved))
                return -1;

        /* Copy page entries from/to I/O buffer */
        for (pl = 0; pl < io->n_pl; pl++) {

            trf_sz = (ent_left >= ent_per_pg / io->n_pl) ?
                    (ent_per_pg / io->n_pl) * entry_sz : ent_left * entry_sz;

            from = (direction == APP_TRANS_TO_NVM) ?
                user_buf + (pg_ent_sz * i) + (pl * (pg_ent_sz / io->n_pl)) :
                io->pl_vec[pl];

            to = (direction == APP_TRANS_TO_NVM) ?
                io->pl_vec[pl] :
                user_buf + (pg_ent_sz * i) + (pl * (pg_ent_sz / io->n_pl));

            memcpy(to, from, trf_sz);

            ent_left = (ent_left >= ent_per_pg / io->n_pl) ?
                ent_left - (ent_per_pg / io->n_pl) : 0;

            if (!ent_left)
                break;
        }

        if (direction == APP_TRANS_TO_NVM)
            if (app_pg_io_switch (io->lch, MMGR_WRITE_PG, (void **) io->pl_vec,
                                                            &ppa_io, reserved))
                return -1;
    }

    return 0;
}

int app_pg_io (struct app_channel *lch, uint8_t cmdtype,
                                      void **pl_vec, struct nvm_ppa_addr *ppa)
{
    int pl, ret = -1;
    void *buf = NULL;
    struct nvm_channel *ch = lch->ch;
    struct nvm_mmgr_io_cmd *cmd = malloc(sizeof(struct nvm_mmgr_io_cmd));
    if (!cmd)
        return EMEM;

    for (pl = 0; pl < ch->geometry->n_of_planes; pl++) {
        memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
        cmd->ppa.g.blk = ppa->g.blk;
        cmd->ppa.g.pl = pl;
        cmd->ppa.g.ch = ch->ch_mmgr_id;
        cmd->ppa.g.lun = ppa->g.lun;
        cmd->ppa.g.pg = ppa->g.pg;

        if (cmdtype != MMGR_ERASE_BLK)
            buf = pl_vec[pl];

        ret = nvm_submit_sync_io (ch, cmd, buf, cmdtype);
        if (ret)
            break;
    }
    free(cmd);

    return ret;
}

int app_io_rsv_blk (struct app_channel *lch, uint8_t cmdtype,
                                     void **pl_vec, uint16_t blk, uint16_t pg)
{
    int pl, ret = -1;
    void *buf = NULL;
    struct nvm_channel *ch = lch->ch;
    struct nvm_mmgr_io_cmd *cmd = malloc(sizeof(struct nvm_mmgr_io_cmd));
    if (!cmd)
        return EMEM;

    for (pl = 0; pl < ch->geometry->n_of_planes; pl++) {
        memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
        cmd->ppa.g.blk = blk;
        cmd->ppa.g.pl = pl;
        cmd->ppa.g.ch = ch->ch_mmgr_id;

        /* TODO: RAID 1 among all LUNs in the channel */
        cmd->ppa.g.lun = 0;

        cmd->ppa.g.pg = pg;

        if (cmdtype != MMGR_ERASE_BLK)
            buf = pl_vec[pl];

        ret = nvm_submit_sync_io (ch, cmd, buf, cmdtype);
        if (ret)
            break;
    }
    free(cmd);

    return ret;
}

static void app_callback_io (struct nvm_mmgr_io_cmd *cmd)
{
    appnvm()->ppa_io->callback_fn (cmd);
}

static int app_submit_io (struct nvm_io_cmd *cmd)
{
    return appnvm()->lba_io->submit_fn (cmd);
}

static int app_init_channel (struct nvm_channel *ch)
{
    int ret;

    ret = appnvm()->channels.init_fn (ch, app_nch);
    if (ret)
        return ret;

    app_nch++;

    return 0;
}

static int app_ftl_get_bbtbl (struct nvm_ppa_addr *ppa, uint8_t *bbtbl,
                                                                    uint32_t nb)
{
    struct app_channel *lch = appnvm()->channels.get_fn (ppa->g.ch);
    struct nvm_channel *ch = lch->ch;
    int l_addr = ppa->g.lun * ch->geometry->blk_per_lun *
                                                     ch->geometry->n_of_planes;

    if (nvm_memcheck(bbtbl) || nvm_memcheck(ch) ||
                                        nb != ch->geometry->blk_per_lun *
                                        (ch->geometry->n_of_planes & 0xffff))
        return -1;

    memcpy(bbtbl, &lch->bbtbl->tbl[l_addr], nb);

    return 0;
}

static int app_ftl_set_bbtbl (struct nvm_ppa_addr *ppa, uint8_t value)
{
    int l_addr, n_pl, flush, ret;
    struct app_channel *lch = appnvm()->channels.get_fn (ppa->g.ch);

    n_pl = lch->ch->geometry->n_of_planes;

    if ((ppa->g.blk * n_pl + ppa->g.pl) >
                                   (lch->ch->geometry->blk_per_lun * n_pl - 1))
        return -1;

    l_addr = ppa->g.lun * lch->ch->geometry->blk_per_lun * n_pl;

    /* flush the table if the value changes */
    flush = (lch->bbtbl->tbl[l_addr+(ppa->g.blk * n_pl + ppa->g.pl)] == value)
                                                                        ? 0 : 1;
    lch->bbtbl->tbl[l_addr + (ppa->g.blk * n_pl + ppa->g.pl)] = value;

    if (flush) {
        ret = appnvm()->bbt->flush_fn(lch);
        if (ret)
            log_info("[ftl WARNING: Error flushing bad block table to NVM!]");
    }

    return 0;
}

static void app_exit (void)
{
    struct app_channel *lch[app_nch];
    int nch = app_nch, nth, retry, i;

    appnvm()->channels.get_list_fn (lch, nch);

    for (i = 0; i < nch; i++) {

        /* Check if channel is busy before exit */
        retry = 0;
        do {
            nth = appnvm_ch_nthreads (lch[i]);
            if (nth) {
                usleep (5000);
                retry++;
            }
        } while (nth && retry < 200); /* Waiting max of 1 second */

        appnvm()->channels.exit_fn (lch[i]);
        app_nch--;
    }
}

static int app_global_init (void)
{
    if (!app_nch)
        return 0;

    if (appnvm()->gl_prov->init_fn ()) {
        log_err ("[appnvm: Global Provisioning NOT started.\n");
        return -1;
    }

    if (appnvm()->gl_map->init_fn ()) {
        log_err ("[appnvm: Global Mapping NOT started.\n");
        goto EXIT_GL_PROV;
    }

    if (appnvm()->lba_io->init_fn ()) {
        log_err ("[appnvm: LBA I/O NOT started.\n");
        goto EXIT_GL_MAP;
    }

    if (pthread_mutex_init (&gc_ns_mutex, NULL))
        goto EXIT_LBA_IO;

    if (appnvm()->gc->init_fn ()) {
        log_err ("[appnvm: GC NOT started.\n");
        goto NS_MUTEX;
    }

    /* Limit the global namespace size for overprov space */
    core.nvm_ns_size -= core.nvm_ns_size * APPNVM_GC_OVERPROV;

    return 0;

NS_MUTEX:
    pthread_mutex_destroy (&gc_ns_mutex);
EXIT_LBA_IO:
    appnvm()->lba_io->exit_fn ();
EXIT_GL_MAP:
    appnvm()->gl_map->exit_fn ();
EXIT_GL_PROV:
    appnvm()->gl_prov->exit_fn ();
    return -1;
}

static void app_global_exit (void)
{
    appnvm()->gc->exit_fn ();
    pthread_mutex_destroy (&gc_ns_mutex);
    appnvm()->lba_io->exit_fn ();
    appnvm()->gl_map->exit_fn ();
    appnvm()->gl_prov->exit_fn ();

    if (md_ch_spin)
        free ((void *)md_ch_spin);
}

static int app_init_fn (uint16_t fn_id, void *arg)
{
    switch (fn_id) {
        case APP_FN_GLOBAL:
            gl_fn = 1;
            return app_global_init();
            break;
        default:
            log_info ("[appnvm (init_fn): Function not found. id %d\n", fn_id);
            return -1;
    }
}

static void app_exit_fn (uint16_t fn_id)
{
    switch (fn_id) {
        case APP_FN_GLOBAL:
            return (gl_fn) ? app_global_exit() : 0;
            break;
        default:
            log_info ("[appnvm (exit_fn): Function not found. id %d\n", fn_id);
    }
}

int appnvm_mod_set (uint8_t *modset)
{
    int mod_i;
    void *mod;

    for (mod_i = 0; mod_i < APPNVM_MOD_COUNT; mod_i++)
        if (modset[mod_i] >= APPNVM_FN_SLOTS)
            return -1;

    for (mod_i = 0; mod_i < APPNVM_MOD_COUNT; mod_i++) {

        /* Set pointer if module ID is positive */
        if (modset[mod_i]) {
            mod = appnvm()->mod_list[mod_i][modset[mod_i]];
            if (!mod)
                continue;

            switch (mod_i) {
                case APPMOD_BBT:
                    appnvm()->bbt = (struct app_global_bbt *) mod;
                    break;
                case APPMOD_BLK_MD:
                    appnvm()->md = (struct app_global_md *) mod;
                    break;
                case APPMOD_CH_PROV:
                    appnvm()->ch_prov = (struct app_ch_prov *) mod;
                    break;
                case APPMOD_GL_PROV:
                    appnvm()->gl_prov = (struct app_gl_prov *) mod;
                    break;
                case APPMOD_CH_MAP:
                    appnvm()->ch_map = (struct app_ch_map *) mod;
                    break;
                case APPMOD_GL_MAP:
                    appnvm()->gl_map = (struct app_gl_map *) mod;
                    break;
                case APPMOD_PPA_IO:
                    appnvm()->ppa_io = (struct app_ppa_io *) mod;
                    break;
                case APPMOD_LBA_IO:
                    appnvm()->lba_io = (struct app_lba_io *) mod;
                    break;
                case APPMOD_GC:
                    appnvm()->gc = (struct app_gc *) mod;
                    break;
            }

            log_info ("  [appnvm: Module set. "
                    "type: %d, id: %d, ptr: %p\n", mod_i, modset[mod_i], mod);
        }
    }
    return 0;
}

int appnvm_mod_register (uint8_t modtype, uint8_t modid, void *mod)
{
    if (modid >= APPNVM_FN_SLOTS || modtype >= APPNVM_MOD_COUNT || !mod) {
        log_err ("[appnvm (mod_register): Module NOT registered. "
                           "type: %d, id: %d, ptr: %p\n", modtype, modid, mod);
        return -1;
    }

    appnvm()->mod_list[modtype][modid] = mod;

    log_info ("  [appnvm: Module registered. "
                           "type: %d, id: %d, ptr: %p\n", modtype, modid, mod);

    return 0;
}

static void ftl_appnvm_mod_probe (void)
{
    channels_register ();

    /* Bad block table modules */
    bbt_byte_register ();
    /* Block meta-data modules */
    blk_md_register ();
    /* Channel provisioning modules */
    ch_prov_register ();
    /* Global provisioning modules */
    gl_prov_register ();
    /* Channel mapping modules */
    ch_map_register ();
    /* Global mapping modules */
    gl_map_register ();
    /* Back-end PPA I/O modules */
    ppa_io_register ();
    /* Front-end LBA I/O modules */
    lba_io_register ();
    /* Garbage collection modules */
    gc_register ();
}

struct nvm_ftl_ops app_ops = {
    .init_ch     = app_init_channel,
    .submit_io   = app_submit_io,
    .callback_io = app_callback_io,
    .exit        = app_exit,
    .get_bbtbl   = app_ftl_get_bbtbl,
    .set_bbtbl   = app_ftl_set_bbtbl,
    .init_fn     = app_init_fn,
    .exit_fn     = app_exit_fn
};

struct nvm_ftl app_ftl = {
    .ftl_id         = FTL_ID_APPNVM,
    .name           = "APPNVM",
    .nq             = 8,
    .ops            = &app_ops,
    .cap            = ZERO_32FLAG,
};

int ftl_appnvm_init (void)
{
    /* This array should come from makefile or user defined configuration */
    uint8_t modset_appftl[APPNVM_MOD_COUNT] = {
        APPFTL_BBT,
        APPFTL_BLK_MD,
        APPFTL_CH_PROV,
        APPFTL_GL_PROV,
        APPFTL_CH_MAP,
        APPFTL_GL_MAP,
        APPFTL_PPA_IO,
        APPFTL_LBA_IO,
        APPFTL_GC
    };

    gl_fn = 0;
    app_nch = 0;
    md_ch_spin = NULL;

    memset (appnvm()->mod_list, 0x0, sizeof (void *) *
                                           APPNVM_FN_SLOTS * APPNVM_MOD_COUNT);

    ftl_appnvm_mod_probe ();
    if (appnvm_mod_set (modset_appftl))
        return -1;

    app_ftl.cap |= 1 << FTL_CAP_GET_BBTBL;
    app_ftl.cap |= 1 << FTL_CAP_SET_BBTBL;
    app_ftl.cap |= 1 << FTL_CAP_INIT_FN;
    app_ftl.cap |= 1 << FTL_CAP_EXIT_FN;
    app_ftl.bbtbl_format = FTL_BBTBL_BYTE;

    return nvm_register_ftl(&app_ftl);
}