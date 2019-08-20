/* OX: Open-Channel NVM Express SSD Controller
 * 
 *  - OX-App Flash Translation Layer (core)
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

#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/queue.h>
#include <ox-app.h>
#include <libox.h>
#include <ox-uatomic.h>

extern struct core_struct core;
struct app_global __oxapp;
static uint8_t gl_fn, tr_fn; /* Positive if function has been called */
uint16_t app_nch;

/* Write context mutexes */
pthread_mutex_t user_w_mutex;
pthread_mutex_t gc_w_mutex;

uint8_t oxapp_modset_block[APP_MOD_COUNT] = {0,0,0,0,0,0,0,0,0,0,0};

static int app_submit_io (struct nvm_io_cmd *);

struct app_global *oxapp (void) {
    return &__oxapp;
}

static void app_callback_io (struct nvm_mmgr_io_cmd *cmd)
{
    oxapp()->ppa_io->callback_fn (cmd);
}

static int app_submit_io (struct nvm_io_cmd *cmd)
{
    return oxapp()->lba_io->submit_fn (cmd);
}

static int app_init_channel (struct nvm_channel *ch)
{
    int ret;
    struct timespec ts;
    uint64_t start, end;

    if (!oxapp()->recovery->running) {
        if (oxapp()->recovery->init_fn ())
            return -1;

        GET_MICROSECONDS (start, ts);
        if (oxapp()->recovery->recover_fn ()) {
            oxapp()->recovery->exit_fn ();
            return -1;
        }
        GET_MICROSECONDS (end, ts);
        ox_stats_add_rec (OX_STATS_REC_CP_READ_US, end - start);
    }

    ret = oxapp()->channels.init_fn (ch, app_nch);
    if (ret) {
        if (oxapp()->recovery->running)
            oxapp()->recovery->exit_fn ();
        return ret;
    }

    app_nch++;

    return 0;
}

static int app_ftl_get_bbtbl (struct nvm_ppa_addr *ppa, uint8_t *bbtbl,
                                                                    uint32_t nb)
{
    struct app_channel *lch = oxapp()->channels.get_fn (ppa->g.ch);
    struct nvm_channel *ch = lch->ch;
    int l_addr = ppa->g.lun * ch->geometry->blk_per_lun *
                                                     ch->geometry->n_of_planes;

    if (!bbtbl || !ch || nb != ch->geometry->blk_per_lun *
                                        (ch->geometry->n_of_planes & 0xffff))
        return -1;

    memcpy(bbtbl, &lch->bbtbl->tbl[l_addr], nb);

    return 0;
}

static int app_ftl_set_bbtbl (struct nvm_ppa_addr *ppa, uint8_t value)
{
    int l_addr, n_pl, flush, ret;
    struct app_channel *lch = oxapp()->channels.get_fn (ppa->g.ch);

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
        ret = oxapp()->bbt->flush_fn(lch);
        if (ret)
            log_info("[ftl WARNING: Error flushing bad block table to NVM!]");
    }

    return 0;
}

static int app_write_mutex_init (void)
{
    if (pthread_mutex_init (&user_w_mutex, NULL))
        return -1;

    if (pthread_mutex_init (&gc_w_mutex, NULL)) {
        pthread_mutex_destroy (&user_w_mutex);
        return -1;
    }

    return 0;
}

static void app_write_mutex_destroy (void)
{
    pthread_mutex_destroy (&gc_w_mutex);
    pthread_mutex_destroy (&user_w_mutex);
}

static void app_exit (void)
{
    struct app_channel *lch[app_nch];
    int nch = app_nch, nth, retry, i;

    oxapp()->channels.get_list_fn (lch, nch);

    for (i = 0; i < nch; i++) {

        /* Check if channel is busy before exit */
        retry = 0;
        do {
            nth = app_ch_nthreads (lch[i]);
            if (nth) {
                usleep (5000);
                retry++;
            }
        } while (nth && retry < 200); /* Waiting max of 1 second */

        oxapp()->channels.exit_fn (lch[i]);
        app_nch--;
    }
}

static int app_global_init (void)
{
    uint32_t i;
    struct app_channel *lch[app_nch];
    struct timespec ts;
    uint64_t start, end;
    float overprov;

    if (!app_nch)
        return 0;

    oxapp()->channels.get_list_fn (lch, app_nch);

    if (app_write_mutex_init ())
        return -1;

    if (app_cache_init ()) {
        log_err ("[ox-app: Caching Framework not started.\n");
        goto WMUTEX;
    }

    if (app_hmap_init ()) {
        log_err ("[ox-app: hierarchical Mapping Framework not started.\n");
        goto CACHE;
    }

    if (oxapp()->gl_prov->init_fn ()) {
        log_err ("[ox-app: Global Provisioning NOT started.\n");
        goto HMAP;
    }

    /* Start Mapping before replaying the log, it might be used */
    GET_MICROSECONDS (start, ts);
    if (oxapp()->gl_map->init_fn ()) {
        log_err ("[ox-app: Global Mapping NOT started.\n");
        goto EXIT_LOG;
    }
    GET_MICROSECONDS (end, ts);
    ox_stats_add_rec (OX_STATS_REC_GL_MAP_US, end - start);

    /* Replay LOG and restart possible updated modules */
    GET_MICROSECONDS (start, ts);
    if (oxapp()->recovery->log_replay_fn ()) {
        log_err ("[ox-app: Recovery Log replay failed.\n");
        goto EXIT_GL_MAP;
    }
    GET_MICROSECONDS (end, ts);
    ox_stats_add_rec (OX_STATS_REC_REPLAY_US, end - start);

    for (i = 0; i < app_nch; i++) {
        /* Complete PUT blocks */
        oxapp()->ch_prov->put_blk_fn (lch[i], 0xffff, 0xffff);

        /* Restart channel provisioning */
        oxapp()->ch_prov->exit_fn (lch[i]);
        if (oxapp()->ch_prov->init_fn (lch[i]))
            log_err ("[ox-app: Ch %d Prov NOT restarted after log replay.\n", i);
    }

    /* Log is started after replaying the log. If gl_map evicts a page before,
     *                                                    it won't be logged */
    if (oxapp()->log->init_fn ()) {
        log_err ("[ox-app: Log Management NOT started.\n");
        goto EXIT_GL_PROV;
    }

    /* Create a checkpoint for a clean startup */
    GET_MICROSECONDS (start, ts);
    if (oxapp()->recovery->checkpoint_fn ())
        log_err ("[ox-app: Clean startup checkpoint has failed]");
    GET_MICROSECONDS (end, ts);
    ox_stats_add_rec (OX_STATS_REC_CP_WRITE_US, end - start);

    if (oxapp()->lba_io->init_fn ()) {
        log_err ("[ox-app: LBA I/O NOT started.\n");
        goto EXIT_GL_MAP;
    }

    if (oxapp()->gc->init_fn ()) {
        log_err ("[ox-app: GC NOT started.\n");
        goto EXIT_LBA_IO;
    }

    /* Limit the global namespace size for overprov space */
    overprov = APP_GC_OVERPROV;

    /* In case of tiny devices (memory backend) */
    if (core.nvm_ns_size / 1024 / 1024 / 1024 < 15)
        overprov = 0.5;
    if (core.nvm_ns_size / 1024 / 1024 / 1024 < 7)
        overprov = 0.75;

    core.nvm_ns_size -= core.nvm_ns_size * overprov;
    core.nvm_ns_size -= core.nvm_ns_size % lch[0]->ch->geometry->pl_pg_size;

    return 0;

EXIT_LBA_IO:
    oxapp()->lba_io->exit_fn ();
EXIT_GL_MAP:
    oxapp()->gl_map->exit_fn ();
EXIT_LOG:
    oxapp()->log->exit_fn ();
EXIT_GL_PROV:
    oxapp()->gl_prov->exit_fn ();
HMAP:
    app_hmap_exit ();
CACHE:
    app_cache_exit ();
WMUTEX:
    app_write_mutex_destroy ();
    return -1;
}

static void app_global_exit (void)
{
    /* Create a clean shutdown checkpoint */
    if (oxapp()->recovery->running) {
        if (oxapp()->recovery->checkpoint_fn ())
            log_err ("[ox-app: Checkpoint has failed]");

        oxapp()->recovery->exit_fn ();
    }

    oxapp()->gc->exit_fn ();
    oxapp()->lba_io->exit_fn ();
    oxapp()->gl_map->exit_fn ();
    oxapp()->log->flush_fn (0, NULL, NULL);
    oxapp()->log->exit_fn ();
    oxapp()->gl_prov->exit_fn ();
    app_hmap_exit ();
    app_cache_exit ();
    app_write_mutex_destroy ();
}

static int app_init_fn (uint16_t fn_id, void *arg)
{
    switch (fn_id) {
        case APP_FN_GLOBAL:
            gl_fn = 1;
            return app_global_init();
            break;
        case APP_FN_TRANSACTION:
            tr_fn = 1;
            return app_transaction_init();
            break;
        default:
            log_info ("[ox-app (init_fn): Function not found. id %d\n", fn_id);
            return -1;
    }
}

static void app_exit_fn (uint16_t fn_id)
{
    switch (fn_id) {
        case APP_FN_GLOBAL:
            return (gl_fn) ? app_global_exit() : 0;
            break;
        case APP_FN_TRANSACTION:
            return (tr_fn) ? app_transaction_exit() : 0;
            break;
        default:
            log_info ("[ox-app (exit_fn): Function not found. id %d\n", fn_id);
    }
}

int app_mod_set (uint8_t *modset)
{
    int mod_i;
    void *mod;

    for (mod_i = 0; mod_i < APP_MOD_COUNT; mod_i++)
        if (modset[mod_i] >= APP_FN_SLOTS)
            return -1;

    for (mod_i = 0; mod_i < APP_MOD_COUNT; mod_i++) {

        /* Set pointer if module ID is positive */
        if (modset[mod_i]) {
            mod = oxapp()->mod_list[mod_i][modset[mod_i]];
            if (!mod)
                continue;

            switch (mod_i) {
                case APPMOD_BBT:
                    oxapp()->bbt = (struct app_global_bbt *) mod;
                    break;
                case APPMOD_BLK_MD:
                    oxapp()->md = (struct app_global_md *) mod;
                    break;
                case APPMOD_CH_PROV:
                    oxapp()->ch_prov = (struct app_ch_prov *) mod;
                    break;
                case APPMOD_GL_PROV:
                    oxapp()->gl_prov = (struct app_gl_prov *) mod;
                    break;
                case APPMOD_CH_MAP:
                    oxapp()->ch_map = (struct app_ch_map *) mod;
                    break;
                case APPMOD_GL_MAP:
                    oxapp()->gl_map = (struct app_gl_map *) mod;
                    break;
                case APPMOD_PPA_IO:
                    oxapp()->ppa_io = (struct app_ppa_io *) mod;
                    break;
                case APPMOD_LBA_IO:
                    oxapp()->lba_io = (struct app_lba_io *) mod;
                    break;
                case APPMOD_GC:
                    oxapp()->gc = (struct app_gc *) mod;
                    break;
                case APPMOD_LOG:
                    oxapp()->log = (struct app_log *) mod;
                    break;
                case APPMOD_RECOVERY:
                    oxapp()->recovery = (struct app_recovery *) mod;
                    break;
            }

            log_info ("  [ox-app: Module set. "
                    "type: %d, id: %d, ptr: %p\n", mod_i, modset[mod_i], mod);
        }
    }
    return 0;
}

int app_mod_register (uint8_t modtype, uint8_t modid, void *mod)
{
    if (modid >= APP_FN_SLOTS || modtype >= APP_MOD_COUNT || !mod) {
        log_err ("[ox-app (mod_register): Module NOT registered. "
                           "type: %d, id: %d, ptr: %p\n", modtype, modid, mod);
        return -1;
    }

    oxapp()->mod_list[modtype][modid] = mod;
    oxapp_modset_block[modtype] = modid;

    log_info ("  [ox-app: Module registered. "
                           "type: %d, id: %d, ptr: %p\n", modtype, modid, mod);

    return 0;
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

struct nvm_ftl oxapp_ftl = {
    .ftl_id         = FTL_ID_OXAPP,
    .name           = "OX-APP",
    .nq             = 8,
    .ops            = &app_ops,
    .cap            = ZERO_32FLAG,
};

int ftl_oxapp_init (void)
{
    if (!ox_mem_create_type ("FTL_OXAPP", OX_MEM_OXAPP) ||
        !ox_mem_create_type ("OXBLK_CH_PROV", OX_MEM_OXBLK_CPR))
        return -1;

    gl_fn = 0;
    tr_fn = 0;
    app_nch = 0;

    app_channels_register ();

    if (app_mod_set (oxapp_modset_block))
        return -1;

    oxapp_ftl.cap |= 1 << FTL_CAP_GET_BBTBL;
    oxapp_ftl.cap |= 1 << FTL_CAP_SET_BBTBL;
    oxapp_ftl.cap |= 1 << FTL_CAP_INIT_FN;
    oxapp_ftl.cap |= 1 << FTL_CAP_EXIT_FN;
    oxapp_ftl.bbtbl_format = FTL_BBTBL_BYTE;

    return ox_register_ftl(&oxapp_ftl);
}