/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX-App Transaction Framework
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
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <search.h>
#include <string.h>
#include <ox-app.h>

#define APP_TRANSACTION_COUNT       4096  /* Maximum concurrent transactions */
#define APP_TRANSACTION_LOG_COUNT   4096  /* Maximum entries per transaction */

static struct app_transaction_t *transactions;
TAILQ_HEAD (app_tr_free, app_transaction_t) free_tr_head;
TAILQ_HEAD (app_tr_used, app_transaction_t) used_tr_head;

static pthread_mutex_t    tr_mutex;
static pthread_spinlock_t tr_spin;

static struct nvm_mmgr_geometry *ch_geo;
static struct app_channel **ch;
extern uint16_t app_nch;

/* A thread is created for closing blocks in case of failure */
struct app_tr_close_blk {
    struct nvm_ppa_addr ppa;
    uint8_t             type;
    uint8_t             used;
    TAILQ_ENTRY (app_tr_close_blk) entry;
};

#define APP_TRANSACTION_CLB_LIST    128
TAILQ_HEAD (tr_clb_free, app_tr_close_blk) free_cbl_head;
TAILQ_HEAD (tr_clb_used, app_tr_close_blk) used_cbl_head;
static struct app_tr_close_blk tr_cl_blks[APP_TRANSACTION_CLB_LIST];
static pthread_spinlock_t      tr_cbl_spin;
static pthread_t               tr_cbl_th;
static uint8_t                 tr_cbl_stop;

/* Transactions are started by calling this function, 'entries' should contain
 * the LBAs of the transaction */
struct app_transaction_t *app_transaction_new (uint64_t *lbas, uint32_t count,
                                                                uint8_t tr_type)
{
    uint32_t lba_i;
    struct app_transaction_t *tr;
    struct timespec ts;

    if (count > APP_TRANSACTION_LOG_COUNT) {
        log_info ("[ox-app (transaction): Maximum LBAs exceed. (%d/%d)]\n",
                                            count, APP_TRANSACTION_LOG_COUNT);
        return NULL;
    }

RETRY:
    pthread_spin_lock (&tr_spin);
    tr = TAILQ_FIRST(&free_tr_head);
    if (!tr) {
        pthread_spin_unlock (&tr_spin);
	usleep (5000);
	log_info ("[ox-app (transaction): Transaction slot not available. "
                                                    "Retrying after 500 us.]");
	goto RETRY;
    }
    GET_NANOSECONDS (tr->ts, ts);
    TAILQ_REMOVE(&free_tr_head, tr, entry);
    TAILQ_INSERT_TAIL(&used_tr_head, tr, entry);
    pthread_spin_unlock (&tr_spin);

    tr->flush_commit = 0;
    tr->allocated = 0;
    tr->committed = 0;
    tr->count = count;
    tr->tr_type = tr_type;

    for (lba_i = 0; lba_i < count; lba_i++) {
        tr->entries[lba_i].lba = lbas[lba_i];
        tr->entries[lba_i].ppa.ppa = 0x0;
        GET_NANOSECONDS (tr->entries[lba_i].ts, ts);
        tr->entries[lba_i].tr = tr;
        tr->entries[lba_i].abort = 0;
    }

    tr->ts = tr->entries[count - 1].ts;

    /* Transaction ID is the timestamp of last entry */
    for (lba_i = 0; lba_i < count; lba_i++)
        tr->entries[lba_i].tid = tr->ts;

    return tr;
}

static struct app_prov_ppas *app_transaction_alloc_ch (
                           struct app_transaction_user *ent, uint8_t line_type)
{
    uint32_t log_i, npgs;
    struct app_prov_ppas *prov;
    struct nvm_ppa_addr ppa_list[ent->count];

    npgs = ent->count / ch_geo->sec_per_pl_pg;

    prov = ox_malloc (sizeof (struct app_prov_ppas), OX_MEM_APP_TRANS);
    if (!prov)
        return NULL;

    prov->ppa = ox_calloc (ent->count, sizeof (struct nvm_ppa_addr),
                                                            OX_MEM_APP_TRANS);
    if (!prov->ppa)
        goto FREE_PROV;

    prov->nppas = ent->count;
    prov->nch = 0;

    if (oxapp()->ch_prov->get_ppas_fn(ch[ent->tid], ppa_list, npgs, line_type))
        goto FREE_PPA;

    for (log_i = 0; log_i < ent->count; log_i++)
        prov->ppa[log_i].ppa = ppa_list[log_i].ppa;

    return prov;

FREE_PPA:
    ox_free (prov->ppa, OX_MEM_APP_TRANS);
FREE_PROV:
    ox_free (prov, OX_MEM_APP_TRANS);
    return NULL;
}

/* This function is used to allocate PPAs for the LBAs in 'entries',
 * it updates the PPAs in the transactions created by 'new'. */
struct app_prov_ppas *app_transaction_alloc_list (struct app_transaction_user *ent,
                                                                uint8_t tr_type)
{
    uint32_t npgs, log_i, log_count = 0;
    struct app_prov_ppas *prov = NULL;
    struct app_log_entry log[ent->count];

    uint8_t log_type;

    if ( (ent->count) % ch_geo->sec_per_pl_pg != 0) {
        log_info ("[ox-app (transaction): entries are not page aligned.]\n");
        return NULL;
    }

    npgs = ent->count / ch_geo->sec_per_pl_pg;

    switch (tr_type) {
        case APP_TR_LBA_NS:
            log_type = APP_LOG_WRITE;
            prov = oxapp()->gl_prov->new_fn (npgs, APP_LINE_USER);
            break;

        case APP_TR_GC_NS:
            log_type = APP_LOG_GC_WRITE;
            prov = app_transaction_alloc_ch (ent, APP_LINE_COLD);
            break;

        case APP_TR_GC_MAP:
            log_type = APP_LOG_GC_MAP;
            prov = app_transaction_alloc_ch (ent, APP_LINE_USER);
            break;

        default:
            return NULL;
    }

    if (!prov)
        return NULL;

    for (log_i = 0; log_i < ent->count; log_i++) {
        /* NULL entries are not logged */
        if (ent->entries[log_i]) {
            log[log_count].ts = ent->entries[log_i]->ts;
            log[log_count].type = log_type;
            log[log_count].write.tid = ent->entries[log_i]->tid;
            log[log_count].write.lba = ent->entries[log_i]->lba;
            log[log_count].write.new_ppa = prov->ppa[log_i].ppa;
            log[log_count].write.old_ppa = 0x0;
            ent->entries[log_i]->ppa.ppa = prov->ppa[log_i].ppa;

            pthread_spin_lock (&ent->entries[log_i]->tr->spin);
            ent->entries[log_i]->tr->allocated++;
            pthread_spin_unlock (&ent->entries[log_i]->tr->spin);

            log_count++;
        }
    }

    if (oxapp()->log->append_fn (log, log_count))
        goto ERR;

    return prov;

ERR:
    for (log_i = 0; log_i < ent->count; log_i++) {
        if (ent->entries[log_i]) {
            ent->entries[log_i]->ppa.ppa = 0;

            pthread_spin_lock (&ent->entries[log_i]->tr->spin);
            ent->entries[log_i]->tr->allocated--;
            pthread_spin_unlock (&ent->entries[log_i]->tr->spin);
        }
    }

    /* Invalidate padded sectors */
    /* TODO: Roolback the current page in ch_prov, instead of invalidating
     *       For now, the blocks will be closed by other threads */
    for (log_i = 0; log_i < prov->nppas; log_i++)
        oxapp()->md->invalidate_fn (ch[prov->ppa[log_i].g.ch],
                                        &prov->ppa[log_i], APP_INVALID_SECTOR);
    app_transaction_free_list (prov);

    return NULL;
}

void app_transaction_free_list (struct app_prov_ppas *prov)
{
    if (!prov->nch) {
        ox_free (prov->ppa, OX_MEM_APP_TRANS);
        ox_free (prov, OX_MEM_APP_TRANS);
    } else
        oxapp()->gl_prov->free_fn (prov);
}

static void app_transaction_recycle (struct app_transaction_t *tr)
{
    tr->allocated = 0;
    tr->committed = 0;
    tr->count = 0;
    tr->ts = 0;

    pthread_spin_lock (&tr_spin);
    TAILQ_REMOVE(&used_tr_head, tr, entry);
    TAILQ_INSERT_TAIL(&free_tr_head, tr, entry);
    pthread_spin_unlock (&tr_spin);
}

/* This function ensures that transactions are committed in the same order 
     * they have been created. The caller should lock 'tr_mutex' before */
static void app_transaction_ordered_commit (void)
{
    uint32_t lba_i = 0;
    struct app_log_entry log;
    struct app_transaction_t *tr;
    struct nvm_ppa_addr old_ppas[APP_TRANSACTION_LOG_COUNT];
    uint16_t byte_i;

    while ( (tr = TAILQ_FIRST (&used_tr_head) ) ) {
        if (tr->committed) {

            /* Update mapping table */
            switch (tr->tr_type) {

                case APP_TR_GC_MAP:
                case APP_TR_GC_NS:
                    break;

                case APP_TR_LBA_NS:
                default:

                    for (lba_i = 0; lba_i < tr->count; lba_i++)
                        if (oxapp()->gl_map->upsert_fn (tr->entries[lba_i].lba,
                                        tr->entries[lba_i].ppa.ppa,
                                        (uint64_t *) &old_ppas[lba_i], 0)) {
                            log_err ("[ox-app (transaction): Upsert failed.");
                            goto ROLLBACK;
                        }
            }

            /* Append the commit log */
            memset (&log, 0x0, sizeof (struct app_log_entry));

            /* We might need to drop logs of single sectors created by the GC
               in case of concurrent mapping table update with user thread */
            if (tr->tr_type == APP_TR_GC_NS || tr->tr_type == APP_TR_GC_MAP) {
                byte_i = 0;
                for (lba_i = 0; lba_i < tr->count; lba_i++)
                    if (tr->entries[lba_i].abort) {

                        /* Up to 48 aborted sectors per transaction 
                           Support transactions with a maximum of 256 logs */
                        if (lba_i < 256 && byte_i < 48)
                            log.byte[byte_i] = lba_i + 1;
                        else
                            break;

                        byte_i++;
                    }
            }

            log.ts = tr->ts;
            log.type = APP_LOG_COMMIT;
            if (oxapp()->log->append_fn (&log, 1))
                goto ROLLBACK;

            if (tr->flush_commit == APP_T_FLUSH_YES)
                if (oxapp()->log->flush_fn (0, NULL, tr->cb)) {
                    log_err ("[ox-app (transaction): Flush log failed.");
                    goto ROLLBACK;
                }

            /* Invalidate sectors for GC (only user writes) */
            if (tr->tr_type == APP_TR_LBA_NS)
                for (lba_i = 0; lba_i < tr->count; lba_i++)
                    if (old_ppas[lba_i].ppa)
                        oxapp()->md->invalidate_fn (ch[old_ppas[lba_i].g.ch],
                                          &old_ppas[lba_i], APP_INVALID_SECTOR);

            app_transaction_recycle (tr);

        } else {
            break;
        }
    }
    return;

ROLLBACK:
    if (tr->tr_type != APP_TR_LBA_NS)
        return;

    while (lba_i) {
        lba_i--;
        if (oxapp()->gl_map->upsert_fn (tr->entries[lba_i].lba,
                                old_ppas[lba_i].ppa, &old_ppas[lba_i].ppa, 0))
            log_err ("[transaction: Failed to rollback failed upserted LBAs. "
                     "LBA: %lu, nlbas: %d]", tr->entries[lba_i].lba, lba_i + 1);
    }
}

/* This function is used to complete a transaction, it must be called after
 * all PPAs in the transaction have been filled by 'alloc'. */
int app_transaction_commit (struct app_transaction_t *tr_u,
                                        struct nvm_callback *cb, uint8_t flush)
{
    if (tr_u->allocated != tr_u->count) {
        log_info ("[ox-app (transaction): Transaction entries still missing "
                                "(%d/%d).]\n", tr_u->allocated, tr_u->count);
        return -1;
    }

    pthread_spin_lock (&tr_u->spin);
    tr_u->cb = cb;
    tr_u->flush_commit = flush;
    tr_u->committed = 1;
    pthread_spin_unlock (&tr_u->spin);

    pthread_mutex_lock (&tr_mutex);
    app_transaction_ordered_commit ();
    pthread_mutex_unlock (&tr_mutex);

    return 0;
}

void app_transaction_abort (struct app_transaction_t *tr)
{
    uint32_t lba_i;

    /* Invalidate sectors possibly written before the failure*/
    for (lba_i = 0; lba_i < tr->count; lba_i++)
        if (tr->entries[lba_i].ppa.ppa) {

            if (tr->tr_type == APP_TR_GC_MAP)
                oxapp()->md->invalidate_fn (ch[tr->entries[lba_i].ppa.g.ch],
                                  &tr->entries[lba_i].ppa, APP_INVALID_PAGE);
            else
                oxapp()->md->invalidate_fn (ch[tr->entries[lba_i].ppa.g.ch],
                                  &tr->entries[lba_i].ppa, APP_INVALID_SECTOR);
        }

    app_transaction_recycle (tr);

    pthread_mutex_lock (&tr_mutex);
    app_transaction_ordered_commit ();
    pthread_mutex_unlock (&tr_mutex);
}

void app_transaction_abort_by_ts (uint64_t tid, uint64_t ts) {
    if (transactions[tid].ts == ts)
        app_transaction_abort (&transactions[tid]);
}

static int app_tr_check_cbl_blk (struct nvm_ppa_addr *ppa)
{
    uint32_t blk_i;

    for (blk_i = 0; blk_i < APP_TRANSACTION_CLB_LIST; blk_i++) {
        if (tr_cl_blks[blk_i].used &&
                tr_cl_blks[blk_i].ppa.g.ch == ppa->g.ch &&
                tr_cl_blks[blk_i].ppa.g.lun == ppa->g.lun &&
                tr_cl_blks[blk_i].ppa.g.blk == ppa->g.blk)
            return -1;
    }

    return 0;
}

int app_transaction_close_blk (struct nvm_ppa_addr *ppa, uint8_t type)
{
    struct app_tr_close_blk *blk;

    /* Duplicated block */
    if (app_tr_check_cbl_blk (ppa))
        return 0;

    pthread_spin_lock (&tr_cbl_spin);
    blk = TAILQ_FIRST(&free_cbl_head);
    if (!blk) {
        pthread_spin_unlock (&tr_cbl_spin);
        return -1;
    }
    TAILQ_REMOVE(&free_cbl_head, blk, entry);
    pthread_spin_unlock (&tr_cbl_spin);

    blk->ppa.ppa = ppa->ppa;
    blk->type = type;
    blk->used = 1;

    pthread_spin_lock (&tr_cbl_spin);
    TAILQ_INSERT_TAIL(&used_cbl_head, blk, entry);
    pthread_spin_unlock (&tr_cbl_spin);

    return 0;
}

/* The caller is responsible to call 'oxapp()->gl_prov->free_fn ()' to freeing 
    the return pointer of this function. This function only works for 1 page
    at a time in 'user->entries' (all sectors in a plane page) */
struct app_prov_ppas *app_transaction_amend (struct app_transaction_user *user,
                                                                   uint8_t type)
{
    uint32_t sec_i;
    struct app_prov_ppas *prov_ppa;
    struct app_log_entry log[ch_geo->sec_per_pl_pg];

    if (user->count > ch_geo->sec_per_pl_pg) {
        log_info ("[transaction (amend): This function only supports 1 page.]");
        return NULL;
    }

    if (app_transaction_close_blk (&user->entries[0]->ppa, type))
        return NULL;

    prov_ppa = oxapp()->gl_prov->new_fn (1, type);
    if (!prov_ppa) {
        log_info ("[transaction (amend): New PPA has not been created. "
                                                       "Log is inconsistent]");
        return NULL;
    }

    for (sec_i = 0; sec_i < user->count; sec_i++) {
        memset (&log[sec_i], 0x0, sizeof (struct app_log_entry));
        log->ts = user->entries[sec_i]->ts;
        log->type = APP_LOG_AMEND;
        log->write.tid = user->entries[sec_i]->tid;
        log->write.old_ppa = user->entries[sec_i]->ppa.ppa;
        log->write.new_ppa = prov_ppa->ppa[sec_i].ppa;
        log->write.lba = user->entries[sec_i]->lba;
        user->entries[sec_i]->ppa.ppa = prov_ppa->ppa[sec_i].ppa;
    }

    if (oxapp()->log->append_fn (log, ch_geo->sec_per_pl_pg)) {
        // TODO: WHAT DO WE DO in FAILURE?
    }

    /* We do not flush amend logs here */

    return prov_ppa;
}

static void *app_tr_clb_thread (void *arg)
{
    struct app_tr_close_blk *blk;
    struct nvm_ppa_addr ippa;
    struct app_channel *lch;
    struct app_log_entry log;
    uint32_t pg_i;
    uint64_t ns;
    struct timespec ts;

    while (!tr_cbl_stop) {
        if (TAILQ_EMPTY(&used_cbl_head)) {
            usleep (500);
            continue;
        }

        pthread_spin_lock (&tr_cbl_spin);
        blk = TAILQ_FIRST(&used_cbl_head);
        if (!blk) {
            pthread_spin_unlock (&tr_cbl_spin);
            continue;
        }
        TAILQ_REMOVE(&used_cbl_head, blk, entry);
        pthread_spin_unlock (&tr_cbl_spin);

        lch = ch[blk->ppa.g.ch];

        if (oxapp()->ch_prov->close_blk_fn (lch, &blk->ppa, blk->type))
            log_info ("[transaction (amend): the block might have already been "
                                                                    "closed.]");

        /* Log the write */
        GET_NANOSECONDS (ns, ts);
        log.ts = ns;
        log.type = APP_LOG_AMEND;
        log.amend.ppa = blk->ppa.ppa;
        log.amend.prov_type = blk->type;
        log.amend.new_ppa = 0;
        log.amend.old_ppa = 0;

        if (oxapp()->log->append_fn (&log, 1))
            log_err ("[transaction (amend): Amend log was NOT appended.]");

        ippa.ppa = blk->ppa.ppa;

        for (pg_i = blk->ppa.g.pg; pg_i < lch->ch->geometry->pg_per_blk; pg_i++){
            ippa.g.pg = pg_i;
            oxapp()->md->invalidate_fn (lch, &ippa, APP_INVALID_PAGE);
        }

        pthread_spin_lock (&tr_cbl_spin);
        TAILQ_INSERT_TAIL(&free_cbl_head, blk, entry);
        pthread_spin_unlock (&tr_cbl_spin);
    }

    return NULL;
}

static int app_transaction_clb_init (void)
{
    uint32_t blk_i;

    TAILQ_INIT (&free_cbl_head);
    TAILQ_INIT (&used_cbl_head);

    for (blk_i = 0; blk_i < APP_TRANSACTION_CLB_LIST; blk_i++) {
        tr_cl_blks[blk_i].used = 0;
        TAILQ_INSERT_HEAD(&free_cbl_head, &tr_cl_blks[blk_i], entry);
    }

    if (pthread_spin_init (&tr_cbl_spin, 0))
        return -1;

    tr_cbl_stop = 0;
    if (pthread_create (&tr_cbl_th, NULL, app_tr_clb_thread, NULL)) {
        pthread_spin_destroy (&tr_cbl_spin);
        return -1;
    }

    return 0;
}

static void app_transaction_clb_exit (void)
{
    tr_cbl_stop = 1;
    pthread_join (tr_cbl_th, NULL);

    pthread_spin_destroy (&tr_cbl_spin);
}

int app_transaction_init (void)
{
    uint32_t tr_i, nch;

    if (!ox_mem_create_type ("OXAPP_TRANSACTION", OX_MEM_APP_TRANS))
        return -1;

    ch = ox_malloc (sizeof (struct app_channel *) * app_nch, OX_MEM_APP_TRANS);
    if (!ch)
        return -1;

    nch = oxapp()->channels.get_list_fn (ch, app_nch);
    if (nch != app_nch)
        goto FREE_CH;

    transactions = ox_malloc (sizeof (struct app_transaction_t) *
                                    APP_TRANSACTION_COUNT, OX_MEM_APP_TRANS);
    if (!transactions)
        goto FREE_CH;

    for (tr_i = 0; tr_i < APP_TRANSACTION_COUNT; tr_i++) {
        transactions[tr_i].entries = ox_calloc (APP_TRANSACTION_LOG_COUNT,
                        sizeof (struct app_transaction_log), OX_MEM_APP_TRANS);

        if (!transactions[tr_i].entries)
            goto FREE_TR;

        if (pthread_spin_init (&transactions[tr_i].spin, 0)) {
            ox_free (transactions[tr_i].entries, OX_MEM_APP_TRANS);
            goto FREE_TR;
        }
    }

    if (pthread_spin_init (&tr_spin, 0))
        goto FREE_TR;

    if (pthread_mutex_init (&tr_mutex, NULL))
        goto SPIN;

    if (app_transaction_clb_init ())
        goto MUTEX;

    ch_geo = ch[0]->ch->geometry;

    TAILQ_INIT (&free_tr_head);
    TAILQ_INIT (&used_tr_head);

    for (tr_i = 0; tr_i < APP_TRANSACTION_COUNT; tr_i++) {
        transactions[tr_i].tid = tr_i + 1;
        TAILQ_INSERT_TAIL(&free_tr_head, &transactions[tr_i], entry);
    }

    log_info ("[ox-app: Transaction Framework started.\n");

    return 0;

MUTEX:
    pthread_mutex_destroy (&tr_mutex);
SPIN:
    pthread_spin_destroy (&tr_spin);
FREE_TR:
    while (tr_i) {
        tr_i--;
        pthread_spin_destroy (&transactions[tr_i].spin);
        ox_free (transactions[tr_i].entries, OX_MEM_APP_TRANS);
    }
    ox_free (transactions, OX_MEM_APP_TRANS);
FREE_CH:
    ox_free (ch, OX_MEM_APP_TRANS);
    return -1;
}

void app_transaction_exit (void) {
    uint32_t tr_i;
    struct app_transaction_t *tr;

    while (!TAILQ_EMPTY(&used_tr_head)) {
        tr = TAILQ_FIRST(&used_tr_head);
        if (tr)
            TAILQ_REMOVE(&used_tr_head, tr, entry);
    }
    while (!TAILQ_EMPTY(&free_tr_head)) {
        tr = TAILQ_FIRST(&free_tr_head);
        if (tr)
            TAILQ_REMOVE(&free_tr_head, tr, entry);
    }

    app_transaction_clb_exit ();

    pthread_mutex_destroy (&tr_mutex);
    pthread_spin_destroy (&tr_spin);

    for (tr_i = 0; tr_i < APP_TRANSACTION_COUNT; tr_i++) {
        pthread_spin_destroy (&transactions[tr_i].spin);
        ox_free (transactions[tr_i].entries, OX_MEM_APP_TRANS);
    }

    ox_free (transactions, OX_MEM_APP_TRANS);
    ox_free (ch, OX_MEM_APP_TRANS);

    log_info ("[ox-app: Transaction Framework stopped.\n");
}
