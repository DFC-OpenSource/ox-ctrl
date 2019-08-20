/*  OX: Open-Channel NVM Express SSD Controller
 *
 *  - Runtime statistics
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
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <libox.h>
#include <ox-app.h>

#define OX_STATS_IO_TYPES       45
#define OX_STATS_REC_TYPES      15
#define OX_STATS_LOG_TYPES      12  /* Follows 'enum app_log_type' in ox-app.h*/
#define OX_STATS_CP_TYPES       10
#define OX_STATS_IO_AREAD_TH    128

enum ox_stats_io_types {
    /* General media manager I/O */
    OX_STATS_IO_SYNC_R = 0,
    OX_STATS_IO_SYNC_W,
    OX_STATS_IO_ASYNC_R,
    OX_STATS_IO_ASYNC_W,
    OX_STATS_BYTES_SYNC_W,
    OX_STATS_BYTES_SYNC_R,
    OX_STATS_BYTES_ASYNC_W,
    OX_STATS_BYTES_ASYNC_R,
    OX_STATS_IO_ERASE, /* 9 */

    /* General block information */
    OX_STATS_SEC_USER_W,
    OX_STATS_SEC_USER_R,
    OX_STATS_SEC_MAP_W,
    OX_STATS_SEC_MAP_R,
    OX_STATS_SEC_LOG_W,
    OX_STATS_SEC_LOG_R,
    OX_STATS_SEC_PAD_W,
    OX_STATS_SEC_PAD_R,
    OX_STATS_SEC_OTHER_W,
    OX_STATS_SEC_OTHER_R,
    OX_STATS_SEC_RSV_W,
    OX_STATS_SEC_RSV_R,
    OX_STATS_SEC_CP_W,
    OX_STATS_SEC_CP_R, /* 23 */

    /* Checkpoint block information */
    OX_STATS_SEC_CP_MAPMD_W,
    OX_STATS_SEC_CP_MAPMD_R,
    OX_STATS_SEC_CP_BLK_W,
    OX_STATS_SEC_CP_BLK_R,
    OX_STATS_SEC_CP_TINY, /* 28 */

    /* Garbage collection block information */
    OX_STATS_SEC_GC_USER_W,
    OX_STATS_SEC_GC_USER_R,
    OX_STATS_SEC_GC_MAP_W,
    OX_STATS_SEC_GC_MAP_R,
    OX_STATS_SEC_GC_MAPMD_R,
    OX_STATS_SEC_GC_BLK_R,
    OX_STATS_SEC_GC_LOG_R,
    OX_STATS_SEC_GC_PAD_W,
    OX_STATS_SEC_GC_PAD_R,
    OX_STATS_SEC_GC_UNKOWN, /* 38 */

    /* Garbage collection event information */
    OX_STATS_GC_BLOCK_REC,
    OX_STATS_SEC_GC_FAILED,
    OX_STATS_GC_RACE_BIG,
    OX_STATS_GC_RACE_SMALL,
    OX_STATS_GC_SPACE_REC, /* 43 */

    /* Anomaly */
    OX_STATS_SYNCH_USER_R
};

struct ox_stats_data {
    uint64_t            io     [OX_STATS_IO_TYPES];
    pthread_spinlock_t  io_spin[OX_STATS_IO_TYPES];

    uint64_t            io_asynch_read  [OX_STATS_IO_AREAD_TH];
    uint64_t            io_asynch_bytes [OX_STATS_IO_AREAD_TH];
    uint64_t            io_asynch_user_r[OX_STATS_IO_AREAD_TH];

    uint64_t            rec    [OX_STATS_REC_TYPES];
    uint64_t            log    [OX_STATS_LOG_TYPES];
    uint64_t            cp     [OX_STATS_CP_TYPES];
};

static struct ox_stats_data ox_stats;

static uint8_t reset_read;

void ox_stats_print_checkpoint (void)
{
    struct app_rec_entry *tail_e, *head_e, *prov_ch_e, *ts_e;
    struct nvm_ppa_addr tail, head, cp_addr;
    uint32_t prov_ch;
    uint64_t ts, tinies;

    tail_e    = oxapp()->recovery->get_fn (APP_CP_LOG_TAIL);
    head_e    = oxapp()->recovery->get_fn (APP_CP_LOG_HEAD);
    prov_ch_e = oxapp()->recovery->get_fn (APP_CP_GL_PROV_CH);
    ts_e      = oxapp()->recovery->get_fn (APP_CP_TIMESTAMP);

    tail.ppa = (tail_e) ? ((struct nvm_ppa_addr *) tail_e->data)->ppa : 0;
    head.ppa = (head_e) ? ((struct nvm_ppa_addr *) head_e->data)->ppa : 0;
    prov_ch  = (prov_ch_e) ? *(uint32_t *) prov_ch_e->data : 0;
    ts       = (ts_e) ? *(uint64_t *) ts_e->data : 0;

    cp_addr.ppa = ox_stats.cp[OX_STATS_CP_LOAD_ADDR];
    tinies = ox_stats.cp[OX_STATS_CP_MAP_TINY_SZ] +
                                        ox_stats.cp[OX_STATS_CP_BLK_TINY_SZ];

    printf ("\n Checkpoint interval : %.2f sec\n", (double) APP_CP_INTERVAL);
    printf ("\n Metadata sizes\n");

    printf ("   mapping (BIG)   : %8.3lf MB (%lu bytes)\n",
            (double) ox_stats.cp[OX_STATS_CP_MAP_BIG_SZ] / (double) 1048576,
            ox_stats.cp[OX_STATS_CP_MAP_BIG_SZ]);

    printf ("   mapping (SMALL) : %8.3lf MB (%lu bytes)\n",
            (double) ox_stats.cp[OX_STATS_CP_MAP_SMALL_SZ] / (double) 1048576,
            ox_stats.cp[OX_STATS_CP_MAP_SMALL_SZ]);

    printf ("   mapping (TINY)  : %8.3lf MB (%lu bytes)\n",
            (double) ox_stats.cp[OX_STATS_CP_MAP_TINY_SZ] / (double) 1048576,
            ox_stats.cp[OX_STATS_CP_MAP_TINY_SZ]);

    printf ("   block (SMALL)   : %8.3lf MB (%lu bytes)\n",
            (double) ox_stats.cp[OX_STATS_CP_BLK_SMALL_SZ] / (double) 1048576,
            ox_stats.cp[OX_STATS_CP_BLK_SMALL_SZ]);

    printf ("   block (TINY)    : %8.3lf MB (%lu bytes)\n",
            (double) ox_stats.cp[OX_STATS_CP_BLK_TINY_SZ] / (double) 1048576,
            ox_stats.cp[OX_STATS_CP_BLK_TINY_SZ]);

    printf ("\n Latest checkpoint\n");
    printf ("   size          : %.3lf KB (%lu bytes)\n",
            (double) ox_stats.cp[OX_STATS_CP_SZ] / (double) 1024,
            ox_stats.cp[OX_STATS_CP_SZ]);
    printf ("   location      : (%d/%d/%d/%d)\n",
                cp_addr.g.ch, cp_addr.g.lun, cp_addr.g.blk, cp_addr.g.pg);
    printf ("   content\n");
    printf ("     timestamp   : %lu\n", ts);
    printf ("     tiny tables : %.3lf KB (%lu bytes)\n",
                                    (double) tinies / (double) 1024, tinies);
    printf ("     log tail    : (%d/%d/%d/%d)\n",
                            tail.g.ch, tail.g.lun, tail.g.blk, tail.g.pg);
    printf ("     log head    : (%d/%d/%d/%d)\n",
                            head.g.ch, head.g.lun, head.g.blk, head.g.pg);
    printf ("     prov ch     : %d\n", prov_ch);

    printf ("   persisted metadata\n");
    printf ("     map (BIG)   : %lu pages\n", ox_stats.cp[OX_STATS_CP_MAP_EVICT]);
    printf ("     map (SMALL) : %lu pages\n", ox_stats.cp[OX_STATS_CP_MAPMD_EVICT]);
    printf ("     blk (SMALL) : %lu pages\n", ox_stats.cp[OX_STATS_CP_BLK_EVICT]);
    printf ("\n");
}

void ox_stats_print_recovery (void)
{
    uint64_t startup, map, other, logs = 0;
    uint32_t log_i;

    startup = ox_stats.rec[OX_STATS_REC_START2_US] -
                                        ox_stats.rec[OX_STATS_REC_START1_US];
    map = ox_stats.rec[OX_STATS_REC_CH_MAP_US] +
                                        ox_stats.rec[OX_STATS_REC_GL_MAP_US];
    other = startup -
                ox_stats.rec[OX_STATS_REC_CP_READ_US] -
                ox_stats.rec[OX_STATS_REC_BBT_US] -
                ox_stats.rec[OX_STATS_REC_BLK_US] -
                map -
                ox_stats.rec[OX_STATS_REC_REPLAY_US] -
                ox_stats.rec[OX_STATS_REC_CP_WRITE_US];

    for (log_i = 1; log_i < OX_STATS_LOG_TYPES; log_i++)
        logs += ox_stats.log[log_i];

    printf ("\n Startup time      : %9.6lf sec\n", (double) startup / (double) 1000000);
    printf ("   checkpoint read : %9.6lf sec\n",
            (double) ox_stats.rec[OX_STATS_REC_CP_READ_US] / (double) 1000000);
    printf ("   bad block       : %9.6lf sec\n",
            (double) ox_stats.rec[OX_STATS_REC_BBT_US] / (double) 1000000);
    printf ("   block metadata  : %9.6lf sec\n",
            (double) ox_stats.rec[OX_STATS_REC_BLK_US] / (double) 1000000);
    printf ("   mapping         : %9.6lf sec\n", (double) map / (double) 1000000);
    printf ("     SMALL (read)  : %9.6lf sec\n",
            (double) ox_stats.rec[OX_STATS_REC_CH_MAP_US] / (double) 1000000);
    printf ("     BIG   (cache) : %9.6lf sec\n",
            (double) ox_stats.rec[OX_STATS_REC_GL_MAP_US] / (double) 1000000);
    printf ("   log replay      : %9.6lf sec\n",
            (double) ox_stats.rec[OX_STATS_REC_REPLAY_US] / (double) 1000000);
    printf ("   new checkpoint  : %9.6lf sec\n",
            (double) ox_stats.rec[OX_STATS_REC_CP_WRITE_US] / (double) 1000000);
    printf ("   other (ox-ctrl) : %9.6lf sec\n", (double) other / (double) 1000000);

    printf ("\n Log replay information\n");
    printf ("   log chain\n");
    printf ("     NVM pages      : %lu (%.5f MB)\n",
                ox_stats.rec[OX_STATS_REC_LOG_PGS],
                (double) ox_stats.rec[OX_STATS_REC_LOG_SZ] / (double) 1048576);
    printf ("     dropped logs   : %lu\n", ox_stats.rec[OX_STATS_REC_DROPPED_LOGS]);
    printf ("     log entries    : %lu\n", logs);
    printf ("       chain ptr    : %lu\n", ox_stats.log[APP_LOG_POINTER]);
    printf ("       namespace    : %lu\n", ox_stats.log[APP_LOG_WRITE]);
    printf ("       map (BIG)    : %lu\n", ox_stats.log[APP_LOG_MAP]);
    printf ("       map (SMALL)  : %lu\n", ox_stats.log[APP_LOG_MAP_MD]);
    printf ("       blk (SMALL)  : %lu\n", ox_stats.log[APP_LOG_BLK_MD]);
    printf ("       GC namespace : %lu\n", ox_stats.log[APP_LOG_GC_WRITE]);
    printf ("       GC map (BIG) : %lu\n", ox_stats.log[APP_LOG_GC_MAP]);
    printf ("       amend        : %lu\n", ox_stats.log[APP_LOG_AMEND]);
    printf ("       commit       : %lu\n", ox_stats.log[APP_LOG_COMMIT]);
    printf ("       abort write  : %lu\n", ox_stats.log[APP_LOG_ABORT_W]);
    printf ("       recycle blk  : %lu\n", ox_stats.log[APP_LOG_PUT_BLK]);
    printf ("   transactions\n");
    printf ("     committed      : %lu\n", ox_stats.rec[OX_STATS_REC_TR_COMMIT]);
    printf ("     aborted        : %lu\n", ox_stats.rec[OX_STATS_REC_TR_ABORT]);
    printf ("\n");
}

static void ox_stats_print_gc_blks (void)
{
    uint64_t tot_w, tot_r;
    uint64_t *val;

    tot_w = ox_stats.io[OX_STATS_SEC_GC_USER_W] +
            ox_stats.io[OX_STATS_SEC_GC_MAP_W] +
            ox_stats.io[OX_STATS_SEC_GC_PAD_W];

    tot_r = ox_stats.io[OX_STATS_SEC_GC_USER_R] +
            ox_stats.io[OX_STATS_SEC_GC_MAP_R] +
            ox_stats.io[OX_STATS_SEC_GC_MAPMD_R] +
            ox_stats.io[OX_STATS_SEC_GC_BLK_R] +
            ox_stats.io[OX_STATS_SEC_GC_LOG_R] +
            ox_stats.io[OX_STATS_SEC_GC_PAD_R] +
            ox_stats.io[OX_STATS_SEC_GC_UNKOWN];

    printf ("\n Garbage Collection blocks (%d bytes each):\n",
                                                        NVME_KERNEL_PG_SIZE);

    val = &tot_w;
    printf ("   write          : %-7lu -> %10.2lf MB (%lu bytes)\n", *val,
                (double) (*val * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                *val * NVME_KERNEL_PG_SIZE);

    val = &ox_stats.io[OX_STATS_SEC_GC_USER_W];
    printf ("      namespace   : %-7lu -> %10.2lf MB (%lu bytes)\n", *val,
                (double) (*val * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                *val * NVME_KERNEL_PG_SIZE);

    val = &ox_stats.io[OX_STATS_SEC_GC_MAP_W];
    printf ("      map (BIG)   : %-7lu -> %10.2lf MB (%lu bytes)\n", *val,
                (double) (*val * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                *val * NVME_KERNEL_PG_SIZE);

    val = &ox_stats.io[OX_STATS_SEC_GC_PAD_W];
    printf ("      padding     : %-7lu -> %10.2lf MB (%lu bytes)\n", *val,
                (double) (*val * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                *val * NVME_KERNEL_PG_SIZE);

    printf ("   read           : %-7lu -> %10.2lf MB (%lu bytes)\n", tot_r,
                (double) (tot_r * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                tot_r * NVME_KERNEL_PG_SIZE);

    val = &ox_stats.io[OX_STATS_SEC_GC_USER_R];
    printf ("      namespace   : %-7lu -> %10.2lf MB (%lu bytes)\n", *val,
                (double) (*val * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                *val * NVME_KERNEL_PG_SIZE);

    val = &ox_stats.io[OX_STATS_SEC_GC_MAP_R];
    printf ("      map (BIG)   : %-7lu -> %10.2lf MB (%lu bytes)\n", *val,
                (double) (*val * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                *val * NVME_KERNEL_PG_SIZE);

    val = &ox_stats.io[OX_STATS_SEC_GC_MAPMD_R];
    printf ("      map (SMALL) : %-7lu -> %10.2lf MB (%lu bytes)\n", *val,
                (double) (*val * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                *val * NVME_KERNEL_PG_SIZE);

    val = &ox_stats.io[OX_STATS_SEC_GC_BLK_R];
    printf ("      blk (SMALL) : %-7lu -> %10.2lf MB (%lu bytes)\n", *val,
                (double) (*val * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                *val * NVME_KERNEL_PG_SIZE);

    val = &ox_stats.io[OX_STATS_SEC_GC_LOG_R];
    printf ("      log (WAL)   : %-7lu -> %10.2lf MB (%lu bytes)\n", *val,
                (double) (*val * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                *val * NVME_KERNEL_PG_SIZE);

    val = &ox_stats.io[OX_STATS_SEC_GC_PAD_R];
    printf ("      padding     : %-7lu -> %10.2lf MB (%lu bytes)\n", *val,
                (double) (*val * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                *val * NVME_KERNEL_PG_SIZE);

    val = &ox_stats.io[OX_STATS_SEC_GC_UNKOWN];
    printf ("      unknown     : %-7lu -> %10.2lf MB (%lu bytes)\n", *val,
                (double) (*val * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                *val * NVME_KERNEL_PG_SIZE);
}

void ox_stats_print_gc (void)
{
    printf ("\n Garbage Collection events\n");
    printf ("   recycled NVM blocks   : %lu\n", ox_stats.io[OX_STATS_GC_BLOCK_REC]);
    printf ("   reclaimed space       : %.2lf MB (%lu bytes)\n",
            (double) ox_stats.io[OX_STATS_GC_SPACE_REC] / (double) 1048576,
            ox_stats.io[OX_STATS_GC_SPACE_REC]);
    printf ("   race conditions\n");
    printf ("      map update (BIG)   : %lu\n", ox_stats.io[OX_STATS_GC_RACE_BIG]);
    printf ("      map update (SMALL) : %lu\n", ox_stats.io[OX_STATS_GC_RACE_SMALL]);
    printf ("   write failures        : %lu\n", ox_stats.io[OX_STATS_SEC_GC_FAILED]);

    ox_stats_print_gc_blks ();
    printf ("\n");
}

static void ox_stats_reset_read (void)
{
    /* reset read values */
    if (reset_read) {
        pthread_spin_lock (&ox_stats.io_spin[OX_STATS_IO_ASYNC_R]);
        if (!reset_read) {
            pthread_spin_unlock (&ox_stats.io_spin[OX_STATS_IO_ASYNC_R]);
            return;
        }
        memset (ox_stats.io_asynch_read, 0x0, sizeof(uint64_t) * OX_STATS_IO_AREAD_TH);
        memset (ox_stats.io_asynch_bytes, 0x0, sizeof(uint64_t) * OX_STATS_IO_AREAD_TH);
        memset (ox_stats.io_asynch_user_r, 0x0, sizeof(uint64_t) * OX_STATS_IO_AREAD_TH);
        reset_read = 0;
        pthread_spin_unlock (&ox_stats.io_spin[OX_STATS_IO_ASYNC_R]);
    }
}

void ox_stats_print_io (void)
{
    uint64_t tot_io, tot_io_w, tot_io_r, tot_md_w, tot_md_r;
    uint64_t user_w, map_w, pad_w,
             user_synch_r, map_r, mapmd_r, blk_r, log_r, pad_r, other_r;
    double tot_b, tot_b_w, tot_b_r;
    uint32_t th_i;

    ox_stats_reset_read  ();

    /* Sum read threads */
    ox_stats.io[OX_STATS_IO_ASYNC_R] = 0;
    ox_stats.io[OX_STATS_SEC_USER_R] = 0;
    ox_stats.io[OX_STATS_BYTES_ASYNC_R] = 0;
    for (th_i = 0; th_i < OX_STATS_IO_AREAD_TH; th_i++) {
        ox_stats.io[OX_STATS_IO_ASYNC_R] += ox_stats.io_asynch_read[th_i];
        ox_stats.io[OX_STATS_SEC_USER_R] += ox_stats.io_asynch_user_r[th_i];
        ox_stats.io[OX_STATS_BYTES_ASYNC_R] += ox_stats.io_asynch_bytes[th_i];
    }

    tot_io_w = ox_stats.io[OX_STATS_IO_SYNC_W] +
               ox_stats.io[OX_STATS_IO_ASYNC_W];
    tot_io_r = ox_stats.io[OX_STATS_IO_SYNC_R] +
               ox_stats.io[OX_STATS_IO_ASYNC_R];
    
    tot_io = tot_io_w + tot_io_r;

    tot_b_w = ox_stats.io[OX_STATS_BYTES_SYNC_W] +
               ox_stats.io[OX_STATS_BYTES_ASYNC_W];
    tot_b_r = ox_stats.io[OX_STATS_BYTES_SYNC_R] +
               ox_stats.io[OX_STATS_BYTES_ASYNC_R];
    
    tot_b = tot_b_w + tot_b_r;

    printf ("\n Physical I/O count: %lu\n", tot_io);
    printf ("   write : %-7lu -> user: %-7lu meta+gc: %lu\n", tot_io_w,
            ox_stats.io[OX_STATS_IO_ASYNC_W], ox_stats.io[OX_STATS_IO_SYNC_W]);
    printf ("   read  : %-7lu -> user: %-7lu meta+gc: %lu\n", tot_io_r,
            ox_stats.io[OX_STATS_IO_ASYNC_R], ox_stats.io[OX_STATS_IO_SYNC_R]);
    printf ("   erase : %lu\n", ox_stats.io[OX_STATS_IO_ERASE]);

    printf ("\n\n Data transferred (to/from NVM): %.2f MB (%lu bytes)\n",
                            tot_b / (double) 1048576, (uint64_t) tot_b);
    printf ("   data written     : %10.2lf MB (%lu bytes)\n",
                    (double) tot_b_w / (double) 1048576, (uint64_t) tot_b_w);
    printf ("      namespace+pad : %10.2lf MB (%lu bytes)\n",
                (double) ox_stats.io[OX_STATS_BYTES_ASYNC_W] / (double) 1048576,
                ox_stats.io[OX_STATS_BYTES_ASYNC_W]);
    printf ("      meta+gc       : %10.2lf MB (%lu bytes)\n",
                (double) ox_stats.io[OX_STATS_BYTES_SYNC_W] / (double) 1048576,
                ox_stats.io[OX_STATS_BYTES_SYNC_W]);
    printf ("   data read        : %10.2lf MB (%lu bytes)\n",
                (double) tot_b_r / (double) 1048576, (uint64_t) tot_b_r);
    printf ("      namespace+pad : %10.2lf MB (%lu bytes)\n",
                (double) ox_stats.io[OX_STATS_BYTES_ASYNC_R] / (double) 1048576,
                ox_stats.io[OX_STATS_BYTES_ASYNC_R]);
    printf ("      meta+gc       : %10.2lf MB (%lu bytes)\n",
                (double) ox_stats.io[OX_STATS_BYTES_SYNC_R] / (double) 1048576,
                ox_stats.io[OX_STATS_BYTES_SYNC_R]);

    /* Fix numbers by subtracting the GC */
    user_w = ox_stats.io[OX_STATS_SEC_USER_W] - ox_stats.io[OX_STATS_SEC_GC_USER_W];
    map_w = ox_stats.io[OX_STATS_SEC_MAP_W] - ox_stats.io[OX_STATS_SEC_GC_MAP_W];
    pad_w = ox_stats.io[OX_STATS_SEC_PAD_W] - ox_stats.io[OX_STATS_SEC_GC_PAD_W];

    user_synch_r = ox_stats.io[OX_STATS_SYNCH_USER_R] - ox_stats.io[OX_STATS_SEC_GC_USER_R];
    map_r = ox_stats.io[OX_STATS_SEC_MAP_R] - ox_stats.io[OX_STATS_SEC_GC_MAP_R];
    mapmd_r = ox_stats.io[OX_STATS_SEC_CP_MAPMD_R] - ox_stats.io[OX_STATS_SEC_GC_MAPMD_R];
    blk_r = ox_stats.io[OX_STATS_SEC_CP_BLK_R] - ox_stats.io[OX_STATS_SEC_GC_BLK_R];
    log_r = ox_stats.io[OX_STATS_SEC_LOG_R] - ox_stats.io[OX_STATS_SEC_GC_LOG_R];
    pad_r = ox_stats.io[OX_STATS_SEC_PAD_R] - ox_stats.io[OX_STATS_SEC_GC_PAD_R];
    other_r = ox_stats.io[OX_STATS_SEC_OTHER_R] - ox_stats.io[OX_STATS_SEC_GC_UNKOWN];

    tot_md_w = map_w +
               ox_stats.io[OX_STATS_SEC_LOG_W] +
               pad_w +
               ox_stats.io[OX_STATS_SEC_CP_MAPMD_W] +
               ox_stats.io[OX_STATS_SEC_CP_BLK_W] +
               ox_stats.io[OX_STATS_SEC_RSV_W] +
               ox_stats.io[OX_STATS_SEC_CP_W] +
               ox_stats.io[OX_STATS_SEC_OTHER_W];

    tot_md_r = map_r +
               log_r +
               pad_r +
               mapmd_r +
               blk_r +
               ox_stats.io[OX_STATS_SEC_RSV_R] +
               ox_stats.io[OX_STATS_SEC_CP_R] +
               other_r +
               user_synch_r;

    printf ("\n\n Namespace blocks (%d bytes each):\n", NVME_KERNEL_PG_SIZE);

    printf ("   write          : %-7lu -> %10.2lf MB (%lu bytes)\n",
                user_w,
                (double) (user_w * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                user_w * NVME_KERNEL_PG_SIZE);

    printf ("   read           : %-7lu -> %10.2lf MB (%lu bytes)\n",
                ox_stats.io[OX_STATS_SEC_USER_R],
                (double) (ox_stats.io[OX_STATS_SEC_USER_R] *
                NVME_KERNEL_PG_SIZE) / (double) 1048576,
                ox_stats.io[OX_STATS_SEC_USER_R] * NVME_KERNEL_PG_SIZE);

    printf ("\n Metadata blocks (%d bytes each):\n", NVME_KERNEL_PG_SIZE);

    printf ("   write          : %-7lu -> %10.2lf MB (%lu bytes)\n",
                tot_md_w, (double) (tot_md_w * NVME_KERNEL_PG_SIZE) /
                                                            (double) 1048576,
                tot_md_w * NVME_KERNEL_PG_SIZE);

    printf ("      map (BIG)   : %-7lu -> %10.2lf MB (%lu bytes)\n",
                map_w,
                (double) (map_w * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                map_w * NVME_KERNEL_PG_SIZE);

    printf ("      map (SMALL) : %-7lu -> %10.2lf MB (%lu bytes)\n",
                ox_stats.io[OX_STATS_SEC_CP_MAPMD_W],
                (double) (ox_stats.io[OX_STATS_SEC_CP_MAPMD_W] *
                                    NVME_KERNEL_PG_SIZE) / (double) 1048576,
                ox_stats.io[OX_STATS_SEC_CP_MAPMD_W] * NVME_KERNEL_PG_SIZE);

    printf ("      blk (SMALL) : %-7lu -> %10.2lf MB (%lu bytes)\n",
                ox_stats.io[OX_STATS_SEC_CP_BLK_W],
                (double) (ox_stats.io[OX_STATS_SEC_CP_BLK_W] *
                                    NVME_KERNEL_PG_SIZE) / (double) 1048576,
                ox_stats.io[OX_STATS_SEC_CP_BLK_W] * NVME_KERNEL_PG_SIZE);

    printf ("      log (WAL)   : %-7lu -> %10.2lf MB (%lu bytes)\n",
                ox_stats.io[OX_STATS_SEC_LOG_W],
                (double) (ox_stats.io[OX_STATS_SEC_LOG_W] *
                                    NVME_KERNEL_PG_SIZE) / (double) 1048576,
                ox_stats.io[OX_STATS_SEC_LOG_W] * NVME_KERNEL_PG_SIZE);

    printf ("      padding     : %-7lu -> %10.2lf MB (%lu bytes)\n",
                pad_w,
                (double) (pad_w * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                pad_w * NVME_KERNEL_PG_SIZE);

    printf ("      checkpoint  : %-7lu -> %10.2lf MB (%lu bytes)\n",
                ox_stats.io[OX_STATS_SEC_CP_W],
                (double) (ox_stats.io[OX_STATS_SEC_CP_W] *
                                    NVME_KERNEL_PG_SIZE) / (double) 1048576,
                ox_stats.io[OX_STATS_SEC_CP_W] * NVME_KERNEL_PG_SIZE);

    printf ("      reserved    : %-7lu -> %10.2lf MB (%lu bytes)\n",
                ox_stats.io[OX_STATS_SEC_RSV_W],
                (double) (ox_stats.io[OX_STATS_SEC_RSV_W] *
                                    NVME_KERNEL_PG_SIZE) / (double) 1048576,
                ox_stats.io[OX_STATS_SEC_RSV_W] * NVME_KERNEL_PG_SIZE);

    printf ("      other       : %-7lu -> %10.2lf MB (%lu bytes)\n",
                ox_stats.io[OX_STATS_SEC_OTHER_W],
                (double) (ox_stats.io[OX_STATS_SEC_OTHER_W] *
                                    NVME_KERNEL_PG_SIZE) / (double) 1048576,
                ox_stats.io[OX_STATS_SEC_OTHER_W] * NVME_KERNEL_PG_SIZE);

    printf ("   read           : %-7lu -> %10.2lf MB (%lu bytes)\n",
                tot_md_r, (double) (tot_md_r * NVME_KERNEL_PG_SIZE) /
                                                            (double) 1048576,
                tot_md_r * NVME_KERNEL_PG_SIZE);

    printf ("      map (BIG)   : %-7lu -> %10.2lf MB (%lu bytes)\n",
                map_r,
                (double) (map_r * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                map_r * NVME_KERNEL_PG_SIZE);

    printf ("      map (SMALL) : %-7lu -> %10.2lf MB (%lu bytes)\n",
                mapmd_r,
                (double) (mapmd_r * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                mapmd_r * NVME_KERNEL_PG_SIZE);

    printf ("      blk (SMALL) : %-7lu -> %10.2lf MB (%lu bytes)\n",
                blk_r,
                (double) (blk_r * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                blk_r * NVME_KERNEL_PG_SIZE);

    printf ("      log (WAL)   : %-7lu -> %10.2lf MB (%lu bytes)\n",
                log_r,
                (double) (log_r * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                log_r * NVME_KERNEL_PG_SIZE);

    printf ("      padding     : %-7lu -> %10.2lf MB (%lu bytes)\n",
                pad_r,
                (double) (pad_r * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                pad_r * NVME_KERNEL_PG_SIZE);

    printf ("      checkpoint  : %-7lu -> %10.2lf MB (%lu bytes)\n",
                ox_stats.io[OX_STATS_SEC_CP_R],
                (double) (ox_stats.io[OX_STATS_SEC_CP_R] *
                                    NVME_KERNEL_PG_SIZE) / (double) 1048576,
                ox_stats.io[OX_STATS_SEC_CP_R] * NVME_KERNEL_PG_SIZE);

    printf ("      reserved    : %-7lu -> %10.2lf MB (%lu bytes)\n",
                ox_stats.io[OX_STATS_SEC_RSV_R],
                (double) (ox_stats.io[OX_STATS_SEC_RSV_R] *
                                    NVME_KERNEL_PG_SIZE) / (double) 1048576,
                ox_stats.io[OX_STATS_SEC_RSV_R] * NVME_KERNEL_PG_SIZE);

    printf ("      namespace * : %-7lu -> %10.2lf MB (%lu bytes)\n",
                user_synch_r,
                (double) (user_synch_r * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                user_synch_r * NVME_KERNEL_PG_SIZE);

    printf ("      other     * : %-7lu -> %10.2lf MB (%lu bytes)\n",
                other_r,
                (double) (other_r * NVME_KERNEL_PG_SIZE) / (double) 1048576,
                other_r * NVME_KERNEL_PG_SIZE);

    ox_stats_print_gc_blks ();
    printf ("\n");
}

void ox_stats_add_gc (uint16_t type, uint8_t is_write, uint32_t count)
{
    uint16_t index;

    switch (type) {
        case APP_PG_NAMESPACE:
            index =  (is_write) ? OX_STATS_SEC_GC_USER_W : OX_STATS_SEC_GC_USER_R;
            break;
        case APP_PG_MAP:
            index =  (is_write) ? OX_STATS_SEC_GC_MAP_W : OX_STATS_SEC_GC_MAP_R;
            break;
        case APP_PG_MAP_MD:
            index = OX_STATS_SEC_GC_MAPMD_R;
            break;
        case APP_PG_BLK_MD:
            index = OX_STATS_SEC_GC_BLK_R;
            break;
        case APP_PG_PADDING:
            index =  (is_write) ? OX_STATS_SEC_GC_PAD_W : OX_STATS_SEC_GC_PAD_R;
            break;
        case APP_PG_LOG:
            index = OX_STATS_SEC_GC_LOG_R;
            break;
        case APP_PG_UNKNOWN:
        default:
            index = OX_STATS_SEC_GC_UNKOWN;
            break;
    }

    pthread_spin_lock (&ox_stats.io_spin[index]);
    ox_stats.io[index] += count;
    pthread_spin_unlock (&ox_stats.io_spin[index]);
}

void ox_stats_add_event (uint16_t type, uint32_t count)
{
    uint16_t index;

    switch (type) {
        case OX_GC_EVENT_BLOCK_REC:
            index = OX_STATS_GC_BLOCK_REC;
            break;
        case OX_GC_EVENT_FAILED:
            index = OX_STATS_SEC_GC_FAILED;
            break;
        case OX_GC_EVENT_RACE_BIG:
            index = OX_STATS_GC_RACE_BIG;
            break;
        case OX_GC_EVENT_RACE_SMALL:
            index = OX_STATS_GC_RACE_SMALL;
            break;
        case OX_GC_EVENT_SPACE_REC:
            index = OX_STATS_GC_SPACE_REC;
            break;
        case OX_CP_EVENT_FLUSH:
            index = OX_STATS_SEC_CP_TINY;
            break;
        default:
            return;
    }

    pthread_spin_lock (&ox_stats.io_spin[index]);
    ox_stats.io[index] += count;
    pthread_spin_unlock (&ox_stats.io_spin[index]);
}

void ox_stats_add_io (struct nvm_mmgr_io_cmd *cmd, uint8_t synch, uint16_t tid)
{
    struct app_sec_oob *oob = (struct app_sec_oob *) cmd->md_prp;
    struct nvm_ppa_addr ppa;
    uint32_t sec_i;
    uint8_t is_write = 0;
    uint16_t index, index2;

    switch (cmd->cmdtype) {

        case MMGR_READ_PG:
        case MMGR_READ_SGL:
        case MMGR_READ_PL_PG:
            index =  (synch) ? OX_STATS_IO_SYNC_R : OX_STATS_IO_ASYNC_R;
            index2 = (synch) ? OX_STATS_BYTES_SYNC_R : OX_STATS_BYTES_ASYNC_R;
            break;

        case MMGR_WRITE_PG:
        case MMGR_WRITE_SGL:
        case MMGR_WRITE_PL_PG:
            index =  (synch) ? OX_STATS_IO_SYNC_W : OX_STATS_IO_ASYNC_W;
            index2 = (synch) ? OX_STATS_BYTES_SYNC_W : OX_STATS_BYTES_ASYNC_W;
            is_write = 1;
            break;

        case MMGR_ERASE_BLK:
            pthread_spin_lock (&ox_stats.io_spin[OX_STATS_IO_ERASE]);
            ox_stats.io[OX_STATS_IO_ERASE]++;
            pthread_spin_unlock (&ox_stats.io_spin[OX_STATS_IO_ERASE]);
            return;

        default:
            return;
    }

    ox_stats_reset_read  ();

    if (!is_write && !synch) {
        ox_stats.io_asynch_read[tid]++;
    } else {
        pthread_spin_lock (&ox_stats.io_spin[index]);
        ox_stats.io[index]++;
        ox_stats.io[index2] += cmd->n_sectors * cmd->sec_sz;
        pthread_spin_unlock (&ox_stats.io_spin[index]);
    }

    for (sec_i = 0; sec_i < cmd->n_sectors; sec_i++) {

        /* user asynchronous reads have non-sequential sectors */
        if (!is_write && !synch) {
            if (cmd->prp[sec_i]) {
               ox_stats.io_asynch_user_r[tid]++;
               ox_stats.io_asynch_bytes[tid] += cmd->sec_sz;
            }
            continue;
        }

        /* check if it is a checkpoint block */
        /* TODO: Make it automatic, for now, checkpoint is fixed in a single blk */
        if ( (cmd->ppa.g.lun == 0) &&
             (cmd->ppa.g.blk == APP_RSV_CP_OFF + cmd->ch->mmgr_rsv)) {
            index = (is_write) ? OX_STATS_SEC_CP_W : OX_STATS_SEC_CP_R;
            pthread_spin_lock (&ox_stats.io_spin[index]);
            ox_stats.io[index] += cmd->n_sectors;
            pthread_spin_unlock (&ox_stats.io_spin[index]);
            break;
        }

        /* check if it is a reserved block */
        ppa.ppa = 0;
        ppa.g.ch = cmd->ppa.g.ch;
        ppa.g.lun = cmd->ppa.g.lun;
        ppa.g.blk = cmd->ppa.g.blk;
        if (ox_contains_ppa(cmd->ch->mmgr_rsv_list, cmd->ch->mmgr_rsv *
                                cmd->ch->geometry->n_of_planes, ppa) ||
            ox_contains_ppa(cmd->ch->ftl_rsv_list, cmd->ch->ftl_rsv *
                                cmd->ch->geometry->n_of_planes, ppa)){
            index = (is_write) ? OX_STATS_SEC_RSV_W : OX_STATS_SEC_RSV_R;
            pthread_spin_lock (&ox_stats.io_spin[index]);
            ox_stats.io[index] += cmd->n_sectors;
            pthread_spin_unlock (&ox_stats.io_spin[index]);
            break;
        }

        switch (oob[sec_i].pg_type) {
            
            case APP_PG_NAMESPACE:
                index = (is_write) ? OX_STATS_SEC_USER_W : OX_STATS_SYNCH_USER_R;
                break;
            case APP_PG_MAP:
                index = (is_write) ? OX_STATS_SEC_MAP_W : OX_STATS_SEC_MAP_R;
                break;
            case APP_PG_MAP_MD:
                index = (is_write) ? OX_STATS_SEC_CP_MAPMD_W :
                                     OX_STATS_SEC_CP_MAPMD_R;
                break;
            case APP_PG_BLK_MD:
                index = (is_write) ? OX_STATS_SEC_CP_BLK_W :
                                     OX_STATS_SEC_CP_BLK_R;
                break;
            case APP_PG_PADDING:
                index = (is_write) ? OX_STATS_SEC_PAD_W : OX_STATS_SEC_PAD_R;
                break;
            case APP_PG_LOG:
                index = (is_write) ? OX_STATS_SEC_LOG_W : OX_STATS_SEC_LOG_R;
                break;
            default:
                index = (is_write) ? OX_STATS_SEC_OTHER_W : OX_STATS_SEC_OTHER_R;
        }

        pthread_spin_lock (&ox_stats.io_spin[index]);
        ox_stats.io[index]++;
        pthread_spin_unlock (&ox_stats.io_spin[index]);
    }
}

void ox_stats_add_rec (uint16_t type, uint64_t value)
{
    ox_stats.rec[type] += value;
}

void ox_stats_add_log (uint16_t type, uint64_t count)
{
    ox_stats.log[type] += count;
}

void ox_stats_add_cp (uint16_t type, uint64_t value)
{
    ox_stats.cp[type] += value;
}

void ox_stats_set_cp (uint16_t type, uint64_t value)
{
    ox_stats.cp[type] = value;
}

void ox_stats_reset_io (void)
{
    uint32_t type_i;

    pthread_spin_lock (&ox_stats.io_spin[OX_STATS_IO_SYNC_R]);
    ox_stats.io[OX_STATS_IO_SYNC_R] = 0;
    ox_stats.io[OX_STATS_BYTES_SYNC_R] = 0;
    pthread_spin_unlock (&ox_stats.io_spin[OX_STATS_IO_SYNC_R]);

    pthread_spin_lock (&ox_stats.io_spin[OX_STATS_IO_SYNC_W]);
    ox_stats.io[OX_STATS_IO_SYNC_W] = 0;
    ox_stats.io[OX_STATS_BYTES_SYNC_W] = 0;
    pthread_spin_unlock (&ox_stats.io_spin[OX_STATS_IO_SYNC_W]);
    
    pthread_spin_lock (&ox_stats.io_spin[OX_STATS_IO_ASYNC_R]);
    ox_stats.io[OX_STATS_IO_ASYNC_R] = 0;
    ox_stats.io[OX_STATS_BYTES_ASYNC_R] = 0;
    pthread_spin_unlock (&ox_stats.io_spin[OX_STATS_IO_ASYNC_R]);

    pthread_spin_lock (&ox_stats.io_spin[OX_STATS_IO_ASYNC_W]);
    ox_stats.io[OX_STATS_IO_ASYNC_W] = 0;
    ox_stats.io[OX_STATS_BYTES_ASYNC_W] = 0;
    pthread_spin_unlock (&ox_stats.io_spin[OX_STATS_IO_ASYNC_W]);

    pthread_spin_lock (&ox_stats.io_spin[OX_STATS_IO_ERASE]);
    ox_stats.io[OX_STATS_IO_ERASE] = 0;
    pthread_spin_unlock (&ox_stats.io_spin[OX_STATS_IO_ERASE]);

    for (type_i = 9; type_i < OX_STATS_IO_TYPES; type_i++) {
        pthread_spin_lock (&ox_stats.io_spin[type_i]);
        ox_stats.io[type_i] = 0;
        pthread_spin_unlock (&ox_stats.io_spin[type_i]);
    }

    reset_read = 1;
}

void ox_stats_exit (void)
{
    uint32_t type_i;

    for (type_i = 0; type_i < OX_STATS_IO_TYPES; type_i++)
        pthread_spin_destroy (&ox_stats.io_spin[type_i]);
}

int ox_stats_init (void)
{
    uint32_t type_i;

    for (type_i = 0; type_i < OX_STATS_IO_TYPES; type_i++) {
        if (pthread_spin_init (&ox_stats.io_spin[type_i], 0))
            goto CLEAN_IO;
    }
    memset (ox_stats.io, 0x0, sizeof(uint64_t) * OX_STATS_IO_TYPES);
    memset (ox_stats.io_asynch_read, 0x0, sizeof(uint64_t) * OX_STATS_IO_AREAD_TH);
    memset (ox_stats.io_asynch_bytes, 0x0, sizeof(uint64_t) * OX_STATS_IO_AREAD_TH);
    memset (ox_stats.io_asynch_user_r, 0x0, sizeof(uint64_t) * OX_STATS_IO_AREAD_TH);
    memset (ox_stats.rec, 0x0, sizeof(uint64_t) * OX_STATS_REC_TYPES);
    memset (ox_stats.log, 0x0, sizeof(uint64_t) * OX_STATS_LOG_TYPES);
    memset (ox_stats.cp, 0x0, sizeof(uint64_t) * OX_STATS_CP_TYPES);
    reset_read = 0;

    return 0;

CLEAN_IO:
    while (type_i) {
        type_i--;
        pthread_spin_destroy (&ox_stats.io_spin[type_i]);
    }
    return -1;
}