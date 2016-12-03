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
#include <stdio.h>
#include <stdlib.h>
#include <sys/queue.h>
#include "include/ox-mq.h"
#include "include/ssd.h"

void ox_mq_show_stats (struct ox_mq *mq)
{
    int i;
    struct ox_mq_queue *q;

    for (i = 0; i < mq->n_queues; i++) {
        q = &mq->queues[i];
        log_info ("Q %d. SF: %d, SU: %d, SW: %d, CF: %d, CU: %d\n", i,
                atomic_read(&q->stats.sq_free),
                atomic_read(&q->stats.sq_used),
                atomic_read(&q->stats.sq_wait),
                atomic_read(&q->stats.cq_free),
                atomic_read(&q->stats.cq_used));
    }
}

static int ox_mq_init_sq (struct ox_mq_queue *q, uint32_t size)
{
    TAILQ_INIT (&q->sq_free);
    TAILQ_INIT (&q->sq_used);
    TAILQ_INIT (&q->sq_wait);
    pthread_mutex_init (&q->sq_free_mutex, NULL);
    pthread_mutex_init (&q->sq_used_mutex, NULL);
    pthread_mutex_init (&q->sq_wait_mutex, NULL);

    q->sq_entries = malloc (sizeof (struct ox_mq_entry) * size);
    if (!q->sq_entries)
        goto CLEAN;

    memset (q->sq_entries, 0, sizeof (struct ox_mq_entry) * size);

    return 0;

CLEAN:
    pthread_mutex_destroy (&q->sq_free_mutex);
    pthread_mutex_destroy (&q->sq_used_mutex);
    pthread_mutex_destroy (&q->sq_wait_mutex);
    return -1;
}

static int ox_mq_init_cq (struct ox_mq_queue *q, uint32_t size)
{
    TAILQ_INIT (&q->cq_free);
    TAILQ_INIT (&q->cq_used);
    pthread_mutex_init (&q->cq_free_mutex, NULL);
    pthread_mutex_init (&q->cq_used_mutex, NULL);

    q->cq_entries = malloc (sizeof (struct ox_mq_entry) * size);
    if (!q->cq_entries)
        goto CLEAN;

    memset (q->cq_entries, 0, sizeof (struct ox_mq_entry) * size);

    return 0;

CLEAN:
    pthread_mutex_destroy (&q->cq_free_mutex);
    pthread_mutex_destroy (&q->cq_used_mutex);
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

    q->stats.cq_free.counter = ATOMIC_INIT_RUNTIME(0);
    q->stats.cq_used.counter = ATOMIC_INIT_RUNTIME(0);
    q->stats.sq_free.counter = ATOMIC_INIT_RUNTIME(0);
    q->stats.sq_used.counter = ATOMIC_INIT_RUNTIME(0);
    q->stats.sq_wait.counter = ATOMIC_INIT_RUNTIME(0);

    for (i = 0; i < size; i++) {
        TAILQ_INSERT_TAIL (&q->sq_free, &q->sq_entries[i], entry);
        atomic_inc(&q->stats.sq_free);
        TAILQ_INSERT_TAIL (&q->cq_free, &q->cq_entries[i], entry);
        atomic_inc(&q->stats.cq_free);
    }

    q->running = 1; /* ready */

    return 0;

CLEAN_SQ:
    pthread_mutex_destroy (&q->sq_free_mutex);
    pthread_mutex_destroy (&q->sq_used_mutex);
    pthread_mutex_destroy (&q->sq_wait_mutex);
    free (q->sq_entries);
    return -1;
}

static void ox_mq_free_queues (struct ox_mq *mq, uint32_t n_queues)
{
    int i;
    struct ox_mq_queue *q;

    for (i = 0; i < n_queues; i++) {
        q = &mq->queues[i];
        q->running = 0; /* stop threads */
        pthread_join(q->sq_tid, NULL);
        pthread_join(q->cq_tid, NULL);
        pthread_mutex_destroy (&q->sq_free_mutex);
        pthread_mutex_destroy (&q->sq_used_mutex);
        pthread_mutex_destroy (&q->sq_wait_mutex);
        free (q->sq_entries);
        pthread_mutex_destroy (&q->cq_free_mutex);
        pthread_mutex_destroy (&q->cq_used_mutex);
        free (q->cq_entries);
    }
}

static void *ox_mq_sq_thread (void *arg)
{
    struct ox_mq_queue *q = (struct ox_mq_queue *) arg;
    struct ox_mq_entry *req;

    while (q->running) {
        if (TAILQ_EMPTY (&q->sq_used)) {
            usleep(100);
            continue;
        }

        pthread_mutex_lock (&q->sq_used_mutex);
        req = TAILQ_FIRST (&q->sq_used);
        TAILQ_REMOVE (&q->sq_used, req, entry);
        pthread_mutex_unlock (&q->sq_used_mutex);
        atomic_dec(&q->stats.sq_used);

        pthread_mutex_lock (&q->sq_wait_mutex);
        TAILQ_INSERT_TAIL (&q->sq_wait, req, entry);
        pthread_mutex_unlock (&q->sq_wait_mutex);
        atomic_inc(&q->stats.sq_wait);

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
        if (TAILQ_EMPTY (&q->cq_used)) {
            usleep(100);
            continue;
        }

        pthread_mutex_lock (&q->cq_used_mutex);
        req = TAILQ_FIRST (&q->cq_used);
        TAILQ_REMOVE (&q->cq_used, req, entry);
        pthread_mutex_unlock (&q->cq_used_mutex);
        atomic_dec(&q->stats.cq_used);

        opaque = req->opaque;

        memset (req, 0, sizeof (struct ox_mq_entry));
        pthread_mutex_lock (&q->cq_free_mutex);
        TAILQ_INSERT_TAIL (&q->cq_free, req, entry);
        pthread_mutex_unlock (&q->cq_free_mutex);
        atomic_inc(&q->stats.cq_free);

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

    if (qid >= mq->n_queues)
        return -1;

    q = &mq->queues[qid];

    /* TODO: retry user defined times if queue is full */
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
    TAILQ_INSERT_TAIL (&q->sq_used, req, entry);
    pthread_mutex_unlock (&q->sq_used_mutex);
    atomic_inc(&q->stats.sq_used);

    return 0;
}

int ox_mq_complete_req (struct ox_mq *mq, struct ox_mq_entry *req_sq)
{
    struct ox_mq_queue *q;
    struct ox_mq_entry *req_cq;

    if (!req_sq || !req_sq->opaque)
        return -1;

    q = &mq->queues[req_sq->qid];

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

    req_cq->opaque = req_sq->opaque;
    req_cq->qid = req_sq->qid;

    pthread_mutex_lock (&q->sq_wait_mutex);
    TAILQ_REMOVE (&q->sq_wait, req_sq, entry);
    pthread_mutex_unlock (&q->sq_wait_mutex);
    atomic_dec(&q->stats.sq_wait);

    memset (req_sq, 0, sizeof (struct ox_mq_entry));
    pthread_mutex_lock (&q->sq_free_mutex);
    TAILQ_INSERT_TAIL (&q->sq_free, req_sq, entry);
    pthread_mutex_unlock (&q->sq_free_mutex);
    atomic_inc(&q->stats.sq_free);

    pthread_mutex_lock (&q->cq_used_mutex);
    TAILQ_INSERT_TAIL (&q->cq_used, req_cq, entry);
    pthread_mutex_unlock (&q->cq_used_mutex);
    atomic_inc(&q->stats.cq_used);

    return 0;
}

struct ox_mq *ox_mq_init (uint32_t n_queues, uint32_t q_size,
                                        ox_mq_sq_fn *sq_fn, ox_mq_cq_fn *cq_fn)
{
    int i;

    if (q_size < 1 || q_size > 0x10000 || n_queues < 1 || n_queues > 0x10000)
        return NULL;

    struct ox_mq *mq = malloc (sizeof (struct ox_mq));
    if (!mq)
        return NULL;

    mq->queues = malloc (sizeof (struct ox_mq_queue) * n_queues);
    if (!mq->queues)
        goto CLEAN_MQ;
    memset (mq->queues, 0, sizeof (struct ox_mq_queue) * n_queues);

    mq->q_size = q_size;
    mq->n_queues = n_queues;

    for (i = 0; i < n_queues; i++) {
        if (ox_mq_init_queue (&mq->queues[i], q_size, sq_fn, cq_fn)) {
            ox_mq_free_queues (mq, i);
            goto CLEAN_Q;
        }

        if (ox_mq_start_thread (&mq->queues[i])) {
            ox_mq_free_queues (mq, i + 1);
            goto CLEAN_Q;
        }
    }

    log_info (" [ox-mq: Multi queue started (nq: %d, qs: %d)\n", n_queues,
                                                                       q_size);

    return mq;

CLEAN_Q:
    free (mq->queues);
CLEAN_MQ:
    free (mq);
    return NULL;
}

void ox_mq_destroy (struct ox_mq *mq)
{
    ox_mq_free_queues(mq, mq->n_queues);
    free (mq->queues);
    free (mq);
}
