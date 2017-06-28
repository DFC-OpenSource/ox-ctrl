/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX LightNVM Tests
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

#include <time.h>
#include <string.h>
#include "../include/ssd.h"
#include "../include/tests.h"
#include "../include/nvme.h"
#include "../include/lightnvm.h"
#include "../include/uatomic.h"

extern struct core_struct core;
extern struct tests_init_st tests_is;

static void ***wbuf;
static void ***rbuf;
extern atomic_t         pgs_ok;
extern pthread_mutex_t  pgs_ok_mutex;
extern volatile uint64_t t_usec;
extern pthread_mutex_t  usec_mutex;
extern int              rand_lun[4];
extern int              rand_blk[4];

static void tests_start_global ()
{
    pgs_ok.counter = ATOMIC_INIT_RUNTIME(0);
    pthread_mutex_init (&pgs_ok_mutex, NULL);
    pthread_mutex_init (&usec_mutex, NULL);
    t_usec = 0;
}

static void tests_free_global ()
{
    pthread_mutex_destroy (&pgs_ok_mutex);
    pthread_mutex_destroy (&usec_mutex);
}

static int tests_compare_free_buf (int pgs)
{
    int match = 0, ch_i, pg_i, n_pl = tests_is.geo.n_pl;
    void *wptr, *rptr;
    uint32_t pg_sz = NVM_PG_SIZE + NVM_OOB_SIZE;

    for (ch_i = 0; ch_i < core.nvm_ch_count; ch_i++) {
        for (pg_i = 0; pg_i < pgs; pg_i++) {
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

    return match;
}

static void tests_init_buf (int pgs)
{
    int ci, pi;
    int n_pl = tests_is.geo.n_pl;
    uint32_t pg_sz = NVM_PG_SIZE + NVM_OOB_SIZE;

    wbuf = malloc (sizeof(void *) * core.nvm_ch_count);
    rbuf = malloc (sizeof(void *) * core.nvm_ch_count);
    for (ci = 0; ci < core.nvm_ch_count; ci++) {
        wbuf[ci] = malloc (sizeof(void *) * pgs);
        rbuf[ci] = malloc (sizeof(void *) * pgs);
        for (pi = 0; pi < pgs; pi++) {
            wbuf[ci][pi] = calloc (pg_sz * n_pl, 1);
            rbuf[ci][pi] = calloc (pg_sz * n_pl, 1);
            memset (wbuf[ci][pi], 0x8e, pg_sz * n_pl);
        }
    }
}

static struct tests_io_request *tests_new_io_req (struct nvm_ppa_addr *ppa,
                                                  uint16_t cid, uint8_t opcode)
{
    struct tests_io_request *test_req;
    struct NvmeRequest  *req;
    struct NvmeCmd      *cmd;
    struct LnvmRwCmd    *lrw;
    struct nvm_ppa_addr *ppa_list;
    void *first_pg, *meta;
    void **prp_list;
    void ***cbuf;
    int n_sec = tests_is.geo.n_sec * tests_is.geo.n_pl;
    int sec_pg = tests_is.geo.n_sec;
    int sec_i;

    if (opcode == LNVM_CMD_ERASE_SYNC)
        n_sec /= sec_pg;

    req = calloc (sizeof(struct NvmeRequest), 1);
    if (!req) goto OUT;

    req->sq = calloc (sizeof(struct NvmeSQ), 1);
    if (!req->sq) goto FREE1;

    cmd = calloc (sizeof(struct NvmeCmd), 1);
    if (!cmd) goto FREE2;

    ppa_list = calloc (sizeof (struct nvm_ppa_addr) * n_sec, 1);
    if (!ppa_list) goto FREE3;

    prp_list = malloc (sizeof(void *) * n_sec);
    if (!prp_list) goto FREE4;

    test_req = malloc (sizeof(struct tests_io_request));
    if (!test_req) goto FREE5;

    if (opcode == LNVM_CMD_ERASE_SYNC) goto ERASE;

    cbuf = (opcode == LNVM_CMD_PHYS_WRITE) ? wbuf : rbuf;

    meta = cbuf[ppa->g.ch][ppa->g.pg] + NVM_PG_SIZE * tests_is.geo.n_pl;
    first_pg = cbuf[ppa->g.ch][ppa->g.pg];
    for (sec_i = 1; sec_i < n_sec; sec_i++)
            prp_list[sec_i - 1] = cbuf[ppa->g.ch][ppa->g.pg] +
                                                   tests_is.geo.sec_sz * sec_i;

ERASE:
    for (sec_i = 0; sec_i < n_sec; sec_i++) {
        ppa_list[sec_i].g.ch = ppa->g.ch;
        ppa_list[sec_i].g.lun = ppa->g.lun;
        ppa_list[sec_i].g.blk = ppa->g.blk;
        ppa_list[sec_i].g.pg = (opcode == LNVM_CMD_ERASE_SYNC) ?
                                                        0 : ppa->g.pg;
        ppa_list[sec_i].g.pl = (opcode == LNVM_CMD_ERASE_SYNC) ?
                                                        sec_i : sec_i / sec_pg;
        ppa_list[sec_i].g.sec = (opcode == LNVM_CMD_ERASE_SYNC) ?
                                                        0 : sec_i % sec_pg;
    }

    lrw = (struct LnvmRwCmd *) cmd;
    req->sq->sqid = 1;
    lrw->cid = cid;
    lrw->opcode = opcode;
    lrw->nsid = 1;
    lrw->nlb = n_sec - 1;
    lrw->prp1 = (opcode == LNVM_CMD_ERASE_SYNC) ? 0 : (uint64_t) first_pg;
    lrw->prp2 = (opcode == LNVM_CMD_ERASE_SYNC) ? 0 : (uint64_t) prp_list;
    lrw->metadata = (opcode == LNVM_CMD_ERASE_SYNC) ? 0 : (uint64_t) meta;
    lrw->slba = 0;
    lrw->spba = (uint64_t) ppa_list;

    test_req->cmd = cmd;
    test_req->req = req;
    test_req->n_sec = n_sec;

    return test_req;

FREE5:
    free (prp_list);
FREE4:
    free (ppa_list);
FREE3:
    free (cmd);
FREE2:
    free (req->sq);
FREE1:
    free (req);
OUT:
    return NULL;
}

static void tests_lnvm_free_test_req (struct tests_io_request *test_req)
{
    struct NvmeRequest  *req = test_req->req;
    struct LnvmRwCmd    *cmd = (struct LnvmRwCmd *) test_req->cmd;
    void *prp_list = (void *) cmd->prp2;
    void *ppa_list = (void *) cmd->spba;

    free (prp_list);
    free (ppa_list);
    free (cmd);
    free (req->sq);
    free (req);
    free (test_req);
}

static int tests_lnvm_erase_fn (int rand_i)
{
    int ret, i, n_blk = 0, err = 0;
    struct tests_io_request **req;
    struct nvm_ppa_addr ppa;
    struct timeval start, end;

    ppa.g.blk = rand_blk[rand_i];

    req = malloc (sizeof(void *) * core.nvm_ch_count);
    if (!req)
        return -1;

    tests_start_global();

    gettimeofday(&start, NULL);
    for (i = 0; i < core.nvm_ch_count; i++) {
        ppa.g.ch = i;
        ppa.g.lun = rand_lun[rand_i];
        req[i] = tests_new_io_req (&ppa, i, LNVM_CMD_ERASE_SYNC);
        if (!req[i]) {
            err += tests_is.geo.n_pl;
            continue;
        }

        ret = nvme_io_cmd (core.nvm_nvme_ctrl, req[i]->cmd, req[i]->req);
        if (ret != NVME_NO_COMPLETE && ret != NVME_SUCCESS) {
            err += tests_is.geo.n_pl;
            continue;
        }

        n_blk += tests_is.geo.n_pl;
    }

    do {
        usleep(1);
        pthread_mutex_lock(&pgs_ok_mutex);
        ret = atomic_read(&pgs_ok);
        pthread_mutex_unlock(&pgs_ok_mutex);
    } while (ret < n_blk);

    gettimeofday(&end, NULL);
    uint64_t usec_e = (end.tv_sec*(uint64_t)1000000+end.tv_usec) -
                               (start.tv_sec*(uint64_t)1000000+start.tv_usec);

    for (i = 0; i < core.nvm_ch_count; i++) {
        if (req[i]->req->status != NVME_SUCCESS)
            err += tests_is.geo.n_pl;
        tests_lnvm_free_test_req (req[i]);
    }

    printf("       Total blocks to be erased: %d\n", n_blk);
    printf("       SUCESS: %d\n", n_blk - err);
    printf("       FAIL  : %d\n", err);
    printf("       Time Elapsed          : %llu u-sec\n", usec_e);
    printf("       Total Erase Time (sum): %llu u-sec\n", t_usec);
    printf("       Block Erase Avg       : %llu u-sec\n",
                                                       t_usec/(n_blk & AND64));

    free (req);
    tests_free_global ();
    return err;
}

static int tests_lnvm_rw_fn (uint16_t opcode, int n_pgs, int rand_i)
{
    int ret, ch_i, pg_i, n_pg = 0, err = 0, match;
    struct tests_io_request ***req;
    struct nvm_ppa_addr ppa;
    struct timeval start, end;
    uint32_t pg_sz = NVM_PG_SIZE + NVM_OOB_SIZE;

    ppa.g.blk = rand_blk[rand_i];

    if (opcode == LNVM_CMD_PHYS_WRITE)
        tests_init_buf (n_pgs);

    req = malloc (sizeof(void *) * core.nvm_ch_count);
    for (ch_i = 0; ch_i < core.nvm_ch_count; ch_i++)
        req[ch_i] = malloc (sizeof(void *) * n_pgs);

    tests_start_global();

    gettimeofday(&start, NULL);
    printf("      ");
    for (pg_i = 0; pg_i < n_pgs; pg_i++) {
        for (ch_i = 0; ch_i < core.nvm_ch_count; ch_i++) {

            ppa.g.ch = ch_i;
            ppa.g.lun = rand_lun[rand_i];
            ppa.g.pg = pg_i;
            req[ch_i][pg_i] = tests_new_io_req (&ppa, ch_i, opcode);
            if (!req[ch_i][pg_i]) {
                err += tests_is.geo.n_pl;
                continue;
            }

            ret = nvme_io_cmd (core.nvm_nvme_ctrl, req[ch_i][pg_i]->cmd,
                                                        req[ch_i][pg_i]->req);

            if (ret != NVME_NO_COMPLETE && ret != NVME_SUCCESS) {
                err += tests_is.geo.n_pl;
                continue;
            }

            n_pg += tests_is.geo.n_pl;

            if (n_pg % 8 == 0)
                usleep((opcode == LNVM_CMD_PHYS_WRITE) ? 2400: 6000);

            if (n_pg % 1000 == 0) {
                printf(".");
                fflush(stdout);
            }
        }
    }

    do {
        usleep(1);
        pthread_mutex_lock(&pgs_ok_mutex);
        ret = atomic_read(&pgs_ok);
        pthread_mutex_unlock(&pgs_ok_mutex);
    } while (ret < n_pg);

    gettimeofday(&end, NULL);
    uint64_t usec_e = (end.tv_sec*(uint64_t)1000000+end.tv_usec) -
                               (start.tv_sec*(uint64_t)1000000+start.tv_usec);

    for (ch_i = 0; ch_i < core.nvm_ch_count; ch_i++) {
        for (pg_i = 0; pg_i < n_pgs; pg_i++) {
            if (req[ch_i][pg_i]->req->status != NVME_SUCCESS)
                err += tests_is.geo.n_pl;
            tests_lnvm_free_test_req (req[ch_i][pg_i]);
        }
        free (req[ch_i]);
    }

    printf("\n       Total Pages: %d\n", n_pg);
    printf("       Data Transfered: %llu KB\n", (n_pg * pg_sz) / 1024);
    printf("       SUCESS: %d\n", n_pg - err);
    printf("       FAIL  : %d\n", err);
    printf("       Time Elapsed       : %llu u-sec\n", usec_e);
    printf("       Total IO Time (sum): %llu u-sec\n", t_usec);
    printf("       Page IO Avg        : %llu u-sec\n", t_usec/(n_pg & AND64));

    if (opcode == LNVM_CMD_PHYS_READ) {
        match = tests_compare_free_buf (n_pgs);
        err += n_pg/tests_is.geo.n_pl - match;
        printf("       Page (2 pl) data OK  : %d\n", match);
        printf("       Page (2 pl) data FAIL: %d\n", n_pg/tests_is.geo.n_pl -
                                                                         match);
    }

    free (req);
    tests_free_global ();
    return err;
}

static int test_s02_lnvm_erase_single_fn (struct tests_test *test)
{
    return tests_lnvm_erase_fn (2);
}

static int test_s02_lnvm_write_single_pg_fn (struct tests_test *test)
{
    return tests_lnvm_rw_fn (LNVM_CMD_PHYS_WRITE, 1, 2);
}

static int test_s02_lnvm_read_single_pg_fn (struct tests_test *test)
{
    return tests_lnvm_rw_fn (LNVM_CMD_PHYS_READ, 1, 2);
}

static int test_s02_lnvm_erase_full_fn (struct tests_test *test)
{
    return tests_lnvm_erase_fn (3);
}

static int test_s02_lnvm_write_full_blk_fn (struct tests_test *test)
{
    return tests_lnvm_rw_fn (LNVM_CMD_PHYS_WRITE, tests_is.geo.n_pg, 3);
}

static int test_s02_lnvm_read_full_blk_fn (struct tests_test *test)
{
    return tests_lnvm_rw_fn (LNVM_CMD_PHYS_READ, tests_is.geo.n_pg, 3);
}

static int test_s02_lnvm_identify_fn (struct tests_test *test)
{
    int ret;
    struct NvmeIdentify *cmd = calloc (sizeof(struct NvmeIdentify), 1);
    struct NvmeRequest *req = calloc (sizeof(struct NvmeRequest), 1);
    struct LnvmIdCtrl *ctrl;
    struct LnvmIdGroup *gr;

    cmd->opcode = LNVM_ADM_CMD_IDENTITY;
    cmd->cid = 0;
    cmd->nsid = 1;
    cmd->prp1 = (uint64_t) malloc (sizeof (struct LnvmIdCtrl));
    if (!cmd->prp1)
        return -1;

    ret = nvme_admin_cmd (core.nvm_nvme_ctrl, (struct NvmeCmd *) cmd, req);

    if (ret) {
        printf ("     IDENTIFY COMMAND FAILED.\n");
        return -1;
    }

    ctrl = (LnvmIdCtrl *) cmd->prp1;
    gr = &ctrl->groups[0];

    printf ("     LightNVM device identified. Check ctrl parameters: \n");
    printf("ver_id: %d\n", ctrl->ver_id);
    printf("vmnt:   %d\n", ctrl->vmnt);
    printf("cgrps:  %d\n", ctrl->cgrps);
    printf("cap:    0x%x\n", ctrl->cap);
    printf("dom:    0x%x\n", ctrl->dom);
    printf("ppaf:\n");
    printf("  ch_off:  %d\n", ctrl->ppaf.ch_offset);
    printf("  ch len:  %d\n", ctrl->ppaf.ch_len);
    printf("  lun_off: %d\n", ctrl->ppaf.lun_offset);
    printf("  lun len: %d\n", ctrl->ppaf.lun_len);
    printf("  pln_off: %d\n", ctrl->ppaf.pln_offset);
    printf("  pln len: %d\n", ctrl->ppaf.pln_len);
    printf("  blk_off: %d\n", ctrl->ppaf.blk_offset);
    printf("  blk len: %d\n", ctrl->ppaf.blk_len);
    printf("  pg_off:  %d\n", ctrl->ppaf.pg_offset);
    printf("  pg len:  %d\n", ctrl->ppaf.pg_len);
    printf("  sec_off: %d\n", ctrl->ppaf.sect_offset);
    printf("  sec len: %d\n", ctrl->ppaf.sect_len);
    printf("group[0]:\n");
    printf("  mtype:   %d\n", gr->mtype);
    printf("  fmtype:  %d\n", gr->fmtype);
    printf("  num_ch:  %d\n", gr->num_ch);
    printf("  num_lun: %d\n", gr->num_lun);
    printf("  num_pln: %d\n", gr->num_pln);
    printf("  num_blk: %d\n", gr->num_blk);
    printf("  num_pg:  %d\n", gr->num_pg);
    printf("  fpg_sz:  %d\n", gr->fpg_sz);
    printf("  csecs:   %d\n", gr->csecs);
    printf("  sos:     %d\n", gr->sos);
    printf("  trdt:    %d\n", gr->trdt);
    printf("  trdm:    %d\n", gr->trdm);
    printf("  tprt:    %d\n", gr->tprt);
    printf("  tprm:    %d\n", gr->tprm);
    printf("  tbet:    %d\n", gr->tbet);
    printf("  tbem:    %d\n", gr->tbem);
    printf("  mpos:    0x%x\n", gr->mpos);
    printf("  mccap:   0x%x\n", gr->mccap);
    printf("  cpar:    0x%x\n", gr->cpar);

    free (ctrl);
    free (cmd);
    free (req);

    return 0;
}

struct tests_set testset_02 = {
    .name       = "lightnvm",
    .desc       = "Tests related to the LightNVM cmd parser, FTL LNVM and DMA."
};

struct tests_test test_01_lnvm_identify = {
    .name       = "lnvm_identify",
    .desc       = "Send an identify command to check the controller ID and "
            "\n\t LightNVM groups ID.",
    .run_fn     = test_s02_lnvm_identify_fn,
    .flags      = 0x0
};

struct tests_test test_02_lnvm_erase_single = {
    .name       = "lnvm_erase_single",
    .desc       = "Erases 1 random block from all channels.",
    .run_fn     = test_s02_lnvm_erase_single_fn,
    .flags      = 0x0
};

struct tests_test test_03_lnvm_write_single_pg = {
    .name       = "lnvm_write_single",
    .desc       = "Writes 1 page from 1 random block from all channels.",
    .run_fn     = test_s02_lnvm_write_single_pg_fn,
    .flags      = 0x0
};

struct tests_test test_04_lnvm_read_single_pg = {
    .name       = "lnvm_read_single",
    .desc       = "Reads 1 page from 1 random block from all channels and "
            "\n\t compare the data.",
    .run_fn     = test_s02_lnvm_read_single_pg_fn,
    .flags      = 0x0
};

struct tests_test test_05_lnvm_erase_full = {
    .name       = "lnvm_erase_full",
    .desc       = "Erases 1 random block from all channels.",
    .run_fn     = test_s02_lnvm_erase_full_fn,
    .flags      = 0x0
};

struct tests_test test_06_lnvm_write_full_blk = {
    .name       = "lnvm_write_full_blk",
    .desc       = "Writes 1 full block from all channels.",
    .run_fn     = test_s02_lnvm_write_full_blk_fn,
    .flags      = 0x0
};

struct tests_test test_07_lnvm_read_full_blk = {
    .name       = "lnvm_read_full_blk",
    .desc       = "Reads 1 full block from all channels and compare the data.",
    .run_fn     = test_s02_lnvm_read_full_blk_fn,
    .flags      = 0x0
};

int testset_lnvm_init (struct nvm_init_arg *args) {
    int nt = 7, i;
    struct tests_set *s = &testset_02;

    struct tests_test *t[] = {
        &test_07_lnvm_read_full_blk,
        &test_06_lnvm_write_full_blk,
        &test_05_lnvm_erase_full,
        &test_04_lnvm_read_single_pg,
        &test_03_lnvm_write_single_pg,
        &test_02_lnvm_erase_single,
        &test_01_lnvm_identify
    };

    if(tests_register_set (&testset_02))
        return -1;

    for (i = 0; i < nt; i ++) {
        if(tests_register (t[i], s))
            return -1;
    }

    rand_lun[2] = rand() % tests_is.geo.n_lun;
    rand_blk[2] = (rand() % (tests_is.geo.n_blk - 4)) + 4;
    rand_lun[3] = rand() % tests_is.geo.n_lun;
    rand_blk[3] = (rand() % (tests_is.geo.n_blk - 4)) + 4;

    if (args->arg_flag & CMDARG_FLAG_A || args->arg_flag & CMDARG_FLAG_S) {
        printf("[LIGHTNVM_TESTS: random lun: %d, random blk: %d]\n",
                                                     rand_lun[0], rand_blk[2]);
        printf("[LIGHTNVM_TESTS: random lun: %d, random blk: %d]\n\n",
                                                     rand_lun[1], rand_blk[3]);
    }

    return 0;
}