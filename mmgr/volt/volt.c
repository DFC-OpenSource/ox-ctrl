#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <mqueue.h>
#include <syslog.h>
#include "volt.h"
#include "../../include/ssd.h"
#include "../../include/ox-mq.h"
#include "../../include/uatomic.h"

static atomic_t       nextprp[VOLT_CHIP_COUNT];
static pthread_mutex_t  prpmap_mutex[VOLT_CHIP_COUNT];
static uint32_t         prp_map[VOLT_CHIP_COUNT];

static VoltCtrl             *volt;
static struct nvm_mmgr      volt_mmgr;
static void                 **dma_buf;
extern struct core_struct   core;

static int volt_start_prp_map(void)
{
    int i;

    for (i = 0; i < VOLT_CHIP_COUNT; i++) {
        nextprp[i].counter = ATOMIC_INIT_RUNTIME(0);
        pthread_mutex_init (&prpmap_mutex[i], NULL);
        memset(&prp_map[i], 0x0, sizeof (uint32_t));
    }

    return 0;
}

static void volt_set_prp_map(uint32_t index, uint32_t ch, uint8_t flag)
{
    pthread_mutex_lock(&prpmap_mutex[ch]);
    prp_map[ch] = (flag)
            ? prp_map[ch] | (1 << (index - 1))
            : prp_map[ch] ^ (1 << (index - 1));
    pthread_mutex_unlock(&prpmap_mutex[ch]);
}

static uint32_t volt_get_next_prp(struct volt_dma *dma, uint32_t ch){
    uint32_t next = 0;

    do {
        pthread_mutex_lock(&prpmap_mutex[ch]);

        next = atomic_read(&nextprp[ch]);
        if (next == VOLT_DMA_SLOT_CH)
            next = 1;
        else
            next++;
        atomic_set(&nextprp[ch], next);

        if (prp_map[ch] & (1 << (next - 1))) {
            pthread_mutex_unlock(&prpmap_mutex[ch]);
            usleep(1);
            continue;
        }
        pthread_mutex_unlock(&prpmap_mutex[ch]);

        volt_set_prp_map(next, ch, 0x1);

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
    return malloc(volt_add_mem (sz));
}

static void volt_free (void *ptr, uint64_t sz)
{
    free (ptr);
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
    volt_free (volt->edma, VOLT_PAGE_SIZE + VOLT_OOB_SIZE);
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

    volt->edma = volt_alloc(VOLT_PAGE_SIZE + VOLT_OOB_SIZE);
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
    volt_free (volt->edma, VOLT_PAGE_SIZE + VOLT_OOB_SIZE);
    return -1;
}

static int volt_host_dma_helper (struct nvm_mmgr_io_cmd *nvm_cmd)
{
    uint32_t dma_sz, sec_map = 0, dma_sec, i, c = 0, ret = 0;
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
            direction = (nvm_cmd->sync_count) ? NVM_DMA_SYNC_WRITE :
                                                             NVM_DMA_FROM_HOST;
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
            for (i = 0; i < nvm_cmd->n_sectors - 1; i++)
                if (sec_map & 1 << i)
                    memcpy (oob_addr + LNVM_SEC_OOBSZ * i,
                            oob_addr + LNVM_SEC_OOBSZ * (i + 1),
                            LNVM_SEC_OOBSZ * (nvm_cmd->n_sectors - i - 1));

        ret = nvm_dma ((void *)(dma->virt_addr +
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

        nvm_cmd->status = (ret) ? NVM_IO_FAIL : NVM_IO_SUCCESS;
    } else {
        nvm_cmd->status = NVM_IO_FAIL;
    }

OUT:
    if (nvm_cmd->cmdtype == MMGR_WRITE_PG || nvm_cmd->cmdtype == MMGR_READ_PG)
        volt_set_prp_map(dma->prp_index, nvm_cmd->ppa.g.ch, 0x0);

    nvm_callback(nvm_cmd);
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
        log_err ("[ERROR: Cmd %x not completed. Aborted.]\n", cmd->cmdtype);
        cmd->status = NVM_IO_FAIL;
        goto COMPLETE;
    }

    cmd->status = NVM_IO_SUCCESS;

COMPLETE:
    retry = NVM_QUEUE_RETRY;
    do {
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

    if(volt_enqueue_io(cmd_nvm))
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

static void volt_exit (struct nvm_mmgr *mmgr)
{
    int i;
    volt_clean_mem();
    volt->status.active = 0;
    ox_mq_destroy(volt->mq);
    for (i = 0; i < mmgr->geometry->n_of_ch; i++) {
        pthread_mutex_destroy(&prpmap_mutex[i]);
        free(mmgr->ch_info[i].mmgr_rsv_list);
        free(mmgr->ch_info[i].ftl_rsv_list);
    }
    free (volt);
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
                       VOLT_BLOCK_COUNT *
                       VOLT_PLANE_COUNT *
                       VOLT_PAGE_COUNT;

        ch[i].mmgr_rsv = VOLT_RSV_BLK;
        trsv = ch[i].mmgr_rsv * VOLT_PLANE_COUNT;
        ch[i].mmgr_rsv_list = malloc (trsv * sizeof(struct nvm_ppa_addr));

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
        ch[i].ftl_rsv_list = malloc (sizeof(struct nvm_ppa_addr));
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

struct ox_mq_config volt_mq = {
    .n_queues   = VOLT_CHIP_COUNT,
    .q_size     = VOLT_QUEUE_SIZE,
    .sq_fn      = volt_execute_io,
    .cq_fn      = volt_callback,
    .to_fn      = volt_req_timeout,
    .to_usec    = VOLT_QUEUE_TO,
    .flags      = 0x0,
};

/* DEBUG (disabled): Thread to show multi-queue statistics */
static void *volt_queue_show (void *arg)
{
    /*
    while (1) {
        usleep (200000);
        ox_mq_show_stats(volt->mq);
    }
    */

    return NULL;
}

static int volt_init(void)
{
    struct nvm_mmgr_geometry *geo = volt_mmgr.geometry;
    int tot_blk = geo->n_of_planes * geo->blk_per_lun * geo->lun_per_ch *
                                                                   geo->n_of_ch;

    volt = malloc (sizeof (VoltCtrl));
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

    volt->mq = ox_mq_init(&volt_mq);
    if (!volt->mq) {
        volt_clean_mem();
        goto OUT;
    }

    /* DEBUG: Thread to show multi-queue statistics */
    pthread_t debug_th;
    pthread_create(&debug_th,NULL,volt_queue_show,NULL);

    volt->status.ready = 1; /* ready to use */

    log_info(" [volt: Volatile memory usage: %lu Mb]\n",
                                      volt->status.allocated_memory / 1048576);
    printf(" [volt: Volatile memory usage: %lu Mb]\n",
                                      volt->status.allocated_memory / 1048576);
    return 0;

OUT:
    free (volt);
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

int mmgr_volt_init(void)
{
    int ret = 0;

    volt_mmgr.name     = "VOLT";
    volt_mmgr.ops      = &volt_ops;
    volt_mmgr.geometry = &volt_geo;

    ret = volt_init();
    if(ret) {
        log_err(" [volt: Not possible to start VOLT.]\n");
        return -1;
    }

    return nvm_register_mmgr(&volt_mmgr);
}