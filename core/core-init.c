/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX Controller Core Initialization
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
 *
 * The controller core is responsible to handle:
 *
 *      - Media managers: NAND, DDR, OCSSD, etc.
 *      - Flash Translation Layers + LightNVM raw FTL support
 *      - NVMe Transports: PCIe, network fabric, etc.
 *      - NVMe, NVMe over Fabrics and LightNVM Standards
 *      - RDMA handler: TCP, RoCE, InfiniBand, etc.
 */

#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/queue.h>
#include <syslog.h>
#include <mqueue.h>
#include <sched.h>
#include <pthread.h>
#include <sys/time.h>
#include <nvme.h>
#include <libox.h>
#include <ox-mq.h>
#include <ox-app.h>

/* FTL multi-queue functions, implemented in core-exec.c */
void ox_ftl_process_sq (struct ox_mq_entry *req);
void ox_ftl_process_cq (void *opaque);
void ox_ftl_process_to (void **opaque, int counter);

struct ox_mod_fn {
    ox_module_init_fn  *fn;
    LIST_ENTRY (ox_mod_fn) entry;
};

struct core_struct core;

LIST_HEAD(mmgr_list, nvm_mmgr) mmgr_head = LIST_HEAD_INITIALIZER(mmgr_head);
LIST_HEAD(ftl_list, nvm_ftl) ftl_head = LIST_HEAD_INITIALIZER(ftl_head);
LIST_HEAD(parser_list, nvm_parser) parser_head = LIST_HEAD_INITIALIZER(parser_head);

LIST_HEAD(mmgr_fn, ox_mod_fn) mmgr_fh = LIST_HEAD_INITIALIZER(mmgr_fh);
LIST_HEAD(ftl_fn, ox_mod_fn) ftl_fh = LIST_HEAD_INITIALIZER(ftl_fh);
LIST_HEAD(parser_fn, ox_mod_fn) parser_fh = LIST_HEAD_INITIALIZER(parser_fh);

static ox_module_init_fn *transport_init = NULL;

struct nvm_ftl *ox_get_ftl_instance (uint16_t ftl_id)
{
    struct nvm_ftl *ftl;
    LIST_FOREACH(ftl, &ftl_head, entry){
        if(ftl->ftl_id == ftl_id)
            return ftl;
    }

    return NULL;
}

/* For now, this function returns the first MMGR in the list */
struct nvm_mmgr *ox_get_mmgr_instance (void)
{
    return LIST_FIRST (&mmgr_head);
}

static void nvm_calc_print_geo (struct nvm_mmgr_geometry *g)
{
    g->sec_per_pl_pg = g->sec_per_pg * g->n_of_planes;
    g->sec_per_blk = g->sec_per_pl_pg * g->pg_per_blk;
    g->sec_per_lun = g->sec_per_blk * g->blk_per_lun;
    g->sec_per_ch = g->sec_per_lun * g->lun_per_ch;
    g->pg_per_lun = g->pg_per_blk * g->blk_per_lun;
    g->pg_per_ch = g->pg_per_lun * g->lun_per_ch;
    g->blk_per_ch = g->blk_per_lun * g->lun_per_ch;
    g->tot_sec = g->sec_per_ch * g->n_of_ch;
    g->tot_pg = g->pg_per_ch * g->n_of_ch;
    g->tot_blk = g->blk_per_ch * g->n_of_ch;
    g->tot_lun = g->lun_per_ch * g->n_of_ch;
    g->sec_size = g->pg_size / g->sec_per_pg;
    g->pl_pg_size = g->pg_size * (uint32_t) g->n_of_planes;
    g->blk_size = g->pl_pg_size * (uint32_t) g->pg_per_blk;
    g->lun_size = g->blk_size * (uint64_t) g->blk_per_lun;
    g->ch_size = g->lun_size * (uint64_t) g->lun_per_ch;
    g->tot_size = g->ch_size * (uint64_t) g->n_of_ch;
    g->pg_oob_sz = g->sec_oob_sz * g->sec_per_pg;
    g->pl_pg_oob_sz = g->pg_oob_sz * g->n_of_planes;
    g->blk_oob_sz = g->pl_pg_oob_sz * g->pg_per_blk;
    g->lun_oob_sz = g->blk_oob_sz * g->blk_per_lun;
    g->ch_oob_sz = g->lun_oob_sz * g->lun_per_ch;
    g->tot_oob_sz = g->ch_oob_sz * g->n_of_ch;

    log_info ("   [n_of_planes   = %d]", g->n_of_planes);
    log_info ("   [n_of_channels = %d]", g->n_of_ch);
    log_info ("   [sec_per_pg    = %d]", g->sec_per_pg);
    log_info ("   [sec_per_pl_pg = %d]", g->sec_per_pl_pg);
    log_info ("   [sec_per_blk   = %d]", g->sec_per_blk);
    log_info ("   [sec_per_lun   = %d]", g->sec_per_lun);
    log_info ("   [sec_per_ch    = %d]", g->sec_per_ch);
    log_info ("   [tot_sec       = %lu]", g->tot_sec);
    log_info ("   [pg_per_blk    = %d]", g->pg_per_blk);
    log_info ("   [pg_per_lun    = %d]", g->pg_per_lun);
    log_info ("   [pg_per_ch     = %d]", g->pg_per_ch);
    log_info ("   [tot_pg        = %lu]", g->tot_pg);
    log_info ("   [blk_per_lun   = %d]", g->blk_per_lun);
    log_info ("   [blk_per_ch    = %d]", g->blk_per_ch);
    log_info ("   [tot_blk       = %d]", g->tot_blk);
    log_info ("   [lun_per_ch    = %d]", g->lun_per_ch);
    log_info ("   [tot_lun       = %d]", g->tot_lun);
    log_info ("   [sector_size   = %d bytes, %d KB]",
                                              g->sec_size, g->sec_size / 1024);
    log_info ("   [page_size     = %d bytes, %d KB]",
                                                g->pg_size, g->pg_size / 1024);
    log_info ("   [pl_page_size  = %d bytes, %d KB]",
                                          g->pl_pg_size, g->pl_pg_size / 1024);
    log_info ("   [blk_size      = %d bytes, %d MB]",
                                       g->blk_size, g->blk_size / 1024 / 1024);
    log_info ("   [lun_size      = %lu bytes, %lu MB]",
                                       g->lun_size, g->lun_size / 1024 / 1024);
    log_info ("   [ch_size       = %lu bytes, %lu MB]",
                                         g->ch_size, g->ch_size / 1024 / 1024);
    log_info ("   [tot_size      = %lu bytes, %lu GB]",
                                g->tot_size, g->tot_size / 1024 / 1024 / 1024);
    log_info ("   [sec_oob_sz    = %d bytes]", g->sec_oob_sz);
    log_info ("   [pg_oob_sz     = %d bytes]", g->pg_oob_sz);
    log_info ("   [pl_pg_oob_sz  = %d bytes]", g->pl_pg_oob_sz);
    log_info ("   [blk_oob_sz    = %d bytes, %d KB]",
                                          g->blk_oob_sz, g->blk_oob_sz / 1024);
    log_info ("   [lun_oob_sz    = %d bytes, %d KB]",
                                          g->lun_oob_sz, g->lun_oob_sz / 1024);
    log_info ("   [ch_oob_sz     = %lu bytes, %lu KB]",
                                            g->ch_oob_sz, g->ch_oob_sz / 1024);
    log_info ("   [tot_oob_sz    = %lu bytes, %lu MB]",
                                   g->tot_oob_sz, g->tot_oob_sz / 1024 / 1024);
}

int ox_add_parser (ox_module_init_fn *fn)
{
    struct ox_mod_fn *mod;

    if (!fn)
        return -1;

    mod = malloc (sizeof (struct ox_mod_fn));
    if (!mod)
        return -1;

    mod->fn = fn;

    LIST_INSERT_HEAD(&parser_fh, mod, entry);
    return 0;
}

int ox_add_mmgr (ox_module_init_fn *fn)
{
    struct ox_mod_fn *mod;

    if (!fn)
        return -1;

    mod = malloc (sizeof (struct ox_mod_fn));
    if (!mod)
        return -1;

    mod->fn = fn;

    LIST_INSERT_HEAD(&mmgr_fh, mod, entry);
    return 0;
}

int ox_add_ftl (ox_module_init_fn *fn)
{
    struct ox_mod_fn *mod;

    if (!fn)
        return -1;

    mod = malloc (sizeof (struct ox_mod_fn));
    if (!mod)
        return -1;

    mod->fn = fn;

    LIST_INSERT_HEAD(&ftl_fh, mod, entry);
    return 0;
}

int ox_add_transport (ox_module_init_fn *fn)
{
    if (!fn)
        return -1;

    if (transport_init) {
        log_err (" [ox: Only 1 NVMe transport can be added.\n");
        return -1;
    }

    transport_init = fn;

    return 0;
}

int ox_add_net_interface (const char *addr, uint16_t port)
{
    if (core.std_transport != NVM_TRANSP_FABRICS) {
        log_err ("[ox: Use 'ox_set_std_transport(NVM_TRANSP_FABRICS)'"
                                        " before adding network interfaces.]");
        return -1;
    }
    memcpy (core.net_ifaces[core.net_ifaces_count].addr, addr, strlen (addr));
    core.net_ifaces[core.net_ifaces_count].port = port;
    core.net_ifaces_count++;

    return 0;
}

void ox_set_std_ftl (uint8_t ftl_id)
{
    core.std_ftl = ftl_id;
}

void ox_set_std_oxapp (uint8_t ftl_id)
{
    core.std_oxapp = ftl_id;
}

void ox_set_std_transport (uint8_t transp_id)
{
    core.std_transport = transp_id;
    memset (core.net_ifaces, 0x0, 64 * sizeof (struct nvm_net_iface));
    core.net_ifaces_count = 0;
}

int ox_register_pcie_handler (struct nvm_pcie *pcie)
{
    if (strlen(pcie->name) > MAX_NAME_SIZE)
        return EMAX_NAME_SIZE;

    if (core.nvm_pcie) {
        log_err ("  [nvm: PCI Transport already registered.]");
        return -1;
    }

    if(pthread_create (&pcie->io_thread, NULL, pcie->ops->nvme_consumer, pcie))
        return EPCIE_REGISTER;

    core.nvm_pcie = pcie;

    log_info("  [nvm: PCI Transport registered: %s]\n", pcie->name);

    return 0;
}

int ox_register_fabrics (struct nvm_fabrics *fabrics)
{
    if (strlen(fabrics->name) > MAX_NAME_SIZE)
        return EMAX_NAME_SIZE;

    core.nvm_fabrics = fabrics;

    log_info("  [nvm: Fabrics Transport registered: %s]\n", fabrics->name);

    return 0;
}

static void ox_register_parser_cmd (struct nvm_parser_cmd *cmd)
{
    struct nvm_parser_cmd *parser;

    if (cmd->opcode > 0xff) {
        log_err ("[parser: Invalid opcode: %x\n", cmd->opcode);
        return;
    }

    parser = (cmd->queue_type == NVM_CMD_ADMIN) ? core.parser_admin :
                                                  core.parser_io;

    if (parser[cmd->opcode].opcode_fn) {
        log_err ("[parser: Opcode already registered: %x\n", cmd->opcode);
        return;
    }

    if (!cmd->opcode_fn)
        return;

    strcpy ((char *) parser[cmd->opcode].name, cmd->name);
    parser[cmd->opcode].opcode = cmd->opcode;
    parser[cmd->opcode].opcode_fn = cmd->opcode_fn;

    log_info ("   [parser: Opcode registered: %x - %s\n", cmd->opcode, cmd->name);
}

int ox_register_mmgr (struct nvm_mmgr *mmgr)
{
    struct nvm_mmgr_geometry *g = mmgr->geometry;

    if (strlen(mmgr->name) > MAX_NAME_SIZE)
        return EMAX_NAME_SIZE;

    mmgr->ch_info = ox_calloc(sizeof(struct nvm_channel), g->n_of_ch,
                                                            OX_MEM_CORE_INIT);
    if (!mmgr->ch_info)
        return EMMGR_REGISTER;

    if (mmgr->ops->get_ch_info(mmgr->ch_info, g->n_of_ch))
        return EMMGR_REGISTER;

    LIST_INSERT_HEAD(&mmgr_head, mmgr, entry);
    core.mmgr_count++;
    core.nvm_ch_count += g->n_of_ch;

    log_info("  [nvm: Media Manager registered: %s]]", mmgr->name);

    nvm_calc_print_geo (g);

    return 0;
}

int ox_register_parser (struct nvm_parser *parser)
{
    uint32_t cmd_i;

    if (strlen(parser->name) > MAX_NAME_SIZE)
        return EMAX_NAME_SIZE;

    for (cmd_i = 0; cmd_i < parser->cmd_count; cmd_i++)
        ox_register_parser_cmd (&parser->cmd[cmd_i]);

    core.parser_count++;
    LIST_INSERT_HEAD(&parser_head, parser, entry);
    
    log_info("  [nvm: Parser Set registered: %s]", parser->name);

    return 0;
}

static void ox_ftl_stats_fill_row (struct oxmq_output_row *row, void *opaque)
{
    struct nvm_io_cmd *cmd;

    cmd = (struct nvm_io_cmd *) opaque;

    row->lba = cmd->cid;
    row->ch = 0;
    row->lun = 0;
    row->blk = 0;
    row->pg = 0;
    row->pl = 0;
    row->sec = 0;
    row->type = (cmd->cmdtype == MMGR_WRITE_PG) ? 'W' : 'R';
    row->failed = 0;
    row->datacmp = 0;
    row->size = cmd->n_sec * cmd->sec_sz;
}

int ox_register_ftl (struct nvm_ftl *ftl)
{
    struct ox_mq_config mq_config;
    uint16_t qid;

    if (strlen(ftl->name) > MAX_NAME_SIZE)
        return EMAX_NAME_SIZE;

    /* Start FTL multi-queue */
    sprintf(mq_config.name, "%s", ftl->name);
    mq_config.n_queues = ftl->nq;
    mq_config.q_size = NVM_FTL_QUEUE_SIZE;
    mq_config.sq_fn = ox_ftl_process_sq;
    mq_config.cq_fn = ox_ftl_process_cq;
    mq_config.to_fn = ox_ftl_process_to;
    mq_config.output_fn = ox_ftl_stats_fill_row;
    mq_config.to_usec = NVM_FTL_QUEUE_TO;
    mq_config.flags = (OX_MQ_TO_COMPLETE | OX_MQ_CPU_AFFINITY);

    /* Set thread affinity, if enabled */
    for (qid = 0; qid < mq_config.n_queues; qid++) {
        mq_config.sq_affinity[qid] = 0;
        mq_config.cq_affinity[qid] = 0;

#if OX_TH_AFFINITY
        if (qid < mq_config.n_queues / 2) {
            mq_config.sq_affinity[qid] |= ((uint64_t) 1 << 0);
	    mq_config.sq_affinity[qid] |= ((uint64_t) 1 << 4);
            mq_config.sq_affinity[qid] |= ((uint64_t) 1 << 5);
	    mq_config.cq_affinity[qid] |= ((uint64_t) 1 << 0);
            mq_config.cq_affinity[qid] |= ((uint64_t) 1 << 4);
	    mq_config.cq_affinity[qid] |= ((uint64_t) 1 << 5);
        } else {
            mq_config.sq_affinity[qid] |= ((uint64_t) 1 << 0);
            mq_config.cq_affinity[qid] |= ((uint64_t) 1 << 0);
        }
#endif /* OX_TH_AFFINITY */
    }

    ftl->mq = ox_mq_init(&mq_config);
    if (!ftl->mq)
        return -1;

    ftl->next_queue[0] = 0;
    ftl->next_queue[1] = 0;

    if (pthread_spin_init (&ftl->next_queue_spin[0], 0)) {
        ox_mq_destroy (ftl->mq);
        return -1;
    }

    if (pthread_spin_init (&ftl->next_queue_spin[1], 0)) {
        pthread_spin_destroy (&ftl->next_queue_spin[0]);
        ox_mq_destroy (ftl->mq);
        return -1;
    }

    core.ftl_q_count += ftl->nq;

    LIST_INSERT_HEAD(&ftl_head, ftl, entry);
    core.ftl_count++;

    log_info("  [nvm: FTL (%s)(%d) registered.]\n", ftl->name, ftl->ftl_id);
    if (ftl->cap & 1 << FTL_CAP_GET_BBTBL)
        log_info("    [%s cap: Get Bad Block Table]\n", ftl->name);
    if (ftl->cap & 1 << FTL_CAP_SET_BBTBL)
        log_info("    [%s cap: Set Bad Block Table]\n", ftl->name);
    if (ftl->cap & 1 << FTL_CAP_GET_L2PTBL)
        log_info("    [%s cap: Get Logical to Physical Table]\n", ftl->name);
    if (ftl->cap & 1 << FTL_CAP_GET_L2PTBL)
        log_info("    [%s cap: Set Logical to Physical Table]\n", ftl->name);
    if (ftl->cap & 1 << FTL_CAP_INIT_FN)
        log_info("    [%s cap: Application Function Init]\n", ftl->name);
    if (ftl->cap & 1 << FTL_CAP_EXIT_FN)
        log_info("    [%s cap: Application Function Exit]\n", ftl->name);

    if (ftl->bbtbl_format == FTL_BBTBL_BYTE)
        log_info("    [%s Bad block table type: Byte array. 1 byte per blk.]\n",
                                                                    ftl->name);
    return 0;
}

static void ox_remove_parser (struct nvm_parser_cmd *cmd)
{
    struct nvm_parser_cmd *parser;

    parser = (cmd->queue_type == NVM_CMD_ADMIN) ? core.parser_admin :
                                                  core.parser_io;

    memset ((void *) parser[cmd->opcode].name, 0x0, MAX_NAME_SIZE);
    parser[cmd->opcode].opcode = 0x0;
    parser[cmd->opcode].opcode_fn = NULL;
}

static void nvm_unregister_mmgr (struct nvm_mmgr *mmgr)
{
    if (LIST_EMPTY(&mmgr_head))
        return;

    mmgr->ops->exit(mmgr);
    ox_free(mmgr->ch_info, OX_MEM_CORE_INIT);
    LIST_REMOVE(mmgr, entry);
    core.mmgr_count--;
    core.nvm_ch_count -= mmgr->geometry->n_of_ch;
    log_info(" [nvm: Media Manager unregistered: %s]\n", mmgr->name);
}

static void ox_unregister_ftl (struct nvm_ftl *ftl)
{
    if (LIST_EMPTY(&ftl_head))
        return;

    pthread_spin_destroy (&ftl->next_queue_spin[1]);
    pthread_spin_destroy (&ftl->next_queue_spin[0]);
    ox_mq_destroy(ftl->mq);
    core.ftl_q_count -= ftl->nq;
    ftl->ops->exit();
    LIST_REMOVE(ftl, entry);
    core.ftl_count--;
    log_info(" [nvm: FTL (%s)(%d) unregistered.]\n", ftl->name, ftl->ftl_id);
}

static void ox_unregister_parser_set (struct nvm_parser *parser)
{
    uint32_t cmd_i;

    if (LIST_EMPTY(&parser_head))
        return;

    for (cmd_i = 0; cmd_i < parser->cmd_count; cmd_i++)
        ox_remove_parser (&parser->cmd[cmd_i]);

    parser->exit (parser);
    core.parser_count--;
    LIST_REMOVE(parser, entry);

    log_info(" [nvm: Parser Set (%s) unregistered.]\n", parser->name);
}

static int nvm_ch_config (void)
{
    int i, c = 0, ret;

    core.nvm_ch = ox_calloc(sizeof(struct nvm_channel *), core.nvm_ch_count,
                                                              OX_MEM_CORE_INIT);
    if (!core.nvm_ch)
        return EMEM;

    core.nvm_ns_size = 0;

    struct nvm_channel *ch;
    struct nvm_mmgr *mmgr;
    LIST_FOREACH(mmgr, &mmgr_head, entry){
        for (i = 0; i < mmgr->geometry->n_of_ch; i++){
            ch = core.nvm_ch[c] = &mmgr->ch_info[i];
            ch->ch_id       = c;
            ch->geometry    = mmgr->geometry;
            ch->mmgr        = mmgr;

            /* For now we set all channels to be managed by the standard FTL */
            /* For now all channels are set to the same namespace */
            if (ch->i.in_use != NVM_CH_IN_USE || core.reset) {
                ch->i.in_use = NVM_CH_IN_USE;
                ch->i.ns_id = 0x1;
                ch->i.ns_part = c;
                ch->i.ftl_id = core.std_ftl;

                /* FLush new config to NVM */
                mmgr->ops->set_ch_info(ch, 1);
            }

            ch->ftl = ox_get_ftl_instance(ch->i.ftl_id);

            if(!ch->ftl || !ch->mmgr)
                return ECH_CONFIG;

            /* ftl must set available number of pages */
            ret = ch->ftl->ops->init_ch(ch);
                if (ret) return ret;

            ch->tot_bytes = ch->ns_pgs *
                                  (ch->geometry->pg_size & 0xffffffffffffffff);
            ch->slba = core.nvm_ns_size;
            ch->elba = core.nvm_ns_size + ch->tot_bytes - 1;

            core.nvm_ns_size += ch->tot_bytes;
            c++;
        }
    }

    if (core.std_ftl == FTL_ID_OXAPP) {
        /* APPNVM Transaction Init */
        struct nvm_ftl_cap_gl_fn app_tr;
        app_tr.arg = NULL;
        app_tr.ftl_id = FTL_ID_OXAPP;
        app_tr.fn_id = APP_FN_TRANSACTION;
        if (ox_ftl_cap_exec(FTL_CAP_INIT_FN, &app_tr))
            return -1;

        /* APPNVM Global Init */
        struct nvm_ftl_cap_gl_fn app_gl;
        app_gl.arg = NULL;
        app_gl.ftl_id = FTL_ID_OXAPP;
        app_gl.fn_id = APP_FN_GLOBAL;
        if (ox_ftl_cap_exec(FTL_CAP_INIT_FN, &app_gl))
            return -1;

        core.run_flag |= RUN_OXAPP;
    }
    
    return 0;
}

static void nvm_printerror (int error)
{
    printf ("\n");
    switch (error) {
        case EMAX_NAME_SIZE:
            printf ("ox: Name size out of bounds.\n");
            break;
        case EMMGR_REGISTER:
            printf ("ox: Media Manager error.\n");
            break;
        case EFTL_REGISTER:
            printf ("ox: FTL error.\n");
            break;
        case ETRANSP_REGISTER:
            printf ("ox: NVMe Transport startup failure.\n");
            break;
        case ENVME_REGISTER:
            printf ("ox: NVMe standard startup failure.\n");
            break;
        case ECH_CONFIG:
            printf ("ox: a channel is not set correctly.\n");
            break;
        case EMEM:
            printf ("ox: Memory error.\n");
            break;
        case ENOMMGR:
            printf ("ox: No Media Manager has been registered.\n");
            break;
        case ENOFTL:
            printf ("ox: No Flash Translation Layer has been registered.\n");
            break;
        case ENOPARSER:
            printf ("ox: No Command parser has been registered.\n");
            break;
        case ENOTRANSP:
            printf ("ox: No NVMe Transport has been registered.\n");
            break;
        default:
            printf("ox: Unknown ERROR 0x%x.\n", error);
    }
    printf ("ox: Check '/sys/log/syslog' for further information.\n");
}

static void nvm_print_log (void)
{
    log_info("[nvm: OX Controller ready.]\n");
    log_info("  [nvm: %d media manager(s) up, total of %d channels]\n",
                                           core.mmgr_count, core.nvm_ch_count);

    int i;
    for(i=0; i<core.nvm_ch_count; i++){
        log_info("    [channel: %d, FTL id: %d"
                            " ns_id: %d, ns_part: %d, pg: %lu, in_use: %d]\n",
                            core.nvm_ch[i]->ch_id, core.nvm_ch[i]->i.ftl_id,
                            core.nvm_ch[i]->i.ns_id, core.nvm_ch[i]->i.ns_part,
                            core.nvm_ch[i]->ns_pgs, core.nvm_ch[i]->i.in_use);
    }
    log_info("  [nvm: namespace size: %lu bytes]\n",
                                                core.nvme_ctrl->ns_size[0]);
    log_info("    [nvm: total pages: %lu]\n",
             core.nvme_ctrl->ns_size[0] / core.nvm_ch[0]->geometry->pg_size);
}

static int nvm_check_modules (void)
{
    if (LIST_EMPTY(&mmgr_fh))
        return ENOMMGR;

    if (LIST_EMPTY(&ftl_fh))
        return ENOFTL;

    if (LIST_EMPTY(&parser_fh))
        return ENOPARSER;    

    if (!transport_init)
        return ENOTRANSP;

    return 0;
}

static void nvm_print_init (void)
{
    struct nvm_mmgr     *mmgr;
    struct nvm_ftl      *ftl;
    struct nvm_parser   *parser;
    uint64_t             mem;

    printf ("\n");
    LIST_FOREACH(mmgr, &mmgr_head, entry) {
        printf(" Media Manager  : %s\n", mmgr->name);
    }
    printf ("\n");
    LIST_FOREACH(ftl, &ftl_head, entry) {
        if (ftl->ftl_id == FTL_ID_OXAPP) {
            printf(" FTL            : %s\n", ftl->name);
            printf("  BACK-END  PPA I/O    -> %s\n", oxapp()->ppa_io->name);
            printf("  FRONT-END LBA I/O    -> %s\n", oxapp()->lba_io->name);
            printf("  BAD BLOCK MANAGER    -> %s\n", oxapp()->bbt->name);
            printf("  METADATA  MANAGER    -> %s\n", oxapp()->md->name);
            printf("  UNIT    PROVISIONING -> %s\n", oxapp()->ch_prov->name);
            printf("  UNIT    MAPPING      -> %s\n", oxapp()->ch_map->name);
            printf("  GLOBAL  PROVISIONING -> %s\n", oxapp()->gl_prov->name);
            printf("  GLOBAL  MAPPING      -> %s\n", oxapp()->gl_map->name);
            printf("  GARBAGE COLLECTOR    -> %s\n", oxapp()->gc->name);
            printf("  LOG     MANAGEMENT   -> %s\n\n", oxapp()->log->name);
        }else {
            printf(" FTL            : %s\n", ftl->name);
        }
    }

    printf (" NAMESPACE:\n");
    printf ("  Installed NVM  : %.2lf GB (%lu bytes)\n",
            (double) core.nvm_ch[0]->geometry->tot_size / 1024 / 1024 / 1024,
            core.nvm_ch[0]->geometry->tot_size);
    printf ("  Available Size : %.2lf GB (%lu bytes)\n",
                    (double) core.nvme_ctrl->ns_size[0] / 1024 / 1024 / 1024,
                    core.nvme_ctrl->ns_size[0]);
    printf ("  Logical Blocks : 1 to %lu\n", core.nvme_ctrl->ns_size[0] /
                                          core.nvm_ch[0]->geometry->sec_size);
    printf ("  Block Size     : %d bytes\n\n", 4096);

    LIST_FOREACH(parser, &parser_head, entry) {
        printf(" Command Parser : %s - %d opcodes\n", parser->name,
                                                            parser->cmd_count);
    }
    if (core.std_transport == NVM_TRANSP_PCI)
        printf (" Transport      : NVMe over PCIe (%s)\n", core.nvm_pcie->name);
    else
        printf (" Transport      : NVMe over Fabrics (%s)\n",
                                                        core.nvm_fabrics->name);

#if OX_MEM_MANAGER
    mem = ox_mem_total();
    printf ("\n Controller memory usage: %.5lf MB (%lu bytes).\n",
                        (double) mem / (double) 1024 / (double) 1024, mem);
#endif

    printf("\nOX Controller started succesfully. Log: /var/log/syslog\n");
}

static int nvm_init (uint8_t start_all)
{
    struct ox_mod_fn *mod;
    int ret;

    if ( (ret = nvm_check_modules () ))
        return ret;

    core.ftl_q_count = 0;
    core.run_flag = 0;
    core.ftl_count = 0;
    core.parser_count = 0;
    memset (core.parser_admin, 0x0, 256 * sizeof (struct nvm_parser_cmd));
    memset (core.parser_io, 0x0, 256 * sizeof (struct nvm_parser_cmd));

    if (start_all) {
        core.mmgr_count = 0;
        core.nvm_ch_count = 0;
        core.nvme_ctrl = ox_malloc (sizeof (NvmeCtrl), OX_MEM_CORE_INIT);
        if (!core.nvme_ctrl)
            return EMEM;
        core.run_flag |= RUN_NVME_ALLOC;  
    }
    core.nvme_ctrl->running = 1; /* not ready */

    /* media managers */
    LIST_FOREACH(mod, &mmgr_fh, entry) {
        ret = mod->fn ();
        if(ret) goto OUT;
    }
    core.run_flag |= RUN_MMGR;

    /* flash translation layers */
    LIST_FOREACH(mod, &ftl_fh, entry) {
        ret = mod->fn ();
        if(ret) goto OUT;
    }
    core.run_flag |= RUN_FTL;

    /* create channels and global namespace */
    ret = nvm_ch_config();
    if(ret) goto OUT;
    core.run_flag |= RUN_CH;

    /* parser sets */
    LIST_FOREACH(mod, &parser_fh, entry) {
        ret = mod->fn ();
        if(ret) goto OUT;
    }
    core.run_flag |= RUN_PARSER;  

    /* NVMe Transport */
    if (start_all && transport_init ) {
        ret = transport_init ();
        if(ret) goto OUT;
        core.run_flag |= RUN_TRANSPORT;
    }

    /* NVMe Base Spec + over Fabrics Spec */
    if (core.std_transport == NVM_TRANSP_PCI) {
        if (core.nvm_pcie)
            ret = nvme_init (core.nvme_ctrl);
        else
            ret = ENOTRANSP;
    } else {
        if (core.nvm_fabrics)
            ret = nvmef_init (core.nvme_ctrl);
        else
            ret = ENOTRANSP;
    }
    if(ret) goto OUT;
    core.run_flag |= RUN_NVME;

    core.nvme_ctrl->running = 0; /* ready */
    core.nvme_ctrl->stop = 0;
    core.run_flag |= RUN_READY;

    nvm_print_init ();

    return 0;

OUT:
    return ret;
}

static void nvm_clear_transport (void)
{
    if (core.std_transport == NVM_TRANSP_PCI) {
        if (core.nvm_pcie) {
            core.nvm_pcie->ops->exit();
            log_info (" [nvm: PCI Transport unregistered: %s]\n",
                                                        core.nvm_pcie->name);
        }
    } else {
        if (core.nvm_fabrics) {
            core.nvm_fabrics->ops->exit();
            log_info (" [nvm: Fabrics Transport unregistered: %s]\n",
                                                       core.nvm_fabrics->name);
        }
    }
    core.run_flag ^= RUN_TRANSPORT;
    core.nvm_pcie      = NULL;
    core.nvm_fabrics   = NULL;
}

static void nvm_clear_all (uint8_t stop_all)
{
    struct nvm_parser *parser;
    struct nvm_ftl *ftl;
    struct nvm_mmgr *mmgr;
    struct ox_mod_fn *mod;

    /* Clear NVMe Transport */
    if( (core.run_flag & RUN_TRANSPORT) && stop_all)
        nvm_clear_transport ();

    if (core.std_ftl == FTL_ID_OXAPP) {
        /* APPNVM Global and Transaction Exit */
        struct nvm_ftl_cap_gl_fn app_gl;
        struct nvm_ftl_cap_gl_fn app_tr;
        if (core.run_flag & RUN_OXAPP) {
            app_gl.arg = NULL;
            app_gl.ftl_id = FTL_ID_OXAPP;
            app_gl.fn_id = APP_FN_GLOBAL;
            ox_ftl_cap_exec(FTL_CAP_EXIT_FN, &app_gl);

            app_tr.arg = NULL;
            app_tr.ftl_id = FTL_ID_OXAPP;
            app_tr.fn_id = APP_FN_TRANSACTION;
            ox_ftl_cap_exec(FTL_CAP_EXIT_FN, &app_tr);

            core.run_flag ^= RUN_OXAPP;
        }
    }

    /* Clean all command parsers */
    if (core.run_flag & RUN_PARSER) {
        while (core.parser_count) {
            parser = LIST_FIRST(&parser_head);
            ox_unregister_parser_set (parser);
        };
        core.run_flag ^= RUN_PARSER;
    }

    /* Clean channels */
    if (core.run_flag & RUN_CH) {
        ox_free(core.nvm_ch, OX_MEM_CORE_INIT);
        core.run_flag ^= RUN_CH;
    }

    /* Clean all ftls */
    if (core.run_flag & RUN_FTL) {
        while (core.ftl_count) {
            ftl = LIST_FIRST(&ftl_head);
            ox_unregister_ftl(ftl);
        };
        core.run_flag ^= RUN_FTL;
    }

    /* Clean all media managers */
    if (core.run_flag & RUN_MMGR) {
        while (core.mmgr_count) {
            mmgr = LIST_FIRST(&mmgr_head);
            nvm_unregister_mmgr(mmgr);
        };
        core.run_flag ^= RUN_MMGR;
    }

    if (stop_all == NVM_FULL_UPDOWN) {
        while (!LIST_EMPTY(&mmgr_fh)) {
            mod = LIST_FIRST(&mmgr_fh);
            LIST_REMOVE(mod, entry);
            free (mod);
        };
        while (!LIST_EMPTY(&ftl_fh)) {
            mod = LIST_FIRST(&ftl_fh);
            LIST_REMOVE(mod, entry);
            free (mod);
        };
        while (!LIST_EMPTY(&parser_fh)) {
            mod = LIST_FIRST(&parser_fh);
            LIST_REMOVE(mod, entry);
            free (mod);
        };
    }

    /* Clean Nvme */
    if(core.run_flag & RUN_NVME) {
        core.nvme_ctrl->running = 1; /* not ready */
        usleep (100);
        if (core.std_transport == NVM_TRANSP_PCI)
            nvme_exit();
        else
            nvmef_exit();
        core.run_flag ^= RUN_NVME;
    }
    if ((core.run_flag & RUN_NVME_ALLOC) && stop_all) {
        ox_free(core.nvme_ctrl, OX_MEM_CORE_INIT);
        core.run_flag ^= RUN_NVME_ALLOC;
    }

    printf("\nOX Controller closed succesfully.\n");
}

int ox_restart (void)
{
    int ret;
    nvm_clear_all (NVM_RESTART);
    if ((ret = nvm_init (NVM_RESTART) == 0) &&
                                        (core.std_transport == NVM_TRANSP_PCI))
        core.nvm_pcie->ops->reset();

    return ret;
}

int  ox_ctrl_start (int argc, char **argv)
{
    int ret = 0, exec;
    struct timespec ts;
    uint64_t us;

    core.nvm_pcie      = NULL;
    core.nvm_fabrics   = NULL;
    core.reset         = 0;

    if (ox_mem_init ())
        return -1;

    if (ox_stats_init ()) {
        ox_mem_exit ();
        return -1;
    }

    openlog("OX" , LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL0);

    if (    !ox_mem_create_type ("CORE_INIT", OX_MEM_CORE_INIT) ||
            !ox_mem_create_type ("CORE_EXEC", OX_MEM_CORE_EXEC) ||
            !ox_mem_create_type ("OX_MQ", OX_MEM_OX_MQ)         ||
            !ox_mem_create_type ("MMGR", OX_MEM_MMGR)           ||
            !ox_mem_create_type ("FTL", OX_MEM_FTL)) {
        ox_stats_exit ();
        ox_mem_exit ();
        return -1;
    }

    exec = ox_cmdarg_init (argc, argv);
    if (exec < 0) {
        ox_stats_exit ();
        ox_mem_exit ();
        return -1;
    }

    printf("OX Controller %s - %s\n Starting...\n", OX_VER, LABEL);
    log_info("[nvm: OX Controller is starting...]\n");

    if (!core.nostart) {

        GET_MICROSECONDS (us, ts);
        ox_stats_add_rec (OX_STATS_REC_START1_US, us);

        ret = nvm_init(NVM_FULL_UPDOWN);
        if(ret)
            goto CLEAN;

        GET_MICROSECONDS (us, ts);
        ox_stats_add_rec (OX_STATS_REC_START2_US, us);

        nvm_print_log();
    }

    switch (exec) {
        case OX_ADMIN_MODE:
            ret = ox_admin_init (core.args_global);
            break;
        case OX_RUN_MODE:
            ox_cmdline_init ();
            goto OUT;
        default:
            goto CLEAN;
    }

    goto OUT;
CLEAN:
    nvm_printerror(ret);
    nvm_clear_all(NVM_FULL_UPDOWN);
    ox_cmdarg_exit();
    ox_stats_exit ();
    ox_mem_exit ();
    return -1;
OUT:
    ox_mq_output_stop ();
    nvm_clear_all(NVM_FULL_UPDOWN);
    ox_cmdarg_exit();
    ox_stats_exit ();
    ox_mem_exit ();
    return 0;
}

void ox_ctrl_clear (void)
{
    nvm_clear_all (NVM_FULL_UPDOWN);
}
