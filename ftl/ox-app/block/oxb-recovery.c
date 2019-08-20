/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX-App Checkpoint and Recovery
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

#ifndef __USE_GNU
#define __USE_GNU
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/queue.h>
#include <search.h>
#include <ox-app.h>

#define APP_OXB_REC_DEBUG_LOG     0
#define APP_OXB_REC_DEBUG_LOG_W   0

extern uint16_t app_nch;
extern struct core_struct core;

struct oxb_rec_log {
    struct app_log_entry        log;
    TAILQ_ENTRY (oxb_rec_log)   entry;
};

struct oxb_rec_transaction {
    uint64_t    tid;
    TAILQ_HEAD (trans_list, oxb_rec_log) log_head;
};

static uint16_t cur_flush_pg;

static pthread_t        cp_thread;
static pthread_mutex_t  cp_mutex;
static uint8_t          first_cp;

LIST_HEAD (app_cp_list, app_rec_entry) cp_list;

/* RB tree */
static void *tr_root = NULL;

static uint64_t tot_logs;

static int oxb_recovery_btree_compare(const void *new_ptr, const void *tree_ptr)
{
    struct oxb_rec_transaction *tree_node = (struct oxb_rec_transaction *) tree_ptr;
    struct oxb_rec_transaction *new_node = (struct oxb_rec_transaction *) new_ptr;

    if ( new_node->tid < tree_node->tid )
        return -1;
    if ( new_node->tid > tree_node->tid )
        return 1;
    return 0;
}

static void oxb_recovery_btree_free_act (void *nodep)
{
    struct oxb_rec_transaction *tr = (struct oxb_rec_transaction *) nodep;
    struct oxb_rec_log *log;
    uint32_t log_i = 0;

    while (!TAILQ_EMPTY (&tr->log_head)) {
        log = TAILQ_FIRST (&tr->log_head);
        if (!log)
            break;
        TAILQ_REMOVE (&tr->log_head, log, entry);
        ox_free (log, OX_MEM_OXBLK_REC);
        log_i++;
    }

    if (APP_DEBUG_RECOVERY)
        printf ("[recovery: Aborted transaction: %lu, logs: %d]\n",
                                                                tr->tid, log_i);
    ox_stats_add_rec (OX_STATS_REC_TR_ABORT, 1);

    ox_free (tr, OX_MEM_OXBLK_REC);
}

static int oxb_recovery_btree_add_log (struct app_log_entry *log)
{
    struct oxb_rec_transaction **ret;
    struct oxb_rec_transaction *tr;
    struct oxb_rec_log *rec_log;

    ret = (struct oxb_rec_transaction **) tfind(&log->write.tid, &tr_root,
                                                    oxb_recovery_btree_compare);
    if (!ret) {
        tr = ox_malloc (sizeof (struct oxb_rec_transaction), OX_MEM_OXBLK_REC);
        if (!tr)
            return -1;

        tr->tid = log->write.tid;
        TAILQ_INIT (&tr->log_head);
        tsearch((void *) tr, &tr_root, oxb_recovery_btree_compare);
    } else {
        tr = *ret;
    }

    rec_log = ox_malloc (sizeof (struct oxb_rec_log), OX_MEM_OXBLK_REC);
    if (!rec_log)
        return -1;

    memcpy (&rec_log->log, log, sizeof (struct app_log_entry));
    TAILQ_INSERT_TAIL (&tr->log_head, rec_log, entry);

    return 0;
}

static void oxb_recovery_apply_fix_blk (struct app_channel *lch, uint64_t ppa)
{
    struct app_blk_md_entry *blk;
    struct nvm_ppa_addr *addr = (struct nvm_ppa_addr *) &ppa;

    blk = oxapp()->md->get_fn (lch, addr->g.lun);
    if (!blk)
        return;

    /* Rollback to log page due aborted write */
    if (addr->g.pg < lch->ch->geometry->pg_per_blk - 1 &&
                                (blk[addr->g.blk].flags & APP_BLK_MD_OPEN)) {
        blk[addr->g.blk].current_pg = addr->g.pg;
    }
}

static void oxb_recovery_apply_write (struct app_channel **lch_list,
                    struct app_log_entry *log, uint8_t log_pg, uint8_t inv)
{  
    struct app_blk_md_entry *blk;
    struct nvm_ppa_addr *old_addr = (struct nvm_ppa_addr *) &log->write.old_ppa;
    struct nvm_ppa_addr *addr = (struct nvm_ppa_addr *) &log->write.new_ppa;
    struct app_channel *lch = lch_list[addr->g.ch];
    uint32_t blk_i;

    blk = oxapp()->md->get_fn (lch, addr->g.lun);
    if (!blk)
        return;

    if (APP_OXB_REC_DEBUG_LOG_W)
        printf ("     BLK (%d/%d/%d/%d - %d)\n", addr->g.ch, addr->g.lun,
                        addr->g.blk, addr->g.pg, blk[addr->g.blk].current_pg);

    if (addr->g.pg == 0 && !(blk[addr->g.blk].flags & APP_BLK_MD_OPEN)) {

        /* Open block */
        blk[addr->g.blk].current_pg = 1;
        blk[addr->g.blk].invalid_sec = 0;
        blk[addr->g.blk].erase_count++;
        blk[addr->g.blk].flags |= (APP_BLK_MD_USED | APP_BLK_MD_OPEN);
        if (blk[addr->g.blk].flags & APP_BLK_MD_LINE)
            blk[addr->g.blk].flags ^= APP_BLK_MD_LINE;
        if (blk[addr->g.blk].flags & APP_BLK_MD_META)
            blk[addr->g.blk].flags ^= APP_BLK_MD_META;
        if (blk[addr->g.blk].flags & APP_BLK_MD_COLD)
            blk[addr->g.blk].flags ^= APP_BLK_MD_COLD;
        
        memset (blk[addr->g.blk].pg_state, 0x0, BLK_MD_BYTE_VEC_SZ);

    } else if ((addr->g.pg == lch->ch->geometry->pg_per_blk - 1) &&
                                (blk[addr->g.blk].flags & APP_BLK_MD_OPEN)) {

        /* Close block */
        if (blk[addr->g.blk].flags & APP_BLK_MD_LINE)
            blk[addr->g.blk].flags ^= APP_BLK_MD_LINE;
        if (blk[addr->g.blk].flags & APP_BLK_MD_OPEN)
            blk[addr->g.blk].flags ^= APP_BLK_MD_OPEN;

    } else if (log_pg) {
        blk[addr->g.blk].current_pg = addr->g.pg + 1;
    } else {
        /* Update block */
        if (addr->g.pg >= blk[addr->g.blk].current_pg)
            blk[addr->g.blk].current_pg++;
    }

    blk_i = (addr->g.lun * lch->ch->geometry->blk_per_lun) + addr->g.blk;
    oxapp()->md->mark_fn (lch, blk_i / lch->blk_md->ent_per_pg);

    /* Invalidate old PPA */
    if (inv && old_addr->ppa) {
        lch = lch_list[old_addr->g.ch];
        oxapp()->md->invalidate_fn (lch, old_addr, log_pg);
    }
}

static int oxb_recovery_should_drop (struct app_log_entry *log, uint32_t i)
{
    uint8_t byte_i, drop = 0;

    for (byte_i = 0; byte_i < 48; byte_i++) {
        if (!log->byte[byte_i])
            break;
        if ((uint64_t) log->byte[byte_i] == i) {
            drop++;
            break;
        }
    }

    if (APP_OXB_REC_DEBUG_LOG_W && drop)
        printf ("[recovery: Log dropped. TID: %lu, index: %d]\n",
                                                            log->write.tid, i);

    if (drop)
        ox_stats_add_rec (OX_STATS_REC_DROPPED_LOGS, 1);

    return drop;
}

static void oxb_recovery_btree_commit (struct app_channel **lch,
                                                    struct app_log_entry *log)
{
    struct oxb_rec_transaction **ret;
    struct oxb_rec_transaction *tr;
    struct oxb_rec_log *rec_log;
    struct nvm_ppa_addr *ppa;
    uint32_t log_n = 0;
    uint64_t old_ret;

    ret = (struct oxb_rec_transaction **) tfind(&log->ts, &tr_root,
                                                    oxb_recovery_btree_compare);
    if (!ret)
        return;

    tr = *ret;

    if (APP_OXB_REC_DEBUG_LOG)
        printf ("[recovery: Commit %lu. Applying logs...\n", tr->tid);

    while (!TAILQ_EMPTY (&tr->log_head)) {
        rec_log = TAILQ_FIRST (&tr->log_head);
        if (!rec_log)
            break;

        log_n++;

        /* Execute metadata updates */
        /* User threads have priority in case of race conditions (mapping table)
         * GC logs may contain abort bits if the thread failed. */
        if (rec_log->log.type == APP_LOG_WRITE) {

            if (oxapp()->gl_map->upsert_fn (rec_log->log.write.lba,
                                        rec_log->log.write.new_ppa,
                                        &old_ret, rec_log->log.write.old_ppa))
                log_err ("[recovery: Upsert LOG_WRITE failed.]");
            else {
                /* Invalidate old PPA */
                ppa = (struct nvm_ppa_addr *) &rec_log->log.write.old_ppa;
                oxapp()->md->invalidate_fn (lch[ppa->g.ch], ppa,
                                                            APP_INVALID_SECTOR);
            }

        } else if (rec_log->log.type == APP_LOG_GC_WRITE) {

            /* Do not apply aborted logs caused by race conditions */
            if (!oxb_recovery_should_drop (&rec_log->log, log_n)) {
                if (oxapp()->gl_map->upsert_fn (rec_log->log.write.lba,
                                        rec_log->log.write.new_ppa,
                                        &old_ret, rec_log->log.write.old_ppa))
                    log_err ("[recovery: Upsert LOG_GC_WRITE failed.]");
                else {
                    /* Invalidate old PPA */
                    ppa = (struct nvm_ppa_addr *) &rec_log->log.write.old_ppa;
                    oxapp()->md->invalidate_fn (lch[ppa->g.ch], ppa,
                                                            APP_INVALID_SECTOR);
                }
            } else {
                ppa = (struct nvm_ppa_addr *) &rec_log->log.write.new_ppa;
                oxapp()->md->invalidate_fn (lch[ppa->g.ch], ppa,
                                                            APP_INVALID_SECTOR);
            }

        } else if (rec_log->log.type == APP_LOG_GC_MAP) {

            /* Do not apply aborted logs caused by race conditions */
            if (!oxb_recovery_should_drop (&rec_log->log, log_n)) {
                if (oxapp()->gl_map->upsert_md_fn (rec_log->log.write.lba,
                      rec_log->log.write.new_ppa, rec_log->log.write.old_ppa))
                    log_err ("[recovery: Upsert LOG_GC_MAP failed.]");
            } else {
                ppa = (struct nvm_ppa_addr *) &rec_log->log.write.new_ppa;
                oxapp()->md->invalidate_fn (lch[ppa->g.ch], ppa,
                                                            APP_INVALID_PAGE);
            }
        }

        TAILQ_REMOVE (&tr->log_head, rec_log, entry);
        ox_free (rec_log, OX_MEM_OXBLK_REC);
    }

    tdelete(tr, &tr_root, oxb_recovery_btree_compare);

    if (APP_OXB_REC_DEBUG_LOG)
        printf ("[recovery: Commit %lu completed. Logs: %d\n", tr->tid, log_n);

    ox_stats_add_rec (OX_STATS_REC_TR_COMMIT, 1);

    ox_free (tr, OX_MEM_OXBLK_REC);
}

static int oxb_recovery_log_switch (struct app_channel **lch,
                                struct app_log_entry *entry, uint32_t index)
{
    struct nvm_ppa_addr *ppa;
    struct nvm_ppa_addr ippa;
    uint32_t pg_i;
    uint8_t inv_type = APP_INVALID_PAGE;
    struct nvm_mmgr_geometry *g = lch[0]->ch->geometry;

    switch (entry->type) {
        case APP_LOG_PAD:

            return 1;
            break;

        case APP_LOG_POINTER:

            log_err ("[recovery(log): Duplicated log pointer.]");
            return 1;

        case APP_LOG_WRITE:
        case APP_LOG_GC_WRITE:
            inv_type = APP_INVALID_SECTOR;
        case APP_LOG_GC_MAP:

            oxb_recovery_apply_write (lch, entry, inv_type, 0);
            if (oxb_recovery_btree_add_log (entry))
                log_err ("[recovery: Log was NOT added to tree. Tr "
                                   "%lu, log %d]", entry->write.tid, index);
            break;

        case APP_LOG_MAP:

            oxb_recovery_apply_write (lch, entry, APP_INVALID_PAGE, 1);
            oxapp()->gl_map->upsert_md_fn (
                                    entry->write.lba, entry->write.new_ppa,
                                    entry->write.old_ppa);
            /* If upsert_md fails, this log was already applied */
            break;

        case APP_LOG_BLK_MD:
        case APP_LOG_MAP_MD:
            /* Note: No need to update metadata (tiny table) */

            oxb_recovery_apply_write (lch, entry, APP_INVALID_PAGE, 1);
            break;

        case APP_LOG_AMEND:

            ppa = (struct nvm_ppa_addr *) &entry->amend.ppa;
            if (oxapp()->ch_prov->close_blk_fn (lch[ppa->g.ch],
                                                ppa, entry->amend.prov_type))
                log_err ("[recovery: block might have already been closed.]");

            ippa.ppa = ppa->ppa;
            for (pg_i = ppa->g.pg; pg_i < g->pg_per_blk; pg_i++) {
                ippa.g.pg = pg_i;
                oxapp()->md->invalidate_fn (lch[ppa->g.ch], &ippa,
                                                            APP_INVALID_PAGE);
            }

        case APP_LOG_ABORT_W:

            ppa = (struct nvm_ppa_addr *) &entry->amend.ppa;
            oxb_recovery_apply_fix_blk (lch[ppa->g.ch], ppa->ppa);
            break;

        case APP_LOG_PUT_BLK:

            ppa = (struct nvm_ppa_addr *) &entry->amend.ppa;
            if (oxapp()->ch_prov->put_blk_fn (lch[ppa->g.ch],
                                                    ppa->g.lun, ppa->g.blk)) {
                log_err ("[recovery: Put block failed (%d/%d/%d)]",
                                            ppa->g.ch, ppa->g.lun, ppa->g.blk);
            }
            break;

        case APP_LOG_COMMIT:

            oxb_recovery_btree_commit (lch, entry);
            break;

        default:
            log_err ("[recovery: Unknown log type: %x]\n", entry->type);
           return 0;
    }

    ox_stats_add_log (entry->type, 1);
    tot_logs++;

    return 0;
}

static struct nvm_ppa_addr *oxb_recovery_log_apply (struct nvm_io_data *pg_io,
                                                    struct app_channel **lch)
{
    uint8_t pl, sec, stop;
    uint32_t logs_per_sec, log_i;
    struct app_log_entry *entry;
    struct app_sec_oob *oob;
    struct nvm_ppa_addr *next = NULL;
    struct nvm_mmgr_geometry *g = pg_io->ch->geometry;

    logs_per_sec = g->sec_size / sizeof(struct app_log_entry);

    /* Iterate the logs within the flash page */
    for (pl = 0; pl < pg_io->n_pl; pl++) {
        for (sec = 0; sec < g->sec_per_pg; sec++) {

            oob = (struct app_sec_oob *) 
                    pg_io->oob_vec[pl * g->sec_per_pg + sec];

            if (APP_OXB_REC_DEBUG_LOG)
                printf ("  Log sector (%d/%d) -> type: %x\n",
                                                        pl, sec, oob->pg_type);

            if (oob->pg_type != APP_PG_LOG)
                continue;

            stop = 0;
            for (log_i = 0; log_i < logs_per_sec; log_i++) {

                entry = &((struct app_log_entry *)
                                                pg_io->sec_vec[pl][sec])[log_i];

                if (APP_OXB_REC_DEBUG_LOG)
                    printf ("   Log %d -> ts: %lu, type: %x\n", log_i,
                                                        entry->ts, entry->type);

                if (!entry->ts)
                    break;

                if (entry->type == APP_LOG_POINTER) {
                    ox_stats_add_log (entry->type, 1);
                    if (next) {
                        log_err ("[recovery(log): Duplicated log pointer.]");
                        break;
                    }
                    next = (struct nvm_ppa_addr *) entry->pointer.next;
                    continue;
                }

                stop = oxb_recovery_log_switch (lch, entry, log_i);
                if (stop)
                    break;

            }
        }
    }

    return next;
}

static int oxb_recovery_log_read (struct app_rec_entry *log_head,
                            struct nvm_io_data **io, struct app_channel **lch)
{
    struct nvm_ppa_addr *ppa, *ppa_list = NULL, *ret;
    struct nvm_io_data *pg_io;
    uint32_t tot_pgs = 0, list_i = 0;
    struct app_log_entry log_pg;

    /* Get the log head */
    ppa = (struct nvm_ppa_addr *) log_head->data;

    if (APP_DEBUG_RECOVERY)
        printf ("[recovery (log): Log head -> (%d/%d/%d/%d)]\n",
                                ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg);

    while (ppa->ppa) {
        pg_io = io[ppa->g.ch];

        if (APP_DEBUG_RECOVERY)
            printf ("[recovery (log): Processing log page (%d/%d/%d/%d)]\n",
                                ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg);

        if (ftl_pg_io_switch (pg_io->ch, MMGR_READ_PG, (void **) pg_io->pl_vec,
                                                        ppa, NVM_IO_NORMAL)) {
            if (APP_DEBUG_RECOVERY)
                printf ("[recovery (log): Read log page (%d/%d/%d/%d) failed.]\n",
                                ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg);
            ret = NULL;
            goto CHECK;
        }

        ret = oxb_recovery_log_apply (pg_io, lch);

CHECK:
        /* Fix the log block itself and invalidate log page */
        log_pg.write.old_ppa = 0;
        log_pg.write.new_ppa = ppa->ppa;
        oxb_recovery_apply_write (lch, &log_pg, APP_INVALID_PAGE, 0);
        oxapp()->md->invalidate_fn (lch[ppa->g.ch], ppa, APP_INVALID_PAGE);

        if (!ret && ppa_list && list_i < 2) {
            list_i++;
            ppa = &ppa_list[list_i];
            continue;
        }

        if (!ret && !ppa_list) {
            log_err ("[recovery (log): Log tail NOT found.]");
            break;
        }

        if (!ret && (list_i >= 2))
            break;

        ppa_list = ret;
        ppa = &ppa_list[0];
        list_i = 0;
        tot_pgs++;
    }

    ox_stats_add_rec (OX_STATS_REC_LOG_PGS, tot_pgs);
    ox_stats_add_rec (OX_STATS_REC_LOG_SZ, tot_pgs *
                                            io[0]->ch->geometry->pl_pg_size);
    log_info ("[recovery (log): Log chain is processed. Logs: %lu, Pages: %d]",
                                                            tot_logs, tot_pgs);

    return 0;
}

static int oxb_recovery_log_replay (void)
{
    struct app_rec_entry *log_head;
    struct nvm_io_data *io[app_nch];
    struct app_channel *lch[app_nch];
    uint32_t ch_i, nch;
    int ret = -1;

    log_head = oxapp()->recovery->get_fn (APP_CP_LOG_HEAD);
    if (!log_head)
        return 0;

    nch = oxapp()->channels.get_list_fn (lch, app_nch);
    if (nch != app_nch)
        return -1;

    for (ch_i = 0; ch_i < app_nch; ch_i++) {
        io[ch_i] = ftl_alloc_pg_io (lch[ch_i]->ch);
        if (!io[ch_i])
            goto FREE_IO;
    }

    tot_logs = 0;
    if (oxb_recovery_log_read (log_head, io, lch))
        goto FREE_IO;

    /* Destroy RB Tree */
    tdestroy (tr_root, oxb_recovery_btree_free_act);

    ret = 0;

FREE_IO:
    while (ch_i) {
        ch_i--;
        ftl_free_pg_io (io[ch_i]);
    }
    return ret;
}

static struct app_rec_entry *oxb_recovery_get (uint32_t type)
{
    struct app_rec_entry *cp_entry = NULL;

    LIST_FOREACH (cp_entry, &cp_list, entry) {
        if (cp_entry->type == type)
            break;
    }

    return cp_entry;
}

static int oxb_recovery_set (struct app_rec_entry *user_entry)
{
    struct app_rec_entry *cp_entry = NULL;

    LIST_FOREACH (cp_entry, &cp_list, entry) {
        if (cp_entry->type == user_entry->type)
            break;
    }

    if (cp_entry) {

        /* We found in the list  - update entry */
        cp_entry->size = user_entry->size;
        if (user_entry->size > cp_entry->size) {
            ox_realloc (cp_entry->data, user_entry->size, OX_MEM_OXBLK_REC);
            if (!cp_entry->data) {
                log_err ("[recovery: memory error in realloc CP data.]");
                return -1;
            }
        }

        memcpy (cp_entry->data, user_entry->data, user_entry->size);

    } else {

        /* We did not find in the list - add entry */
        cp_entry = ox_malloc (sizeof (struct app_rec_entry), OX_MEM_OXBLK_REC);
        if (!cp_entry) {
            log_err ("[recovery: memory error in malloc CP entry.]");
            return -1;
        }

        cp_entry->data = ox_malloc (user_entry->size, OX_MEM_OXBLK_REC);
        if (!cp_entry->data) {
            ox_free (cp_entry, OX_MEM_OXBLK_REC);
            log_err ("[recovery: memory error in malloc CP entry data.]");
            return -1;
        }

        memcpy (cp_entry->data, user_entry->data, user_entry->size);
        cp_entry->size = user_entry->size;
        cp_entry->type = user_entry->type;

        LIST_INSERT_HEAD (&cp_list, cp_entry, entry);

    }

    if (APP_DEBUG_RECOVERY)
        printf ("[recovery: New checkpoint entry: %03x, size %d bytes]\n",
                                            user_entry->type, user_entry->size);

    return 0;
}

static int oxb_recovery_read_cp (struct nvm_io_data *io,
                            struct nvm_ppa_addr *ppa, uint32_t cp_size,
                            uint16_t pgs, uint16_t ent_per_pg)
{
    uint8_t *buf;
    uint32_t offset = 0;
    struct app_rec_entry *cp_entry;
    struct app_rec_entry set_entry;

    buf = ox_malloc (cp_size, OX_MEM_OXBLK_REC);
    if (!buf)
        return -1;

    if (ftl_nvm_seq_transfer (io, ppa, buf, pgs, ent_per_pg,
                            cp_size, 1, NVM_TRANS_FROM_NVM, NVM_IO_RESERVED))
        goto ERR;

    /* Set recovery entries */
    while (offset < cp_size) {
        cp_entry = (struct app_rec_entry *) &buf[offset];
        set_entry.size = cp_entry->size;
        set_entry.type = cp_entry->type;
        set_entry.data = &buf[offset] + 8; /* 8 bytes of header */

        if (oxb_recovery_set (&set_entry))
            goto ERR;

        offset += cp_entry->size + 8;
    }

    ox_stats_set_cp (OX_STATS_CP_LOAD_ADDR, ppa->ppa);

    ox_free (buf, OX_MEM_OXBLK_REC);
    return 0;

ERR:
    ox_free (buf, OX_MEM_OXBLK_REC);
    return -1;
}

static int oxb_recovery_recover (void)
{
    int pg;
    struct nvm_channel *ch;
    struct app_rec_oob oob;
    struct nvm_ppa_addr ppa;
    struct nvm_mmgr *mmgr = ox_get_mmgr_instance ();
    uint16_t ent_per_pg, md_pgs, rsvd_blk;

    /* TODO: use round-robin in all channels for checkpoint */
    ch = &mmgr->ch_info[0];
    if (!ch)
        return -1;

    struct nvm_io_data *io = ftl_alloc_pg_io(ch);
    if (io == NULL)
        return -1;

    rsvd_blk = ch->mmgr_rsv + APP_RSV_CP_OFF;
    pg = ftl_blk_current_page (ch, io, rsvd_blk, 1);
    if (pg < 0)
        goto ERR;

    if (!pg || core.reset) {
        if (ftl_io_rsv_blk (ch, MMGR_ERASE_BLK, NULL, rsvd_blk, 0))
            goto ERR;

        pg = 0;

        if (APP_DEBUG_RECOVERY)
            printf ("[recovery: Checkpoint not found, blk (%d/%d/%d) erased]\n",
                                                        ch->ch_id, 0, rsvd_blk);
    } else {
        memcpy (&oob, io->oob_vec[0], sizeof (struct app_rec_oob));

        ent_per_pg = io->pg_sz * io->n_pl;
        md_pgs = oob.checkpoint_sz / ent_per_pg;
        if (oob.checkpoint_sz % ent_per_pg > 0)
            md_pgs++;

        /* read the checkpoint from nvm */
        ppa.g.ch = ch->ch_id;
        ppa.g.lun = 0;
        ppa.g.pg = pg - md_pgs;
        ppa.g.blk = rsvd_blk;

        if (oxb_recovery_read_cp (io, &ppa, oob.checkpoint_sz,
                                                           md_pgs, ent_per_pg))
            goto ERR;

        if (APP_DEBUG_RECOVERY)
            printf ("[recovery: Checkpoint is loaded from (%d/%d/%d/%d), size: "
                "%d]\n", ch->ch_id, 0, rsvd_blk, ppa.g.pg, oob.checkpoint_sz);
    }

    cur_flush_pg = pg;

    ftl_free_pg_io(io);
    return 0;

ERR:
    ftl_free_pg_io(io);
    return -1;
}

static int oxb_recovery_nvm_write_cp (struct app_channel *lch, uint8_t *flush,
                                                            uint32_t flush_sz)
{
    int pg;
    struct app_rec_oob oob;
    struct nvm_ppa_addr ppa;
    uint16_t ent_per_pg, md_pgs;

    struct nvm_io_data *io = ftl_alloc_pg_io(lch->ch);
    if (io == NULL)
        return -1;

    ent_per_pg = io->pg_sz * io->n_pl;
    md_pgs = flush_sz / ent_per_pg;
    if (flush_sz % ent_per_pg > 0)
        md_pgs++;

    if (md_pgs > io->ch->geometry->pg_per_blk) {
        log_err("[recovery: Ch %d -> Maximum Checkpoint size: %d bytes]\n",
            io->ch->ch_id, io->pg_sz * io->n_pl * io->ch->geometry->pg_per_blk);
        goto ERR;
    }

    if (cur_flush_pg == 0xffff)
        pg = ftl_blk_current_page (lch->ch, io, lch->cp_blk, md_pgs);
    else
        pg = cur_flush_pg;

    if (pg < 0)
        goto ERR;

    if (pg >= io->ch->geometry->pg_per_blk - md_pgs) {
        if (ftl_io_rsv_blk (lch->ch, MMGR_ERASE_BLK, NULL, lch->cp_blk, 0))
            goto ERR;
        pg = 0;
    }

    oob.magic         = APP_MAGIC;
    oob.checkpoint_sz = flush_sz;

    /* set info to OOB area */
    memcpy (io->oob_vec[0], &oob, lch->ch->geometry->pg_oob_sz);

    /* flush the checkpoint to nvm */
    ppa.g.ch = lch->ch->ch_id;
    ppa.g.lun = 0;
    ppa.g.pg = pg;
    ppa.g.blk = lch->cp_blk;

    if (ftl_nvm_seq_transfer (io, &ppa, flush, md_pgs, ent_per_pg,
                        flush_sz, 1, NVM_TRANS_TO_NVM, NVM_IO_RESERVED))
        goto ERR;

    ox_stats_add_event (OX_CP_EVENT_FLUSH,
                                    md_pgs * io->ch->geometry->sec_per_pl_pg);

    cur_flush_pg = pg + md_pgs;

    ox_stats_set_cp (OX_STATS_CP_LOAD_ADDR, ppa.ppa);

    if (APP_DEBUG_RECOVERY)
        printf ("[recovery: Checkpoint flushed to (%d/%d/%d/%d), size: %d bytes"
                    "]\n", lch->ch->ch_id, 0, lch->cp_blk, ppa.g.pg, flush_sz);

    ftl_free_pg_io(io);
    return 0;

ERR:
    ftl_free_pg_io(io);
    return -1;
}

static int oxb_recovery_flush_cp (struct app_channel *lch)
{
    uint8_t *flush;
    uint32_t offset = 0;
    uint32_t flush_sz = 0;
    struct app_rec_entry *cp_entry = NULL;

    LIST_FOREACH (cp_entry, &cp_list, entry) {
        flush_sz += cp_entry->size + 8; /* 8 bytes for the header */
    }

    if (flush_sz > lch->ch->geometry->blk_size) {
        log_err ("[recovery: Checkpoint is too big: %d bytes. Max: %d bytes.]",
                                        flush_sz, lch->ch->geometry->blk_size);
        return -1;
    }

    flush = ox_malloc (flush_sz, OX_MEM_OXBLK_REC);
    if (!flush)
        return -1;

    LIST_FOREACH (cp_entry, &cp_list, entry) {
        memcpy (&flush[offset], cp_entry, 8);             /* Copy the header */
        memcpy (&flush[offset + 8], cp_entry->data, cp_entry->size); /* Data */
        offset += cp_entry->size + 8; 
    }

    if (oxb_recovery_nvm_write_cp (lch, flush, flush_sz)) {
        ox_free (flush, OX_MEM_OXBLK_REC);
        return -1;
    }

    ox_stats_set_cp (OX_STATS_CP_SZ, flush_sz);

    ox_free (flush, OX_MEM_OXBLK_REC);
    return 0;
}

static int oxb_recovery_checkpoint (void)
{
    int ret, retry, i;
    struct nvm_ppa_addr log_head, log_tail;
    struct app_rec_entry cp_entry;
    struct app_channel *lch[app_nch];
    uint32_t gl_prov_ch;
    uint64_t ns;
    struct timespec ts;
    int nch = app_nch;

    pthread_mutex_lock (&cp_mutex);

    GET_NANOSECONDS (ns, ts);
    oxapp()->channels.get_list_fn (lch, nch);

    /* Get new log head from current tail */
    log_head.ppa = get_log_tail ();

    ox_stats_set_cp (OX_STATS_CP_MAP_EVICT, 0);
    ox_stats_set_cp (OX_STATS_CP_MAPMD_EVICT, 0);
    ox_stats_set_cp (OX_STATS_CP_BLK_EVICT, 0);

    /* Flush cached mapping table */
    oxapp()->gl_map->clear_fn ();

    /* Flush metadata */
    for (i = 0; i < nch; i++) {
        retry = 0;
        do {
            retry++;
            ret = oxapp()->ch_map->flush_fn (lch[i]);
        } while (ret && retry < APP_FLUSH_RETRY);

        retry = 0;
        do {
            retry++;
            ret = oxapp()->md->flush_fn (lch[i]);
        } while (ret && retry < APP_FLUSH_RETRY);
    }

    /* Set the new log head in the log module */
    set_log_head (log_head.ppa);

    log_tail.ppa = get_log_tail ();

    cp_entry.type = APP_CP_LOG_HEAD;
    cp_entry.size = sizeof (struct nvm_ppa_addr);
    cp_entry.data = &log_head;

    /* Checkpoint log head */
    if (oxapp()->recovery->set_fn (&cp_entry))
        return -1;

    cp_entry.type = APP_CP_LOG_TAIL;
    cp_entry.data = &log_tail;

    /* Checkpoint log tail */
    if (oxapp()->recovery->set_fn (&cp_entry))
        return -1;

    cp_entry.type = APP_CP_GL_PROV_CH;
    cp_entry.size = sizeof (uint32_t);
    gl_prov_ch = get_gl_prov_current_ch ();
    cp_entry.data = &gl_prov_ch;

    /* Checkpoint current global provisioning channel */
    if (oxapp()->recovery->set_fn (&cp_entry))
        return -1;

    cp_entry.type = APP_CP_TIMESTAMP;
    cp_entry.size = sizeof (uint64_t);
    cp_entry.data = &ns;

    /* Checkpoint Timestamp */
    if (oxapp()->recovery->set_fn (&cp_entry))
        return -1;

    /* TODO: use round-robin in all channels for checkpoint */
    if (oxb_recovery_flush_cp (lch[0])) {
        log_err(" [recovery: Checkpoint NOT flushed.]");
        return -1;
    }

    oxapp()->log->truncate_fn ();

    /* Complete PUT blocks */
    for (i = 0; i < nch; i++)
        oxapp()->ch_prov->put_blk_fn (lch[i], 0xffff, 0xffff);

    pthread_mutex_unlock (&cp_mutex);

    first_cp = 1;

    return 0;
}

static void *oxb_recovery_cp_thread (void *arg)
{
    uint32_t it = 0;
    double cp_int;
    struct nvm_mmgr *mmgr = ox_get_mmgr_instance ();

    cp_int = (mmgr->flags & MMGR_FLAG_MIN_CP_TIME) ?
                                        APP_CP_MIN_INTERVAL : APP_CP_INTERVAL;

    while (oxapp()->recovery->running) {
        usleep (1000);
        if (!oxapp()->recovery->running)
            break;

        it++;
        if (it * 1000 < cp_int * (double) 1000000)
            continue;

        /* Wait until the startup completes */
        if (!first_cp)
            continue;

        it = 0;

        if (oxapp()->recovery->checkpoint_fn ())
            log_err ("[recovery: Checkpoint NOT completed.]");
    }

    return NULL;
}

static int oxb_recovery_init (void)
{
    LIST_INIT (&cp_list);

    if (!ox_mem_create_type ("OXBLK_RECOVERY", OX_MEM_OXBLK_REC))
        return -1;

    cur_flush_pg = 0xffff;

    if (pthread_mutex_init (&cp_mutex, 0))
        return -1;

    first_cp = 0;
    oxapp()->recovery->running = 1;

    if (pthread_create (&cp_thread, NULL, oxb_recovery_cp_thread, NULL)) {
        pthread_mutex_destroy (&cp_mutex);
        return -1;
    }

    log_info("    [ox-blk: Recovery started.]\n");

    return 0;
}

static void oxb_recovery_exit (void)
{
    struct app_rec_entry *cp_entry;

    oxapp()->recovery->running = 0;
    pthread_join (cp_thread, NULL);

    pthread_mutex_destroy (&cp_mutex);

    while (!LIST_EMPTY (&cp_list)) {
        cp_entry = LIST_FIRST (&cp_list);
        LIST_REMOVE (cp_entry, entry);

        ox_free (cp_entry->data, OX_MEM_OXBLK_REC);
        ox_free (cp_entry, OX_MEM_OXBLK_REC);
    }

    log_info("    [ox-blk: Recovery stopped.]\n");
}

static struct app_recovery oxblk_recovery = {
    .mod_id         = OXBLK_RECOVERY,
    .name           = "OX-BLOCK-RECOVERY",
    .init_fn        = oxb_recovery_init,
    .exit_fn        = oxb_recovery_exit,
    .checkpoint_fn  = oxb_recovery_checkpoint,
    .recover_fn     = oxb_recovery_recover,
    .log_replay_fn  = oxb_recovery_log_replay,
    .set_fn         = oxb_recovery_set,
    .get_fn         = oxb_recovery_get,
    .running        = 0
};

void oxb_recovery_register (void) {
    app_mod_register (APPMOD_RECOVERY, OXBLK_RECOVERY, &oxblk_recovery);
}