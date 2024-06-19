/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX-App Flash Translation Layer (Log Management)
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

#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <sched.h>
#include <sys/queue.h>
#include <string.h>
#include <libox.h>
#include <ox-mq.h>
#include <ox-app.h>

#define APP_LOG_BUF_SZ      4096    /* Buffer entries */
#define APP_LOG_TRUNC_SZ    511     /* Standard flush size (1 flash page) */
#define APP_LOG_ASYNCH_TO   1000000
#define APP_LOG_MQ_SZ       512

#define APP_LOG_ASYNCH  0
#define APP_LOG_SYNCH   1

struct app_log_buf_entry {
    struct app_log_entry              log;
    CIRCLEQ_ENTRY(app_log_buf_entry)  entry;
};

struct app_log_flushed_pg {
    struct app_channel *lch;
    struct nvm_ppa_addr ppa;
    LIST_ENTRY (app_log_flushed_pg) entry;
};

struct app_log_ctrl {
    struct app_log_buf_entry     *entries;
    struct app_log_buf_entry     *append_ptr;   /* Circular Write pointer*/
    struct app_log_buf_entry     *flush_ptr;    /* Circular Read pointer */
    pthread_spinlock_t            append_spin;
    pthread_mutex_t               log_mutex;
    pthread_mutex_t               flush_mutex;
    struct nvm_ppa_addr           phy_log_tail;
    struct nvm_ppa_addr           phy_log_head;
    struct app_prov_ppas          next_ppa[3];
    struct nvm_io_data           *flush_buf;
    struct ox_mq                 *log_mq;
    uint16_t                      logs_per_pg;
    uint64_t                      cur_ts;       /* Last flushed timestamp */
    uint64_t                      cur_ts_aux;
    uint8_t                       running;
    CIRCLEQ_HEAD (circ_buf, app_log_buf_entry)      log_buf;
    LIST_HEAD (flushed_pages, app_log_flushed_pg)  flushed_pgs;
};

static struct app_log_ctrl   log_ctrl;
extern uint16_t              app_nch;

static int appftl_log_renew_ppa (uint32_t index)
{
    struct app_prov_ppas *ppas, *lppa;

    if (index >= 3)
        return -1;

    lppa = &log_ctrl.next_ppa[index];

    ppas = oxapp()->gl_prov->new_fn (1, APP_LINE_META);
    if (!ppas)
        return -1;

    memcpy (lppa, ppas, sizeof (struct app_prov_ppas));

    lppa->ppa = ox_calloc (lppa->nppas, sizeof (struct nvm_ppa_addr),
                                                             OX_MEM_OXBLK_LOG);
    if (!lppa->ppa)
        goto FREE_PROV;

    lppa->ch = ox_calloc (app_nch, sizeof (struct app_channel *),
                                                             OX_MEM_OXBLK_LOG);
    if (!lppa->ch)
        goto FREE_PPA;

    memcpy (lppa->ppa, ppas->ppa, lppa->nppas * sizeof (struct nvm_ppa_addr));
    memcpy (lppa->ch, ppas->ch, app_nch * sizeof (struct app_channel *));

    oxapp()->gl_prov->free_fn (ppas);

    return 0;

FREE_PPA:
    ox_free (lppa->ppa, OX_MEM_OXBLK_LOG);
FREE_PROV:
    oxapp()->gl_prov->free_fn (ppas);
    return -1;
}

static void appftl_log_free_ppa (struct app_prov_ppas *lppa, uint8_t log_it)
{
    struct app_log_entry log;
    struct timespec ts;
    uint64_t ns;

    if (log_it) {
        GET_NANOSECONDS (ns, ts);
        memset (&log, 0x0, sizeof (struct app_log_entry));
        log.type = APP_LOG_ABORT_W;
        log.ts = ns;
        log.amend.ppa = lppa->ppa[0].ppa;

        if (oxapp()->log->append_fn (&log, 1))
            log_err ("[log: Fix block log NOT appended during shutdown.]");
    }

    ox_free (lppa->ch, OX_MEM_OXBLK_LOG);
    ox_free (lppa->ppa, OX_MEM_OXBLK_LOG);
}

static int appftl_log_append (struct app_log_entry *list, uint16_t size)
{
    int lid = 0;
    struct app_log_buf_entry *lnext;
    struct app_log_ctrl *lc = &log_ctrl;

    if (!lc->running)
        return 0;

 /*   if (size > APP_LOG_TRUNC_SZ) {
        log_err ("[log: Append failed. Log list exceed %d entries.]",
                                                             APP_LOG_TRUNC_SZ);
        return -1;
    }
*/
    pthread_mutex_lock (&lc->log_mutex);

    while (lid < size) {
        pthread_spin_lock (&lc->append_spin);

        lnext = CIRCLEQ_LOOP_NEXT(&lc->log_buf, lc->append_ptr, entry);

        /* If the buffer will be full, flush it */
        if (lnext == lc->flush_ptr) {
            pthread_spin_unlock (&lc->append_spin);
            if (oxapp()->log->flush_fn (APP_LOG_TRUNC_SZ, NULL, NULL)) {
                pthread_mutex_unlock (&lc->log_mutex);
                return -1;
            }

            continue;
        }

        memcpy (&lc->append_ptr->log, &list[lid], sizeof(struct app_log_entry));
        lc->append_ptr = lnext;

    //    printf (" LOG (%d). ti: %lu, ts: %lu. LBA: %lu, PPA: %lu\n", list[lid].type, list[lid].write.tid, list[lid].ts,
    //            list[lid].write.lba, list[lid].write.new_ppa);

        pthread_spin_unlock (&lc->append_spin);

        lid++;
    }

    pthread_mutex_unlock (&lc->log_mutex);
    return 0;
}

static int appftl_log_get_ppa (struct app_prov_ppas *ppa)
{
    struct app_prov_ppas tmp[3];

    memcpy (tmp, log_ctrl.next_ppa, 3 * sizeof (struct app_prov_ppas));

    if (appftl_log_renew_ppa (2))
        return -1;

    memcpy (ppa, &tmp[0], sizeof (struct app_prov_ppas));
    memcpy (&log_ctrl.next_ppa[0], &tmp[1], 2 * sizeof (struct app_prov_ppas));

    return 0;
}

static int appftl_log_flush_pg (struct app_prov_ppas *uppa,
                    struct app_prov_ppas *wppa, struct app_transaction_pad *pad)
{
    struct app_log_entry *lp;
    struct nvm_ppa_addr *prev, *next;
    struct app_channel *lch;
    struct app_log_entry *offset;
    struct app_log_ctrl *lc = &log_ctrl;
    uint16_t retries = 0, log_i, sec, pl;

    memcpy (wppa, uppa, sizeof (struct app_prov_ppas));

RETRY:
    lch = oxapp()->channels.get_fn (wppa->ppa[0].g.ch);
    if (!lch)
        return -1;

    app_ch_inc_thread (lch);

    /* Try to write twice (log entry contains 3 possible next pointers) */
    if (ftl_pg_io_switch (lch->ch, MMGR_WRITE_PG,
            (void **) log_ctrl.flush_buf->pl_vec, &wppa->ppa[0], NVM_IO_NORMAL)) {

        if (app_transaction_close_blk (&wppa->ppa[0], APP_LINE_META))
            log_info ("[log (write amend): block is not closed.]");

        app_ch_dec_thread (lch);

        log_err ("[log: Page write failed. Retries: %d\n", retries + 1);

        if (lc->running && retries < 2) {
            appftl_log_free_ppa (wppa, 0);

            if (appftl_log_get_ppa (wppa))
                return -1;

            /* Fix the log pointer next locations */
            offset = (struct app_log_entry *) lc->flush_buf->pl_vec[0];
            offset->pointer.next[0] = lc->next_ppa[0].ppa[0].ppa;
            offset->pointer.next[1] = lc->next_ppa[1].ppa[0].ppa;
            offset->pointer.next[2] = lc->next_ppa[2].ppa[0].ppa;

            /* Fix the user padding logs */
            for (log_i = 0; log_i < pad->padded; log_i++) {
                prev = (struct nvm_ppa_addr *) &pad->rsv[log_i]->write.new_ppa;
                sec = prev->g.sec;
                pl = prev->g.pl;
                prev->ppa = wppa->ppa[0].ppa;
                prev->g.pl = pl;
                prev->g.sec = sec;
            }

            retries++;
            goto RETRY;
        }

        appftl_log_free_ppa (wppa, 0);
        return -1;
    }

    if (APP_DEBUG_LOG) {
        lp = (struct app_log_entry *) log_ctrl.flush_buf->buf;
        prev = (struct nvm_ppa_addr *) &lp->pointer.prev[0];
        next = (struct nvm_ppa_addr *) &lp->pointer.next[0];
        printf (" LOG Page (flush): PPA (%d/%d/%d/%d), "
            "PREV (%d/%d/%d/%d), NEXT[0] (%d/%d/%d/%d)\n",
            wppa->ppa->g.ch, wppa->ppa->g.lun, wppa->ppa->g.blk,wppa->ppa->g.pg,
            prev->g.ch, prev->g.lun, prev->g.blk, prev->g.pg,
            next->g.ch, next->g.lun, next->g.blk, next->g.pg);
    }

    app_ch_dec_thread (lch);

    return 0;
}

static uint16_t appftl_log_flush_padding (uint16_t processed,
                   struct app_transaction_pad *pad, struct app_prov_ppas *uppa)
{
    struct app_log_entry log, *offset;
    uint16_t slots_left, real_pad = 0, sec, pl_sec, l_sec, l_pl, last_proc;
    uint16_t aligned = 0, pad_i, pl, off_id;
    struct app_log_ctrl *lc = &log_ctrl;
    struct nvm_mmgr_geometry *g = oxapp()->channels.get_fn (0)->ch->geometry;

    l_sec = lc->logs_per_pg / g->sec_per_pl_pg;
    l_pl = lc->logs_per_pg / g->n_of_planes;
    last_proc = processed;
    
ALIGN:
    /* Align to the flash sector */
    if (processed % l_sec != 0)
        processed += l_sec - (processed % l_sec);

    if (processed == lc->logs_per_pg) {
        if (aligned)
            ((struct app_sec_oob *)lc->flush_buf->oob_vec
                             [g->sec_per_pl_pg - 1])->pg_type = APP_PG_PADDING;
        goto RETURN;
    }
    
    pl_sec = processed / l_sec;

    /* We also have to log the user padding, so, if these logs traverse
     * a new sector, a new alignment is needed */
    if (!aligned) {
        slots_left = l_sec - (last_proc % l_sec);
        real_pad = MIN (slots_left, pad->count);
        real_pad = MIN (g->sec_per_pl_pg - pl_sec, real_pad);        
        processed = last_proc + real_pad;
        aligned++;

        goto ALIGN;
    }
    
    /* Copy the user padding to the write buffer */
    for (pad_i = 0; pad_i < real_pad; pad_i++) {
        pl_sec = processed / l_sec;
        pl = processed / l_pl;
        sec = pl_sec % g->sec_per_pg;
    
        ((struct app_sec_oob *)lc->flush_buf->oob_vec[pl_sec])->
                                                 pg_type = APP_PG_NAMESPACE;

        memcpy (lc->flush_buf->sec_vec[pl][sec], pad->pad[pad_i], g->sec_size);
        pad->log[pad_i].ppa.ppa = uppa->ppa->ppa;
        pad->log[pad_i].ppa.g.pl = pl;
        pad->log[pad_i].ppa.g.sec = sec;
        pad->padded++;

        processed += l_sec;
    }

    /* Mark possible sectors left with normal padding */
    for (pl_sec = processed / l_sec; pl_sec < g->sec_per_pl_pg; pl_sec++) {
        ((struct app_sec_oob *)lc->flush_buf->oob_vec[pl_sec])->
                                                    pg_type = APP_PG_PADDING;
        processed += l_sec;
    }
    
    /* Add the user padding logs */
    /* mark the sector as log, if this log is the first in the sector */
    for (pad_i = 0; pad_i < real_pad; pad_i++) {
        pl_sec = last_proc / l_sec;
        pl = last_proc / l_pl;

        if ( last_proc % l_sec == 0 )
            ((struct app_sec_oob *)lc->flush_buf->oob_vec[pl_sec])->
                                                        pg_type = APP_PG_LOG;

        off_id = last_proc % l_pl;
        offset = ((struct app_log_entry *) lc->flush_buf->pl_vec[pl]) + off_id;

        memset (&log, 0x0, sizeof (struct app_log_entry));
        log.type = APP_LOG_WRITE;
        log.ts = pad->log[pad_i].ts;
        log.write.tid = pad->log[pad_i].tid;
        log.write.lba = pad->log[pad_i].lba;
        log.write.new_ppa = pad->log[pad_i].ppa.ppa;

        memcpy (offset, &log, sizeof (struct app_log_entry));
        pad->rsv[pad_i] = offset;

        last_proc++;
    }

RETURN:
    if (APP_DEBUG_LOG)
        printf ("[ox-blk (log): Padded log page with %d logs.]\n", last_proc);

    if (APP_DEBUG_LOG && real_pad)
        printf (" LOG (flush): User padding -> %d sectors.\n", real_pad);
    
    if (processed != lc->logs_per_pg)
        log_info ("[ox-blk (log): BUG: Processed entries unstable. %d\n",
                                                                    processed);

    return processed;
}

static uint16_t appftl_log_flush_fill_pg (struct app_transaction_pad *pad,
        struct app_log_buf_entry *truncate,
        struct app_log_buf_entry **read_ptr, uint16_t *flushed, uint16_t size,
        struct app_prov_ppas *uppa)
{
    uint16_t processed, pl, pl_sec, l_sec, off_id;
    struct app_log_buf_entry *buf_ent;    
    struct app_log_entry *offset;
    struct timespec ts;
    struct app_log_ctrl *lc = &log_ctrl;
    struct nvm_mmgr_geometry *g = oxapp()->channels.get_fn (0)->ch->geometry;

    memset (lc->flush_buf->buf, 0x0, lc->flush_buf->buf_sz);

    l_sec = lc->logs_per_pg / g->sec_per_pl_pg;

    /* Mark the first sector as log */
    ((struct app_sec_oob *)lc->flush_buf->oob_vec[0])->pg_type = APP_PG_LOG;

    /* Create the log pointer (previous and next log page) */
    offset = (struct app_log_entry *) lc->flush_buf->pl_vec[0];
    memset (offset, 0x0, sizeof (struct app_log_entry));
    GET_NANOSECONDS (offset->ts, ts);
    offset->type = APP_LOG_POINTER;
    offset->pointer.prev[0] = lc->phy_log_tail.ppa;
    offset->pointer.next[0] = (lc->running) ? lc->next_ppa[0].ppa->ppa : 0;
    offset->pointer.next[1] = (lc->running) ? lc->next_ppa[1].ppa->ppa : 0;
    offset->pointer.next[2] = (lc->running) ? lc->next_ppa[2].ppa->ppa : 0;
    processed = 1;

    /* Fill up flash page with log entries */
    *read_ptr = lc->flush_ptr;
    do {
        pl = processed / (lc->logs_per_pg / g->n_of_planes);
        pl_sec = processed / l_sec;        

        /* If page is not full, fill it with user data or padding */
        if ((*read_ptr == truncate) || (*flushed + processed >= size)) {

            processed = appftl_log_flush_padding (processed, pad, uppa);
            continue;
            
        }

        /* mark the sector as log, if this log is the first in the sector */
        if ( processed % l_sec == 0)
            ((struct app_sec_oob *)lc->flush_buf->oob_vec[pl_sec])->
                                                       pg_type = APP_PG_LOG;

        off_id = processed % (lc->logs_per_pg / g->n_of_planes);
        offset = ((struct app_log_entry *) lc->flush_buf->pl_vec[pl]) +
                                                                        off_id;
        memcpy (offset, &(*read_ptr)->log, sizeof (struct app_log_entry));

        buf_ent = *read_ptr;
        *read_ptr = CIRCLEQ_LOOP_NEXT(&lc->log_buf, buf_ent, entry);

        processed++;
    } while (processed < lc->logs_per_pg);

    lc->cur_ts_aux = offset->ts;

    return processed;
}

static int appftl_log_flush_buffer (uint16_t size,
                              struct app_transaction_pad *pad_u, uint8_t synch)
{
    uint16_t flushed = 0, processed, pl, sec, pad_id;
    struct app_log_buf_entry *truncate, *read_ptr = NULL;
    struct app_log_ctrl *lc = &log_ctrl;
    struct app_prov_ppas uppa, wppa;
    struct app_transaction_pad pad_f;
    struct app_transaction_pad *pad;
    struct app_log_flushed_pg *flushed_pg;
    struct nvm_mmgr_geometry *g = oxapp()->channels.get_fn (0)->ch->geometry;

    pad = (pad_u) ? pad_u : &pad_f;
    if (!pad_u)
        memset (pad, 0x0, sizeof (struct app_transaction_pad));
    
    if (pad->count > g->sec_per_pl_pg - 1) {
        log_err ("[log: Pad is larger than the page minus 1 sector.]");
        return -1;
    }

    if (pad->count) {
        pad->rsv = ox_malloc (sizeof(struct app_log_entry *) * pad->count,
                                                             OX_MEM_OXBLK_LOG);
        if (!pad->rsv)
            return -1;
    }

    pad->padded = 0;
    size = (!size) ? APP_LOG_BUF_SZ - 1 : size;
    for (pad_id = 0; pad_id < pad->count; pad_id++)
        pad->log[pad_id].ppa.ppa = 0;

    if (synch == APP_LOG_SYNCH)
        pthread_mutex_lock (&lc->flush_mutex);

    /* Flush circular buffer up to the current append pointer */
    /* Truncate is defined with 'flush_mutex' locked. We can't guarantee
        that waiting threads will execute in the same sequence as 'truncate'
        was assigned to them, which would lead to unstable ring buffer. */
    pthread_spin_lock (&lc->append_spin);
    truncate = lc->append_ptr;
    pthread_spin_unlock (&lc->append_spin);

    while ((lc->flush_ptr != truncate) && (flushed < size)) {

        /* If it is not a shutdown, get the physical address */
        if (lc->running) {
            if (appftl_log_get_ppa (&uppa)) {
                log_err ("[log: Get PPA error. Buffer is not flushed.]\n");
                goto ERR;
            }
        } else {
            memcpy (&uppa, &log_ctrl.next_ppa[0], sizeof(struct app_prov_ppas));
        }

        processed = appftl_log_flush_fill_pg (pad, truncate, &read_ptr,
                                                        &flushed, size, &uppa);

        if (appftl_log_flush_pg (&uppa, &wppa, pad)) {

            /* TODO: In case of write error even after retrying, the FTL should
               stop writing and make a new checkpoint. It truncates the log and
               fix the log chain.*/

            log_err ("[log FATAL error: Flush FAILED. Log chain is broken.]\n");
            goto ERR;
        }

        lc->cur_ts = lc->cur_ts_aux;

        /* PPA in wppa is equal to PPA in ppa if the the write succeed
           in the first try, and different if the function retried */
        if (wppa.ppa->ppa != uppa.ppa->ppa) {

            /* Correct the return PPAs in case of write retries */
            for (pad_id = 0; pad_id < pad->padded; pad_id++) {
                sec = pad->log[pad_id].ppa.g.sec;
                pl  = pad->log[pad_id].ppa.g.pl;
                pad->log[pad_id].ppa.ppa = wppa.ppa->ppa;
                pad->log[pad_id].ppa.g.pl = pl;
                pad->log[pad_id].ppa.g.sec = sec;
            }

        }

        if (!lc->phy_log_head.ppa)
            lc->phy_log_head.ppa = wppa.ppa->ppa;
        lc->phy_log_tail.ppa = wppa.ppa->ppa;

        pthread_spin_lock (&lc->append_spin);
        lc->flush_ptr = read_ptr;
        pthread_spin_unlock (&lc->append_spin);

        flushed_pg = ox_malloc (sizeof (struct app_log_flushed_pg),
                                                            OX_MEM_OXBLK_LOG);
        if (!flushed_pg)
            log_err ("[log: Flushed page was not added to flushed list]");
        else {
            flushed_pg->ppa.ppa = wppa.ppa->ppa;
            flushed_pg->lch = oxapp()->channels.get_fn (wppa.ppa->g.ch);
            if (flushed_pg->lch)
                LIST_INSERT_HEAD (&lc->flushed_pgs, flushed_pg, entry);
            else
                log_err ("[log: Flushed page not added to flushed list]");
        }

        appftl_log_free_ppa (&wppa, 0);

        flushed += processed;
    }

    if (pad->count)
        ox_free (pad->rsv, OX_MEM_OXBLK_LOG);

    if (synch == APP_LOG_SYNCH)
        pthread_mutex_unlock (&lc->flush_mutex);

    return 0;

ERR:
    /* Here we don't free uppa because flush_pg already did if it failed */
    if (pad->count)
        ox_free (pad->rsv, OX_MEM_OXBLK_LOG);

    if (synch == APP_LOG_SYNCH)
        pthread_mutex_unlock (&lc->flush_mutex);

    return -1;
}

/* size: Maximum number of log entries to be flushed
 * pad: Used only if 'cb' is NULL. This is a buffer struct to be used as padding.
 *      The function will fill the PPAs where the pad buffer has been flushed in
 *      the logs inside this struct. If a PPA in the vector is 0x0, the data has
 *      NOT been used as padding.
 * cb: Callback for asynchronous flush. 'pad' is discarded.
 */
static int appftl_log_flush (uint16_t size, struct app_transaction_pad *pad_u,
                                                        struct nvm_callback *cb)
{
    /* If callback is not NULL, the call is asynchronous */
    if (cb)
        return ox_mq_submit_req(log_ctrl.log_mq, 0, (void *) cb);

    return appftl_log_flush_buffer (size, pad_u, APP_LOG_SYNCH);
}

static void appftl_log_sq (struct ox_mq_entry *req)
{
    struct app_log_ctrl *lc = &log_ctrl;
    struct nvm_callback *cb = (struct nvm_callback *) req->opaque;

    if (cb->ts <= lc->cur_ts)
        goto COMPLETE;

    pthread_mutex_lock (&lc->flush_mutex);

    if (cb->ts <= lc->cur_ts) {
        pthread_mutex_unlock (&lc->flush_mutex);
        goto COMPLETE;
    }
    if (appftl_log_flush_buffer (0, NULL, APP_LOG_ASYNCH))
        log_err ("[log: Asynch flush not completed. cb %lu\n]", cb->ts);

    pthread_mutex_unlock (&lc->flush_mutex);

COMPLETE:
    if (ox_mq_complete_req (lc->log_mq, req))
        log_err ("[log: Asynch completion not posted.\n]");
}

static void appftl_log_cq (void *opaque)
{
    struct nvm_callback *cb = (struct nvm_callback *) opaque;

    cb->cb_fn (opaque);
}

static void appftl_log_to (void **opaque, int counter)
{
    while (counter) {
        counter--;
        log_err ("[log: Asynchronous callback timeout.]\n");
    }
}

static void appftl_log_truncate (void)
{
    struct app_log_flushed_pg *head, *pg;
    uint32_t truncate = 0;

    pthread_mutex_lock (&log_ctrl.flush_mutex);

    head = LIST_FIRST (&log_ctrl.flushed_pgs);
    while (head) {
        if (head->ppa.ppa == log_ctrl.phy_log_head.ppa)
            truncate++;

        pg = LIST_NEXT (head, entry);
        if (truncate) {
            if (pg) {
                LIST_REMOVE (pg, entry);
                oxapp()->md->invalidate_fn(pg->lch, &pg->ppa, APP_INVALID_PAGE);
                ox_free (pg, OX_MEM_OXBLK_LOG);
            } else
                head = pg;
        } else
            head = pg;
    }

    pthread_mutex_unlock (&log_ctrl.flush_mutex);

    truncate = (truncate) ? truncate - 1 : 0;
    if (APP_DEBUG_LOG)
        printf ("[ox-blk (log): Truncated log pages: %d]\n", truncate);
}

static void log_stats_fill_row (struct oxmq_output_row *row, void *opaque)
{
    struct nvm_callback *cb;

    cb = (struct nvm_callback *) opaque;

    row->lba = cb->ts;
    row->ch = 0;
    row->lun = 0;
    row->blk = 0;
    row->pg = 0;
    row->pl = 0;
    row->sec = 0;
    row->type = 0;
    row->failed = 0;
    row->datacmp = 0;
    row->size = 0;
}

struct ox_mq_config log_mq_config = {
    .name       = "LOG",
    .n_queues   = 1,
    .q_size     = APP_LOG_MQ_SZ,
    .sq_fn      = appftl_log_sq,
    .cq_fn      = appftl_log_cq,
    .to_fn      = appftl_log_to,
    .output_fn  = log_stats_fill_row,
    .to_usec    = APP_LOG_ASYNCH_TO,
    .flags      = (OX_MQ_TO_COMPLETE | OX_MQ_CPU_AFFINITY)
};

static int appftl_log_init (void)
{
    int eid, qid;
    struct app_log_buf_entry *le;
    struct app_log_ctrl *lc = &log_ctrl;

    if (!ox_mem_create_type ("OXBLK_LOG", OX_MEM_OXBLK_LOG))
        return -1;

    CIRCLEQ_INIT(&lc->log_buf);
    LIST_INIT(&lc->flushed_pgs);

    lc->entries = ox_calloc (1, sizeof(struct app_log_buf_entry) *
                                              APP_LOG_BUF_SZ, OX_MEM_OXBLK_LOG);
    if (!lc->entries)
        return -1;

    for (eid = 0; eid < APP_LOG_BUF_SZ; eid++)
        CIRCLEQ_INSERT_TAIL(&lc->log_buf, &lc->entries[eid], entry);

    if (pthread_spin_init (&lc->append_spin, 0))
        goto FREE_ENTRIES;

    if (pthread_mutex_init (&lc->log_mutex, NULL))
        goto APPEND_SPIN;

    if (pthread_mutex_init (&lc->flush_mutex, NULL))
        goto LOG_MUTEX;

    lc->flush_buf = ftl_alloc_pg_io (oxapp()->channels.get_fn(0)->ch);
    if (!lc->flush_buf)
        goto FLUSH_MUTEX;

    /* Set thread affinity, if enabled */
    for (qid = 0; qid < log_mq_config.n_queues; qid++) {
        log_mq_config.sq_affinity[qid] = 0;
	log_mq_config.cq_affinity[qid] = 0;

#if OX_TH_AFFINITY
        /* Set threads to CPUs 5 */
        for (qid = 0; qid < log_mq_config.n_queues; qid++) {
            log_mq_config.sq_affinity[qid] = 0;
            log_mq_config.sq_affinity[qid] |= ((uint64_t) 1 << 5);

            log_mq_config.cq_affinity[qid] = 0;
            log_mq_config.cq_affinity[qid] |= ((uint64_t) 1 << 5);
        }
#endif /* OX_TH_AFFINITY */
    }

    lc->log_mq = ox_mq_init (&log_mq_config);
    if (!lc->log_mq)
        goto FREE_BUF;

    for (eid = 0; eid < 3; eid++)
        if (appftl_log_renew_ppa (eid))
            goto DESTROY_MQ;

    lc->append_ptr = lc->flush_ptr = CIRCLEQ_FIRST(&lc->log_buf);

    lc->phy_log_tail.ppa = lc->phy_log_head.ppa = log_ctrl.next_ppa[0].ppa->ppa;

    lc->cur_ts = 0;
    lc->cur_ts_aux = 0;

    lc->logs_per_pg = lc->flush_buf->ch->geometry->pl_pg_size /
                                                 sizeof (struct app_log_entry);

    lc->running = 1;

    log_info("    [ox-blk: Log Management started.]\n");

    return 0;

DESTROY_MQ:
    while (eid) {
        eid--;
        appftl_log_free_ppa (&lc->next_ppa[eid], 0);
    }
    ox_mq_destroy (lc->log_mq);
FREE_BUF:
    ftl_free_pg_io (lc->flush_buf);
FLUSH_MUTEX:
    pthread_mutex_destroy (&lc->flush_mutex);
LOG_MUTEX:
    pthread_mutex_destroy (&lc->log_mutex);
APPEND_SPIN:
    pthread_spin_destroy (&lc->append_spin);
FREE_ENTRIES:
    while (!CIRCLEQ_EMPTY(&lc->log_buf)) {
        le = CIRCLEQ_FIRST(&lc->log_buf);
        if (le)
            CIRCLEQ_REMOVE(&lc->log_buf, le, entry);
    }
    ox_free (lc->entries, OX_MEM_OXBLK_LOG);
    return -1;
}

static void appftl_log_exit (void)
{
    int eid;
    struct app_log_buf_entry *le;
    struct app_log_flushed_pg *pg;
    struct app_log_ctrl *lc = &log_ctrl;

    /* Free and log the last 2 allocated pages, keep first and flush the logs */
    for (eid = 1; eid < 3; eid++)
        appftl_log_free_ppa (&lc->next_ppa[eid], 1);

    lc->running = 0;
    oxapp()->log->flush_fn (0, NULL, NULL);

    ox_mq_destroy (lc->log_mq);
    ftl_free_pg_io (lc->flush_buf);
    pthread_mutex_destroy (&lc->flush_mutex);
    pthread_mutex_destroy (&lc->log_mutex);
    pthread_spin_destroy (&lc->append_spin);

    while (!CIRCLEQ_EMPTY(&lc->log_buf)) {
        le = CIRCLEQ_FIRST(&lc->log_buf);
        if (le)
            CIRCLEQ_REMOVE(&lc->log_buf, le, entry);
    }
    ox_free (lc->entries, OX_MEM_OXBLK_LOG);

    while (!LIST_EMPTY(&lc->flushed_pgs)) {
        pg = LIST_FIRST(&lc->flushed_pgs);
        LIST_REMOVE (pg, entry);
        ox_free (pg, OX_MEM_OXBLK_LOG);
    }

    log_info("    [appnvm: Log Management stopped.]\n");
}

static struct app_log oxblk_log = {
    .mod_id      = OXBLK_LOG,
    .name        = "OX-BLOCK-LOG",
    .init_fn     = appftl_log_init,
    .exit_fn     = appftl_log_exit,
    .append_fn   = appftl_log_append,
    .flush_fn    = appftl_log_flush,
    .truncate_fn = appftl_log_truncate
};

void oxb_log_register (void) {
    app_mod_register (APPMOD_LOG, OXBLK_LOG, &oxblk_log);
}

uint64_t get_log_tail (void) {
    struct nvm_ppa_addr tail;

    pthread_mutex_lock (&log_ctrl.flush_mutex);
    tail.ppa = log_ctrl.phy_log_tail.ppa;
    pthread_mutex_unlock (&log_ctrl.flush_mutex);

    return tail.ppa;
}

void set_log_head (uint64_t new) {
    if (new)
        log_ctrl.phy_log_head.ppa = new;
}
