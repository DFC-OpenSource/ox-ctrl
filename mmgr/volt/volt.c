/* OX: Open-Channel NVM Express SSD Controller
 * 
 *  - VOLT: Volatile storage and media manager
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <mqueue.h>
#include <syslog.h>
#include <string.h>
#include <volt.h>
#include <ox-uatomic.h>
#include <libox.h>
#include <ox-mq.h>

static u_atomic_t                nextprp[VOLT_CHIP_COUNT];
static pthread_spinlock_t        prpmap_spin[VOLT_CHIP_COUNT];
static volatile uint32_t         prp_map[VOLT_CHIP_COUNT];

static VoltCtrl             *volt;
static struct nvm_mmgr      volt_mmgr;
static void                 **dma_buf;
extern struct core_struct   core;
uint8_t                     disk;

static const char *volt_disk = "volt_disk";

static int volt_start_prp_map(void)
{
    int i;

    for (i = 0; i < VOLT_CHIP_COUNT; i++) {
        nextprp[i].counter = U_ATOMIC_INIT_RUNTIME(0);
        pthread_spin_init (&prpmap_spin[i], 0);
        prp_map[i] = 0;
    }

    return 0;
}

static void volt_set_prp_map(uint32_t index, uint32_t ch, uint8_t flag)
{
    pthread_spin_lock(&prpmap_spin[ch]);
    prp_map[ch] = (flag)
            ? prp_map[ch] | (1 << (index - 1))
            : prp_map[ch] ^ (1 << (index - 1));
    pthread_spin_unlock(&prpmap_spin[ch]);
}

static uint32_t volt_get_next_prp(struct volt_dma *dma, uint32_t ch){
    uint32_t next = 0;

    do {
        pthread_spin_lock(&prpmap_spin[ch]);

        next = u_atomic_read(&nextprp[ch]);
        if (next == VOLT_DMA_SLOT_CH)
            next = 1;
        else
            next++;
        u_atomic_set(&nextprp[ch], next);

        if (prp_map[ch] & (1 << (next - 1))) {
            pthread_spin_unlock(&prpmap_spin[ch]);
            usleep(1);
            continue;
        }

        /* set prp_map instantly */
        prp_map[ch] |= 1 << (next - 1);

        pthread_spin_unlock(&prpmap_spin[ch]);

        dma->prp_index = next;

        return next - 1;
    } while (1);
}

static VoltBlock *volt_get_block(struct nvm_ppa_addr addr){
    VoltCh *ch;
    VoltLun *lun;
    VoltBlock *blk;

    ch = &volt->channels[addr.g.ch];
    lun = &ch->lun_offset[addr.g.lun];
    blk = &lun->blk_offset[addr.g.blk * volt_mmgr.geometry->n_of_planes +
                                                                    addr.g.pl];
    return blk;
}

static uint64_t volt_add_mem(uint64_t bytes)
{
    volt->status.allocated_memory += bytes;
    return bytes;
}

static void volt_sub_mem(uint64_t bytes)
{
    volt->status.allocated_memory -= bytes;
}

static void *volt_alloc (uint64_t sz)
{
    return ox_malloc(volt_add_mem (sz), OX_MEM_MMGR_VOLT);
}

static void volt_free (void *ptr, uint64_t sz)
{
    ox_free (ptr, OX_MEM_MMGR_VOLT);
    volt_sub_mem(sz);
}

static void volt_free_page_data(VoltPage *pg)
{
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;
    volt_free (pg->data, geo->pg_size + (geo->sec_oob_sz * geo->sec_per_pg));
}

static void volt_free_block_data (VoltBlock *blk)
{
    int i_pg;

    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;

    for (i_pg = 0; i_pg < geo->pg_per_blk; i_pg++)
        volt_free_page_data (&blk->pages[i_pg]);

    volt_free (blk->pages, sizeof(VoltPage) * geo->pg_per_blk);
}

static void volt_free_blocks (int nblk, int npg_lb, int free_pg_lb)
{
    int i_blk, i_pg;
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;

    int total_blk = geo->n_of_planes * geo->blk_per_lun * geo->lun_per_ch *
                                                                   geo->n_of_ch;
    /* Free pages in the completed allocated blocks */
    for (i_blk = 0; i_blk < nblk; i_blk++)
        volt_free_block_data (&volt->blocks[i_blk]);

    /* Free pages in the last failed block */
    for (i_pg = 0; i_pg < npg_lb; i_pg++)
        volt_free_page_data (&volt->blocks[nblk].pages[i_pg]);

    if (free_pg_lb)
        volt_free (volt->blocks[nblk].pages,sizeof(VoltPage) * geo->pg_per_blk);

    volt_free (volt->blocks, sizeof(VoltBlock) * total_blk);
}

static void volt_free_luns (int tot_blk)
{
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;
    int total_luns = geo->lun_per_ch * geo->n_of_ch;

    volt_free_blocks(tot_blk, 0, 0);
    volt_free (volt->luns, sizeof (VoltLun) * total_luns);
}

static void volt_free_channels (int tot_blk)
{
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;

    volt_free_luns(tot_blk);
    volt_free (volt->channels, sizeof (VoltCh) * geo->n_of_ch);
}

static void volt_free_dma_buf (void)
{
    int slots;

    for (slots = 0; slots < VOLT_DMA_SLOT_INDEX; slots++)
        volt_free (dma_buf[slots], VOLT_PAGE_SIZE + VOLT_SECTOR_SIZE);

    volt_free (dma_buf, sizeof (void *) * VOLT_DMA_SLOT_INDEX);
    volt_free (volt->edma, VOLT_PAGE_SIZE + VOLT_SECTOR_SIZE);
}

static void volt_clean_mem(void)
{
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;
    int tot_blk = geo->n_of_planes * geo->blk_per_lun * geo->lun_per_ch *
                                                                   geo->n_of_ch;
    volt_free_channels(tot_blk);
    volt_free_dma_buf();
}

static int volt_init_page(VoltPage *pg)
{
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;

    pg->state = 0;
    pg->data = volt_alloc(geo->pg_size + (geo->sec_oob_sz * geo->sec_per_pg));
    if (!pg->data)
        return -1;

    return 0;
}

static int volt_init_blocks(void)
{
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;

    int page_count = 0, pg_blk, blk_count = 0, free_pg;
    int i_blk, i_pg;
    int total_blk = geo->n_of_planes * geo->blk_per_lun * geo->lun_per_ch *
                                                                   geo->n_of_ch;

    volt->blocks = volt_alloc(sizeof(VoltBlock) * total_blk);
    if (!volt->blocks)
        return -1;

    for (i_blk = 0; i_blk < total_blk; i_blk++) {
        VoltBlock *blk = &volt->blocks[i_blk];
        blk->id = i_blk;
        blk->life = VOLT_BLK_LIFE;

        pg_blk = 0;

        free_pg = 0;
        blk->pages = volt_alloc(sizeof(VoltPage) * geo->pg_per_blk);
        if (!blk->pages)
            goto FREE;
        free_pg = 1;

        blk->next_pg = blk->pages;

        for (i_pg = 0; i_pg < geo->pg_per_blk; i_pg++) {
            if (volt_init_page(&blk->pages[i_pg]))
                goto FREE;

            page_count++;
            pg_blk++;
        }
        blk_count++;
    }
    return page_count;

FREE:
    volt_free_blocks (blk_count, pg_blk, free_pg);
    return VOLT_MEM_ERROR;
}

static int volt_init_luns(void)
{
    int i_lun;
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;
    int total_luns = geo->lun_per_ch * geo->n_of_ch;

    volt->luns = volt_alloc(sizeof (VoltLun) * total_luns);
    if (!volt->luns)
        return VOLT_MEM_ERROR;

    for (i_lun = 0; i_lun < total_luns; i_lun++)
        volt->luns[i_lun].blk_offset =
                    &volt->blocks[i_lun * geo->blk_per_lun * geo->n_of_planes];

    return VOLT_MEM_OK;
}

static int volt_init_channels(void)
{
    int i_ch;
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;

    volt->channels = volt_alloc(sizeof (VoltCh) * geo->n_of_ch);
    if (!volt->channels)
        return VOLT_MEM_ERROR;

    for (i_ch = 0; i_ch < geo->n_of_ch; i_ch++)
        volt->channels[i_ch].lun_offset = &volt->luns[i_ch * geo->lun_per_ch];

    return VOLT_MEM_OK;
}

static int volt_init_dma_buf (void)
{
    int slots = 0, slots_i;

    volt->edma = volt_alloc(VOLT_PAGE_SIZE + VOLT_SECTOR_SIZE);
    if (!volt->edma)
        return -1;

    dma_buf = volt_alloc(sizeof (void *) * VOLT_DMA_SLOT_INDEX);
    if (!dma_buf)
        goto FREE;

    for (slots_i = 0; slots_i < VOLT_DMA_SLOT_INDEX; slots_i++) {
        dma_buf[slots_i] = volt_alloc(VOLT_PAGE_SIZE + VOLT_SECTOR_SIZE);
        if (!dma_buf[slots_i])
            goto FREE_SLOTS;
        slots++;
    }

    return 0;

FREE_SLOTS:
        for (slots_i = 0; slots_i < slots; slots_i++)
            volt_free (dma_buf[slots_i], VOLT_PAGE_SIZE + VOLT_SECTOR_SIZE);
        volt_free (dma_buf, sizeof (void *) * VOLT_DMA_SLOT_INDEX);
FREE:
    volt_free (volt->edma, VOLT_PAGE_SIZE + VOLT_SECTOR_SIZE);
    return -1;
}

/* Reorder the OOB data in case of single sector read */
static void volt_oob_reorder (uint8_t *oob, uint32_t map)
{
    int i;
    uint8_t oob_off = 0;
    uint32_t meta_sz = VOLT_OOB_SIZE;

    for (i = 0; i < VOLT_SECTOR_COUNT - 1; i++)
        if (map & (1 << i))
            memcpy (oob + oob_off,
                    oob + oob_off + VOLT_SEC_OOB_SIZE,
                    meta_sz - oob_off - VOLT_SEC_OOB_SIZE);
        else
            oob_off += VOLT_SEC_OOB_SIZE;
}

static int volt_host_dma_helper (struct nvm_mmgr_io_cmd *nvm_cmd)
{
    uint32_t dma_sz, sec_map = 0, dma_sec, c = 0, ret = 0;
    uint64_t prp;
    uint8_t direction;
    uint8_t *oob_addr;
    struct volt_dma *dma = (struct volt_dma *) nvm_cmd->rsvd;

    switch (nvm_cmd->cmdtype) {
        case MMGR_READ_PG:
            direction = (nvm_cmd->sync_count) ? NVM_DMA_SYNC_READ :
                                                             NVM_DMA_TO_HOST;
            break;
        case MMGR_WRITE_PG:
            direction = (nvm_cmd->sync_count) ?
                                        NVM_DMA_SYNC_WRITE : NVM_DMA_FROM_HOST;
            break;
        default:
            return -1;
    }

    oob_addr = dma->virt_addr + nvm_cmd->sec_sz * nvm_cmd->n_sectors;
    dma_sec = nvm_cmd->n_sectors + 1;

    for (; c < dma_sec; c++) {
        dma_sz = (c == dma_sec - 1) ? nvm_cmd->md_sz : nvm_cmd->sec_sz;
        prp = (c == dma_sec - 1) ? nvm_cmd->md_prp : nvm_cmd->prp[c];

        if (!prp) {
            sec_map |= 1 << c;
            continue;
        }

        /* Fix metadata per sector in case of reading single sector */
        if (sec_map && nvm_cmd->cmdtype == MMGR_READ_PG &&
                                            nvm_cmd->md_sz && c == dma_sec - 1)
            volt_oob_reorder (oob_addr, sec_map);

        if (c == dma_sec - 1 && nvm_cmd->force_sync_md)
            direction = (nvm_cmd->cmdtype == MMGR_READ_PG)
                                      ? NVM_DMA_SYNC_READ : NVM_DMA_SYNC_WRITE;

        if (c < dma_sec - 1 && nvm_cmd->force_sync_data[c])
            direction = (nvm_cmd->cmdtype == MMGR_READ_PG)
                                      ? NVM_DMA_SYNC_READ : NVM_DMA_SYNC_WRITE;

        ret = ox_dma ((void *)(dma->virt_addr +
                                nvm_cmd->sec_sz * c), prp, dma_sz, direction);
        if (ret) break;
    }

    return ret;
}

static void volt_callback (void *opaque)
{
    int ret = 0;
    struct nvm_mmgr_io_cmd *nvm_cmd = (struct nvm_mmgr_io_cmd *) opaque;
    struct volt_dma *dma = (struct volt_dma *) nvm_cmd->rsvd;

    if (!nvm_cmd || nvm_cmd->status == NVM_IO_TIMEOUT)
        goto OUT;

    if (dma->status) {
        if (nvm_cmd->cmdtype == MMGR_READ_PG)
            ret = volt_host_dma_helper (nvm_cmd);

        if (nvm_cmd->status != NVM_IO_FAIL)
            nvm_cmd->status = (ret) ? NVM_IO_FAIL : NVM_IO_SUCCESS;
    } else {
        nvm_cmd->status = NVM_IO_FAIL;
    }

OUT:
    if (nvm_cmd->cmdtype == MMGR_WRITE_PG || nvm_cmd->cmdtype == MMGR_READ_PG)
        volt_set_prp_map(dma->prp_index, nvm_cmd->ppa.g.ch, 0x0);

    ox_mmgr_callback (nvm_cmd);
}

static void volt_nand_dma (void *paddr, void *buf, size_t sz, uint8_t dir)
{
    switch (dir) {
        case VOLT_DMA_READ:
            memcpy(buf, paddr, sz);
            break;
        case VOLT_DMA_WRITE:
            memcpy(paddr, buf, sz);
            break;
    }
}

static int volt_process_io (struct nvm_mmgr_io_cmd *cmd)
{
    VoltBlock *blk;
    uint8_t dir;
    struct volt_dma *dma = (struct volt_dma *) cmd->rsvd;
    uint32_t pg_size = volt_mmgr.geometry->pg_size +
            (volt_mmgr.geometry->sec_oob_sz * volt_mmgr.geometry->sec_per_pg);
    int pg_i;

    blk = volt_get_block(cmd->ppa);

    dir = VOLT_DMA_WRITE;

    switch (cmd->cmdtype) {
        case MMGR_READ_PG:
            dir = VOLT_DMA_READ;
        case MMGR_WRITE_PG:
            volt_nand_dma (blk->pages[cmd->ppa.g.pg].data,
                                                dma->virt_addr, pg_size, dir);
            break;
        case MMGR_ERASE_BLK:
            if (blk->life > 0) {
                blk->life--;
            } else {
                dma->status = 0;
                return -1;
            }
            for (pg_i = 0; pg_i < volt_mmgr.geometry->pg_per_blk; pg_i++)
                memset(blk->pages[pg_i].data, 0xff, pg_size);

            break;
        default:
            dma->status = 0;
            return -1;
    }
    dma->status = 1;

    /* DEBUG: Force timeout for testing */
    /*
    if (cmd->ppa.g.pg > 28 && cmd->ppa.g.pg < 31)
        usleep (100000);
    */

    return 0;
}

static void volt_execute_io (struct ox_mq_entry *req)
{
    struct nvm_mmgr_io_cmd *cmd = (struct nvm_mmgr_io_cmd *) req->opaque;
    int ret, retry;

    ret = volt_process_io(cmd);

    if (ret) {
        if (core.debug)
            log_err ("[volt: Cmd 0x%x NOT completed. (%d/%d/%d/%d/%d)]\n",
                cmd->cmdtype, cmd->ppa.g.ch, cmd->ppa.g.lun, cmd->ppa.g.blk,
                cmd->ppa.g.pl, cmd->ppa.g.pg);
        cmd->status = NVM_IO_FAIL;
        goto COMPLETE;
    }

    cmd->status = NVM_IO_SUCCESS;

COMPLETE:
    retry = NVM_QUEUE_RETRY;
    do {
        /* Mark output as failed */
        if (cmd->status != NVM_IO_SUCCESS && req->out_row)
            req->out_row->failed = cmd->status;

        ret = ox_mq_complete_req(volt->mq, req);
        if (ret) {
            retry--;
            usleep (NVM_QUEUE_RETRY_SLEEP);
        }
    } while (ret && retry);
}

static int volt_enqueue_io (struct nvm_mmgr_io_cmd *io)
{
    int ret, retry;

    retry = 16;
    do {
        ret = ox_mq_submit_req(volt->mq, io->ppa.g.ch, io);
    	if (ret < 0)
            retry--;
        else if (core.debug)
            printf(" MMGR_CMD type: 0x%x submitted to VOLT.\n  "
                    "Channel: %d, lun: %d, blk: %d, pl: %d, "
                    "pg: %d]\n", io->cmdtype, io->ppa.g.ch, io->ppa.g.lun,
                    io->ppa.g.blk, io->ppa.g.pl, io->ppa.g.pg);

    } while (ret < 0 && retry);

    return (retry) ? 0 : -1;
}

static int volt_prepare_rw (struct nvm_mmgr_io_cmd *cmd_nvm)
{
    struct volt_dma *dma = (struct volt_dma *) cmd_nvm->rsvd;

    uint32_t sec_sz = volt_mmgr.geometry->pg_size /
                                                volt_mmgr.geometry->sec_per_pg;
    uint32_t pg_sz  = volt_mmgr.geometry->pg_size;
    uint32_t oob_sz = volt_mmgr.geometry->sec_oob_sz *
                                                volt_mmgr.geometry->sec_per_pg;

    // For now we only accept up to 16K page size and 4K sector size + 1K OOB
    if (cmd_nvm->pg_sz > pg_sz || cmd_nvm->sec_sz != sec_sz ||
                                                    cmd_nvm->md_sz > oob_sz)
        return -1;

    memset(dma, 0, sizeof(struct volt_dma));

    uint32_t prp_map = volt_get_next_prp(dma, cmd_nvm->ppa.g.ch);

    dma->virt_addr = dma_buf[(cmd_nvm->ppa.g.ch * VOLT_DMA_SLOT_CH) + prp_map];

    if (cmd_nvm->cmdtype == MMGR_READ_PG)
        memset(dma->virt_addr, 0, pg_sz + sec_sz);

    cmd_nvm->n_sectors = cmd_nvm->pg_sz / sec_sz;

    if (cmd_nvm->cmdtype == MMGR_WRITE_PG) {
        if (volt_host_dma_helper (cmd_nvm))
            return -1;
    }

    return 0;
}

static int volt_read_page (struct nvm_mmgr_io_cmd *cmd_nvm)
{
    struct volt_dma *dma = (struct volt_dma *) cmd_nvm->rsvd;

    if (volt_prepare_rw(cmd_nvm))
        goto CLEAN;

    if (volt_enqueue_io (cmd_nvm))
        goto CLEAN;

    return 0;

CLEAN:
    log_err("[MMGR Read ERROR: NVM  returned -1]\n");
    volt_set_prp_map(dma->prp_index, cmd_nvm->ppa.g.ch, 0x0);
    cmd_nvm->status = NVM_IO_FAIL;
    return -1;
}

static int volt_write_page (struct nvm_mmgr_io_cmd *cmd_nvm)
{
    struct volt_dma *dma = (struct volt_dma *) cmd_nvm->rsvd;

    if (volt_prepare_rw(cmd_nvm))
        goto CLEAN;

    if (volt_enqueue_io (cmd_nvm))
        goto CLEAN;

    return 0;

CLEAN:
    log_err("[MMGR Write ERROR: DMA or NVM returned -1]\n");
    volt_set_prp_map(dma->prp_index, cmd_nvm->ppa.g.ch, 0x0);
    cmd_nvm->status = NVM_IO_FAIL;
    return -1;
}

static int volt_erase_blk (struct nvm_mmgr_io_cmd *cmd)
{
    if(volt_enqueue_io (cmd))
        goto CLEAN;

    return 0;

CLEAN:
    log_err("[MMGR Erase ERROR: NVM library returned -1]\n");
    cmd->status = NVM_IO_FAIL;
    return -1;
}

static int volt_set_ch_info (struct nvm_channel *ch, uint16_t nc)
{
    return 0;
}

static int volt_get_ch_info (struct nvm_channel *ch, uint16_t nc)
{
    int i, n, pl, nsp = 0, trsv;
    struct nvm_ppa_addr *ppa;

    for(i = 0; i < nc; i++){
        ch[i].ch_mmgr_id = i;
        ch[i].mmgr = &volt_mmgr;
        ch[i].geometry = volt_mmgr.geometry;

        if (ch[i].i.in_use != NVM_CH_IN_USE) {
            ch[i].i.ns_id = 0x0;
            ch[i].i.ns_part = 0x0;
            ch[i].i.ftl_id = 0x0;
            ch[i].i.in_use = 0x0;
        }

        ch[i].ns_pgs = VOLT_VIRTUAL_LUNS *
                       ch->geometry->blk_per_lun *
                       VOLT_PLANE_COUNT *
                       ch->geometry->pg_per_blk;

        ch[i].mmgr_rsv = VOLT_RSV_BLK;
        trsv = ch[i].mmgr_rsv * VOLT_PLANE_COUNT;
        ch[i].mmgr_rsv_list = ox_malloc (trsv * sizeof(struct nvm_ppa_addr),
                                                                  OX_MEM_MMGR);

        if (!ch[i].mmgr_rsv_list)
            return EMEM;

        memset (ch[i].mmgr_rsv_list, 0, trsv * sizeof(struct nvm_ppa_addr));

        for (n = 0; n < ch[i].mmgr_rsv; n++) {
            for (pl = 0; pl < VOLT_PLANE_COUNT; pl++) {
                ppa = &ch[i].mmgr_rsv_list[VOLT_PLANE_COUNT * n + pl];
                ppa->g.ch = ch[i].ch_mmgr_id;
                ppa->g.lun = 0;
                ppa->g.blk = n;
                ppa->g.pl = pl;
            }
        }

        ch[i].ftl_rsv = 0;
        ch[i].ftl_rsv_list = ox_malloc(sizeof(struct nvm_ppa_addr),OX_MEM_MMGR);
        if (!ch[i].ftl_rsv_list)
            return EMEM;

        ch[i].tot_bytes = 0;
        ch[i].slba = 0;
        ch[i].elba = 0;
        nsp++;
    }

    return 0;
}

static void volt_req_timeout (void **opaque, int counter)
{
    struct nvm_mmgr_io_cmd *cmd;
    struct volt_dma *dma;

    while (counter) {
        counter--;
        cmd = (struct nvm_mmgr_io_cmd *) opaque[counter];
        dma = (struct volt_dma *) cmd->rsvd;
        cmd->status = NVM_IO_TIMEOUT;

        /* During request timeout dma->prp_index is freed and
         * dma->virt_addr is redirected to the emergency pointer */
        if (cmd->cmdtype == MMGR_WRITE_PG || cmd->cmdtype == MMGR_READ_PG) {
            dma->virt_addr = volt->edma;
            volt_set_prp_map(dma->prp_index, cmd->ppa.g.ch, 0x0);
        }
    }
}

static void volt_stats_fill_row (struct oxmq_output_row *row, void *opaque)
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

struct ox_mq_config volt_mq = {
    .name       = "VOLT",
    .n_queues   = VOLT_CHIP_COUNT,
    .q_size     = VOLT_QUEUE_SIZE,
    .sq_fn      = volt_execute_io,
    .cq_fn      = volt_callback,
    .to_fn      = volt_req_timeout,
    .output_fn  = volt_stats_fill_row,
    .to_usec    = 0,
    .flags      = 0x0
};

/* DEBUG (disabled): Thread to show multi-queue statistics */
/*static void *volt_queue_show (void *arg)
{
    
    while (1) {
        usleep (200000);
        ox_mq_show_stats(volt->mq);
    }

    return NULL;
}*/

static int volt_disk_flush (void)
{
    FILE *file;
    uint32_t blk_i, pg_i;
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;
    uint32_t pg_sz = geo->pg_size + (geo->sec_oob_sz * geo->sec_per_pg);
    uint32_t tot_blk = geo->n_of_planes * geo->blk_per_lun * geo->lun_per_ch *
                                                                   geo->n_of_ch;
    file = fopen(volt_disk, "w+");
    for (blk_i = 0; blk_i < tot_blk; blk_i++) {
        for (pg_i = 0; pg_i < geo->pg_per_blk; pg_i++) {
            if (fwrite(volt->blocks[blk_i].pages[pg_i].data, pg_sz, 1, file)<1){
                fclose (file);
                return -1;
            }
        }
    }

    fclose (file);
    return 0;
}

static int volt_disk_load (void)
{
    FILE *file;
    uint32_t blk_i, pg_i;
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;
    uint32_t pg_sz = geo->pg_size + (geo->sec_oob_sz * geo->sec_per_pg);
    uint32_t tot_blk = geo->n_of_planes * geo->blk_per_lun * geo->lun_per_ch *
                                                                   geo->n_of_ch;
    file = fopen(volt_disk, "r");
    for (blk_i = 0; blk_i < tot_blk; blk_i++) {
        for (pg_i = 0; pg_i < geo->pg_per_blk; pg_i++) {
            if (fread(volt->blocks[blk_i].pages[pg_i].data, pg_sz, 1, file)<1) {
                fclose (file);
                return -1;
            }
        }
    }

    fclose (file);
    return 0;
}

static int volt_init_disk (void)
{
    FILE *file;

    file = fopen(volt_disk, "r");
    if (!file){
        printf(" [volt: Creating disk...]\n");

        if (volt_disk_flush ())
            goto WERR;
    } else {
        printf(" [volt: Loading disk...]\n");

        fclose (file);
        if (volt_disk_load ())
            goto RERR;
    }

    printf(" [volt: Disk is ready.]\n");
    return 0;

WERR:
    remove (volt_disk);
    printf(" [volt: Disk creation failed!]\n");
    return -1;
RERR:
    printf(" [volt: Disk loading failed!]\n");
    return -1;
}

static void volt_exit (struct nvm_mmgr *mmgr)
{
    int i;

    if (disk) {
        printf(" [volt: Flushing disk...]\n");
        if (volt_disk_flush ())
            printf (" [volt: Disk flush FAILED.]\n");
    }    

    volt_clean_mem();
    volt->status.active = 0;
    ox_mq_destroy(volt->mq);
    for (i = 0; i < mmgr->geometry->n_of_ch; i++) {
        pthread_spin_destroy (&prpmap_spin[i]);
        ox_free(mmgr->ch_info[i].mmgr_rsv_list, OX_MEM_MMGR);
        ox_free(mmgr->ch_info[i].ftl_rsv_list, OX_MEM_MMGR);
    }
    ox_free (volt, OX_MEM_MMGR_VOLT);
}

static int volt_init(void)
{
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;
    int tot_blk = geo->n_of_planes * geo->blk_per_lun * geo->lun_per_ch *
                                                                   geo->n_of_ch;

    volt = ox_malloc (sizeof (VoltCtrl), OX_MEM_MMGR_VOLT);
    if (!volt)
        return -1;

    volt->status.allocated_memory = 0;

    if (volt_start_prp_map())
        goto OUT;

    if (!volt_init_blocks())
        goto OUT;

    if (!volt_init_luns()) {
        volt_free_blocks(tot_blk, 0, 0);
        goto OUT;
    }

    if (!volt_init_channels()) {
        volt_free_luns(tot_blk);
        goto OUT;
    }

    if (volt_init_dma_buf()) {
        volt_free_channels(tot_blk);
        goto OUT;
    }

    if (disk && volt_init_disk()) {
        volt_clean_mem();
        goto OUT;
    }

    sprintf(volt_mq.name, "%s", "VOLT_MMGR");
    volt->mq = ox_mq_init(&volt_mq);
    if (!volt->mq) {
        volt_clean_mem();
        goto OUT;
    }

    /* DEBUG: Thread to show multi-queue statistics */
    /*pthread_t debug_th;
    if (pthread_create(&debug_th,NULL,volt_queue_show,NULL)) {
        ox_mq_destroy (volt->mq);
        volt_clean_mem ();
        goto OUT;
    }*/

    volt->status.ready = 1; /* ready to use */

    log_info(" [volt: Volatile memory usage: %lu Mb]\n",
                                      volt->status.allocated_memory / 1048576);
    return 0;

OUT:
    ox_free (volt, OX_MEM_MMGR_VOLT);
    printf(" [volt: Not initialized! Memory allocation failed.]\n");
    printf(" [volt: Volatile memory usage: %lu bytes.]\n",
                                                volt->status.allocated_memory);
    return -1;
}

struct nvm_mmgr_ops volt_ops = {
    .write_pg       = volt_write_page,
    .read_pg        = volt_read_page,
    .erase_blk      = volt_erase_blk,
    .exit           = volt_exit,
    .get_ch_info    = volt_get_ch_info,
    .set_ch_info    = volt_set_ch_info,
};

struct nvm_mmgr_geometry volt_geo = {
    .n_of_ch        = VOLT_CHIP_COUNT,
    .lun_per_ch     = VOLT_VIRTUAL_LUNS,
    .blk_per_lun    = VOLT_BLOCK_COUNT,
    .pg_per_blk     = VOLT_PAGE_COUNT,
    .sec_per_pg     = VOLT_SECTOR_COUNT,
    .n_of_planes    = VOLT_PLANE_COUNT,
    .pg_size        = VOLT_PAGE_SIZE,
    .sec_oob_sz     = VOLT_OOB_SIZE / VOLT_SECTOR_COUNT
};

static int __mmgr_volt_init (uint8_t flag)
{
    int ret = 0;

    if (!ox_mem_create_type ("MMGR_VOLT", OX_MEM_MMGR_VOLT))
        return -1;

#ifdef CONFIG_VOLT_GB
    if (CONFIG_VOLT_GB != 4 && CONFIG_VOLT_GB != 8 && CONFIG_VOLT_GB != 16 &&
                               CONFIG_VOLT_GB != 32 && CONFIG_VOLT_GB != 64) {
        printf (" VOLT WARNING: VOLT_GB options: 4, 8, 16, 32, 64."
                                                        " Default: 4 GB.\n");
        goto DEFAULT;
    }
    if (CONFIG_VOLT_GB > 4)
        volt_geo.blk_per_lun += 64;
    if (CONFIG_VOLT_GB > 8)
        volt_geo.pg_per_blk += 64;
    if (CONFIG_VOLT_GB > 16)
        volt_geo.blk_per_lun += 128;
    if (CONFIG_VOLT_GB > 32)
        volt_geo.pg_per_blk += 128;
#endif /* CONFIG_VOLT_GB */

DEFAULT:
    volt_mmgr.name     = "VOLT";
    volt_mmgr.ops      = &volt_ops;
    volt_mmgr.geometry = &volt_geo;
    volt_mmgr.flags    = MMGR_FLAG_MIN_CP_TIME;

    disk = flag;

    ret = volt_init();
    if(ret) {
        log_err(" [volt: Not possible to start VOLT.]\n");
        return EMMGR_REGISTER;
    }

    return ox_register_mmgr(&volt_mmgr);
}

int mmgr_volt_init (void)
{
    return __mmgr_volt_init (1);
}

int mmgr_volt_init_nodisk (void)
{
    return __mmgr_volt_init (0);
}
