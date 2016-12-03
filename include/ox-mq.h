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

#ifndef OX_MQ_H
#define OX_MQ_H

#include <sys/queue.h>
#include <pthread.h>
#include <stdint.h>
#include "uatomic.h"

struct ox_mq_entry {
    void                     *opaque;
    uint32_t                 qid;
    uint8_t                  status;
    TAILQ_ENTRY(ox_mq_entry)  entry;
};

struct ox_mq_stats {
    atomic_t    sq_free;
    atomic_t    sq_used;
    atomic_t    sq_wait;
    atomic_t    cq_free;
    atomic_t    cq_used;
};

typedef void (ox_mq_sq_fn)(struct ox_mq_entry *);
typedef void (ox_mq_cq_fn)(void *);

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
    pthread_t                              sq_tid;
    pthread_t                              cq_tid;
    uint8_t                                running; /* if 0, kill threads */
    struct ox_mq_stats                     stats;
};

struct ox_mq {
    uint32_t            n_queues;
    uint32_t            q_size;
    struct ox_mq_queue  *queues;
};

struct ox_mq *ox_mq_init (uint32_t, uint32_t, ox_mq_sq_fn *, ox_mq_cq_fn *);
void          ox_mq_destroy (struct ox_mq *);
int           ox_mq_submit_req (struct ox_mq *, uint32_t, void *);
int           ox_mq_complete_req (struct ox_mq *, struct ox_mq_entry *);
void          ox_mq_show_stats (struct ox_mq *);

#endif /* OX_MQ_H */
