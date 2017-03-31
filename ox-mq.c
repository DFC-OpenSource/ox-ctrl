/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - Multi-Queue Support for Parallel I/O
 *
 * Copyright (C) 2016, IT University of Copenhagen. All rights reserved.
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 *
 * Funding support provided by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *  this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/queue.h>
#include "include/ox-mq.h"
#include "include/ssd.h"

void ox_mq_show_stats (struct ox_mq *mq)
{
    int i;
    struct ox_mq_queue *q;

    for (i = 0; i < mq->config->n_queues; i++) {
        q = &mq->queues[i];
        log_info ("Q %d. SF: %d, SU: %d, SW: %d, CF: %d, CU: %d\n", i,
                atomic_read(&q->stats.sq_free),
                atomic_read(&q->stats.sq_used),
                atomic_read(&q->stats.sq_wait),
                atomic_read(&q->stats.cq_free),
                atomic_read(&q->stats.cq_used));
    }
    log_info ("EXT %d, TO: %d, TO_BACK: %d\n",
                atomic_read(&mq->stats.ext_list),
                atomic_read(&mq->stats.timeout),
                atomic_read(&mq->stats.to_back));
}

static void ox_mq_init_stats (struct ox_mq_stats *stats)
{
    stats->cq_free.counter = ATOMIC_INIT_RUNTIME(0);
    stats->cq_used.counter = ATOMIC_INIT_RUNTIME(0);
    stats->sq_free.counter = ATOMIC_INIT_RUNTIME(0);
    stats->sq_used.counter = ATOMIC_INIT_RUNTIME(0);
    stats->sq_wait.counter = ATOMIC_INIT_RUNTIME(0);
    stats->ext_list.counter = ATOMIC_INIT_RUNTIME(0);
    stats->timeout.counter = ATOMIC_INIT_RUNTIME(0);
    stats->to_back.counter = ATOMIC_INIT_RUNTIME(0);
}

static void ox_mq_destroy_sq (struct ox_mq_queue *q)
{
    pthread_mutex_destroy (&q->sq_free_mutex);
    pthread_mutex_destroy (&q->sq_used_mutex);
    pthread_mutex_destroy (&q->sq_wait_mutex);
    pthread_mutex_destroy (&q->sq_cond_m);
    pthread_cond_destroy (&q->sq_cond);
}

static void ox_mq_destroy_cq (struct ox_mq_queue *q)
{
    pthread_mutex_destroy (&q->cq_free_mutex);
    pthread_mutex_destroy (&q->cq_used_mutex);
    pthread_mutex_destroy (&q->cq_cond_m);
    pthread_cond_destroy (&q->cq_cond);
}

static int ox_mq_init_sq (struct ox_mq_queue *q, uint32_t size)
{
    TAILQ_INIT (&q->sq_free);
    TAILQ_INIT (&q->sq_used);
    TAILQ_INIT (&q->sq_wait);
    pthread_mutex_init (&q->sq_free_mutex, NULL);
    pthread_mutex_init (&q->sq_used_mutex, NULL);
    pthread_mutex_init (&q->sq_wait_mutex, NULL);
    pthread_mutex_init (&q->sq_cond_m, NULL);
    pthread_cond_init (&q->sq_cond, NULL);

    q->sq_entries = malloc (sizeof (struct ox_mq_entry) * size);
    if (!q->sq_entries)
        goto CLEAN;

    memset (q->sq_entries, 0, sizeof (struct ox_mq_entry) * size);

    return 0;

CLEAN:
    ox_mq_destroy_sq (q);
    return -1;
}

static int ox_mq_init_cq (struct ox_mq_queue *q, uint32_t size)
{
    TAILQ_INIT (&q->cq_free);
    TAILQ_INIT (&q->cq_used);
    pthread_mutex_init (&q->cq_free_mutex, NULL);
    pthread_mutex_init (&q->cq_used_mutex, NULL);
    pthread_mutex_init (&q->cq_cond_m, NULL);
    pthread_cond_init (&q->cq_cond, NULL);

    q->cq_entries = malloc (sizeof (struct ox_mq_entry) * size);
    if (!q->cq_entries)
        goto CLEAN;

    memset (q->cq_entries, 0, sizeof (struct ox_mq_entry) * size);

    return 0;

CLEAN:
    ox_mq_destroy_cq (q);
    return -1;
}

static int ox_mq_init_queue (struct ox_mq_queue *q, uint32_t size,
                                        ox_mq_sq_fn *sq_fn, ox_mq_cq_fn *cq_fn)
{
    int i;

    if (!sq_fn || !cq_fn)
        return -1;

    q->sq_fn = sq_fn;
    q->cq_fn = cq_fn;

    if (ox_mq_init_sq (q, size))
        return -1;

    if (ox_mq_init_cq (q, size))
        goto CLEAN_SQ;

    ox_mq_init_stats(&q->stats);

    for (i = 0; i < size; i++) {
        TAILQ_INSERT_TAIL (&q->sq_free, &q->sq_entries[i], entry);
        atomic_inc(&q->stats.sq_free);
        TAILQ_INSERT_TAIL (&q->cq_free, &q->cq_entries[i], entry);
        atomic_inc(&q->stats.cq_free);
        pthread_mutex_init (&q->sq_entries[i].entry_mutex, NULL);
        pthread_mutex_init (&q->cq_entries[i].entry_mutex, NULL);
    }

    q->running = 1; /* ready */

    return 0;

CLEAN_SQ:
    ox_mq_destroy_sq (q);
    free (q->sq_entries);
    return -1;
}

inline static void ox_mq_reset_entry (struct ox_mq_entry *entry)
{
    entry->status = OX_MQ_FREE;
    entry->opaque = NULL;
    entry->qid = 0;
    memset (&entry->wtime, 0, sizeof (struct timeval));
}

static struct ox_mq_entry *ox_mq_create_ext_entry (struct ox_mq *mq)
{
    struct ox_mq_entry *new_entry;

    new_entry = malloc (sizeof (struct ox_mq_entry));
    if (!new_entry)
        return NULL;

    new_entry->is_ext = 0x1;
    pthread_mutex_init (&new_entry->entry_mutex, NULL);
    ox_mq_reset_entry (new_entry);

    LIST_INSERT_HEAD (&mq->ext_list, new_entry, ext_entry);
    atomic_inc (&mq->stats.ext_list);

    return new_entry;
}

static void ox_mq_free_entry (struct ox_mq *mq, struct ox_mq_entry *entry)
{
    if (entry->is_ext) {
        LIST_REMOVE (entry, ext_entry);
        pthread_mutex_destroy (&entry->entry_mutex);
        free (entry);
        atomic_dec (&mq->stats.ext_list);
    }
}

static void ox_mq_free_queues (struct ox_mq *mq, uint32_t n_queues)
{
    int i, j;
    struct ox_mq_queue *q;

    for (i = 0; i < n_queues; i++) {
        q = &mq->queues[i];
        q->running = 0; /* stop threads */
        for (j = 0; j < mq->config->q_size; j++) {
            pthread_mutex_destroy (&q->sq_entries[j].entry_mutex);
            pthread_mutex_destroy (&q->cq_entries[j].entry_mutex);
        }
        ox_mq_destroy_sq (q);
        free (q->sq_entries);
        ox_mq_destroy_cq (q);
        free (q->cq_entries);
    }
}

#define OX_MQ_ENQUEUE(head, elm, mutex, stat) do {                  \
        pthread_mutex_lock((mutex));                                \
        TAILQ_INSERT_TAIL((head), (elm), entry);                    \
        pthread_mutex_unlock((mutex));                              \
        atomic_inc((stat));                                         \
} while (/*CONSTCOND*/0)

#define OX_MQ_DEQUEUE_H(head, elm, mutex, stats) do {               \
        pthread_mutex_lock((mutex));                                \
        (elm) = TAILQ_FIRST((head));                                \
        TAILQ_REMOVE ((head), (elm), entry);                        \
        pthread_mutex_unlock ((mutex));                             \
        atomic_dec((stats));                                        \
} while (/*CONSTCOND*/0)

#define OX_MQ_DEQUEUE(head, elm, mutex, stats) do {                 \
        pthread_mutex_lock((mutex));                                \
        TAILQ_REMOVE ((head), (elm), entry);                        \
        pthread_mutex_unlock ((mutex));                             \
        atomic_dec((stats));                                        \
} while (/*CONSTCOND*/0)

static void *ox_mq_sq_thread (void *arg)
{
    struct ox_mq_queue *q = (struct ox_mq_queue *) arg;
    struct ox_mq_entry *req;

    while (q->running) {
        pthread_mutex_lock(&q->sq_cond_m);

        if (TAILQ_EMPTY (&q->sq_used))
            pthread_cond_wait(&q->sq_cond, &q->sq_cond_m);

        pthread_mutex_unlock(&q->sq_cond_m);

        OX_MQ_DEQUEUE_H(&q->sq_used, req, &q->sq_used_mutex, &q->stats.sq_used);

        gettimeofday(&req->wtime, NULL);

        req->status = OX_MQ_WAITING;
        OX_MQ_ENQUEUE (&q->sq_wait, req, &q->sq_wait_mutex, &q->stats.sq_wait);

        q->sq_fn (req);
    }

    return NULL;
}

static void *ox_mq_cq_thread (void *arg)
{
    struct ox_mq_queue *q = (struct ox_mq_queue *) arg;
    struct ox_mq_entry *req;
    void *opaque;

    while (q->running) {
        pthread_mutex_lock(&q->cq_cond_m);

        if (TAILQ_EMPTY (&q->cq_used))
            pthread_cond_wait(&q->cq_cond, &q->cq_cond_m);

        pthread_mutex_unlock(&q->cq_cond_m);

        OX_MQ_DEQUEUE_H(&q->cq_used, req, &q->cq_used_mutex, &q->stats.cq_used);
        opaque = req->opaque;
        ox_mq_reset_entry (req);
        OX_MQ_ENQUEUE (&q->cq_free, req, &q->cq_free_mutex, &q->stats.cq_free);

        q->cq_fn (opaque);
    }

    return NULL;
}

static int ox_mq_start_thread (struct ox_mq_queue *q)
{
    if (pthread_create(&q->sq_tid, NULL, ox_mq_sq_thread, q))
        return -1;

    if (pthread_create(&q->cq_tid, NULL, ox_mq_cq_thread, q))
        return -1;

    return 0;
}

int ox_mq_submit_req (struct ox_mq *mq, uint32_t qid, void *opaque)
{
    struct ox_mq_queue *q;
    struct ox_mq_entry *req;
    uint8_t wake = 0;

    if (qid >= mq->config->n_queues)
        return -1;

    q = &mq->queues[qid];

    /* If queue is full, the request is rejected */
    pthread_mutex_lock (&q->sq_free_mutex);
    if (TAILQ_EMPTY (&q->sq_free)) {
        pthread_mutex_unlock (&q->sq_free_mutex);
        return -1;
    }

    req = TAILQ_FIRST (&q->sq_free);
    TAILQ_REMOVE (&q->sq_free, req, entry);
    pthread_mutex_unlock (&q->sq_free_mutex);
    atomic_dec(&q->stats.sq_free);

    req->opaque = opaque;
    req->qid = qid;

    pthread_mutex_lock (&q->sq_used_mutex);
    if (TAILQ_EMPTY (&q->sq_used))
        wake++;

    req->status = OX_MQ_QUEUED;
    TAILQ_INSERT_TAIL (&q->sq_used, req, entry);
    atomic_inc(&q->stats.sq_used);

    /* Wake consumer thread if queue was empty */
    if (wake) {
        pthread_mutex_lock (&q->sq_cond_m);
        pthread_cond_signal(&q->sq_cond);
        pthread_mutex_unlock (&q->sq_cond_m);
    }
    pthread_mutex_unlock (&q->sq_used_mutex);

    return 0;
}

int ox_mq_complete_req (struct ox_mq *mq, struct ox_mq_entry *req_sq)
{
    struct ox_mq_queue *q;
    struct ox_mq_entry *req_cq;
    uint8_t wake = 0;

    pthread_mutex_lock (&req_sq->entry_mutex);
    /* Timeout requests are OX_MQ_TIMEOUT_BACK after the first completion try */
    if (!req_sq || !req_sq->opaque || req_sq->status == OX_MQ_TIMEOUT_BACK) {
        pthread_mutex_unlock (&req_sq->entry_mutex);
        return -1;
    }

    /* Check if request is a TIMEOUT_COMPLETED but not TIMEOUT_BACK */
    if (req_sq->status == OX_MQ_TIMEOUT_COMPLETED) {
        ox_mq_free_entry(mq, req_sq);
        atomic_inc(&mq->stats.to_back);
        pthread_mutex_unlock (&req_sq->entry_mutex);
        req_sq->status = OX_MQ_TIMEOUT_BACK;
        return -1;
    }

    q = &mq->queues[req_sq->qid];
    pthread_mutex_unlock (&req_sq->entry_mutex);

    /* TODO: retry user defined times if queue is full */
    pthread_mutex_lock (&q->cq_free_mutex);
    if (TAILQ_EMPTY (&q->cq_free)) {
        pthread_mutex_unlock (&q->cq_free_mutex);
        log_info (" [ox-mq: WARNING: CQ Full, request not completed.\n");
        return -1;
    }

    req_cq = TAILQ_FIRST (&q->cq_free);
    TAILQ_REMOVE (&q->cq_free, req_cq, entry);
    pthread_mutex_unlock (&q->cq_free_mutex);
    atomic_dec(&q->stats.cq_free);

    pthread_mutex_lock (&req_sq->entry_mutex);
    req_cq->opaque = req_sq->opaque;
    req_cq->qid = req_sq->qid;

    if (req_sq->status == OX_MQ_WAITING) {
        OX_MQ_DEQUEUE (&q->sq_wait,req_sq,&q->sq_wait_mutex,&q->stats.sq_wait);

        ox_mq_reset_entry (req_sq);
        OX_MQ_ENQUEUE (&q->sq_free,req_sq,&q->sq_free_mutex,&q->stats.sq_free);
    }
    pthread_mutex_unlock (&req_sq->entry_mutex); /**/

    pthread_mutex_lock (&q->cq_used_mutex);
    if (TAILQ_EMPTY (&q->cq_used))
        wake++;

    req_cq->status = OX_MQ_QUEUED;
    TAILQ_INSERT_TAIL (&q->cq_used, req_cq, entry);
    atomic_inc(&q->stats.cq_used);

    /* Wake consumer thread if queue was empty */
    if (wake) {
        pthread_mutex_lock (&q->cq_cond_m);
        pthread_cond_signal(&q->cq_cond);
        pthread_mutex_unlock (&q->cq_cond_m);
    }
    pthread_mutex_unlock (&q->cq_used_mutex);

    return 0;
}

static int ox_mq_check_entry_to (struct ox_mq *mq, struct ox_mq_entry *entry)
{
    struct timeval cur;
    uint64_t usec_e, usec_s, tot;

    gettimeofday(&cur, NULL);

    usec_e = cur.tv_sec * NVM_SEC;
    usec_e += cur.tv_usec;
    usec_s = entry->wtime.tv_sec * NVM_SEC;
    usec_s += entry->wtime.tv_usec;

    tot = usec_e - usec_s;

    return (tot >= mq->config->to_usec);
}

static int ox_mq_process_to_entry (struct ox_mq *mq, struct ox_mq_queue *q,
                                                      struct ox_mq_entry *req) {
    struct ox_mq_entry *new_req;

    TAILQ_REMOVE (&q->sq_wait, req, entry);
    atomic_dec(&q->stats.sq_wait);

    req->status = OX_MQ_TIMEOUT;

    new_req = ox_mq_create_ext_entry(mq);
    if (!new_req)
        goto ERR;

    OX_MQ_ENQUEUE(&q->sq_free, new_req, &q->sq_free_mutex, &q->stats.sq_free);

    return 0;

ERR:
    log_err (" [ox-mq: WARNING: timeout entry is out of list, not possible "
                        "to allocate new entry. Queue size is now smaller.\n");
    return -1;
}

static void ox_mq_check_queue_to (struct ox_mq *mq, struct ox_mq_queue *q)
{
    struct ox_mq_entry *req;
    struct ox_mq_entry **to_list;
    void **to_opaque;
    int to_count, i;

    to_count = 0;
    to_list = NULL;
    pthread_mutex_lock (&q->sq_wait_mutex);

    /* Check and process the list of timeout requests */
    TAILQ_FOREACH (req, &q->sq_wait, entry) {
        if (ox_mq_check_entry_to(mq, req)) {

            ox_mq_process_to_entry (mq, q, req);
            if (to_count)
                to_list = realloc (to_list, sizeof (void *) * to_count + 1);
            else
                to_list = malloc (sizeof (void *));

            to_list[to_count] = req;
            atomic_inc(&mq->stats.timeout);
            to_count++;

        }
    }

    pthread_mutex_unlock (&q->sq_wait_mutex);

    if (to_count)
        to_opaque = malloc (sizeof (void *) * to_count);

    for (i = 0; i < to_count; i++)
        to_opaque[i] = to_list[i]->opaque;

    /* Call user defined timeout function */
    if (to_count)
        mq->config->to_fn (to_opaque, to_count);

    /* Complete the list of timeout requests, if flag enabled */
    i = to_count;
    while (i) {
        i--;
        if (mq->config->to_fn && (mq->config->flags & OX_MQ_TO_COMPLETE))
            if (ox_mq_complete_req(mq, to_list[i]))
                log_err (" [ox-mq: WARNING: Not possible to post completion "
                                                      "for a timeout request");
        to_list[i]->status = OX_MQ_TIMEOUT_COMPLETED;
    }

    if (to_count) {
        free (to_opaque);
        free (to_list);
    }
}

/*
 * This thread checks all sq_wait queues for timeout requests.
 *
 * If a timeout entry id found, the follow steps are performed:
 *  - Remove the entry from sq_wait;
 *  - Set timeout entry status to OX_MQ_TIMEOUT;
 *  - Allocate a new entry;
 *  - Insert the new entry to the sq_free;
 *  - Insert the new entry to mq->ext_entries for exit free process;
 *
 * After all entries are processed:
 *  - Call the user defined timeout function and pass the list of TO entries;
 *    - In this function, the user should set the opaque structures as failed
 *  - If the flag is enabled, submit all the entries for completion;
 *  - Set timeout entries status to OX_MQ_TIMEOUT_COMPLETED;
 *
 * If the entry is called for completion later:
 *  - Check is the entry is part of the ext_entries, if yes, free memory;
 *    - Set the entry as OX_MQ_TIMEOUT_BACK (to avoid double free)
 */
static void *ox_mq_to_thread (void *arg)
{
    struct ox_mq *mq = (struct ox_mq *) arg;
    int exit, i;

    do {
        usleep (mq->config->to_usec);

        for (i = 0; i < mq->config->n_queues; i++)
            ox_mq_check_queue_to(mq, &mq->queues[i]);

        exit = mq->config->n_queues;
        for (i = 0; i < mq->config->n_queues; i++) {
            if (!mq->queues[i].running)
                exit--;
        }
        if (!exit)
            break;
    } while (1);

    return NULL;
}

static int ox_mq_start_to (struct ox_mq *mq)
{
    LIST_INIT (&mq->ext_list);

    if (pthread_create(&mq->to_tid, NULL, ox_mq_to_thread, mq))
        return -1;

    return 0;
}

struct ox_mq *ox_mq_init (struct ox_mq_config *config)
{
    int i;

    if (config->q_size < 1 || config->q_size > 0x10000 ||
                            config->n_queues < 1 || config->n_queues > 0x10000)
        return NULL;

    struct ox_mq *mq = malloc (sizeof (struct ox_mq));
    if (!mq)
        return NULL;

    mq->queues = malloc (sizeof (struct ox_mq_queue) * config->n_queues);
    if (!mq->queues)
        goto FREE_MQ;
    memset (mq->queues, 0, sizeof (struct ox_mq_queue) * config->n_queues);

    ox_mq_init_stats(&mq->stats);

    for (i = 0; i < config->n_queues; i++) {
        if (ox_mq_init_queue (&mq->queues[i], config->q_size,
                                               config->sq_fn, config->cq_fn)) {
            ox_mq_free_queues (mq, i);
            goto FREE_Q;
        }

        if (ox_mq_start_thread (&mq->queues[i])) {
            ox_mq_free_queues (mq, i + 1);
            goto FREE_Q;
        }
    }

    mq->config = config;

    if (mq->config->to_usec && ox_mq_start_to(mq))
        goto FREE_ALL;

    log_info (" [ox-mq: Multi queue started (nq: %d, qs: %d)\n",
                                             config->n_queues, config->q_size);
    return mq;

FREE_ALL:
    ox_mq_free_queues (mq, config->n_queues);
FREE_Q:
    free (mq->queues);
FREE_MQ:
    free (mq);
    return NULL;
}

static void ox_mq_free_ext_list (struct ox_mq *mq)
{
    struct ox_mq_entry *entry;

    while (!LIST_EMPTY(&mq->ext_list)) {
        entry = LIST_FIRST (&mq->ext_list);
        if (entry)
            ox_mq_free_entry(mq, entry);
    }
}

void ox_mq_destroy (struct ox_mq *mq)
{
    ox_mq_free_queues(mq, mq->config->n_queues);
    pthread_join (mq->to_tid, NULL);
    ox_mq_free_ext_list (mq);
    free (mq->queues);
    free (mq);
}