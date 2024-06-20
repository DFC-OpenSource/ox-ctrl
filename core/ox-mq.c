/* OX: Open-Channel NVM Express SSD Controller
 * 
 *  - Multi-Queue Support for Parallel I/O
 *
 * Copyright 2017 IT University of Copenhagen
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

#define _GNU_SOURCE

#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/queue.h>
#include <ox-mq.h>
#include <libox.h>

static volatile uint8_t mq_output = 0;
static int mq_count = 0;
LIST_HEAD(mq_list, ox_mq) mq_head = LIST_HEAD_INITIALIZER(mq_head);

int ox_mq_get_status (struct ox_mq *mq, struct ox_mq_stats *st, uint16_t qid)
{
    if (!mq || !st || qid > mq->config->n_queues)
        return -1;

    memcpy (st, &mq->queues[qid].stats, sizeof (struct ox_mq_stats));

    return 0;
}

int ox_mq_used_count (struct ox_mq *mq, uint16_t qid)
{
    if (!mq || !mq->config) {
        log_err (" [ox-mq (used_count): WARNING: Suspicious null pointer]");
        return -1;
    }

    if (qid > mq->config->n_queues)
        return -1;

    return u_atomic_read(&mq->queues[qid].stats.sq_used);
}

void ox_mq_show_mq (struct ox_mq *mq)
{
    int i;
    struct ox_mq_queue *q;

    printf ("ox-mq: %s\n", mq->config->name);
    for (i = 0; i < mq->config->n_queues; i++) {
        q = &mq->queues[i];
        printf ("    Q%02d: SF: %d, SU: %d, SW: %d, CF: %d, CU: %d\n", i,
                u_atomic_read(&q->stats.sq_free),
                u_atomic_read(&q->stats.sq_used),
                u_atomic_read(&q->stats.sq_wait),
                u_atomic_read(&q->stats.cq_free),
                u_atomic_read(&q->stats.cq_used));
    }
    printf ("    EXT%02d: TO: %d, TO_BACK: %d\n",
                u_atomic_read(&mq->stats.ext_list),
                u_atomic_read(&mq->stats.timeout),
                u_atomic_read(&mq->stats.to_back));
}

void ox_mq_show_all (void)
{
    struct ox_mq *mq;
    LIST_FOREACH (mq, &mq_head, entry) {
        ox_mq_show_mq (mq);
    }
}

struct ox_mq *ox_mq_get (const char *name) {
    struct ox_mq *mq;
    LIST_FOREACH(mq, &mq_head, entry){
        if(!strcmp (mq->config->name, name))
            return mq;
    }
    return NULL;
}

void ox_mq_output_start (void) {
    struct ox_mq *mq;
    struct timespec ts;
    uint64_t ns;

    if (mq_output)
        return;

    GET_NANOSECONDS (ns, ts);

    LIST_FOREACH(mq, &mq_head, entry){
        mq->output = ox_mq_output_init (ns, mq->config->name,
                                                        mq->config->n_queues);
        if (!mq->output)
            printf ("[ox-mq: Output NOT started -> %s]\n", mq->config->name);
        else
            printf ("[ox-mq: Output started -> %s]\n", mq->config->name);
    }

    mq_output = 1;
}

void ox_mq_output_stop (void) {
    struct ox_mq *mq;

    if (!mq_output)
        return;

    mq_output = 0;
    usleep (1000);

    LIST_FOREACH(mq, &mq_head, entry){
        if (mq->output) {
            ox_mq_output_flush (mq->output);
            ox_mq_output_exit (mq->output);
            mq->output = NULL;
        }
        printf ("[ox-mq: %s -> Output stopped.]\n", mq->config->name);
    }
}

static void ox_mq_init_stats (struct ox_mq_stats *stats)
{
    stats->cq_free.counter = U_ATOMIC_INIT_RUNTIME(0);
    stats->cq_used.counter = U_ATOMIC_INIT_RUNTIME(0);
    stats->sq_free.counter = U_ATOMIC_INIT_RUNTIME(0);
    stats->sq_used.counter = U_ATOMIC_INIT_RUNTIME(0);
    stats->sq_wait.counter = U_ATOMIC_INIT_RUNTIME(0);
    stats->ext_list.counter = U_ATOMIC_INIT_RUNTIME(0);
    stats->timeout.counter = U_ATOMIC_INIT_RUNTIME(0);
    stats->to_back.counter = U_ATOMIC_INIT_RUNTIME(0);
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

    q->sq_entries = ox_malloc (sizeof(struct ox_mq_entry) * size, OX_MEM_OX_MQ);
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

    q->cq_entries = ox_malloc (sizeof(struct ox_mq_entry) * size, OX_MEM_OX_MQ);
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
        u_atomic_inc(&q->stats.sq_free);
        TAILQ_INSERT_TAIL (&q->cq_free, &q->cq_entries[i], entry);
        u_atomic_inc(&q->stats.cq_free);
        pthread_mutex_init (&q->sq_entries[i].entry_mutex, NULL);
        pthread_mutex_init (&q->cq_entries[i].entry_mutex, NULL);
    }

    q->running = 1; /* ready */

    return 0;

CLEAN_SQ:
    ox_mq_destroy_sq (q);
    ox_free (q->sq_entries, OX_MEM_OX_MQ);
    return -1;
}

static inline void ox_mq_reset_entry (struct ox_mq_entry *entry)
{
    entry->status = OX_MQ_FREE;
    entry->opaque = NULL;
    entry->qid = 0;
    entry->out_row = NULL;
    memset (&entry->wtime, 0, sizeof (struct timeval));
}

static struct ox_mq_entry *ox_mq_create_ext_entry (struct ox_mq *mq)
{
    struct ox_mq_entry *new_entry;

    new_entry = ox_malloc (sizeof (struct ox_mq_entry), OX_MEM_OX_MQ);
    if (!new_entry)
        return NULL;

    new_entry->is_ext = 0x1;
    pthread_mutex_init (&new_entry->entry_mutex, NULL);
    ox_mq_reset_entry (new_entry);

    LIST_INSERT_HEAD (&mq->ext_list, new_entry, ext_entry);
    u_atomic_inc (&mq->stats.ext_list);

    return new_entry;
}

static void ox_mq_free_entry (struct ox_mq *mq, struct ox_mq_entry *entry)
{
    if (entry->is_ext) {
        LIST_REMOVE (entry, ext_entry);
        pthread_mutex_destroy (&entry->entry_mutex);
        ox_free (entry, OX_MEM_OX_MQ);
        u_atomic_dec (&mq->stats.ext_list);
    }
}

static void ox_mq_free_queues (struct ox_mq *mq, uint32_t n_queues)
{
    int i, j;
    struct ox_mq_queue *q;

    for (i = 0; i < n_queues; i++) {
        q = &mq->queues[i];

        /* Wake threads if queue was empty and stop it */
        q->running = 0;
        pthread_mutex_lock (&q->sq_used_mutex);
        if (TAILQ_EMPTY (&q->sq_used)) {
            pthread_mutex_lock (&q->sq_cond_m);
            pthread_cond_signal(&q->sq_cond);
            pthread_mutex_unlock (&q->sq_cond_m);
        }
        pthread_mutex_unlock (&q->sq_used_mutex);

        pthread_mutex_lock (&q->cq_used_mutex);
        if (TAILQ_EMPTY (&q->cq_used)) {
            pthread_mutex_lock (&q->cq_cond_m);
            pthread_cond_signal(&q->cq_cond);
            pthread_mutex_unlock (&q->cq_cond_m);
        }
        pthread_mutex_unlock (&q->cq_used_mutex);

        pthread_join(q->sq_tid, NULL);
        pthread_join(q->cq_tid, NULL);

        for (j = 0; j < mq->config->q_size; j++) {
            pthread_mutex_destroy (&q->sq_entries[j].entry_mutex);
            pthread_mutex_destroy (&q->cq_entries[j].entry_mutex);
        }
        ox_mq_destroy_sq (q);
        ox_free (q->sq_entries, OX_MEM_OX_MQ);
        ox_mq_destroy_cq (q);
        ox_free (q->cq_entries, OX_MEM_OX_MQ);
    }
}

#define OX_MQ_ENQUEUE(head, elm, mutex, stat) do {                  \
        pthread_mutex_lock((mutex));                                \
        TAILQ_INSERT_TAIL((head), (elm), entry);                    \
        u_atomic_inc((stat));                                       \
        pthread_mutex_unlock((mutex));                              \
} while (/*CONSTCOND*/0)

#define OX_MQ_DEQUEUE_H(head, elm, mutex, stats) do {               \
        pthread_mutex_lock((mutex));                                \
        (elm) = TAILQ_FIRST((head));                                \
        TAILQ_REMOVE ((head), (elm), entry);                        \
        u_atomic_dec((stats));                                      \
        pthread_mutex_unlock ((mutex));                             \
} while (/*CONSTCOND*/0)

#define OX_MQ_DEQUEUE(head, elm, mutex, stats) do {                 \
        pthread_mutex_lock((mutex));                                \
        TAILQ_REMOVE ((head), (elm), entry);                        \
        u_atomic_dec((stats));                                      \
        pthread_mutex_unlock ((mutex));                             \
} while (/*CONSTCOND*/0)

static void *ox_mq_sq_thread (void *arg)
{
    struct ox_mq_queue *q = (struct ox_mq_queue *) arg;
    struct ox_mq_entry *req;
    struct timespec ts;
    struct timeval tv;
    uint64_t ns;
    pthread_t current_thread;
    cpu_set_t cpuset;
    uint16_t cpu_i;
    char name[128] = "";

    sprintf(name, "-SQ-%s-%d", q->mq->config->name, q->qid);
    pthread_setname_np(pthread_self(), name);

    if (q->mq->config->flags & OX_MQ_CPU_AFFINITY) {
        if (!q->mq->config->sq_affinity[q->qid])
            goto NO_AFFINITY;

        CPU_ZERO(&cpuset);

        for (cpu_i = 0; cpu_i < 64; cpu_i++) {
            if (q->mq->config->sq_affinity[q->qid] & ((uint64_t) 1 << cpu_i)) {
                CPU_SET(cpu_i, &cpuset);

                log_info (" [ox-mq (%s): SQ %d affinity to CPU %d.\n",
                                            q->mq->config->name, q->qid, cpu_i);
            }
        }

        current_thread = pthread_self();
        pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    }

NO_AFFINITY:
    while (q->running) {
        pthread_mutex_lock(&q->sq_cond_m);

        if (TAILQ_EMPTY (&q->sq_used)) {
            gettimeofday(&tv, NULL);
            ts.tv_sec = tv.tv_sec + 1; /* 1 second timeout */
            ts.tv_nsec = tv.tv_usec * 1000;
            pthread_cond_timedwait(&q->sq_cond, &q->sq_cond_m, &ts);
        }

        pthread_mutex_unlock(&q->sq_cond_m);

        if (!q->running)
            pthread_exit(NULL);

        if (TAILQ_EMPTY (&q->sq_used))
            continue;

        OX_MQ_DEQUEUE_H(&q->sq_used, req, &q->sq_used_mutex, &q->stats.sq_used);

        gettimeofday(&req->wtime, NULL);

        req->status = OX_MQ_WAITING;
        OX_MQ_ENQUEUE (&q->sq_wait, req, &q->sq_wait_mutex, &q->stats.sq_wait);

        /* Output statistics */
        if (mq_output && q->mq->output) {
            req->out_row = ox_mq_output_new (q->mq->output, req->qid);
            if (!req->out_row)
                goto FN;
            if (q->mq->config->output_fn)
                q->mq->config->output_fn (req->out_row, req->opaque);
            GET_NANOSECONDS (ns, ts);
            req->out_row->tstart = ns;
        }

FN:
        q->sq_fn (req);
    }

    return NULL;
}

static void *ox_mq_cq_thread (void *arg)
{
    struct ox_mq_queue *q = (struct ox_mq_queue *) arg;
    struct ox_mq_entry *req;
    void *opaque;
    struct timespec ts;
    struct timeval tv;
    pthread_t current_thread;
    cpu_set_t cpuset;
    uint16_t cpu_i;
    char name[128] = "";

    sprintf(name, "-CQ-%s-%d", q->mq->config->name, q->qid);
    pthread_setname_np(pthread_self(), name);

    if (q->mq->config->flags & OX_MQ_CPU_AFFINITY) {
        if (!q->mq->config->cq_affinity[q->qid])
            goto NO_AFFINITY;

        CPU_ZERO(&cpuset);

        for (cpu_i = 0; cpu_i < 64; cpu_i++) {
            if (q->mq->config->cq_affinity[q->qid] & ((uint64_t) 1 << cpu_i)) {
                CPU_SET(cpu_i, &cpuset);

                log_info (" [ox-mq (%s): CQ %d affinity to CPU %d.\n",
                                            q->mq->config->name, q->qid, cpu_i);
            }
        }

        current_thread = pthread_self();
        pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    }

NO_AFFINITY:
    while (q->running) {
        pthread_mutex_lock(&q->cq_cond_m);

        if (TAILQ_EMPTY (&q->cq_used)) {
            gettimeofday(&tv, NULL);
            ts.tv_sec = tv.tv_sec + 1; /* 1 second timeout */
            ts.tv_nsec = tv.tv_usec * 1000;
            pthread_cond_timedwait(&q->cq_cond, &q->cq_cond_m, &ts);
        }

        pthread_mutex_unlock(&q->cq_cond_m);

        if (!q->running)
            pthread_exit(NULL);

        if (TAILQ_EMPTY (&q->cq_used))
            continue;

        OX_MQ_DEQUEUE_H(&q->cq_used, req, &q->cq_used_mutex, &q->stats.cq_used);
        opaque = req->opaque;
        ox_mq_reset_entry (req);
        OX_MQ_ENQUEUE (&q->cq_free, req, &q->cq_free_mutex, &q->stats.cq_free);

        if (!opaque)
            continue;

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

    if (!mq || !mq->config) {
        log_err (" [ox-mq (submission): WARNING: Suspicious null pointer]");
        return -1;
    }

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
    u_atomic_dec(&q->stats.sq_free);
    pthread_mutex_unlock (&q->sq_free_mutex);

    req->opaque = opaque;
    req->qid = qid;

    pthread_mutex_lock (&q->sq_used_mutex);
    if (TAILQ_EMPTY (&q->sq_used))
        wake++;

    req->status = OX_MQ_QUEUED;
    TAILQ_INSERT_TAIL (&q->sq_used, req, entry);
    u_atomic_inc(&q->stats.sq_used);

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
    struct timespec ts;
    uint64_t ns;

    if (!mq || !mq->config) {
        log_err (" [ox-mq (completion): WARNING: Suspicious null pointer]");
        return -1;
    }

    pthread_mutex_lock (&req_sq->entry_mutex);
    /* Timeout requests are OX_MQ_TIMEOUT_BACK after the first completion try */
    if (!req_sq || !req_sq->opaque || req_sq->status == OX_MQ_TIMEOUT_BACK) {
        pthread_mutex_unlock (&req_sq->entry_mutex);
        return -1;
    }

    /* Check if request is a TIMEOUT_COMPLETED but not TIMEOUT_BACK */
    if (req_sq->status == OX_MQ_TIMEOUT_COMPLETED) {
        req_sq->status = OX_MQ_TIMEOUT_BACK;
        ox_mq_free_entry(mq, req_sq);
        u_atomic_inc(&mq->stats.to_back);
        pthread_mutex_unlock (&req_sq->entry_mutex);
        return -1;
    }

    q = &mq->queues[req_sq->qid];
    pthread_mutex_unlock (&req_sq->entry_mutex);

    /* TODO: retry user defined times if queue is full */
    pthread_mutex_lock (&q->cq_free_mutex);
    if (TAILQ_EMPTY (&q->cq_free)) {
        pthread_mutex_unlock (&q->cq_free_mutex);
        log_info (" [ox-mq (%s): WARNING: CQ Full, request not completed.]\n",
                                                              mq->config->name);
        return -1;
    }

    req_cq = TAILQ_FIRST (&q->cq_free);
    TAILQ_REMOVE (&q->cq_free, req_cq, entry);
    u_atomic_dec(&q->stats.cq_free);
    pthread_mutex_unlock (&q->cq_free_mutex);

    pthread_mutex_lock (&req_sq->entry_mutex);
    req_cq->opaque = req_sq->opaque;
    req_cq->qid = req_sq->qid;

    /* Output statistics */
    if (mq_output && q->mq->output && req_sq->out_row) {
        GET_NANOSECONDS (ns, ts);
        req_sq->out_row->tend = ns;
    }

    if (req_sq->status == OX_MQ_WAITING) {
        OX_MQ_DEQUEUE (&q->sq_wait,req_sq,&q->sq_wait_mutex,&q->stats.sq_wait);

        ox_mq_reset_entry (req_sq);
        OX_MQ_ENQUEUE (&q->sq_free,req_sq,&q->sq_free_mutex,&q->stats.sq_free);
    }
    pthread_mutex_unlock(&req_sq->entry_mutex);

    pthread_mutex_lock (&q->cq_used_mutex);
    if (TAILQ_EMPTY (&q->cq_used))
        wake++;

    req_cq->status = OX_MQ_QUEUED;
    TAILQ_INSERT_TAIL (&q->cq_used, req_cq, entry);
    u_atomic_inc(&q->stats.cq_used);

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

    usec_e = cur.tv_sec * SEC64;
    usec_e += cur.tv_usec;
    usec_s = entry->wtime.tv_sec * SEC64;
    usec_s += entry->wtime.tv_usec;

    tot = usec_e - usec_s;

    return (tot >= mq->config->to_usec);
}

static int ox_mq_process_to_entry (struct ox_mq *mq, struct ox_mq_queue *q,
                                                      struct ox_mq_entry *req) {
    struct ox_mq_entry *new_req;

    TAILQ_REMOVE (&q->sq_wait, req, entry);
    u_atomic_dec(&q->stats.sq_wait);

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
                to_list = ox_realloc (to_list, sizeof (void *) * to_count + 1,
                                                                  OX_MEM_OX_MQ);
            else
                to_list = ox_malloc (sizeof (void *), OX_MEM_OX_MQ);

            to_list[to_count] = req;
            u_atomic_inc(&mq->stats.timeout);
            to_count++;

        }
    }

    pthread_mutex_unlock (&q->sq_wait_mutex);

    if (to_count)
        to_opaque = ox_malloc (sizeof (void *) * to_count, OX_MEM_OX_MQ);

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
                log_err (" [ox-mq (%s): WARNING: Not possible to post "
                        "completion for a timeout request]", mq->config->name);
        to_list[i]->status = OX_MQ_TIMEOUT_COMPLETED;
    }

    if (to_count) {
        ox_free (to_opaque, OX_MEM_OX_MQ);
        ox_free (to_list, OX_MEM_OX_MQ);
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
    uint64_t count, time = mq->config->to_usec / 200;

    do {
        count = 0;
LOOP:
        usleep (200);
        count++;

        if (mq->stop)
            break;

        if (count < time)
            goto LOOP;

        for (i = 0; i < mq->config->n_queues; i++)
            ox_mq_check_queue_to(mq, &mq->queues[i]);

        exit = mq->config->n_queues;
        for (i = 0; i < mq->config->n_queues; i++) {
            if (!mq->queues[i].running)
                exit--;
        }
        if (!exit)
            break;
    } while (!mq->stop);

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

    if (config->q_size < 1 || config->q_size > OX_MQ_MAX_Q_DEPTH ||
                config->n_queues < 1 || config->n_queues > OX_MQ_MAX_QUEUES)
        return NULL;

    struct ox_mq *mq = ox_malloc (sizeof (struct ox_mq), OX_MEM_OX_MQ);
    if (!mq)
        return NULL;

    mq->queues = ox_malloc (sizeof (struct ox_mq_queue) * config->n_queues,
                                                                  OX_MEM_OX_MQ);
    if (!mq->queues)
        goto FREE_MQ;
    memset (mq->queues, 0, sizeof (struct ox_mq_queue) * config->n_queues);

    ox_mq_init_stats(&mq->stats);
    mq->stop = 0;
    mq->output = NULL;

    mq->config = ox_malloc (sizeof(struct ox_mq_config), OX_MEM_OX_MQ);
    if (!mq->config)
        goto FREE_Q;

    memcpy (mq->config, config, sizeof(struct ox_mq_config));

    for (i = 0; i < config->n_queues; i++) {
        mq->queues[i].mq = mq;

        mq->queues[i].qid = i;
        if (ox_mq_init_queue (&mq->queues[i], config->q_size,
                                               config->sq_fn, config->cq_fn)) {
            ox_mq_free_queues (mq, i);
            goto FREE_ALL;
        }

        if (ox_mq_start_thread (&mq->queues[i])) {
            ox_mq_free_queues (mq, i + 1);
            goto FREE_ALL;
        }
    }

    if (mq->config->to_usec && ox_mq_start_to(mq))
        goto FREE_ALL;

    if (!mq_count)
        LIST_INIT(&mq_head);

    LIST_INSERT_HEAD(&mq_head, mq, entry);
    mq_count++;

    log_info (" [ox-mq (%s): Multi queue started (nq: %d, qs: %d)]\n",
                           mq->config->name, config->n_queues, config->q_size);
    return mq;

FREE_ALL:
    ox_mq_free_queues (mq, config->n_queues);
    ox_free (mq->config, OX_MEM_OX_MQ);
FREE_Q:
    ox_free (mq->queues, OX_MEM_OX_MQ);
FREE_MQ:
    ox_free (mq, OX_MEM_OX_MQ);
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
    mq->stop = 1;
    if (mq->config->to_usec) {
        pthread_join (mq->to_tid, NULL);
        ox_mq_free_ext_list (mq);
    }
    ox_mq_free_queues(mq, mq->config->n_queues);

    LIST_REMOVE(mq, entry);
    mq_count--;

    log_info (" [ox-mq (%s): Multi queue stopped]\n", mq->config->name);

    ox_free (mq->queues, OX_MEM_OX_MQ);
    ox_free (mq->config, OX_MEM_OX_MQ);
    ox_free (mq, OX_MEM_OX_MQ);
}
