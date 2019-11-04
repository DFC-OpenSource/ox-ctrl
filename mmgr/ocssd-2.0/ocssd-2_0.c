/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - Open-Channel SSD Media Manager (header)
 *
 * Copyright (c) 2018, Microsoft Research
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 * Written by Niclas Hedam <nhed@itu.dk>
 */

#include <stdio.h>
#include <string.h>
#include <liblightnvm.h>
#include <sched.h>
#include "../../include/libox.h"
#include "../../include/ox-mq.h"
#include "ocssd-2_0.h"

extern struct core_struct core;
static struct ocssd_ctrl ocssd;

static uint8_t **read_buf;
static uint8_t **write_buf;

static int ocssd_enqueue_cmd (struct nvm_mmgr_io_cmd *cmd)
{
    int ret, retry, qid/*, qch*/;

    /* Number of queues need to be at least the number of channels */
    //qch = ocssd.mq->config->n_queues / ocssd.mmgr_geo.n_of_ch;
    //qid = (cmd->ppa.g.lun % qch) + (cmd->ppa.g.ch * qch);
    qid = cmd->ppa.g.ch * ocssd.mmgr_geo.lun_per_ch + cmd->ppa.g.lun;

    retry = 16;
    do {
        ret = ox_mq_submit_req(ocssd.mq, qid, cmd);
    	if (ret < 0)
            retry--;
        else if (core.debug)
            printf(" MMGR_CMD type: 0x%x submitted to OCSSD.\n  "
                    "Channel: %d, lun: %d, blk: %d, pl: %d, "
                    "pg: %d]\n", cmd->cmdtype, cmd->ppa.g.ch, cmd->ppa.g.lun,
                    cmd->ppa.g.blk, cmd->ppa.g.pl, cmd->ppa.g.pg);

    } while (ret < 0 && retry);

    //if (!retry)
    //    ox_mq_show_mq (ocssd.mq);

    return (retry) ? 0 : -1;
}

static int ocssd_read_page (struct nvm_mmgr_io_cmd *cmd)
{
    if (!cmd->n_sectors)
        cmd->n_sectors = ocssd.mmgr_geo.sec_per_pg;

    if (ocssd_enqueue_cmd (cmd))
        goto CLEAN;

    return 0;

CLEAN:
    log_err("[ocssd: Read NOT enqueued.]\n");
    cmd->status = NVM_IO_FAIL;
    return -1;
}

static int ocssd_write_page (struct nvm_mmgr_io_cmd *cmd)
{
    if (!cmd->n_sectors)
        cmd->n_sectors = ocssd.mmgr_geo.sec_per_pg * ocssd.mmgr_geo.n_of_planes;

    if (ocssd_enqueue_cmd (cmd))
        goto CLEAN;

    return 0;

CLEAN:
    log_err("[ocssd: Write NOT enqueued.]\n");
    cmd->status = NVM_IO_FAIL;
    return -1;
}

static int ocssd_erase_blk (struct nvm_mmgr_io_cmd *cmd)
{
    if (!cmd->n_sectors)
        cmd->n_sectors = ocssd.mmgr_geo.n_of_planes;

    if (ocssd_enqueue_cmd (cmd))
        goto CLEAN;

    return 0;

CLEAN:
    log_err("[ocssd: Erase NOT enqueued.]\n");
    cmd->status = NVM_IO_FAIL;
    return -1;
}

static int ocssd_set_ch_info (struct nvm_channel *ch, uint16_t nc)
{
    return mmgr_set_ch_info (ch, nc);
}

static int ocssd_get_ch_info (struct nvm_channel *ch, uint16_t nc)
{
    return mmgr_get_ch_info (&ocssd.mmgr, ch, nc, 1);
}

static void ocssd_print_cmd (struct nvm_mmgr_io_cmd *cmd,
                       struct nvm_addr *addr, uint64_t *prp, uint64_t *md_prp)
{
    uint64_t md_low, md_high;

    for (int sec = 0; sec < cmd->n_sectors; sec++) {
        md_low =  (md_prp[sec]) ? *((uint64_t *) md_prp[sec]) : 0;
        md_high = (md_prp[sec]) ? *((uint64_t *) (md_prp[sec] + 8)) : 0;

        printf ("  (%d) pugrp: %d, punit: %d, chunk: %d, sectr: %d, "
                "prp: %lx, md: %lx, md_low: %lx, md_high: %lx\n",
                sec, addr[sec].l.pugrp, addr[sec].l.punit, addr[sec].l.chunk,
                addr[sec].l.sectr, prp[sec],
                md_prp[sec], md_low, md_high);
    }
}

static int ocssd_prepare_host_dma (struct nvm_mmgr_io_cmd *cmd, uint64_t *prp,
        uint64_t *md_prp, uint32_t qid)
{
    uint32_t sec_pl = ocssd.mmgr_geo.sec_per_pg * ocssd.mmgr_geo.n_of_planes;
    uint8_t dir;

    if (cmd->cmdtype == MMGR_WRITE_PG) {

        /* Move data */
        for (int sec = 0; sec < cmd->n_sectors; sec++) {

            dir = (cmd->force_sync_data[sec]) ?
                                        NVM_DMA_SYNC_WRITE : NVM_DMA_FROM_HOST;

            if (ox_dma (write_buf[qid] + ocssd.mmgr_geo.sec_size * sec,
                         cmd->prp[sec], ocssd.mmgr_geo.sec_size,
                         dir))
                return -1;

            prp[sec] = (uint64_t) write_buf[qid] + ocssd.mmgr_geo.sec_size * sec;
            md_prp[sec] = (uint64_t) write_buf[qid] +
                                    (ocssd.mmgr_geo.sec_size * sec_pl) +
                                    (ocssd.mmgr_geo.sec_oob_sz * sec);
        }

        dir = (cmd->force_sync_md) ? NVM_DMA_SYNC_WRITE : NVM_DMA_FROM_HOST;
        /* Move metadata */
        if (cmd->md_prp)
            if (ox_dma (write_buf[qid] + ocssd.mmgr_geo.sec_size * sec_pl,
                                    cmd->md_prp,
                                    ocssd.mmgr_geo.sec_oob_sz * sec_pl,
                                    dir))
            return -1;

        return 0;
    }

    for (int sec = 0; sec < cmd->n_sectors; sec++) {
        prp[sec] = (uint64_t) read_buf[qid] + (ocssd.mmgr_geo.sec_size * sec);
        if (cmd->md_prp)
            md_prp[sec] = (uint64_t) read_buf[qid] +
                            (ocssd.mmgr_geo.sec_size * cmd->n_sectors) +
                            (ocssd.mmgr_geo.sec_oob_sz * sec);
    }

    return 0;
}

static int ocssd_prepare_soc_dma (struct nvm_mmgr_io_cmd *cmd, uint64_t *prp,
                                                              uint64_t *md_prp)
{
    if (cmd->cmdtype == MMGR_READ_PG) {
        for (int sec = 0; sec < cmd->n_sectors; sec++) {
            if (cmd->prp[sec])
                prp[sec] = cmd->prp[sec];
            if (cmd->md_prp)
                md_prp[sec] = cmd->md_prp + ocssd.mmgr_geo.sec_oob_sz * sec;
        }
        return 0;
    }

    /* Write buffer must be sequential in user-space */
    for (int sec = 1; sec < cmd->n_sectors; sec++) {
        if (cmd->prp[sec] - ocssd.mmgr_geo.sec_size != cmd->prp[sec - 1]) {
            log_err ("[ocssd: Write buffers are not sequential.]\n");
            return -1;
        }
        prp[sec] = cmd->prp[sec];

        if (cmd->md_prp)
            md_prp[sec] = cmd->md_prp + ocssd.mmgr_geo.sec_oob_sz * sec;
    }

    prp[0] = cmd->prp[0];
    md_prp[0] = cmd->md_prp;

    return 0;
}

static void ocssd_process_sq (struct ox_mq_entry *req)
{
    struct nvm_mmgr_io_cmd *cmd = (struct nvm_mmgr_io_cmd *) req->opaque;
    struct nvm_addr addrs[cmd->n_sectors];
    struct nvm_ret ret;
    int retry, err;
    uint64_t prp[cmd->n_sectors], md_prp[cmd->n_sectors];
    const int PMODE = nvm_dev_get_pmode(ocssd.dev);
    uint8_t direction;

    memset (prp, 0x0, sizeof(uint64_t) * cmd->n_sectors);
    memset (md_prp, 0x0, sizeof(uint64_t) * cmd->n_sectors);
    memset (addrs, 0x0, sizeof(struct nvm_addr) * cmd->n_sectors);

    cmd->status = NVM_IO_PROCESS;

    /* OCCSDs require writes issued at plane-page granularity, while
     * reads are allowed at sector granularity. This loop fills the addrs
     * vector as required by OCSSDs, it means the FTL should issue the
     * write (and erase) commands including all planes. */

    if (cmd->cmdtype == MMGR_ERASE_BLK) {
        // if (cmd->ppa.g.pl > 0)
        //     goto COMPLETE;

        // OCSSD2 does not expose planes to the developer, so there should
        // only be one!
        assert(ocssd.mmgr_geo.n_of_planes == 1);

        addrs[0].l.pugrp = cmd->ppa.g.ch;
        addrs[0].l.punit = cmd->ppa.g.lun;
        addrs[0].l.chunk = cmd->ppa.g.blk;

        goto EXEC;
    }

    for (int sec = 0; sec < cmd->n_sectors; sec++) {
        if (!cmd->prp[sec] && cmd->cmdtype == MMGR_WRITE_PG) {
            log_err ("[ocssd: Write buffer missing in sector %d.]\n", sec);
            goto ERR;
        } else {
            addrs[sec].l.pugrp = cmd->ppa.g.ch;
            addrs[sec].l.punit = cmd->ppa.g.lun;
            addrs[sec].l.chunk = cmd->ppa.g.blk;
            addrs[sec].l.sectr = (cmd->ppa.g.pg * ocssd.mmgr_geo.sec_per_pg) + sec;
        }
    }

    if (!cmd->sync_count) {
        if (ocssd_prepare_host_dma (cmd, prp, md_prp, req->qid))
            goto ERR;
    } else {
        if (ocssd_prepare_soc_dma (cmd, prp, md_prp))
            goto ERR;
    }

EXEC:
    if (core.debug)
        ocssd_print_cmd (cmd, addrs, prp, md_prp);

    switch (cmd->cmdtype) {
        case MMGR_READ_PG:

            if (nvm_cmd_read (ocssd.dev, addrs, cmd->n_sectors,
                        (void *) prp[0], (void *) md_prp[0], PMODE, &ret)) {

                /* Flash page hasn't been programmed, return zeros */
                if (ret.result.cdw0 == 0x2ff && (ret.status == 0x1 || ret.status == 0xff) ) {
                    memset ((void *) prp[0], 0x0, ocssd.mmgr_geo.sec_size *
                                                                cmd->n_sectors);
                    if (md_prp[0])
                        memset ((void *) md_prp[0], 0x0,
                                    ocssd.mmgr_geo.sec_oob_sz * cmd->n_sectors);
                } else {
                    log_info ("[ocssd: READ ERROR %x/%hx\n", ret.result.cdw0, ret.status);
                    goto ERR;
                }

            }

            direction = (cmd->sync_count) ?
                                        NVM_DMA_SYNC_READ : NVM_DMA_TO_HOST;
            for (int sec = 0; sec < cmd->n_sectors; sec++)
                if (cmd->prp[sec])
                    if (ox_dma ((void *) prp[sec], cmd->prp[sec],
                                ocssd.mmgr_geo.sec_size, direction))
                        goto ERR;

            direction = (cmd->force_sync_md) ?
                                        NVM_DMA_SYNC_READ : NVM_DMA_TO_HOST;
            if (!cmd->sync_count && cmd->md_prp)
                if (ox_dma ((void *) md_prp[0], cmd->md_prp,
                        ocssd.mmgr_geo.sec_oob_sz * cmd->n_sectors, direction))
                    goto ERR;

            if (core.debug)
                ocssd_print_cmd (cmd, addrs, prp, md_prp);

            goto COMPLETE;

        case MMGR_WRITE_PG:
            if (nvm_cmd_write (ocssd.dev, addrs, cmd->n_sectors,
                                   (void *) prp[0], (void *) md_prp[0],
                                   PMODE, &ret))
                    goto ERR;

            goto COMPLETE;

        case MMGR_ERASE_BLK:
            if (nvm_cmd_erase (ocssd.dev, addrs, cmd->n_sectors, NULL,
                                                                  PMODE, &ret))
                goto ERR;

            goto COMPLETE;

        default:
            goto ERR;
    }

ERR:
    cmd->status = NVM_IO_FAIL;
COMPLETE:
    if (cmd->status == NVM_IO_PROCESS) cmd->status = NVM_IO_SUCCESS;
    retry = NVM_QUEUE_RETRY;
    do {
        /* Mark output as failed */
        if (cmd->status != NVM_IO_SUCCESS && req->out_row)
            req->out_row->failed = cmd->status;

        err = ox_mq_complete_req(ocssd.mq, req);
        if (err) {
            retry--;
            usleep (NVM_QUEUE_RETRY_SLEEP);
        }
    } while (err && retry);
    if (!retry)
        printf ("Command not completed %x.\n", cmd->cmdtype);
}

static void ocssd_process_cq (void *opaque)
{
    struct nvm_mmgr_io_cmd *nvm_cmd = (struct nvm_mmgr_io_cmd *) opaque;

    ox_mmgr_callback (nvm_cmd);
}

static void ocssd_process_to (void **opaque, int counter)
{
    struct nvm_mmgr_io_cmd *cmd;

    while (counter) {
        counter--;
        cmd = (struct nvm_mmgr_io_cmd *) opaque[counter];
        cmd->status = NVM_IO_TIMEOUT;
    }
}

static int ocssd_buf_init (void) {
    uint32_t i;

    read_buf = ox_malloc (sizeof (uint8_t *) * OCSSD_QUEUE_COUNT,
                                                            OX_MEM_MMGR_OCSSD);
    if (!read_buf)
        return -1;

    write_buf = ox_malloc (sizeof (uint8_t *) * OCSSD_QUEUE_COUNT,
                                                            OX_MEM_MMGR_OCSSD);
    if (!write_buf) {
        ox_free (read_buf, OX_MEM_MMGR_OCSSD);
        return -1;
    }

    for (i = 0; i < OCSSD_QUEUE_COUNT; i++) {
        read_buf[i] = ox_malloc (ocssd.mmgr_geo.sec_per_pg *
                         ocssd.mmgr_geo.n_of_planes *
                        (ocssd.mmgr_geo.sec_size + ocssd.mmgr_geo.sec_oob_sz),
                        OX_MEM_MMGR_OCSSD);
        if (!read_buf[i])
            goto FREE;

        write_buf[i] = ox_malloc (ocssd.mmgr_geo.sec_per_pg *
                         ocssd.mmgr_geo.n_of_planes *
                        (ocssd.mmgr_geo.sec_size + ocssd.mmgr_geo.sec_oob_sz),
                        OX_MEM_MMGR_OCSSD);
        if (!write_buf[i]) {
            ox_free (read_buf[i], OX_MEM_MMGR_OCSSD);
            goto FREE;
        }
    }

    return 0;

FREE:
    while (i--) {
        ox_free (read_buf[i], OX_MEM_MMGR_OCSSD);
        ox_free (write_buf[i], OX_MEM_MMGR_OCSSD);
    }

    ox_free (read_buf, OX_MEM_MMGR_OCSSD);
    ox_free (write_buf, OX_MEM_MMGR_OCSSD);
    return -1;
}

static void ocssd_buf_free (void)
{
    uint32_t i = OCSSD_QUEUE_COUNT;

    while (i--) {
        ox_free (read_buf[i], OX_MEM_MMGR_OCSSD);
        ox_free (write_buf[i], OX_MEM_MMGR_OCSSD);
    }

    ox_free (read_buf, OX_MEM_MMGR_OCSSD);
    ox_free (write_buf, OX_MEM_MMGR_OCSSD);
}

static void ocssd_stats_fill_row (struct oxmq_output_row *row, void *opaque)
{
    struct nvm_mmgr_io_cmd *cmd;

    cmd = (struct nvm_mmgr_io_cmd *) opaque;

    row->lba = 0;
    row->ch = cmd->ppa.g.ch;
    row->lun = cmd->ppa.g.lun;
    row->blk = cmd->ppa.g.blk;
    row->pg = cmd->ppa.g.pg;
    row->pl = cmd->ppa.g.pl;
    row->sec = cmd->ppa.g.sec;
    row->type = (cmd->cmdtype == MMGR_READ_PG)  ? 'R' :
                (cmd->cmdtype == MMGR_WRITE_PG) ? 'W' : 'E';
    row->failed = 0;
    row->datacmp = 0;
    row->size = cmd->n_sectors * cmd->sec_sz;
}

static int ocssd_start_mq (void)
{
    struct ox_mq_config mq_config;
    uint16_t qid;

    sprintf(mq_config.name, "%s", "OCSSD_MMGR");
    mq_config.n_queues = OCSSD_QUEUE_COUNT;
    mq_config.q_size = OCSSD_QUEUE_SIZE;
    mq_config.sq_fn = ocssd_process_sq;
    mq_config.cq_fn = ocssd_process_cq;
    mq_config.to_fn = ocssd_process_to;
    mq_config.output_fn = ocssd_stats_fill_row;
    mq_config.to_usec = OCSSD_QUEUE_TO;
    mq_config.flags = OX_MQ_CPU_AFFINITY;

    /* Set thread affinity, if enabled */
    for (qid = 0; qid < mq_config.n_queues; qid++) {
        mq_config.sq_affinity[qid] = 0;
	mq_config.cq_affinity[qid] = 0;

#if OX_TH_AFFINITY
        mq_config.sq_affinity[qid] |= ((uint64_t) 1 << 4);
        mq_config.sq_affinity[qid] |= ((uint64_t) 1 << 5);
	mq_config.sq_affinity[qid] |= ((uint64_t) 1 << 6);

        mq_config.cq_affinity[qid] |= ((uint64_t) 1 << 7);
#endif /* OX_TH_AFFINITY */
    }

    ocssd.mq = ox_mq_init(&mq_config);
    if (!ocssd.mq)
        return -1;

    return 0;
}

static int ocssd_init_geo (void)
{
    struct nvm_mmgr_geometry *g = &ocssd.mmgr_geo;

    ocssd.geo = nvm_dev_get_geo (ocssd.dev);
    if (!ocssd.geo)
        return -1;

    int sec_per_pg = nvm_dev_get_ws_min(ocssd.dev);

    g->n_of_ch      = ocssd.geo->l.npugrp;
    g->lun_per_ch   = ocssd.geo->l.npunit;
    g->blk_per_lun  = ocssd.geo->l.nchunk;

    g->n_of_planes  = 1;

    g->pg_per_blk   = ocssd.geo->l.nsectr / sec_per_pg;
    g->pg_size      = ocssd.geo->l.nbytes * sec_per_pg;

    g->sec_per_pg   = sec_per_pg;
    g->sec_oob_sz   = ocssd.geo->l.nbytes_oob;
    g->sec_size     = ocssd.geo->l.nbytes;

    return 0;
}

static void ocssd_exit (struct nvm_mmgr *mmgr)
{
    int i;

    for (i = 0; i < mmgr->geometry->n_of_ch; i++) {
        ox_free(mmgr->ch_info[i].mmgr_rsv_list, OX_MEM_MMGR);
        ox_free(mmgr->ch_info[i].ftl_rsv_list, OX_MEM_MMGR);
    }
    ox_mq_destroy(ocssd.mq);
    ocssd_buf_free ();
    nvm_dev_close (ocssd.dev);
}

static void ocssd_init_ops (void)
{
    struct nvm_mmgr_ops *o = &ocssd.mmgr_ops;

    o->write_pg       = ocssd_write_page;
    o->read_pg        = ocssd_read_page;
    o->erase_blk      = ocssd_erase_blk;
    o->exit           = ocssd_exit;
    o->get_ch_info    = ocssd_get_ch_info;
    o->set_ch_info    = ocssd_set_ch_info;
}

int mmgr_ocssd_2_0_init (void)
{
    memcpy (ocssd.dev_name, OCSSD_FILE, 13);

    if (!ox_mem_create_type ("MMGR_OCSSD", OX_MEM_MMGR_OCSSD))
        return -1;

    ocssd.dev = nvm_dev_open (ocssd.dev_name);
    if (!ocssd.dev) {
        log_err ("ocssd: Device not found.\n");
        return EMMGR_REGISTER;
    }

    if (ocssd_init_geo ()) {
        log_err ("[ocssd: Geometry command failed.]\n");
        nvm_dev_close (ocssd.dev);
        return EMMGR_REGISTER;
    }

    ocssd_init_ops ();

    if (ocssd_buf_init ()) {
        log_err ("[ocssd: Read buffer not started.]\n");
        nvm_dev_close (ocssd.dev);
        return EMMGR_REGISTER;
    }

    if (ocssd_start_mq ()) {
        log_err ("[ocssd: Multi-queue startup failed.]\n");
        ocssd_buf_free ();
        nvm_dev_close (ocssd.dev);
        return EMMGR_REGISTER;
    }

    ocssd.mmgr.name     = "OCSSD2";
    ocssd.mmgr.ops      = &ocssd.mmgr_ops;
    ocssd.mmgr.geometry = &ocssd.mmgr_geo;
    ocssd.mmgr.flags    = MMGR_FLAG_PL_CMD;

    return ox_register_mmgr(&ocssd.mmgr);
}
