/* OX: Open-Channel NVM Express SSD Controller
 * 
 *  - NVMe Command Parser
 *
 * Copyright 2018 IT University of Copenhagen
 * Copyright 2018 Microsoft Research
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
#include <string.h>
#include <libox.h>
#include <nvme.h>
#include <nvmef.h>
#include <ox-app.h>

#define PARSER_NVME_COUNT   5

extern struct core_struct core;

/* Bypass FTL queue */
void ox_ftl_process_cq (void *opaque);

static void nvme_debug_print_io (NvmeRwCmd *cmd, uint32_t bs, uint64_t dt_sz,
        uint64_t md_sz, uint64_t elba, uint64_t *prp)
{
    int i;

    printf("  fuse: %d, psdt: %d\n", cmd->fuse, cmd->psdt);
    printf("  number of LBAs: %d, bs: %d\n", cmd->nlb + 1, bs);
    printf("  DMA size: %lu (data) + %lu (meta) = %lu bytes\n",
                                                 dt_sz, md_sz, dt_sz + md_sz);
    printf("  starting LBA: %lu, ending LBA: %lu\n", cmd->slba, elba);
    printf("  meta_prp: 0x%016lx\n", cmd->mptr);

    for (i = 0; i < cmd->nlb + 1; i++)
        printf("  [prp(%d): 0x%016lx\n", i, prp[i]);

    fflush (stdout);
}

static void nvme_parser_prepare_read (struct nvm_io_cmd *cmd)
{
    uint32_t i, pg, nsec;

    cmd->status.status = NVM_IO_PROCESS;

    pg = 0;
    nsec = 0;
    for (i = 0; i <= cmd->n_sec; i++) {

        /* this is an user IO, so data is transferred to host */
        cmd->mmgr_io[pg].force_sync_data[i] = 0;

        /* create flash pages */
        if ((i == cmd->n_sec) ||
            (i && (cmd->ppalist[i].ppa == cmd->ppalist[i - 1].ppa)) ||
            (i && ( cmd->ppalist[i].g.ch != cmd->ppalist[i - 1].g.ch ||
                    cmd->ppalist[i].g.lun != cmd->ppalist[i - 1].g.lun ||
                    cmd->ppalist[i].g.blk != cmd->ppalist[i - 1].g.blk ||
                    cmd->ppalist[i].g.pg != cmd->ppalist[i - 1].g.pg ||
                    cmd->ppalist[i].g.pl != cmd->ppalist[i - 1].g.pl))) {

            cmd->mmgr_io[pg].pg_index = pg;
            cmd->mmgr_io[pg].status = NVM_IO_NEW;
            cmd->mmgr_io[pg].nvm_io = cmd;
            cmd->mmgr_io[pg].pg_sz = nsec * NVME_KERNEL_PG_SIZE;
            cmd->mmgr_io[pg].n_sectors = nsec;
            cmd->mmgr_io[pg].sec_offset = i - nsec;
            cmd->mmgr_io[pg].sync_count = NULL;
            cmd->mmgr_io[pg].sync_mutex = NULL;
            cmd->mmgr_io[pg].force_sync_md = 1;

            /* No metadata is transfered */
            cmd->md_prp[pg] = 0;

            pg++;
            nsec = 0;
        }
        nsec++;
    }

    cmd->status.total_pgs = pg;
}

static int nvme_parser_read_submit (struct nvm_io_cmd *cmd)
{
    int ret;
    uint32_t sec_i, pgs;
    struct nvm_ppa_addr sec_ppa;
    struct nvm_mmgr *mmgr = ox_get_mmgr_instance ();

    pgs = cmd->n_sec / mmgr->geometry->sec_per_pl_pg;
    if (cmd->n_sec % mmgr->geometry->sec_per_pl_pg > 0)
        pgs++;

    for (sec_i = 0; sec_i < cmd->n_sec; sec_i++) {

        sec_ppa.ppa = oxapp()->gl_map->read_fn (cmd->slba + sec_i);
        if (sec_ppa.ppa == AND64)
            return 1;

        /* 'cmd->ppalist[sec_i].ppa'is replaced for the PPA */
        cmd->ppalist[sec_i].ppa = sec_ppa.ppa;

        cmd->channel[sec_i] = &mmgr->ch_info[sec_ppa.g.ch];
    }

    nvme_parser_prepare_read (cmd);

    cmd->callback.cb_fn = ox_ftl_process_cq;
    cmd->callback.opaque = (void *) cmd;
    ret = oxapp()->ppa_io->submit_fn (cmd);

    return ret;
}

static int parser_nvme_rw (NvmeRequest *req, NvmeCmd *cmd)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    NvmeNamespace *ns = req->ns;
    NvmeCtrl *n = core.nvme_ctrl;
    uint64_t keys[16];
    int i;

    uint32_t nlb  = rw->nlb + 1;
    uint64_t slba = rw->slba;

    const uint64_t elba = slba + nlb;
    const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint8_t data_shift = ns->id_ns.lbaf[lba_index].ds;
    uint64_t data_size = nlb << data_shift;

    req->nvm_io.status.status = NVM_IO_NEW;
    req->is_write = rw->opcode == NVME_CMD_WRITE;

    if (elba > (ns->id_ns.nsze) + 1)
	return NVME_LBA_RANGE | NVME_DNR;

    if (n->id_ctrl.mdts && data_size > n->page_size * (1 << n->id_ctrl.mdts))
	return NVME_LBA_RANGE | NVME_DNR;

    if (nlb > 256)
	return NVME_INVALID_FIELD | NVME_DNR;

    /* Metadata and End-to-end Data protection are disabled */

    /* Map PRPs and SGL addresses */
    switch (rw->psdt) {
        case CMD_PSDT_PRP:
        case CMD_PSDT_RSV:
            req->nvm_io.prp[0] = rw->prp1;

            if (nlb == 2)
                req->nvm_io.prp[1] = rw->prp2;
            else if (nlb > 2)
                ox_dma ((void *)(&req->nvm_io.prp[1]), rw->prp2,
                            (nlb - 1) * sizeof(uint64_t), NVM_DMA_FROM_HOST);
            break;
        case CMD_PSDT_SGL:
        case CMD_PSDT_SGL_MD:
            if (nvmef_sgl_to_prp (nlb, &cmd->sgl, req->nvm_io.prp, keys))
                return NVME_INVALID_FIELD;
            break;
        default:
            return NVME_INVALID_FORMAT;
    }

    req->slba = slba;
    req->meta_size = 0;
    req->status = NVME_SUCCESS;
    req->nlb = nlb;
    req->lba_index = lba_index;

    req->nvm_io.cid = rw->cid;
    req->nvm_io.sec_sz = NVME_KERNEL_PG_SIZE;
    req->nvm_io.md_sz = 0;
    req->nvm_io.cmdtype = (req->is_write) ? MMGR_WRITE_PG : MMGR_READ_PG;
    req->nvm_io.n_sec = nlb;
    req->nvm_io.req = (void *) req;
    req->nvm_io.slba = slba;

    req->nvm_io.status.pg_errors = 0;
    req->nvm_io.status.ret_t = 0;
    req->nvm_io.status.total_pgs = 0;
    req->nvm_io.status.pgs_p = 0;
    req->nvm_io.status.pgs_s = 0;
    req->nvm_io.status.status = NVM_IO_NEW;

    for (i = 0; i < 8; i++) {
        req->nvm_io.status.pg_map[i] = 0;
    }

    if (core.debug)
        nvme_debug_print_io (rw, req->nvm_io.sec_sz, data_size,
                                     req->nvm_io.md_sz, elba, req->nvm_io.prp);

    if (req->is_write)
        return ox_submit_ftl (&req->nvm_io);
    else
        return (!nvme_parser_read_submit (&req->nvm_io)) ? NVME_NO_COMPLETE :
                                                            NVME_CMD_ABORT_REQ;
}

static int parser_nvme_null (NvmeRequest *req, NvmeCmd *cmd)
{
    return NVME_SUCCESS;
}

static int parser_nvme_identify (NvmeRequest *req, NvmeCmd *cmd)
{
    NvmeIdentify *id_cmd = (NvmeIdentify *) cmd;
    uint64_t prp[1];
    uint64_t keys[16];
    void *data;

    /* Map PRPs and SGL addresses */
    switch (id_cmd->psdt) {
        case CMD_PSDT_PRP:
        case CMD_PSDT_RSV:
            prp[0] = id_cmd->prp1;
            break;
        case CMD_PSDT_SGL:
        case CMD_PSDT_SGL_MD:
            if (nvmef_sgl_to_prp (1, &id_cmd->sgl, prp, keys))
                return NVME_INVALID_FIELD;
            break;
        default:
            return NVME_INVALID_FORMAT;
    }

    switch (id_cmd->cns) {
	case NVME_CNS_CTRL:
	    data = (void *)(&core.nvme_ctrl->id_ctrl);
	    break;
	case NVME_CNS_NS:
	    data = (void *)(&core.nvme_ctrl->namespaces[0].id_ns);
    }

    ox_dma (data, prp[0], sizeof (NvmeIdCtrl), NVM_DMA_TO_HOST);

    return NVME_SUCCESS;
}

static struct nvm_parser_cmd nvme_cmds[PARSER_NVME_COUNT] = {
    {
        .name       = "NVME_WRITE",
        .opcode     = NVME_CMD_WRITE,
        .opcode_fn  = parser_nvme_rw,
        .queue_type = NVM_CMD_IO
    },
    {
        .name       = "NVME_READ",
        .opcode     = NVME_CMD_READ,
        .opcode_fn  = parser_nvme_rw,
        .queue_type = NVM_CMD_IO
    },
    {
        .name       = "NVME_WRITE_NULL",
        .opcode     = NVME_CMD_WRITE_NULL,
        .opcode_fn  = parser_nvme_null,
        .queue_type = NVM_CMD_IO
    },
    {
        .name       = "NVME_READ_NULL",
        .opcode     = NVME_CMD_READ_NULL,
        .opcode_fn  = parser_nvme_null,
        .queue_type = NVM_CMD_IO
    },
    {
	.name	    = "NVME_ADM_IDENTIFY",
	.opcode	    = NVME_ADM_CMD_IDENTIFY,
	.opcode_fn  = parser_nvme_identify,
	.queue_type = NVM_CMD_ADMIN
    }
};

static void parser_nvme_exit (struct nvm_parser *parser)
{
    return;
}

static struct nvm_parser parser_nvme = {
    .name       = "NVME_PARSER",
    .cmd_count  = PARSER_NVME_COUNT,
    .exit       = parser_nvme_exit
};

int parser_nvme_init (void)
{
    parser_nvme.cmd = nvme_cmds;

    return ox_register_parser (&parser_nvme);
}
