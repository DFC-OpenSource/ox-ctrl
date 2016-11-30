/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX MMGR Tests
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

#include "../include/tests.h"
#include "../include/ssd.h"
#include "../mmgr/dfc_nand/dfc_nand.h"
#include "../include/uatomic.h"
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern struct core_struct core;
extern struct tests_init_st tests_is;
pthread_t               *threads;
static int              thread_err;
static void             ***wbuf;
static void             ***rbuf;
extern atomic_t         pgs_ok;
extern pthread_mutex_t  pgs_ok_mutex;
extern volatile uint64_t t_usec;
extern pthread_mutex_t  usec_mutex;
extern int              rand_lun[4];
extern int              rand_blk[4];

struct tests_sync_io {
    struct nvm_channel      *ch;
    struct nvm_mmgr_io_cmd  *cmd;
    void                    **buf;
    uint16_t                cmdtype;
    int                     thread_i;
    uint16_t                total_pgs;
    uint16_t                n_pl;
};

static void test_s01_print_result (int n_pgs, uint8_t cmdtype, uint64_t usec)
{
    switch (cmdtype) {
        case MMGR_ERASE_BLK:
            printf (" %d blocks.\n      Elapsed Time (S+W)    : %llu u-sec\n",
                                                                n_pgs, usec);
            break;
        case MMGR_WRITE_PG:
        case MMGR_READ_PG:
            printf (" %d pages. Data: %llu KB.\n      Elapsed Time (S+W)    :"
            " %lu u-sec\n", n_pgs, (uint64_t) n_pgs * NVM_PG_SIZE / 1024, usec);
    }
}

int test_s01_compare_page_by_page_sync (int total_pgs)
{
    int ret, pg_i, pl_i, i;
    int n_pl = tests_is.geo.n_pl, n_pgs = 0;
    struct nvm_channel *ch;
    int pg_sz = (NVM_PG_SIZE + NVM_OOB_SIZE);
    int pl_pg = pg_sz * n_pl;
    int buf_sz = pl_pg * total_pgs;
    uint64_t usec = 0;

    void *buf = malloc(buf_sz);
    if (!buf)
        return EMEM;

    void *buf2 = malloc(buf_sz);
    if (!buf2) {
        free (buf);
        return EMEM;
    }

    void *buf_vec[total_pgs];
    void *buf_vec2[total_pgs];
    for (i = 0; i < total_pgs; i++) {
        buf_vec[i] = buf + i * pl_pg;
        buf_vec2[i] = buf2 + i * pl_pg;
    }

    struct nvm_mmgr_io_cmd *cmd = malloc(sizeof(struct nvm_mmgr_io_cmd));
    if (!cmd) {
        free(buf);
        free(buf2);
        return EMEM;
    }

    printf("     ");
    printf(".");
    fflush(stdout);

    for (pl_i = 0; pl_i < n_pl; pl_i++) {
        memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
        cmd->ppa.g.blk = rand_blk[0];
        cmd->ppa.g.pl = pl_i;
        cmd->ppa.g.lun = 0;
        ch = core.nvm_ch[0];
        cmd->ppa.g.ch = ch->ch_mmgr_id;
        ret = nvm_submit_sync_io (ch, cmd, NULL, MMGR_ERASE_BLK);
        usec += tests_get_cmd_usec(cmd);
        if (ret) {
            printf(" Erase error.\n");
            break;
        }
    }
    if (ret) goto OUT;

    printf(".");
    fflush(stdout);

    for (pg_i = 0; pg_i < total_pgs; pg_i++) {
        memset (buf_vec[pg_i], 0xac, pl_pg);
        for (pl_i = 0; pl_i < n_pl; pl_i++) {
            memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
            cmd->ppa.g.pg = pg_i;
            cmd->ppa.g.blk = rand_blk[0];
            cmd->ppa.g.pl = pl_i;
            cmd->ppa.g.lun = 0;
            ch = core.nvm_ch[0];
            cmd->ppa.g.ch = ch->ch_mmgr_id;
            ret = nvm_submit_sync_io (ch, cmd, buf_vec[pg_i]
                                                + pg_sz * pl_i, MMGR_WRITE_PG);
            usec += tests_get_cmd_usec(cmd);
            if (ret) break;
            n_pgs++;
        }
        if (ret) {
            printf(" Write error.\n");
            break;
        }
    }
    if (ret) goto OUT;

    printf(".");
    fflush(stdout);

    for (pg_i = 0; pg_i < total_pgs; pg_i++) {
        memset (buf_vec2[pg_i], 0, pl_pg);
        for (pl_i = 0; pl_i < n_pl; pl_i++) {
            memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
            cmd->ppa.g.pg = pg_i;
            cmd->ppa.g.blk = rand_blk[0];
            cmd->ppa.g.pl = pl_i;
            cmd->ppa.g.lun = 0;
            ch = core.nvm_ch[0];
            cmd->ppa.g.ch = ch->ch_mmgr_id;
            ret = nvm_submit_sync_io (ch, cmd, buf_vec2[pg_i] +
                                                 + pg_sz * pl_i, MMGR_READ_PG);
            usec += tests_get_cmd_usec(cmd);
            if (ret) break;
        }
        if (ret) {
            printf(" Write error.\n");
            break;
        }
    }
    if (ret) goto OUT;

    printf(" 1 block erased, %d pages written/read. Time: %lu u-sec\n",
                                                                n_pgs, usec);

    ret = memcmp(buf, buf2, buf_sz);
    if (!ret)
        printf("      Data are equal. SUCCESS\n");
    else
        printf("      Data are NOT equal. FAIL\n");

OUT:
    free(cmd);
    free(buf);
    free(buf2);

    return ret;
}

int test_s01_compare_data_sync (int total_pgs)
{
    int ret, pg_i, pl_i, i;
    int n_pl = tests_is.geo.n_pl, n_pgs = 0;
    struct nvm_channel *ch;
    int pg_sz = (NVM_PG_SIZE + NVM_OOB_SIZE);
    int pl_pg = pg_sz * n_pl;
    int buf_sz = pl_pg * total_pgs;
    uint64_t usec = 0;

    void *buf = malloc(buf_sz);
    if (!buf)
        return EMEM;

    void *buf2 = malloc(buf_sz);
    if (!buf2) {
        free (buf);
        return EMEM;
    }

    void *buf_vec[total_pgs];
    void *buf_vec2[total_pgs];
    for (i = 0; i < total_pgs; i++) {
        buf_vec[i] = buf + i * pl_pg;
        buf_vec2[i] = buf2 + i * pl_pg;
    }

    struct nvm_mmgr_io_cmd *cmd = malloc(sizeof(struct nvm_mmgr_io_cmd));
    if (!cmd) {
        free(buf);
        free(buf2);
        return EMEM;
    }

    printf("     ");
    printf(".");
    fflush(stdout);

    for (pl_i = 0; pl_i < n_pl; pl_i++) {
        memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
        cmd->ppa.g.blk = rand_blk[0];
        cmd->ppa.g.pl = pl_i;
        cmd->ppa.g.lun = 0;
        ch = core.nvm_ch[0];
        cmd->ppa.g.ch = ch->ch_mmgr_id;
        ret = nvm_submit_sync_io (ch, cmd, NULL, MMGR_ERASE_BLK);
        usec += tests_get_cmd_usec(cmd);
        if (ret) {
            printf(" Erase error.\n");
            break;
        }
    }
    if (ret) goto OUT;

    printf(".");
    fflush(stdout);

    for (pg_i = 0; pg_i < total_pgs; pg_i++) {
        memset (buf_vec[pg_i], 0xac, pl_pg);
        for (pl_i = 0; pl_i < n_pl; pl_i++) {
            memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
            cmd->ppa.g.pg = pg_i;
            cmd->ppa.g.blk = rand_blk[0];
            cmd->ppa.g.pl = pl_i;
            cmd->ppa.g.lun = 0;
            ch = core.nvm_ch[0];
            cmd->ppa.g.ch = ch->ch_mmgr_id;
            ret = nvm_submit_sync_io (ch, cmd, buf_vec[pg_i]
                                                + pg_sz * pl_i, MMGR_WRITE_PG);
            usec += tests_get_cmd_usec(cmd);
            if (ret) break;
            n_pgs++;
        }
        if (ret) {
            printf(" Write error.\n");
            break;
        }
    }
    if (ret) goto OUT;

    printf(".");
    fflush(stdout);

    for (pg_i = 0; pg_i < total_pgs; pg_i++) {
        memset (buf_vec2[pg_i], 0, pl_pg);
        for (pl_i = 0; pl_i < n_pl; pl_i++) {
            memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
            cmd->ppa.g.pg = pg_i;
            cmd->ppa.g.blk = rand_blk[0];
            cmd->ppa.g.pl = pl_i;
            cmd->ppa.g.lun = 0;
            ch = core.nvm_ch[0];
            cmd->ppa.g.ch = ch->ch_mmgr_id;
            ret = nvm_submit_sync_io (ch, cmd, buf_vec2[pg_i] +
                                                 + pg_sz * pl_i, MMGR_READ_PG);
            usec += tests_get_cmd_usec(cmd);
            if (ret) break;
        }
        if (ret) {
            printf(" Write error.\n");
            break;
        }
    }
    if (ret) goto OUT;

    printf(" 1 block erased, %d pages written/read. Time: %lu u-sec\n",
                                                                n_pgs, usec);

    ret = memcmp(buf, buf2, buf_sz);
    if (!ret)
        printf("      Data are equal. SUCCESS\n");
    else
        printf("      Data are NOT equal. FAIL\n");

OUT:
    free(cmd);
    free(buf);
    free(buf2);

    return ret;
}

int test_s01_full_blks_sync (uint8_t cmdtype)
{
    int ret, ch_i, pg_i, pl_i, i, match = 0;
    int total_pgs = tests_is.geo.n_pg, n_pl = tests_is.geo.n_pl, n_pgs = 0;
    struct nvm_channel *ch;
    struct timeval start, end;
    struct nvm_mmgr_io_cmd cmd[n_pl];
    void *wptr, *rptr, *buf;
    void ***cbuf;
    uint64_t usec = 0;
    uint32_t pg_sz = NVM_PG_SIZE + NVM_OOB_SIZE;

    if (cmdtype == MMGR_ERASE_BLK)
        goto ERASE;

    cbuf = malloc(sizeof(void *) * core.nvm_ch_count);
    for (ch_i = 0; ch_i < core.nvm_ch_count; ch_i++) {
        cbuf[ch_i] = malloc(sizeof(void *) * total_pgs);
        for (pg_i = 0; pg_i < total_pgs; pg_i++)
            cbuf[ch_i][pg_i] = calloc (pg_sz * n_pl, 1);
    }

    if (cmdtype == MMGR_READ_PG)
        rbuf = cbuf;
    else
        wbuf = cbuf;

ERASE:
    gettimeofday(&start, NULL);

    printf("     ");
    for (pg_i = 0; pg_i < total_pgs; pg_i++) {
        if (pg_i % 50 == 0) {
            printf(".");
            fflush(stdout);
        }
        for (ch_i = 0; ch_i < core.nvm_ch_count; ch_i++) {
            if (cmdtype == MMGR_ERASE_BLK)
                pg_i = total_pgs;

            ch = core.nvm_ch[ch_i];
            for (pl_i = 0; pl_i < n_pl; pl_i++) {
                memset (&cmd[pl_i], 0, sizeof (struct nvm_mmgr_io_cmd));
                cmd[pl_i].ppa.g.pg = pg_i;
                cmd[pl_i].ppa.g.blk = rand_blk[0];
                cmd[pl_i].ppa.g.pl = pl_i;
                cmd[pl_i].ppa.g.lun = rand_lun[0];
                cmd[pl_i].ppa.g.ch = ch->ch_mmgr_id;
            }

            if (cmdtype != MMGR_ERASE_BLK) {
                buf = cbuf[ch_i][pg_i];
                memset(buf,(cmdtype == MMGR_WRITE_PG) ? 0xac : 0, pg_sz * n_pl);
            }

            ret = nvm_submit_multi_plane_sync_io (ch, cmd, buf, cmdtype, 350);

            for (pl_i = 0; pl_i < n_pl; pl_i++)
                usec += tests_get_cmd_usec(&cmd[pl_i]);

            if (ret)
                break;
            n_pgs += n_pl;
        }
    }
    gettimeofday(&end, NULL);
    uint64_t usec_e = (end.tv_sec*(uint64_t)1000000+end.tv_usec) -
                               (start.tv_sec*(uint64_t)1000000+start.tv_usec);

    test_s01_print_result(n_pgs, cmdtype, usec_e);
    printf ("      IO Commands (sum)     : %llu u-sec\n", usec);
    printf ("      Command Average       : %llu u-sec\n", usec/(n_pgs & AND64));
    printf ("      Threads: 1\n");

    if (cmdtype == MMGR_READ_PG) {
        match = 0;
        for (ch_i = 0; ch_i < core.nvm_ch_count; ch_i++) {
            for (pg_i = 0; pg_i < total_pgs; pg_i++) {
                wptr = wbuf[ch_i][pg_i];
                rptr = rbuf[ch_i][pg_i];
                if (!memcmp (wptr, rptr, pg_sz * n_pl))
                    match++;
                free(wptr);
                free(rptr);
            }
            free(wbuf[ch_i]);
            free(rbuf[ch_i]);
        }
        free(wbuf);
        free(rbuf);

        printf ("      Page(%d pl) data   OK : %d\n", n_pl, match);
        printf ("      Page(%d pl) data  FAIL: %d\n", n_pl,
                                        total_pgs * core.nvm_ch_count - match);

        if (total_pgs * core.nvm_ch_count - match > 0)
            return -1;
    }

    return ret;
}

static void *tests_io_thread (void *arg)
{
    int pg_i, pl_i;
    uint32_t pg_sz = NVM_PG_SIZE + NVM_OOB_SIZE;
    struct tests_sync_io *args = (struct tests_sync_io *) arg;

    for (pg_i = 0; pg_i < args->total_pgs; pg_i++) {
        if (pg_i % 200 == 0) {
            printf(".");
            fflush(stdout);
        }
        if (args->cmdtype == MMGR_ERASE_BLK)
            pg_i = args->total_pgs;
        else
            memset(args->buf[pg_i],(args->cmdtype == MMGR_WRITE_PG) ? 0xac :
                                                        0, pg_sz * args->n_pl);

        for (pl_i = 0; pl_i < args->n_pl; pl_i++) {
            memset (&args->cmd[pl_i], 0, sizeof (struct nvm_mmgr_io_cmd));
            args->cmd[pl_i].ppa.g.pg =
                                   (args->cmdtype == MMGR_ERASE_BLK) ? 0: pg_i;
            args->cmd[pl_i].ppa.g.blk = rand_blk[1];
            args->cmd[pl_i].ppa.g.pl = pl_i;
            args->cmd[pl_i].ppa.g.lun = rand_lun[1];
            args->cmd[pl_i].ppa.g.ch = args->ch->ch_mmgr_id;


            if(nvm_submit_sync_io (args->ch, &args->cmd[pl_i], args->buf[pg_i] +
                                (pg_sz * pl_i), args->cmdtype) && !thread_err)
                thread_err++;

            pthread_mutex_lock(&usec_mutex);
            t_usec += tests_get_cmd_usec(&args->cmd[pl_i]);
            pthread_mutex_unlock(&usec_mutex);

            pthread_mutex_lock(&pgs_ok_mutex);
            atomic_inc(&pgs_ok);
            pthread_mutex_unlock(&pgs_ok_mutex);

            usleep(2400);
        }

    }
}

int test_s01_io_thread (uint8_t cmdtype, int total_pgs)
{
    int ch_i, pg_i, n_th = 0, match;
    int n_pl = tests_is.geo.n_pl, n_pgs = 0;
    struct nvm_channel *ch;
    struct tests_sync_io sync_args[core.nvm_ch_count];
    struct nvm_mmgr_io_cmd cmd[core.nvm_ch_count][n_pl];
    struct timeval start, end, middle;
    uint32_t pg_sz = NVM_PG_SIZE + NVM_OOB_SIZE;
    void *wptr, *rptr;
    void ***cbuf;

    thread_err = 0;
    t_usec = 0;

    threads = malloc (sizeof (pthread_t) * core.nvm_ch_count);
    if (!threads)
        return EMEM;

    if (cmdtype == MMGR_ERASE_BLK)
        goto ERASE;

    cbuf = malloc(sizeof(void *) * core.nvm_ch_count);
    for (ch_i = 0; ch_i < core.nvm_ch_count; ch_i++) {
        cbuf[ch_i] = malloc(sizeof(void *) * total_pgs);
        for (pg_i = 0; pg_i < total_pgs; pg_i++)
            cbuf[ch_i][pg_i] = calloc (pg_sz * n_pl, 1);
    }

    if (cmdtype == MMGR_READ_PG)
        rbuf = cbuf;
    else
        wbuf = cbuf;

ERASE:
    pgs_ok.counter = ATOMIC_INIT_RUNTIME(0);
    pthread_mutex_init (&pgs_ok_mutex, NULL);
    pthread_mutex_init (&usec_mutex, NULL);

    gettimeofday(&start, NULL);

    printf("     ");
    for (ch_i = 0; ch_i < core.nvm_ch_count; ch_i++) {
        ch = core.nvm_ch[ch_i];

        if (cmdtype != MMGR_ERASE_BLK)
            sync_args[ch_i].buf = cbuf[ch_i];

        sync_args[ch_i].ch = ch;
        sync_args[ch_i].cmd = cmd[ch_i];
        sync_args[ch_i].cmdtype = cmdtype;
        sync_args[ch_i].thread_i = ch_i;
        sync_args[ch_i].total_pgs = total_pgs;
        sync_args[ch_i].n_pl = n_pl;

        pthread_create(&threads[ch_i], NULL,&tests_io_thread, &sync_args[ch_i]);

        n_pgs += total_pgs * n_pl;
        n_th++;
    }

    gettimeofday(&middle, NULL);
    uint64_t usec_s = (middle.tv_sec * (uint64_t) 1000000 + middle.tv_usec) -
                       (start.tv_sec * (uint64_t)1000000 + start.tv_usec);

    for (ch_i = 0; ch_i < core.nvm_ch_count; ch_i++)
        pthread_join (threads[ch_i], NULL);

    gettimeofday(&end, NULL);

    uint64_t usec = (end.tv_sec*(uint64_t)1000000+end.tv_usec) -
                               (start.tv_sec*(uint64_t)1000000+start.tv_usec);
    uint64_t usec_w = (end.tv_sec*(uint64_t)1000000+end.tv_usec) -
                               (middle.tv_sec*(uint64_t)1000000+middle.tv_usec);

    if (cmdtype == MMGR_ERASE_BLK)
        n_pgs = n_pgs / total_pgs;
    test_s01_print_result(n_pgs, cmdtype, usec);
    printf ("      Threads: %d\n", n_th);
    printf ("      Starting threads (S)  : %llu u-sec\n", usec_s);
    printf ("      Waiting completion (W): %llu u-sec\n", usec_w);
    printf ("      IO Commands (sum)     : %llu u-sec\n", t_usec);
    printf ("      Thread Time Average   : %llu u-sec\n", t_usec /
                                                                (n_th & AND64));
    printf ("      Command Average       : %llu u-sec\n",
                                                      t_usec / (n_pgs & AND64));

    if (cmdtype == MMGR_READ_PG) {
        match = 0;
        for (ch_i = 0; ch_i < core.nvm_ch_count; ch_i++) {
            for (pg_i = 0; pg_i < total_pgs; pg_i++) {
                wptr = wbuf[ch_i][pg_i];
                rptr = rbuf[ch_i][pg_i];
                if (!memcmp (wptr, rptr, pg_sz * n_pl))
                    match++;
                free(wptr);
                free(rptr);
            }
            free(wbuf[ch_i]);
            free(rbuf[ch_i]);
        }
        free(wbuf);
        free(rbuf);

        printf ("      Page(%d pl) data   OK : %d\n", n_pl, match);
        printf ("      Page(%d pl) data  FAIL: %d\n", n_pl,
                                        total_pgs * core.nvm_ch_count - match);
    }

    free(threads);
    pthread_mutex_destroy (&pgs_ok_mutex);
    pthread_mutex_destroy (&usec_mutex);
    t_usec = 0;

    if (cmdtype == MMGR_READ_PG && total_pgs * core.nvm_ch_count - match > 0)
        return -1;

    return (thread_err) ? -1 : 0;
}

int test_s01_compare_data_sync_fn (struct tests_test *test)
{
    return test_s01_compare_data_sync(tests_is.geo.n_pg);
}

int test_s01_read_full_blks_sync_fn (struct tests_test *test)
{
    return test_s01_full_blks_sync(MMGR_READ_PG);
}

int test_s01_write_full_blks_sync_fn (struct tests_test *test)
{
    return test_s01_full_blks_sync(MMGR_WRITE_PG);
}

int test_s01_erase_full_blks_sync_fn (struct tests_test *test)
{
    return test_s01_full_blks_sync(MMGR_ERASE_BLK);
}

int test_s01_read_full_blks_thread_fn (struct tests_test *test)
{
    return test_s01_io_thread (MMGR_READ_PG, tests_is.geo.n_pg);
}

int test_s01_write_full_blks_thread_fn (struct tests_test *test)
{
    return test_s01_io_thread (MMGR_WRITE_PG, tests_is.geo.n_pg);
}

int test_s01_erase_full_blks_thread_fn (struct tests_test *test)
{
    return test_s01_io_thread (MMGR_ERASE_BLK, tests_is.geo.n_pg);
}

int test_s01_read_single_page_thread_fn (struct tests_test *test)
{
    return test_s01_io_thread(MMGR_READ_PG, 1);
}

int test_s01_write_single_page_thread_fn (struct tests_test *test)
{
    return test_s01_io_thread(MMGR_WRITE_PG, 1);
}

int test_s01_erase_single_blk_thread_fn (struct tests_test *test)
{
    return test_s01_io_thread(MMGR_ERASE_BLK, 1);
}

struct tests_set testset_01 = {
    .name       = "mmgr",
    .desc       = "Tests related to all Media Managers."
};

struct tests_test test_10_compare_data_sync = {
    .name       = "compare_data_sync",
    .desc       = "Erase 1 random block, write full, read full and compare"
            "\n\t the data between write and read buffers.",
    .run_fn     = test_s01_compare_data_sync_fn,
    .flags      = 0x0
};

struct tests_test test_09_read_full_blks_sync = {
    .name       = "read_full_blocks_sync",
    .desc       = "Read 1 random full block on 1 random LUN on all channels."
            "\n\tA single thread is triggered.",
    .run_fn     = test_s01_read_full_blks_sync_fn,
    .flags      = 0x0
};

struct tests_test test_08_write_full_blks_sync = {
    .name       = "write_full_blocks_sync",
    .desc       = "Write 1 random full block on 1 random LUN on all channels."
            "\n\tA single thread is triggered.",
    .run_fn     = test_s01_write_full_blks_sync_fn,
    .flags      = 0x0
};

struct tests_test test_07_erase_full_blks_sync = {
    .name       = "erase_full_blocks_sync",
    .desc       = "Erase 1 random block on 1 random LUN on all channels."
            "\n\tA single thread is triggered.",
    .run_fn     = test_s01_erase_full_blks_sync_fn,
    .flags      = 0x0
};

struct tests_test test_06_read_full_blks_thread = {
    .name       = "read_full_blocks_thread",
    .desc       = "Read 1 random full block on 1 random LUN on all channels."
            "\n\tMultiple threads(per channel) are triggered in parallel.",
    .run_fn     = test_s01_read_full_blks_thread_fn,
    .flags      = 0x0
};

struct tests_test test_05_write_full_blks_thread = {
    .name       = "write_full_blocks_thread",
    .desc       = "Write 1 random full block on 1 random LUN on all channels."
            "\n\tMultiple threads(per channel) are triggered in parallel.",
    .run_fn     = test_s01_write_full_blks_thread_fn,
    .flags      = 0x0
};

struct tests_test test_04_erase_full_blks_thread = {
    .name       = "erase_full_blocks_thread",
    .desc       = "Erase 1 random block on 1 random LUN on all the channels."
            "\n\tMultiple threads(per channel) are triggered in parallel.",
    .run_fn     = test_s01_erase_full_blks_thread_fn,
    .flags      = 0x0
};

struct tests_test test_03_read_single_page_thread = {
    .name       = "read_single_page_thread",
    .desc       = "Read first page from 1 random block on 1 random LUN on "
                                                             "all the channels."
            "\n\tMultiple threads(per channel) are triggered in parallel.",
    .run_fn     = test_s01_read_single_page_thread_fn,
    .flags      = 0x0
};

struct tests_test test_02_write_single_page_thread = {
    .name       = "write_single_page_thread",
    .desc       = "Write first page from 1 random block on 1 random LUN on "
                                                            "all the channels."
            "\n\tMultiple threads(per channel) are triggered in parallel.",
    .run_fn     = test_s01_write_single_page_thread_fn,
    .flags      = 0x0
};

struct tests_test test_01_erase_single_blk_thread = {
    .name       = "erase_single_block_thread",
    .desc       = "Erase 1 random block on 1 random LUN on all the channels."
            "\n\tMultiple threads(per channel) are triggered in parallel.",
    .run_fn     = test_s01_erase_single_blk_thread_fn,
    .flags      = 0x0
};

int testset_mmgr_dfcnand_init (struct nvm_init_arg *args) {
    int nt = 10, i;
    struct tests_set *s = &testset_01;
    struct tests_test *t[] = {
        &test_09_read_full_blks_sync,
        &test_08_write_full_blks_sync,
        &test_07_erase_full_blks_sync,
        &test_06_read_full_blks_thread,
        &test_05_write_full_blks_thread,
        &test_04_erase_full_blks_thread,
        &test_03_read_single_page_thread,
        &test_02_write_single_page_thread,
        &test_01_erase_single_blk_thread,
        &test_10_compare_data_sync,
    };

    if(tests_register_set (&testset_01))
        return -1;

    for (i = 0; i < nt; i ++) {
        if(tests_register (t[i], s))
            return -1;
    }

    rand_lun[0] = rand() % tests_is.geo.n_lun;
    rand_blk[0] = (rand() % tests_is.geo.n_blk) + 4;
    rand_lun[1] = rand() % tests_is.geo.n_lun;
    rand_blk[1] = (rand() % tests_is.geo.n_blk) + 4;

    if (args->arg_flag & CMDARG_FLAG_A || args->arg_flag & CMDARG_FLAG_S) {
        printf("\n[MMGR_TESTS: random lun: %d, random blk: %d]\n",
                                                     rand_lun[0], rand_blk[0]);
        printf("[MMGR_TESTS: random lun: %d, random blk: %d]\n\n",
                                                     rand_lun[1], rand_blk[1]);
    }


    return 0;
}