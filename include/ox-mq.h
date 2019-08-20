/* OX: Open-Channel NVM Express SSD Controller
 * 
 *  - Multi-Queue Support for Parallel I/O (header)
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

#ifndef OX_MQ_H
#define OX_MQ_H

#include <sys/queue.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <ox-uatomic.h>

#define OX_MQ_MAX_QUEUES    0x4000
#define OX_MQ_MAX_Q_DEPTH   0x80000

enum {
    OX_MQ_FREE = 1,
    OX_MQ_QUEUED,
    OX_MQ_WAITING,
    OX_MQ_TIMEOUT,
    OX_MQ_TIMEOUT_COMPLETED,
    OX_MQ_TIMEOUT_BACK
};

struct oxmq_output_row;
struct ox_mq_entry {
    void                     *opaque;
    uint32_t                 qid;
    uint8_t                  status;
    struct timeval           wtime; /* timestamp for timeout */
    uint8_t                  is_ext; /* if > 0, allocated due timeout */
    TAILQ_ENTRY(ox_mq_entry) entry;
    LIST_ENTRY(ox_mq_entry)  ext_entry;
    pthread_mutex_t          entry_mutex;
    struct oxmq_output_row   *out_row;
};

/* Keeps a set of counters related to the multi-queue */
struct ox_mq_stats {
    u_atomic_t    sq_free;
    u_atomic_t    sq_used;
    u_atomic_t    sq_wait;
    u_atomic_t    cq_free;
    u_atomic_t    cq_used;
    u_atomic_t    ext_list; /* extended entry list size */
    u_atomic_t    timeout;  /* total timeout entries */
    u_atomic_t    to_back;  /* timeout entries asked for a late completion */
};

typedef void (ox_mq_sq_fn)(struct ox_mq_entry *);

/* void * is the pointer to the opaque user entry */
typedef void (ox_mq_cq_fn)(void *);

/* void ** is an array of timeout opaque entries, int is the array size */
typedef void (ox_mq_to_fn)(void **, int);

/* Fill the statistics rows for file output */
typedef void (ox_mq_set_output_fn)(struct oxmq_output_row *row, void *opaque);

struct ox_mq_queue {
    pthread_mutex_t                        sq_free_mutex;
    pthread_mutex_t                        cq_free_mutex;
    pthread_mutex_t                        sq_used_mutex;
    pthread_mutex_t                        cq_used_mutex;
    pthread_mutex_t                        sq_wait_mutex;
    struct ox_mq_entry                     *sq_entries;
    struct ox_mq_entry                     *cq_entries;
    TAILQ_HEAD (sq_free_head, ox_mq_entry) sq_free;
    TAILQ_HEAD (sq_used_head, ox_mq_entry) sq_used;
    TAILQ_HEAD (sq_wait_head, ox_mq_entry) sq_wait;
    TAILQ_HEAD (cq_free_head, ox_mq_entry) cq_free;
    TAILQ_HEAD (cq_used_head, ox_mq_entry) cq_used;
    ox_mq_sq_fn                            *sq_fn;
    ox_mq_cq_fn                            *cq_fn;
    pthread_mutex_t                        sq_cond_m;
    pthread_mutex_t                        cq_cond_m;
    pthread_cond_t                         sq_cond;
    pthread_cond_t                         cq_cond;
    pthread_t                              sq_tid;
    pthread_t                              cq_tid;
    uint8_t                                running; /* if 0, kill threads */
    struct ox_mq_stats                     stats;
    struct ox_mq                           *mq;
    uint16_t                               qid;
};

#define OX_MQ_TO_COMPLETE   (1 << 0) /* Complete request after timeout */
#define OX_MQ_CPU_AFFINITY  (1 << 1) /* Forces all threads to run a specific core */

struct oxmq_output_row {
    /* Should be set by user in 'ox_mq_set_output_fn' function */
    uint64_t    lba;
    uint16_t    ch;
    uint16_t    lun;
    uint32_t    blk;
    uint32_t    pg;
    uint8_t     pl;
    uint8_t     sec;
    uint8_t     type;
    uint8_t     failed;
    uint8_t     datacmp;
    uint32_t    size;

    /* Filled automatically by ox-mq */
    uint64_t    seq;
    uint64_t    node_seq;
    uint16_t    node_id;
    uint64_t    tstart;
    uint64_t    tend;
    uint64_t    ulat;
    TAILQ_ENTRY(oxmq_output_row) entry;
};

struct oxmq_output_tq {
    uint64_t                row_off;
    struct oxmq_output_row *rows;
    TAILQ_HEAD(out_list,oxmq_output_row) out_head;
};
struct oxmq_output {
    uint64_t               id;
    uint32_t               nodes;
    char                   name[64];
    pthread_mutex_t        file_mutex;
    uint64_t               sequence;
    uint64_t              *node_seq;
    struct oxmq_output_tq *queues;
};

struct ox_mq_config {
    char                name[40];
    uint32_t            n_queues;
    uint32_t            q_size;
    ox_mq_sq_fn         *sq_fn;     /* submission queue consumer */
    ox_mq_cq_fn         *cq_fn;     /* completion queue consumer */
    ox_mq_to_fn         *to_fn;     /* timeout call */
    ox_mq_set_output_fn *output_fn; /* Fill output data */
    uint64_t            to_usec;    /* timeout in microseconds */
    uint8_t             flags;

    /* Used if OX_MQ_CPU_AFFINITY flag is set, max of 64 CPUs */
    uint64_t            sq_affinity[OX_MQ_MAX_QUEUES];
    uint64_t            cq_affinity[OX_MQ_MAX_QUEUES];
};

struct ox_mq {
    LIST_ENTRY(ox_mq)                 entry;
    struct ox_mq_queue                *queues;
    struct ox_mq_config               *config;
    pthread_t                         to_tid;       /* timeout thread */
    LIST_HEAD(oxmq_ext, ox_mq_entry)  ext_list;     /* new allocated entries */
    struct ox_mq_stats                stats;
    struct oxmq_output                *output;
    uint8_t                           stop;         /* Set to 1, stop threads */
};

struct ox_mq *ox_mq_init (struct ox_mq_config *);
void          ox_mq_destroy (struct ox_mq *);
int           ox_mq_submit_req (struct ox_mq *, uint32_t, void *);
int           ox_mq_complete_req (struct ox_mq *, struct ox_mq_entry *);
void          ox_mq_show_mq (struct ox_mq *);
void          ox_mq_show_all (void);
struct ox_mq *ox_mq_get (const char *);
int           ox_mq_used_count (struct ox_mq *, uint16_t qid);
int           ox_mq_get_status (struct ox_mq *, struct ox_mq_stats *,
                                                                  uint16_t qid);
void                    ox_mq_output_start (void);
void                    ox_mq_output_stop (void);
struct oxmq_output_row *ox_mq_output_new (struct oxmq_output *output,
                                                                int node_id);
void                    ox_mq_output_flush (struct oxmq_output *output);
void                    ox_mq_output_exit (struct oxmq_output *output);
struct oxmq_output     *ox_mq_output_init (uint64_t id, const char *name,
                                                                uint32_t nodes);

#endif /* OX_MQ_H */