/* OX: Open-Channel NVM Express SSD Controller
 * 
 *  - LightNVM NVMe Extension
 *
 * Copyright 2016 IT University of Copenhagen
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
#include <math.h>
#include <ox-lightnvm.h>
#include <libox.h>
#include <nvme.h>

extern struct core_struct core;

uint8_t lnvm_dev(NvmeCtrl *n)
{
    return (n->lightnvm_ctrl.id_ctrl.ver_id != 0);
}

void lnvm_set_default(LnvmCtrl *ctrl)
{
    struct nvm_mmgr_geometry *g = core.nvm_ch[0]->geometry;
    ctrl->id_ctrl.ver_id = LNVM_VER_ID;
    ctrl->id_ctrl.dom = LNVM_DOM;
    ctrl->id_ctrl.cap = LNVM_CAP;
    ctrl->params.sec_size = g->sec_size;
    ctrl->params.secs_per_pg = g->sec_per_pg;
    ctrl->params.pgs_per_blk = g->pg_per_blk;
    ctrl->params.max_sec_per_rq = LNVM_MAX_SEC_RQ;
    ctrl->params.mtype = LNVM_MTYPE;
    ctrl->params.fmtype = LNVM_FMTYPE;
    ctrl->params.num_ch = g->n_of_ch;
    ctrl->params.num_lun = g->lun_per_ch;
    ctrl->params.num_blk = g->blk_per_lun;
    ctrl->params.num_pln = g->n_of_planes;
    ctrl->bb_gen_freq = LNVM_BB_GEN_FREQ;
    ctrl->err_write = LNVM_ERR_WRITE;
}

void lnvm_init_id_ctrl (LnvmIdCtrl *ln_id)
{
    struct LnvmIdAddrFormat *ppaf = &ln_id->ppaf;
    struct nvm_mmgr_geometry *g = core.nvm_ch[0]->geometry;

    ln_id->ver_id = LNVM_VER_ID;
    ln_id->vmnt = LNVM_VMNT;
    ln_id->cgrps = LNVM_CGRPS;
    ln_id->cap = LNVM_CAP;
    ln_id->dom = LNVM_DOM;

    ppaf->sect_len    = (uint8_t) log2 (g->sec_per_pg);
    ppaf->pln_len     = (uint8_t) log2 (g->n_of_planes);
    ppaf->ch_len      = (uint8_t) log2 (g->n_of_ch);
    ppaf->lun_len     = (uint8_t) log2 (g->lun_per_ch);
    ppaf->pg_len      = (uint8_t) log2 (g->pg_per_blk);
    ppaf->blk_len     = (uint8_t) log2 (g->blk_per_lun);

    ppaf->sect_offset  = 0;
    ppaf->pln_offset  += ppaf->sect_len;
    ppaf->ch_offset   += ppaf->pln_offset + ppaf->pln_len;
    ppaf->lun_offset  += ppaf->ch_offset + ppaf->ch_len;
    ppaf->pg_offset   += ppaf->lun_offset + ppaf->lun_len;
    ppaf->blk_offset  += ppaf->pg_offset + ppaf->pg_len;
}

void lightnvm_exit(NvmeCtrl *n)
{

}

int lnvm_init(NvmeCtrl *n)
{
    LnvmCtrl *ln;
    LnvmIdGroup *c;
    unsigned int i, cid;
    uint64_t tot_blks = 0, rsv_blks = 0;

    ln = &n->lightnvm_ctrl;

    if (ln->params.mtype != 0)
        log_info("    [lnvm: Only NAND Flash Mem supported at the moment]\n");
    if ((ln->params.num_pln > 4))
        log_info("    [lnvm: Only quad plane mode supported]\n");

    for (i = 0; i < n->num_namespaces; i++) {

        /* For now we export 1 channel containing all LUNs */
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
        c->sos =  core.nvm_ch[0]->geometry->sec_oob_sz;
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
                return -1;
        }

        c->cpar = 0;
        c->mccap = 1;

        /* calculated values */
        ln->params.sec_per_phys_pl = ln->params.secs_per_pg * c->num_pln;
        ln->params.sec_per_blk= ln->params.secs_per_pg * ln->params.pgs_per_blk;
        ln->params.sec_per_lun = ln->params.sec_per_blk * c->num_blk;
        ln->params.sec_per_log_pl = ln->params.sec_per_lun * c->num_lun;
        ln->params.total_secs = ln->params.sec_per_log_pl;

        log_info("   [lnvm: Namespace %d]\n",i);
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
    }

    log_info ("  [nvm: LightNVM is registered]\n");

    return 0;
}