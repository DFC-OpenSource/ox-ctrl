/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - LightNVM NVMe Extension + Command Parser
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

#include <syslog.h>
#include <string.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <errno.h>
#include "include/ssd.h"

#if LIGHTNVM
#include "include/lightnvm.h"
#include "include/uatomic.h"

extern struct core_struct core;

uint8_t lnvm_dev(NvmeCtrl *n)
{
    return (n->lightnvm_ctrl.id_ctrl.ver_id != 0);
}

static void lnvm_debug_print_io (struct nvm_ppa_addr *list, uint64_t *prp,
               uint64_t *md_prp, uint16_t size, uint64_t dt_sz, uint64_t md_sz)
{
    int i;
    printf(" Number of sectors: %d\n", size);
    printf(" DMA size: %lu (data) + %lu (meta) = %lu bytes\n",
                                                 dt_sz, md_sz, dt_sz + md_sz);
    for (i = 0; i < size; i++) {
        printf (" [ppa(%d): ch: %d, lun: %d, blk: %d, pl: %d, pg: %d, "
                "sec: %d]\n", i, list[i].g.ch, list[i].g.lun, list[i].g.blk,
                list[i].g.pl, list[i].g.pg, list[i].g.sec);
    }
    for (i = 0; i < size; i++)
        printf (" [prp(%d): 0x%016lx\n", i, prp[i]);
    printf (" [meta_prp(0): 0x%016lx\n", md_prp[0]);
}

void lnvm_set_default(LnvmCtrl *ctrl)
{
    ctrl->id_ctrl.ver_id = LNVM_VER_ID;
    ctrl->id_ctrl.dom = LNVM_DOM;
    ctrl->id_ctrl.cap = LNVM_CAP;
    ctrl->params.sec_size = LNVM_SECSZ;
    ctrl->params.secs_per_pg = LNVM_SEC_PG;
    ctrl->params.pgs_per_blk = LNVM_PG_BLK;
    ctrl->params.max_sec_per_rq = LNVM_MAX_SEC_RQ;
    ctrl->params.mtype = LNVM_MTYPE;
    ctrl->params.fmtype = LNVM_FMTYPE;
    ctrl->params.num_ch = LNVM_CH;
    ctrl->params.num_lun = LNVM_LUN_CH;
    ctrl->params.num_pln = LNVM_PLANES;
    ctrl->params.num_blk = LNVM_BLK_LUN;
    ctrl->bb_gen_freq = LNVM_BB_GEN_FREQ;
    ctrl->err_write = LNVM_ERR_WRITE;
}

static void lnvm_tbl_initialize(uint64_t *tbl, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++)
        tbl[i] = LNVM_LBA_UNMAPPED;
}

uint16_t lnvm_get_l2p_tbl(NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    NvmeNamespace *ns;
    LnvmGetL2PTbl *gtbl = (LnvmGetL2PTbl*)cmd;
    uint64_t slba = gtbl->slba;
    uint32_t nlb = gtbl->nlb;
    uint64_t prp1 = gtbl->prp1;
    uint32_t nsid = gtbl->nsid;
    uint64_t tbl_sz;
    uint64_t *tbl;
    int ret;

    if (nsid == 0 || nsid > n->num_namespaces) {
        return NVME_INVALID_NSID | NVME_DNR;
    }
    ns = &n->namespaces[nsid - 1];

    tbl_sz = nlb * sizeof(uint64_t);
    tbl = calloc (1, tbl_sz);
    if (!tbl) {
        log_info("[ERROR lnvm: cannot allocate bitmap for l2p table]\n");
        goto out;
    }
    lnvm_tbl_initialize(tbl, nlb);

    // TODO: call core (slba is the first page in a flat addr space)

    if (prp1) {
        ret = nvme_write_to_host((uint8_t *)tbl, prp1, tbl_sz);
        free(tbl);
        return ret;
    }

clean:
    free(tbl);
out:
    return NVME_INVALID_FIELD;
}

uint16_t lnvm_set_bb_tbl(NvmeCtrl *n, NvmeCmd *nvmecmd, NvmeRequest *req)
{
    LnvmSetBBTbl *cmd = (LnvmSetBBTbl*)nvmecmd;
    struct nvm_ppa_addr *psl;
    uint16_t bbtbl_format = FTL_BBTBL_BYTE;
    void *arg[4];
    int i, ret;

    uint64_t spba = cmd->spba;
    uint16_t nlb = cmd->nlb + 1;
    uint8_t value = cmd->value;

    psl = malloc (sizeof(struct nvm_ppa_addr) * nlb);
    if (!psl)
        return NVME_INTERNAL_DEV_ERROR;

    if (nlb > 1) {
        nvme_read_from_host((void *)psl, spba, nlb * sizeof(uint64_t));
    } else {
        psl[0].ppa = spba;
    }

    for(i = 0; i < nlb; i++) {
        /* set single block to FTL */
        psl[i].g.sec = 0;
        arg[0] = &psl[i].ppa;
        arg[1] = &value;
        arg[2] = &bbtbl_format;

        ret = nvm_ftl_cap_exec(FTL_CAP_SET_BBTBL, arg, 3);
        if (ret)
            return NVME_INVALID_FIELD;
    }

    return NVME_SUCCESS;
}

uint16_t lnvm_get_bb_tbl(NvmeCtrl *n, NvmeCmd *cmd, NvmeRequest *req)
{
    NvmeNamespace *ns;
    LnvmCtrl *ln;
    LnvmIdGroup *c;
    LnvmGetBBTbl *bbtbl = (LnvmGetBBTbl*)cmd;

    uint32_t nsid = bbtbl->nsid;
    uint64_t prp1 = bbtbl->prp1;
    struct nvm_ppa_addr ppa;
    uint32_t nr_blocks;
    LnvmBBTbl *bb_tbl;
    int ret = NVME_SUCCESS;
    void *arg[4];
    uint16_t bbtbl_format = FTL_BBTBL_BYTE;

    if (nsid == 0 || nsid > n->num_namespaces) {
        return NVME_INVALID_NSID | NVME_DNR;
    }

    ppa.ppa= bbtbl->spba;
    ns = &n->namespaces[nsid - 1];
    ln = &n->lightnvm_ctrl;
    c = &ln->id_ctrl.groups[0];

    /* blocks per LUN */
    nr_blocks = c->num_blk * c->num_pln;

    bb_tbl = calloc(sizeof(LnvmBBTbl) + nr_blocks, 1);
    if (!bb_tbl) {
        log_info("[ERROR lnvm: cannot allocate bitmap for bad block "
                                                            "table]\n");
        goto out;
    }

    bb_tbl->tblid[0] = 'B';
    bb_tbl->tblid[1] = 'B';
    bb_tbl->tblid[2] = 'L';
    bb_tbl->tblid[3] = 'T';
    bb_tbl->verid = 1;
    bb_tbl->tblks = nr_blocks;

    /* read bb_tbl from FTL */
    arg[0] = &ppa;
    arg[1] = bb_tbl->blk;
    arg[2] = &nr_blocks;
    arg[3] = &bbtbl_format;

    ret = nvm_ftl_cap_exec(FTL_CAP_GET_BBTBL, arg, 4);
    if (ret)
        goto clean;

    if (prp1) {
        ret = nvme_write_to_host(bb_tbl, prp1, sizeof(LnvmBBTbl) + nr_blocks);
        free(bb_tbl);
        return ret;
    }
clean:
    free(bb_tbl);
out:
    return NVME_INVALID_FIELD;
}

uint16_t lnvm_erase_sync(NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
    NvmeRequest *req)
{
    int i;
    LnvmRwCmd *dm = (LnvmRwCmd *)cmd;
    uint64_t spba = dm->spba;
    uint32_t nlb = dm->nlb + 1;
    struct nvm_ppa_addr *psl = req->nvm_io.ppalist;

    if (nlb > LNVM_PLANES) {
        log_info( "[ERROR lnvm: Wrong erase n of blocks (%d). "
                "Max: %d supported]\n", nlb, LNVM_PLANES);
        return NVME_INVALID_FIELD | NVME_DNR;
    } else if (nlb > 1) {
        if (spba == LNVM_PBA_UNMAPPED || !spba)
            return NVME_INVALID_FIELD | NVME_DNR;

        nvme_read_from_host((void *)psl, spba, nlb * sizeof(uint64_t));
    } else {
        psl[0].ppa = spba;
    }

    req->meta_size = 0;
    req->status = NVME_SUCCESS;
    req->nlb = nlb;
    req->ns = ns;

    req->nvm_io.cid = dm->cid;
    req->nvm_io.cmdtype = MMGR_ERASE_BLK;
    req->nvm_io.n_sec = nlb;
    req->nvm_io.req = (void *) req;
    req->nvm_io.sec_offset = 0;
    req->nvm_io.status.pg_errors = 0;
    req->nvm_io.status.ret_t = 0;
    req->nvm_io.status.pgs_p = 0;
    req->nvm_io.status.pgs_s = 0;

    req->nvm_io.status.total_pgs = nlb;
    req->nvm_io.status.status = NVM_IO_NEW;

    for (i = 0; i < 8; i++)
        req->nvm_io.status.pg_map[i] = 0;

    for (i = 0; i < nlb; i++) {
        req->nvm_io.mmgr_io[i].pg_index = i;
        req->nvm_io.mmgr_io[i].status = NVM_IO_SUCCESS;
        req->nvm_io.mmgr_io[i].nvm_io = &req->nvm_io;
        req->nvm_io.mmgr_io[i].pg_sz = 0;
    }

    if (core.debug)
        lnvm_debug_print_io (req->nvm_io.ppalist, req->nvm_io.prp,
                                                req->nvm_io.md_prp, nlb, 0, 0);

    /* NULL IO */
    if (core.null)
        return 0;

    return nvm_submit_ftl(&req->nvm_io);
}

static inline uint64_t nvme_gen_to_dev_addr(LnvmCtrl *ln,struct nvm_ppa_addr *r)
{
    uint64_t pln_off = r->g.pl * ln->params.sec_per_log_pl;
    uint64_t lun_of = r->g.lun * ln->params.sec_per_lun;
    uint64_t blk_off = r->g.blk * ln->params.sec_per_blk;
    uint64_t pg_off = r->g.pg * ln->params.secs_per_pg;
    uint64_t ret;

    ret = r->g.sec + pg_off + blk_off + lun_of + pln_off;

    return ret;
}

uint16_t lnvm_rw(NvmeCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    LnvmCtrl *ln = &n->lightnvm_ctrl;
    LnvmRwCmd *lrw = (LnvmRwCmd *)cmd;
    int i;

    uint64_t sppa, eppa, sec_offset;
    uint32_t nlb  = lrw->nlb + 1;
    uint64_t prp1 = lrw->prp1;
    uint64_t prp2 = lrw->prp2;
    uint64_t spba = lrw->spba;
    uint64_t meta = lrw->metadata;
    struct nvm_ppa_addr *psl = req->nvm_io.ppalist;

    const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint8_t data_shift = ns->id_ns.lbaf[lba_index].ds;
    const uint16_t oob = ns->id_ns.lbaf[lba_index].ms;
    uint64_t data_size = nlb << data_shift;
    uint32_t n_sectors = data_size / LNVM_SECSZ;
    uint32_t n_pages = data_size / LNVM_PG_SIZE;

    uint64_t meta_size = (LNVM_SEC_OOBSZ * n_sectors > oob) ?
             (n_pages < 1) ? oob : nlb * oob :
             (n_pages < 1) ? LNVM_SEC_OOBSZ * n_sectors : nlb * LNVM_SEC_OOBSZ;

    sec_offset = (data_size % LNVM_PG_SIZE) / (1 << data_shift);

    uint16_t is_write = (lrw->opcode == LNVM_CMD_PHYS_WRITE ||
                                          lrw->opcode == LNVM_CMD_HYBRID_WRITE);

    if (n_sectors > ln->params.max_sec_per_rq || n_sectors > 64) {

        log_info( "[ERROR lnvm: npages too large (%u). "
                "Max:%u supported]\n", n_sectors, ln->params.max_sec_per_rq);
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_LBA_RANGE,
                offsetof(LnvmRwCmd, spba), lrw->slba + nlb, ns->id);
        return NVME_INVALID_FIELD | NVME_DNR;

    } else if (n_sectors > 1) {

        if (spba == LNVM_PBA_UNMAPPED || !spba) {
            nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_LBA_RANGE,
                    offsetof(LnvmRwCmd, spba), lrw->slba + nlb, ns->id);
            return NVME_INVALID_FIELD | NVME_DNR;
        }

        nvme_read_from_host((void *)psl, spba, n_sectors * sizeof(uint64_t));
    } else {
        psl[0].ppa = spba;
    }

    req->lightnvm_slba = lrw->slba;
    req->is_write = is_write;

    sppa = eppa = nvme_gen_to_dev_addr(ln, &psl[0]);
    if (n_sectors > 1)
        eppa = nvme_gen_to_dev_addr(ln, &psl[n_sectors - 1]);

    req->nvm_io.prp[0] = prp1;

    if (n_sectors == 2) {
        req->nvm_io.prp[1] = prp2;
    }

    if (n_sectors > 2)
        nvme_read_from_host((void *)(&req->nvm_io.prp[1]), prp2, (n_sectors-1)
                                                        * sizeof(uint64_t));

    meta_size = (meta) ? meta_size : 0;
    req->slba = sppa;
    req->meta_size = meta_size;
    req->status = NVME_SUCCESS;
    req->nlb = nlb;
    req->ns = ns;

    req->nvm_io.cid = lrw->cid;
    req->nvm_io.sec_sz = (1 << data_shift);
    req->nvm_io.md_sz = meta_size;
    req->nvm_io.cmdtype = (req->is_write) ? MMGR_WRITE_PG : MMGR_READ_PG;
    req->nvm_io.n_sec = nlb;
    req->nvm_io.req = (void *) req;
    req->nvm_io.sec_offset = sec_offset; // 4k page inside a plane page
    req->nvm_io.status.pg_errors = 0;
    req->nvm_io.status.ret_t = 0;
    req->nvm_io.status.pgs_p = 0;
    req->nvm_io.status.pgs_s = 0;
    req->nvm_io.status.total_pgs = (sec_offset) ? (nlb / LNVM_SEC_PG) + 1 :
                                                                      n_pages;
    req->nvm_io.status.status = NVM_IO_NEW;

    for (i = 0; i < 8; i++)
        req->nvm_io.status.pg_map[i] = 0;

    for (i = 0; i < req->nvm_io.status.total_pgs; i++) {
        req->nvm_io.mmgr_io[i].pg_index = i;
        req->nvm_io.mmgr_io[i].status = NVM_IO_SUCCESS;
        req->nvm_io.mmgr_io[i].nvm_io = &req->nvm_io;
        req->nvm_io.mmgr_io[i].pg_sz = LNVM_PG_SIZE;
        req->nvm_io.mmgr_io[i].sync_count = NULL;
        req->nvm_io.mmgr_io[i].sync_mutex = NULL;
        req->nvm_io.md_prp[i] = (meta && meta_size) ?
                                meta + (LNVM_SEC_OOBSZ * LNVM_SEC_PG * i) : 0;
    }

    if (core.debug)
        lnvm_debug_print_io (req->nvm_io.ppalist, req->nvm_io.prp,
                                req->nvm_io.md_prp, nlb, data_size, meta_size);

    /* NULL IO */
    if (core.null)
        return 0;

    return nvm_submit_ftl(&req->nvm_io);
}

static int lightnvm_flush_tbls(NvmeCtrl *n)
{
    /* TODO */
    return 0;
}

void lnvm_init_id_ctrl(LnvmIdCtrl *ln_id)
{
    ln_id->ver_id = LNVM_VER_ID;
    ln_id->vmnt = LNVM_VMNT;
    ln_id->cgrps = LNVM_CGRPS;
    ln_id->cap = LNVM_CAP;
    ln_id->dom = LNVM_DOM;

    ln_id->ppaf.blk_offset = 0;
    ln_id->ppaf.blk_len = 16;
    ln_id->ppaf.pg_offset = 16;
    ln_id->ppaf.pg_len = 16;
    ln_id->ppaf.sect_offset = 32;
    ln_id->ppaf.sect_len = 8;
    ln_id->ppaf.pln_offset = 40;
    ln_id->ppaf.pln_len = 8;
    ln_id->ppaf.lun_offset = 48;
    ln_id->ppaf.lun_len = 8;
    ln_id->ppaf.ch_offset = 56;
    ln_id->ppaf.ch_len = 8;
}

uint16_t lnvm_identity(NvmeCtrl *n, NvmeCmd *cmd)
{
    NvmeIdentify *c = (NvmeIdentify *)cmd;
    uint64_t prp1 = c->prp1;

    LnvmIdCtrl *id = &n->lightnvm_ctrl.id_ctrl;

    if (prp1)
        return nvme_write_to_host(id, prp1, sizeof (LnvmIdCtrl));

    return NVME_SUCCESS;
}

void lightnvm_exit(NvmeCtrl *n)
{
    lightnvm_flush_tbls(n);
}

int lnvm_init(NvmeCtrl *n)
{
    LnvmCtrl *ln;
    LnvmIdGroup *c;
    NvmeNamespace *ns;
    unsigned int i, cid;
    uint64_t tot_blks = 0, rsv_blks = 0;

    ln = &n->lightnvm_ctrl;

    if (ln->params.mtype != 0)
        log_info("    [lnvm: Only NAND Flash Mem supported at the moment]\n");
    if ((ln->params.num_pln > 4))
        log_info("    [lnvm: Only quad plane mode supported]\n");

    for (i = 0; i < n->num_namespaces; i++) {
        ns = &n->namespaces[i];

        for (cid = 0; cid < core.nvm_ch_count; cid++){
            tot_blks += core.nvm_ch[cid]->geometry->blk_per_lun *
                            (core.nvm_ch[cid]->geometry->lun_per_ch & 0xffff);
            rsv_blks += core.nvm_ch[cid]->mmgr_rsv;
            rsv_blks += core.nvm_ch[cid]->ftl_rsv;
        }

        c = &ln->id_ctrl.groups[0];
        c->mtype = ln->params.mtype;
        c->fmtype = ln->params.fmtype;
        c->num_ch = ln->params.num_ch;
        c->num_lun = ln->params.num_lun;
        c->num_pln = ln->params.num_pln;
        c->num_blk = ln->params.num_blk;
        c->num_pg = ln->params.pgs_per_blk;
        c->csecs = ln->params.sec_size;
        c->fpg_sz = ln->params.sec_size * ln->params.secs_per_pg;
        c->sos =  LNVM_SEC_OOBSZ * LNVM_SEC_PG;
        c->trdt = LNVM_TRDT;
        c->trdm = LNVM_TRDM;
        c->tprt = LNVM_TPRT;
        c->tprm = LNVM_TPRM;
        c->tbet = LNVM_TBET;
        c->tbem = LNVM_TBEM;

        switch(c->num_pln) {
            case 1:
                c->mpos = 0x10101; /* single plane */
                break;
            case 2:
                c->mpos = 0x20202; /* dual plane */
                break;
            case 4:
                c->mpos = 0x40404; /* quad plane */
                break;
            default:
                log_info("    [lnvm: Invalid plane mode]\n");
                return -EINVAL;
        }

        c->cpar = 0;
        c->mccap = 1;

        /* calculated values */
        ln->params.sec_per_phys_pl = ln->params.secs_per_pg * c->num_pln;
        ln->params.sec_per_blk= ln->params.secs_per_pg * ln->params.pgs_per_blk;
        ln->params.sec_per_lun = ln->params.sec_per_blk * c->num_blk;
        ln->params.sec_per_log_pl = ln->params.sec_per_lun * c->num_lun;
        ln->params.total_secs = ln->params.sec_per_log_pl;
    }

    log_info("    [lnvm: Channels: %d]\n",c->num_ch);
    log_info("    [lnvm: LUNs per Channel: %d]\n",c->num_lun);
    log_info("    [lnvm: Blocks per LUN: %d]\n",c->num_blk);
    log_info("    [lnvm: Pages per Block: %d]\n",c->num_pg);
    log_info("    [lnvm: Planes: %d]\n",c->num_pln);
    log_info("    [lnvm: Total Blocks: %lu]\n", tot_blks);
    log_info("    [lnvm: Total Pages: %lu]\n",c->num_pg * tot_blks
                                                                * c->num_pln);
    log_info("    [lnvm: Page size: %d bytes]\n",c->fpg_sz);
    log_info("    [lnvm: Plane Page size: %d bytes]\n",c->fpg_sz
                                                                * c->num_pln);
    log_info("    [lnvm: Total: %lu MB]\n",(((c->fpg_sz & 0xffffffff)
                          / 1024) * c->num_pg * c->num_pln * tot_blks) / 1024);
    log_info("    [lnvm: Total Available: %lu MB]\n",
                  (((c->fpg_sz & 0xffffffff) / 1024) * c->num_pg * c->num_pln *
                  (tot_blks - rsv_blks)) / 1024);

    return 0;
}
#endif /* LIGHTNVM */