/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - File Backend: File backed Media Manager
 *
 * Copyright 2024 University of Copenhagen
 *
 * Written by Ivan Luiz Picoli <ivpi@di.ku.dk>
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
#include <file-be.h>
#include <ox-uatomic.h>
#include <libox.h>
#include <ox-mq.h>
#include <errno.h>
#include <fcntl.h>

static u_atomic_t                nextprp[FILE_CHIP_COUNT];
static pthread_spinlock_t        prpmap_spin[FILE_CHIP_COUNT];
static volatile uint32_t         prp_map[FILE_CHIP_COUNT];

static FileBECtrl          *fbe;
static struct nvm_mmgr      fbe_mmgr;
static void                 **dma_buf;
extern struct core_struct   core;

static char fbe_disk[256] = "filebe_disk";

static uint8_t *blk_zeros;

static int fbe_start_prp_map(void)
{
    int i;

    for (i = 0; i < FILE_CHIP_COUNT; i++) {
        nextprp[i].counter = U_ATOMIC_INIT_RUNTIME(0);
        pthread_spin_init (&prpmap_spin[i], 0);
        prp_map[i] = 0;
    }

    return 0;
}

static void fbe_set_prp_map(uint32_t index, uint32_t ch, uint8_t flag)
{
    pthread_spin_lock(&prpmap_spin[ch]);
    prp_map[ch] = (flag)
            ? prp_map[ch] | (1 << (index - 1))
            : prp_map[ch] ^ (1 << (index - 1));
    pthread_spin_unlock(&prpmap_spin[ch]);
}

static uint32_t fbe_get_next_prp(struct fbe_dma *dma, uint32_t ch){
    uint32_t next = 0;

    do {
        pthread_spin_lock(&prpmap_spin[ch]);

        next = u_atomic_read(&nextprp[ch]);
        if (next == FILE_DMA_SLOT_CH)
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

/* Reorder the OOB data in case of single sector read */
static void fbe_oob_reorder (uint8_t *oob, uint32_t map)
{
    int i;
    uint8_t oob_off = 0;
    uint32_t meta_sz = FILE_OOB_SIZE;

    for (i = 0; i < FILE_SECTOR_COUNT - 1; i++)
        if (map & (1 << i))
            memcpy (oob + oob_off,
                    oob + oob_off + FILE_SEC_OOB_SIZE,
                    meta_sz - oob_off - FILE_SEC_OOB_SIZE);
        else
            oob_off += FILE_SEC_OOB_SIZE;
}

static int fbe_host_dma_helper (struct nvm_mmgr_io_cmd *nvm_cmd)
{
    uint32_t dma_sz, sec_map = 0, dma_sec, c = 0, ret = 0;
    uint64_t prp;
    uint8_t direction;
    uint8_t *oob_addr;
    struct fbe_dma *dma = (struct fbe_dma *) nvm_cmd->rsvd;

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
            fbe_oob_reorder (oob_addr, sec_map);

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

static uint64_t fbe_add_mem(uint64_t bytes)
{
    fbe->status.allocated_memory += bytes;
    return bytes;
}

static void fbe_sub_mem(uint64_t bytes)
{
    fbe->status.allocated_memory -= bytes;
}

static void *fbe_alloc (uint64_t sz)
{
    return ox_malloc(fbe_add_mem (sz), OX_MEM_MMGR_FILEBE);
}

static void fbe_free (void *ptr, uint64_t sz)
{
    ox_free (ptr, OX_MEM_MMGR_FILEBE);
    fbe_sub_mem(sz);
}

static void fbe_free_block_data (FileBEBlock *blk)
{
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;

    fbe_free (blk->pages, sizeof(FileBEPage) * geo->pg_per_blk);
}

static void fbe_free_blocks (int nblk, int npg_lb, int free_pg_lb)
{
    int i_blk;
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;

    int total_blk = geo->n_of_planes * geo->blk_per_lun * geo->lun_per_ch *
                                                                   geo->n_of_ch;
    /* Free pages in the completed allocated blocks */
    for (i_blk = 0; i_blk < nblk; i_blk++)
        fbe_free_block_data (&fbe->blocks[i_blk]);

    if (free_pg_lb)
        fbe_free (fbe->blocks[nblk].pages,sizeof(FileBEPage) * geo->pg_per_blk);

    fbe_free (fbe->blocks, sizeof(FileBEBlock) * total_blk);
}

static int fbe_init_page(FileBEPage *pg)
{
    pg->state = 0;
    return 0;
}

static int fbe_init_blocks(void)
{
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;

    int page_count = 0, pg_blk, blk_count = 0, free_pg;
    int i_blk, i_pg;
    int total_blk = geo->n_of_planes * geo->blk_per_lun * geo->lun_per_ch *
                                                                   geo->n_of_ch;

    fbe->blocks = fbe_alloc(sizeof(FileBEBlock) * total_blk);
    if (!fbe->blocks)
        return -1;

    for (i_blk = 0; i_blk < total_blk; i_blk++) {
        FileBEBlock *blk = &fbe->blocks[i_blk];
        blk->id = i_blk;
        blk->life = FILE_BLK_LIFE;
	blk->file_id = 0;
	blk->file_offset = 0;

        pg_blk = 0;

        free_pg = 0;
        blk->pages = fbe_alloc(sizeof(FileBEPage) * geo->pg_per_blk);
        if (!blk->pages)
            goto FREE;
        free_pg = 1;

        blk->next_pg = blk->pages;

        for (i_pg = 0; i_pg < geo->pg_per_blk; i_pg++) {
            if (fbe_init_page(&blk->pages[i_pg]))
                goto FREE;

            page_count++;
            pg_blk++;
        }
        blk_count++;
    }
    return page_count;

FREE:
    fbe_free_blocks (blk_count, pg_blk, free_pg);
    return FILE_MEM_ERROR;
}

static int fbe_init_luns(void)
{
    int i_lun;
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;
    int total_luns = geo->lun_per_ch * geo->n_of_ch;

    fbe->luns = fbe_alloc(sizeof (FileBELun) * total_luns);
    if (!fbe->luns)
        return FILE_MEM_ERROR;

    for (i_lun = 0; i_lun < total_luns; i_lun++)
        fbe->luns[i_lun].blk_offset =
                    &fbe->blocks[i_lun * geo->blk_per_lun * geo->n_of_planes];

    return FILE_MEM_OK;
}

static int fbe_init_channels(void)
{
    int i_ch;
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;

    fbe->channels = fbe_alloc(sizeof (FileBECh) * geo->n_of_ch);
    if (!fbe->channels)
        return FILE_MEM_ERROR;

    for (i_ch = 0; i_ch < geo->n_of_ch; i_ch++)
        fbe->channels[i_ch].lun_offset = &fbe->luns[i_ch * geo->lun_per_ch];

    return FILE_MEM_OK;
}

static int fbe_init_dma_buf (void)
{
    int slots = 0, slots_i;

    fbe->edma = fbe_alloc(FILE_PAGE_SIZE + FILE_SECTOR_SIZE);
    if (!fbe->edma)
        return -1;

    dma_buf = fbe_alloc(sizeof (void *) * FILE_DMA_SLOT_INDEX);
    if (!dma_buf)
        goto FREE;

    for (slots_i = 0; slots_i < FILE_DMA_SLOT_INDEX; slots_i++) {
        dma_buf[slots_i] = fbe_alloc(FILE_PAGE_SIZE + FILE_SECTOR_SIZE);
        if (!dma_buf[slots_i])
            goto FREE_SLOTS;
        slots++;
    }

    return 0;

FREE_SLOTS:
        for (slots_i = 0; slots_i < slots; slots_i++)
            fbe_free (dma_buf[slots_i], FILE_PAGE_SIZE + FILE_SECTOR_SIZE);
        fbe_free (dma_buf, sizeof (void *) * FILE_DMA_SLOT_INDEX);
FREE:
    fbe_free (fbe->edma, FILE_PAGE_SIZE + FILE_SECTOR_SIZE);
    return -1;
}

static void fbe_free_luns (int tot_blk)
{
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;
    int total_luns = geo->lun_per_ch * geo->n_of_ch;

    fbe_free_blocks(tot_blk, 0, 0);
    fbe_free (fbe->luns, sizeof (FileBELun) * total_luns);
}

static void fbe_free_channels (int tot_blk)
{
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;

    fbe_free_luns(tot_blk);
    fbe_free (fbe->channels, sizeof (FileBECh) * geo->n_of_ch);
}

static void fbe_free_dma_buf (void)
{
    int slots;

    for (slots = 0; slots < FILE_DMA_SLOT_INDEX; slots++)
        fbe_free (dma_buf[slots], FILE_PAGE_SIZE + FILE_SECTOR_SIZE);

    fbe_free (dma_buf, sizeof (void *) * FILE_DMA_SLOT_INDEX);
    fbe_free (fbe->edma, FILE_PAGE_SIZE + FILE_SECTOR_SIZE);
}

static void fbe_clean_mem(void)
{
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;
    int tot_blk = geo->n_of_planes * geo->blk_per_lun * geo->lun_per_ch *
                                                                   geo->n_of_ch;
    fbe_free_channels(tot_blk);
    fbe_free_dma_buf();
}

static void fbe_callback (void *opaque)
{
    int ret = 0;
    struct nvm_mmgr_io_cmd *nvm_cmd = (struct nvm_mmgr_io_cmd *) opaque;
    struct fbe_dma *dma = (struct fbe_dma *) nvm_cmd->rsvd;

    if (!nvm_cmd || nvm_cmd->status == NVM_IO_TIMEOUT)
        goto OUT;

    if (dma->status) {
        if (nvm_cmd->cmdtype == MMGR_READ_PG)
            ret = fbe_host_dma_helper (nvm_cmd);

        if (nvm_cmd->status != NVM_IO_FAIL)
            nvm_cmd->status = (ret) ? NVM_IO_FAIL : NVM_IO_SUCCESS;
    } else {
        nvm_cmd->status = NVM_IO_FAIL;
    }

OUT:
    if (nvm_cmd->cmdtype == MMGR_WRITE_PG || nvm_cmd->cmdtype == MMGR_READ_PG)
        fbe_set_prp_map(dma->prp_index, nvm_cmd->ppa.g.ch, 0x0);

    ox_mmgr_callback (nvm_cmd);
}

static FileBEBlock *fbe_get_block(struct nvm_ppa_addr addr){
    FileBECh *ch;
    FileBELun *lun;
    FileBEBlock *blk;

    ch = &fbe->channels[addr.g.ch];
    lun = &ch->lun_offset[addr.g.lun];
    blk = &lun->blk_offset[addr.g.blk * fbe_mmgr.geometry->n_of_planes +
                                                                    addr.g.pl];
    return blk;
}

static int fbe_erase_file (struct nvm_ppa_addr ppa)
{
    int file;
    char file_name[256];
    uint32_t blk_size;
    uint64_t offset;
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;
    FileBEBlock *fbe_blk;

    blk_size = (geo->pg_size + (geo->sec_per_pg * geo->sec_oob_sz)) * geo->pg_per_blk;
    sprintf(file_name, "%s_%d_%d", fbe_disk, ppa.g.ch, ppa.g.lun);

    file = open(file_name, O_WRONLY|O_SYNC, 0666);

    fbe_blk = fbe_get_block (ppa);
    offset = fbe_blk->file_offset + (blk_size * ppa.g.pl);

    if (pwrite(file, blk_zeros, blk_size, offset) < 0) {
	close (file);
	return -1;
    }

    close (file);
    return 0;    
}

static int fbe_rw_file (struct nvm_ppa_addr ppa, void *buf, uint8_t dir)
{
    int file;
    char file_name[256];
    uint32_t blk_size, offset, pg_size;
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;
    FileBEBlock *fbe_blk;

    pg_size = geo->pg_size + (geo->sec_per_pg * geo->sec_oob_sz);
    blk_size = pg_size * geo->pg_per_blk;
    sprintf(file_name, "%s_%d_%d", fbe_disk, ppa.g.ch, ppa.g.lun);

    file = (dir == FILE_DMA_READ) ?
	    open(file_name, O_RDONLY, 0666) :
	    open(file_name, O_WRONLY/*|O_SYNC*/, 0666);

    fbe_blk = fbe_get_block (ppa);

    offset = fbe_blk->file_offset + (blk_size * ppa.g.pl);
    offset += ppa.g.pg * pg_size;
    //printf("blk_size: %d, pg_size: %d, blk: %d, pg: %d, pl: %d, file_id: %d, offset: %d, file_offset:%d\n", blk_size, pg_size, ppa.g.blk, ppa.g.pg, ppa.g.pl, fbe_blk->file_id, offset, fbe_blk->file_offset);

    switch (dir) {
	case FILE_DMA_READ:
	    if (pread(file, buf, pg_size, offset) < 0)
		goto ERR;
	    break;
	case FILE_DMA_WRITE:
	    if (pwrite(file, buf, pg_size, offset) < 0)
		goto ERR;
    }

    close (file);
    return 0;

ERR:
    close (file);
    return -1;
}

static int fbe_append_blk_file (struct nvm_ppa_addr ppa) {
    int file;
    char file_name[256];
    uint32_t blk_size;
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;

    blk_size = (geo->pg_size + (geo->sec_per_pg * geo->sec_oob_sz))
		* geo->pg_per_blk * geo->n_of_planes;
    sprintf(file_name, "%s_%d_%d", fbe_disk, ppa.g.ch, ppa.g.lun);

    file = open(file_name, O_WRONLY|O_CREAT|O_APPEND/*|O_SYNC*/, 0666);

    if (write (file, blk_zeros, blk_size) < 0) {
	close (file);
	return -1;
    }

    close (file);
    return 0;    
}

static int fbe_update_md_file (struct nvm_ppa_addr ppa) {
    int file;
    char file_name[256];
    FileBEBlock *blk;
    
    sprintf(file_name, "%s_%d_%d_md", fbe_disk, ppa.g.ch, ppa.g.lun);

    blk = fbe_get_block (ppa);

    file = open(file_name, O_WRONLY|O_SYNC, 0666);

    if (pwrite (file, &blk->file_id, sizeof(uint32_t), ppa.g.blk * sizeof(uint32_t)) < 0) {
	close (file);
	return -1;
    }

    close (file);
    return 0;
}

static int fbe_process_io (struct nvm_mmgr_io_cmd *cmd)
{
    FileBEBlock *blk, *fbe_blk;
    struct fbe_dma *dma = (struct fbe_dma *) cmd->rsvd;
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;
    struct nvm_ppa_addr ppa;
    int blk_i, pl_i, count = 0;
    uint32_t pg_size, blk_size;

    blk = fbe_get_block(cmd->ppa);

    switch (cmd->cmdtype) {
        case MMGR_READ_PG:
	    if (!blk->file_id ||
		 fbe_rw_file (cmd->ppa, dma->virt_addr, FILE_DMA_READ)) {
		
		//dma->status = 0;
		//return -1;
	    }
	    break;
        case MMGR_WRITE_PG:
            if (!blk->file_id) {

		/* Create new blk in a file */
		if (fbe_append_blk_file (cmd->ppa)) {
		    dma->status = 0;
		    return -1;
		}

		/* Update file blk metadata */
		ppa.g.ch = cmd->ppa.g.ch;
		ppa.g.lun = cmd->ppa.g.lun;

		for (blk_i = 0; blk_i < geo->blk_per_lun; blk_i++) {
		    ppa.g.blk = blk_i;
		    ppa.g.pl = 0;
		    fbe_blk = fbe_get_block (ppa);
		    if (fbe_blk->file_id)
			count++;
		}
		count++;

		pg_size = geo->pg_size + (geo->sec_per_pg * geo->sec_oob_sz);
		blk_size = pg_size * geo->pg_per_blk * geo->n_of_planes;
		

		/* Update all plane blocks */
		for (pl_i = 0; pl_i < geo->n_of_planes; pl_i++) {
		    ppa.g.blk = cmd->ppa.g.blk;
		    ppa.g.pl = pl_i;
		    fbe_blk = fbe_get_block (ppa);
		
		    fbe_blk->file_id = count;
		    fbe_blk->file_offset = blk_size * (count - 1);
		}

		/* Flush file MD */
		if (fbe_update_md_file (cmd->ppa)) {
		    /* TODO: Should truncate to return previous file size */
		    dma->status = 0;
		    return -1;
		}
	    }
	    
	    if (fbe_rw_file (cmd->ppa, dma->virt_addr, FILE_DMA_WRITE)) {
		dma->status = 0;
		return -1;
	    }

	    break;
        case MMGR_ERASE_BLK:
            if (blk->life > 0) {
                blk->life--;
            } else {
                dma->status = 0;
                return -1;
            }
            /* Write zeros to blk FILE */
	    if (blk->file_id) {
		if (fbe_erase_file (cmd->ppa))
		    return -1;
	    }

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

static void fbe_execute_io (struct ox_mq_entry *req)
{
    struct nvm_mmgr_io_cmd *cmd = (struct nvm_mmgr_io_cmd *) req->opaque;
    int ret, retry;

    ret = fbe_process_io(cmd);

    if (ret) {
        if (core.debug)
            log_err ("[fbe: Cmd 0x%x NOT completed. (%d/%d/%d/%d/%d)]\n",
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

        ret = ox_mq_complete_req(fbe->mq, req);
        if (ret) {
            retry--;
            usleep (NVM_QUEUE_RETRY_SLEEP);
        }
    } while (ret && retry);
}

static int fbe_enqueue_io (struct nvm_mmgr_io_cmd *io)
{
    int ret, retry;

    retry = 16;
    do {
        ret = ox_mq_submit_req(fbe->mq, io->ppa.g.ch, io);
    	if (ret < 0)
            retry--;
        else if (core.debug)
            printf(" MMGR_CMD type: 0x%x submitted to FILE-BE.\n  "
                    "Channel: %d, lun: %d, blk: %d, pl: %d, "
                    "pg: %d]\n", io->cmdtype, io->ppa.g.ch, io->ppa.g.lun,
                    io->ppa.g.blk, io->ppa.g.pl, io->ppa.g.pg);

    } while (ret < 0 && retry);

    return (retry) ? 0 : -1;
}

static int fbe_prepare_rw (struct nvm_mmgr_io_cmd *cmd_nvm)
{
    struct fbe_dma *dma = (struct fbe_dma *) cmd_nvm->rsvd;

    uint32_t sec_sz = fbe_mmgr.geometry->pg_size /
                                                fbe_mmgr.geometry->sec_per_pg;
    uint32_t pg_sz  = fbe_mmgr.geometry->pg_size;
    uint32_t oob_sz = fbe_mmgr.geometry->sec_oob_sz *
                                                fbe_mmgr.geometry->sec_per_pg;

    // For now we only accept up to 16K page size and 4K sector size + 1K OOB
    if (cmd_nvm->pg_sz > pg_sz || cmd_nvm->sec_sz != sec_sz ||
                                                    cmd_nvm->md_sz > oob_sz)
        return -1;

    memset(dma, 0, sizeof(struct fbe_dma));

    uint32_t prp_map = fbe_get_next_prp(dma, cmd_nvm->ppa.g.ch);

    dma->virt_addr = dma_buf[(cmd_nvm->ppa.g.ch * FILE_DMA_SLOT_CH) + prp_map];

    if (cmd_nvm->cmdtype == MMGR_READ_PG)
        memset(dma->virt_addr, 0, pg_sz + sec_sz);

    cmd_nvm->n_sectors = cmd_nvm->pg_sz / sec_sz;

    if (cmd_nvm->cmdtype == MMGR_WRITE_PG) {
        if (fbe_host_dma_helper (cmd_nvm))
            return -1;
    }

    return 0;
}

static int fbe_read_page (struct nvm_mmgr_io_cmd *cmd_nvm)
{
    struct fbe_dma *dma = (struct fbe_dma *) cmd_nvm->rsvd;

    if (fbe_prepare_rw(cmd_nvm))
        goto CLEAN;

    if (fbe_enqueue_io (cmd_nvm))
        goto CLEAN;

    return 0;

CLEAN:
    log_err("[MMGR Read ERROR: NVM  returned -1]\n");
    fbe_set_prp_map(dma->prp_index, cmd_nvm->ppa.g.ch, 0x0);
    cmd_nvm->status = NVM_IO_FAIL;
    return -1;
}

static int fbe_write_page (struct nvm_mmgr_io_cmd *cmd_nvm)
{
    struct fbe_dma *dma = (struct fbe_dma *) cmd_nvm->rsvd;

    if (fbe_prepare_rw(cmd_nvm))
        goto CLEAN;

    if (fbe_enqueue_io (cmd_nvm))
        goto CLEAN;

    return 0;

CLEAN:
    log_err("[MMGR Write ERROR: DMA or NVM returned -1]\n");
    fbe_set_prp_map(dma->prp_index, cmd_nvm->ppa.g.ch, 0x0);
    cmd_nvm->status = NVM_IO_FAIL;
    return -1;
}

static int fbe_erase_blk (struct nvm_mmgr_io_cmd *cmd)
{
    if(fbe_enqueue_io (cmd))
        goto CLEAN;

    return 0;

CLEAN:
    log_err("[MMGR Erase ERROR: NVM library returned -1]\n");
    cmd->status = NVM_IO_FAIL;
    return -1;
}

static int fbe_set_ch_info (struct nvm_channel *ch, uint16_t nc)
{
    return 0;
}

static int fbe_get_ch_info (struct nvm_channel *ch, uint16_t nc)
{
    int i, n, pl, nsp = 0, trsv;
    struct nvm_ppa_addr *ppa;

    for(i = 0; i < nc; i++){
        ch[i].ch_mmgr_id = i;
        ch[i].mmgr = &fbe_mmgr;
        ch[i].geometry = fbe_mmgr.geometry;

        if (ch[i].i.in_use != NVM_CH_IN_USE) {
            ch[i].i.ns_id = 0x0;
            ch[i].i.ns_part = 0x0;
            ch[i].i.ftl_id = 0x0;
            ch[i].i.in_use = 0x0;
        }

        ch[i].ns_pgs = FILE_VIRTUAL_LUNS *
                       ch->geometry->blk_per_lun *
                       FILE_PLANE_COUNT *
                       ch->geometry->pg_per_blk;

        ch[i].mmgr_rsv = FILE_RSV_BLK;
        trsv = ch[i].mmgr_rsv * FILE_PLANE_COUNT;
        ch[i].mmgr_rsv_list = ox_malloc (trsv * sizeof(struct nvm_ppa_addr),
                                                                  OX_MEM_MMGR);

        if (!ch[i].mmgr_rsv_list)
            return EMEM;

        memset (ch[i].mmgr_rsv_list, 0, trsv * sizeof(struct nvm_ppa_addr));

        for (n = 0; n < ch[i].mmgr_rsv; n++) {
            for (pl = 0; pl < FILE_PLANE_COUNT; pl++) {
                ppa = &ch[i].mmgr_rsv_list[FILE_PLANE_COUNT * n + pl];
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

static int fbe_disk_md_create (char *file_name, uint32_t ch, uint32_t lun)
{
    int file;
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;
    uint32_t blk_i, pl_i, nblks = geo->blk_per_lun;
    uint32_t blk_list[nblks];
    struct nvm_ppa_addr ppa;
    FileBEBlock *fbe_blk;

    memset(blk_list, 0, nblks * sizeof(uint32_t));

    for (blk_i = 0; blk_i < geo->blk_per_lun; blk_i++) {
	ppa.g.ch = ch;
	ppa.g.lun = lun;
	ppa.g.blk = blk_i;
	for (pl_i = 0; pl_i < geo->n_of_planes; pl_i++) {
	    ppa.g.pl = pl_i;
	    fbe_blk = fbe_get_block (ppa);
	    fbe_blk->file_id = 0;
	    fbe_blk->file_offset = 0;
	}
    }

    file = open(file_name, O_WRONLY|O_CREAT|O_SYNC, 0666);
    if (write(file, blk_list, nblks * sizeof(uint32_t)) < 1) {
        close(file);
	return -1;
    }

    close(file);
    return 0;    
}

static int fbe_disk_md_load (int file_md, uint32_t ch, uint32_t lun)
{
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;
    struct nvm_ppa_addr ppa;
    FileBEBlock *fbe_blk;
    uint32_t nblks = geo->blk_per_lun;
    uint32_t blk_list[nblks];
    uint32_t blk_i, pl_i, pg_size, blk_size, tot_size, count = 0;
    char file_name[256] = "";
    int file;

    pg_size = geo->pg_size + (geo->sec_per_pg * geo->sec_oob_sz);
    blk_size = pg_size * geo->pg_per_blk * geo->n_of_planes;
    
    if (read(file_md, blk_list, sizeof(uint32_t) * nblks) < 1)
	return -1;

    /* Check and fix files if needed */
    sprintf(file_name, "%s_%d_%d", fbe_disk, ch, lun);

    printf ("\nch: %d, lun: %d\n", ch, lun);
    for (blk_i = 0; blk_i < geo->blk_per_lun; blk_i++) {
	
	if (blk_list[blk_i])
	    count++;

	if(blk_list[blk_i])
	    printf("  blk: %d/%d\n", blk_i, blk_list[blk_i]);

	ppa.g.ch = ch;
	ppa.g.lun = lun;
	ppa.g.blk = blk_i;
	for (pl_i = 0; pl_i < geo->n_of_planes; pl_i++) {
	    ppa.g.pl = pl_i;
	    fbe_blk = fbe_get_block (ppa);
	    fbe_blk->file_id = blk_list[blk_i];
	    fbe_blk->file_offset = (uint64_t)blk_size * (uint64_t)(blk_list[blk_i] - 1);

	    if(blk_list[blk_i])
		printf ("    blk: %d - file_id: %d, file_off: %lu\n", fbe_blk->id, fbe_blk->file_id, fbe_blk->file_offset);
	}
    }
    printf ("used: %d, total: %d\n", count, geo->blk_per_lun);
    
    if (!count) {
	remove (file_name);
    } else {
	tot_size = blk_size * count;
	file = open(file_name, O_WRONLY|O_SYNC, 0666);
	//if (ftruncate(file, tot_size) < 0) {
	//    close (file);
	//    return -1;
	//}
	close(file);
    }
    
    return 0;
}

static int fbe_init_disk (void)
{
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;
    int file;
    int ch, lun, blk;
    char file_name[256] = "";
    FileBEBlock *fbe_blk;
    struct nvm_ppa_addr ppa;

#ifdef CONFIG_FILE_FOLDER
    strcpy(file_name, fbe_disk);
    if (strcmp(CONFIG_FILE_FOLDER, "."))
	sprintf(fbe_disk, "%s/%s", CONFIG_FILE_FOLDER, file_name);
#endif

    for (ch = 0; ch < geo->n_of_ch; ch++) {
	for (lun = 0; lun < geo->lun_per_ch; lun++){
	    
	    sprintf(file_name, "%s_%d_%d_md", fbe_disk, ch, lun);

	    file = open(file_name, O_RDONLY, 0666);
	    if (file < 0) {
		if (fbe_disk_md_create (file_name, ch, lun))
		    goto WERR;
	    } else {
		if (fbe_disk_md_load (file, ch, lun)) {
		    close(file);
		    goto RERR;
		}
		close(file);
	    }
	}
    }

    printf(" [fbe: Disk is ready.]\n");
    return 0;

WERR:
    remove (fbe_disk);
    printf(" [fbe: Disk creation failed! ch:%d, lun:%d]\n", ch, lun);
    return -1;
RERR:
    printf(" [fbe: Disk loading failed! ch:%d, lun:%d]\n", ch, lun);
    return -1;
}

static void fbe_exit (struct nvm_mmgr *mmgr)
{
    int i;

    fbe_clean_mem();
    fbe->status.active = 0;
    ox_mq_destroy(fbe->mq);
    for (i = 0; i < mmgr->geometry->n_of_ch; i++) {
        pthread_spin_destroy (&prpmap_spin[i]);
        ox_free(mmgr->ch_info[i].mmgr_rsv_list, OX_MEM_MMGR);
        ox_free(mmgr->ch_info[i].ftl_rsv_list, OX_MEM_MMGR);
    }
    ox_free (blk_zeros, OX_MEM_MMGR_FILEBE);
    ox_free (fbe, OX_MEM_MMGR_FILEBE);
}

static void fbe_req_timeout (void **opaque, int counter)
{
    struct nvm_mmgr_io_cmd *cmd;
    struct fbe_dma *dma;

    while (counter) {
        counter--;
        cmd = (struct nvm_mmgr_io_cmd *) opaque[counter];
        dma = (struct fbe_dma *) cmd->rsvd;
        cmd->status = NVM_IO_TIMEOUT;

        /* During request timeout dma->prp_index is freed and
         * dma->virt_addr is redirected to the emergency pointer */
        if (cmd->cmdtype == MMGR_WRITE_PG || cmd->cmdtype == MMGR_READ_PG) {
            dma->virt_addr = fbe->edma;
            fbe_set_prp_map(dma->prp_index, cmd->ppa.g.ch, 0x0);
        }
    }
}

static void fbe_stats_fill_row (struct oxmq_output_row *row, void *opaque)
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

struct ox_mq_config fbe_mq = {
    .name       = "FILEBE",
    .n_queues   = FILE_CHIP_COUNT,
    .q_size     = FILE_QUEUE_SIZE,
    .sq_fn      = fbe_execute_io,
    .cq_fn      = fbe_callback,
    .to_fn      = fbe_req_timeout,
    .output_fn  = fbe_stats_fill_row,
    .to_usec    = 0,
    .flags      = 0x0
};

static int fbe_init(void)
{
    struct nvm_mmgr_geometry *geo = fbe_mmgr.geometry;
    int tot_blk = geo->n_of_planes * geo->blk_per_lun * geo->lun_per_ch *
                                                                   geo->n_of_ch;
    uint32_t blk_size;

    fbe = ox_malloc (sizeof (FileBECtrl), OX_MEM_MMGR_FILEBE);
    if (!fbe)
        return -1;

    blk_size = (geo->pg_size + (geo->sec_per_pg * geo->sec_oob_sz))
		* geo->pg_per_blk * geo->n_of_planes;
    blk_zeros = ox_malloc (blk_size, OX_MEM_MMGR_FILEBE);
    if (!blk_zeros)
	goto OUT;

    memset (blk_zeros, 0, blk_size);

    fbe->status.allocated_memory = 0;

    if (fbe_start_prp_map())
        goto ZEROS;

    if (!fbe_init_blocks())
        goto ZEROS;

    if (!fbe_init_luns()) {
        fbe_free_blocks(tot_blk, 0, 0);
        goto ZEROS;
    }

    if (!fbe_init_channels()) {
        fbe_free_luns(tot_blk);
        goto ZEROS;
    }

    if (fbe_init_dma_buf()) {
        fbe_free_channels(tot_blk);
        goto ZEROS;
    }

    if (fbe_init_disk()) {
        fbe_clean_mem();
        goto ZEROS;
    }

    sprintf(fbe_mq.name, "%s", "FILE-BE_MMGR");
    fbe->mq = ox_mq_init(&fbe_mq);
    if (!fbe->mq) {
        fbe_clean_mem();
        goto ZEROS;
    }

    /* DEBUG: Thread to show multi-queue statistics */
    /*pthread_t debug_th;
    if (pthread_create(&debug_th,NULL,fbe_queue_show,NULL)) {
        ox_mq_destroy (fbe->mq);
        fbe_clean_mem ();
        goto OUT;
    }*/

    fbe->status.ready = 1; /* ready to use */

    log_info(" [fbe: Volatile memory usage: %lu Mb]\n",
                                      fbe->status.allocated_memory / 1048576);
    return 0;

ZEROS:
    ox_free (blk_zeros, OX_MEM_MMGR_FILEBE);
OUT:
    ox_free (fbe, OX_MEM_MMGR_FILEBE);
    printf(" [fbe: Not initialized! Memory allocation failed.]\n");
    printf(" [fbe: Volatile memory usage: %lu bytes.]\n",
                                                fbe->status.allocated_memory);
    return -1;
}

struct nvm_mmgr_ops fbe_ops = {
    .write_pg       = fbe_write_page,
    .read_pg        = fbe_read_page,
    .erase_blk      = fbe_erase_blk,
    .exit           = fbe_exit,
    .get_ch_info    = fbe_get_ch_info,
    .set_ch_info    = fbe_set_ch_info,
};

struct nvm_mmgr_geometry fbe_geo = {
    .n_of_ch        = FILE_CHIP_COUNT,
    .lun_per_ch     = FILE_VIRTUAL_LUNS,
    .blk_per_lun    = FILE_BLOCK_COUNT,
    .pg_per_blk     = FILE_PAGE_COUNT,
    .sec_per_pg     = FILE_SECTOR_COUNT,
    .n_of_planes    = FILE_PLANE_COUNT,
    .pg_size        = FILE_PAGE_SIZE,
    .sec_oob_sz     = FILE_OOB_SIZE / FILE_SECTOR_COUNT,
};

int mmgr_filebe_init (void)
{
    int ret = 0;

    if (!ox_mem_create_type ("MMGR_FILEBE", OX_MEM_MMGR_FILEBE))
        return -1;

#ifdef CONFIG_FILE_GB
    if (CONFIG_FILE_GB != 4 && CONFIG_FILE_GB != 8 && CONFIG_FILE_GB != 16 &&
                               CONFIG_FILE_GB != 32 && CONFIG_FILE_GB != 64 &&
			       CONFIG_FILE_GB != 128 && CONFIG_FILE_GB != 256 &&
			       CONFIG_FILE_GB != 384 && CONFIG_FILE_GB != 512 &&
			       CONFIG_FILE_GB != 768 && CONFIG_FILE_GB != 1024 &&
			       CONFIG_FILE_GB != 1280 && CONFIG_FILE_GB != 1536 &&
			       CONFIG_FILE_GB != 1792 && CONFIG_FILE_GB != 2048 &&
			       CONFIG_FILE_GB != 2560 && CONFIG_FILE_GB != 3072 &&
			       CONFIG_FILE_GB != 3584 && CONFIG_FILE_GB != 4096) {
        printf (" FILE-BE WARNING: FILE_GB options:\n"
		"   4, 8, 16, 32, 64, 128, 256, 384, 512, 768, 1024, 1280, 1536, "
		"   1792, 2048, 2560, 3072, 3584, 4096.\n"
	        " Default: 4 GB\n");
        goto DEFAULT;
    }
    if (CONFIG_FILE_GB > 4)
        fbe_geo.blk_per_lun += 64; // blk_per_lun: 128, pg_per_blk: 64, 4 GB
    if (CONFIG_FILE_GB > 8)
        fbe_geo.pg_per_blk += 64; // blk_per_lun: 128, pg_per_blk: 128, 16 GB
    if (CONFIG_FILE_GB > 16)
        fbe_geo.blk_per_lun += 128; // blk_per_lun: 256, pg_per_blk: 128, 32 GB
    if (CONFIG_FILE_GB > 32)
        fbe_geo.pg_per_blk += 128; // blk_per_lun: 256, pg_per_blk: 256, 64 GB
    if (CONFIG_FILE_GB > 64)
        fbe_geo.blk_per_lun += 256; // blk_per_lun: 512, pg_per_blk: 256, 128 GB
    if (CONFIG_FILE_GB > 128)
        fbe_geo.pg_per_blk += 256; // blk_per_lun: 512, pg_per_blk: 512, 256 GB
    if (CONFIG_FILE_GB > 256)
        fbe_geo.blk_per_lun += 256; // blk_per_lun: 768, pg_per_blk: 512, 384 GB
    if (CONFIG_FILE_GB > 384)
        fbe_geo.blk_per_lun += 256; // blk_per_lun: 1024, pg_per_blk: 512, 512 GB
    if (CONFIG_FILE_GB > 512)
        fbe_geo.pg_per_blk += 256; // blk_per_lun: 1024, pg_per_blk: 768, 768 GB
    if (CONFIG_FILE_GB > 768)
        fbe_geo.pg_per_blk += 256; // blk_per_lun: 1024, pg_per_blk: 1024, 1 TB
    if (CONFIG_FILE_GB > 1024)
        fbe_geo.blk_per_lun += 256; // blk_per_lun: 1280, pg_per_blk: 1024, 1.25 TB
    if (CONFIG_FILE_GB > 1280)
        fbe_geo.blk_per_lun += 256; // blk_per_lun: 1536, pg_per_blk: 1024, 1.5 TB
    if (CONFIG_FILE_GB > 1536)
        fbe_geo.blk_per_lun += 256; // blk_per_lun: 1792, pg_per_blk: 1024, 1.75 TB
    if (CONFIG_FILE_GB > 1792)
        fbe_geo.blk_per_lun += 256; // blk_per_lun: 2048, pg_per_blk: 1024, 2 TB
    if (CONFIG_FILE_GB > 2048)
        fbe_geo.blk_per_lun += 512; // blk_per_lun: 2560, pg_per_blk: 1024, 2.5 TB
    if (CONFIG_FILE_GB > 2560)
        fbe_geo.blk_per_lun += 512; // blk_per_lun: 3072, pg_per_blk: 1024, 3 TB
    if (CONFIG_FILE_GB > 3072)
        fbe_geo.blk_per_lun += 512; // blk_per_lun: 3584, pg_per_blk: 1024, 3.5 TB
    if (CONFIG_FILE_GB > 3584)
        fbe_geo.blk_per_lun += 512; // blk_per_lun: 4096, pg_per_blk: 1024, 4 TB
#endif /* CONFIG_FILE_GB */

DEFAULT:
    fbe_mmgr.name     = "FILE_BE";
    fbe_mmgr.ops      = &fbe_ops;
    fbe_mmgr.geometry = &fbe_geo;
    if (CONFIG_FILE_GB <= 64) {
	fbe_mmgr.flags = MMGR_FLAG_MIN_CP_TIME;
    } else { 
	fbe_mmgr.flags = 0;
    }
    
    ret = fbe_init();
    if (ret) {
	log_err(" [filebe: Not possible to start Media Manager.]\n");
	return EMMGR_REGISTER;
    }

    return ox_register_mmgr(&fbe_mmgr);
}
