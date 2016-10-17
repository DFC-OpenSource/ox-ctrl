/* OX: OpenChannel NVM Express SSD Controller
 *
 * Copyright (C) 2016, IT University of Copenhagen. All rights reserved.
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 *
 * Funding support provided by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#include "../../include/ssd.h"

#include <stdio.h>
#include <syslog.h>
#include <time.h>
#include <pthread.h>
#include "dfc_nand.h"
#include "nand_dma.h"
#include "../../include/uatomic.h"

static atomic_t        nextprp;
static pthread_mutex_t prp_mutex;
static pthread_mutex_t prpmap_mutex;
static uint8_t         *prp_map;
struct nvm_mmgr dfcnand;

static uint16_t dfcnand_vir_to_phy_lun (uint16_t vir){
    uint16_t lunc = NAND_LUN_COUNT;
    uint16_t tgt, lun = 0;

    if (vir == 0)
        return 0;

    tgt = vir / lunc;
    lun = vir % lunc;

    return tgt << 8 | lun;
}

static int dfcnand_start_prp_map()
{
    nextprp.counter = ATOMIC_INIT_RUNTIME(0);
    pthread_mutex_init (&prp_mutex, NULL);
    pthread_mutex_init (&prpmap_mutex, NULL);
    prp_map = calloc(sizeof(uint8_t), 2048);
    if (nvm_memcheck(prp_map))
        return EMEM;
    return 0;
}

static void dfcnand_set_prp_map(uint32_t index, uint8_t flag)
{
    pthread_mutex_lock(&prpmap_mutex);
    prp_map[index / 8] = (flag)
            ? prp_map[index / 8] | 1 << (index % 8)
            : prp_map[index / 8] ^ 1 << (index % 8);
    pthread_mutex_unlock(&prpmap_mutex);
}

static uint32_t dfcnand_get_next_prp(io_cmd *cmd){
    uint16_t ni = 8;
    uint16_t noff = 1;
    uint32_t next;
    pthread_mutex_lock(&prp_mutex);
    do {
        next = atomic_read(&nextprp);
        next = (next >= ni*noff) ? 1 : ++next;
        atomic_set(&nextprp, next);
        next--;
        if (prp_map[next / 8] & 1 << (next % 8)) {
            usleep(1);
            continue;
        }
        pthread_mutex_unlock (&prp_mutex);

        dfcnand_set_prp_map(next, 0x1);

        cmd->dfc_io.prp_index = next;

        /* vector index (16 bits) + offset index (16 bits) */
        return (next / noff)+1 << 16 | (next % noff);
    } while (1);
}

static int dfcnand_dma_helper (io_cmd *cmd)
{
    uint32_t dma_sz;
    int dma_sec, c = 0, ret = 0;
    uint64_t prp;
    uint8_t direction;
    struct nvm_mmgr_io_cmd *nvm_cmd =
                           (struct nvm_mmgr_io_cmd *) cmd->dfc_io.nvm_mmgr_io;

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

    dma_sec = nvm_cmd->n_sectors + 1;
    for (; c < dma_sec; c++) {
        dma_sz = (c == dma_sec - 1) ? nvm_cmd->md_sz : nvm_cmd->sec_sz;
        prp = (c == dma_sec - 1) ? nvm_cmd->md_prp : nvm_cmd->prp[c];

        ret = nvm_dma ((void *)(cmd->dfc_io.virt_addr +
                                nvm_cmd->sec_sz * c), prp, dma_sz, direction);
        if (ret) break;
    }

    return ret;
}

int dfcnand_read_page (struct nvm_mmgr_io_cmd *cmd_nvm)
{
    io_cmd *cmd = (struct io_cmd *) cmd_nvm->fpga_io;
    int c;

    uint32_t sec_sz = NAND_SECTOR_SIZE;
    uint32_t pg_sz  = NAND_PAGE_SIZE;
    uint32_t oob_sz = NAND_OOB_SIZE;

    /* For now we only accept up to 16K page size and 4K sector size + 1K OOB */
    if (cmd_nvm->pg_sz > pg_sz || cmd_nvm->sec_sz != sec_sz ||
                                                    cmd_nvm->md_sz > oob_sz)
        return -1;

    memset(cmd, 0, sizeof(cmd));

    uint16_t phytl = dfcnand_vir_to_phy_lun(cmd_nvm->ppa.g.lun);
    uint32_t prp_map = dfcnand_get_next_prp(cmd);
    uint16_t vec_index = prp_map >> 16;
    uint32_t offset = (prp_map & 0x0000ffff) * sec_sz * 5;

    cmd->dfc_io.virt_addr = virt_addr[vec_index] + offset;
    memset(cmd->dfc_io.virt_addr, 0, pg_sz + oob_sz);

    cmd->dfc_io.nvm_mmgr_io = cmd_nvm;

    /*Page Read -16K + 1K OOB*/
    cmd->lun = phytl & 0x00ff;
    cmd->chip = cmd_nvm->ppa.g.ch;
    cmd->target = phytl >> 8;
    cmd->block = cmd_nvm->ppa.g.blk * 2 + cmd_nvm->ppa.g.pl;
    cmd->page = cmd_nvm->ppa.g.pg;
    cmd->dfc_io.cmd_type = MMGR_READ_PG;

    cmd_nvm->n_sectors = cmd_nvm->pg_sz / sec_sz;

    for (c = 0; c < 5; c++) {
        cmd->len[c] = sec_sz;
        cmd->host_addr[c] = (uint64_t) phy_addr[vec_index]
                                                      + offset + sec_sz * c;
      }
    cmd->col_addr = 0x0;
    cmd->len[4] = cmd_nvm->md_sz;

    if (nand_page_read(cmd))
        goto CLEAN;

    return 0;

CLEAN:
    log_err("[MMGR Read ERROR: FPGA library returned -1]\n");
    dfcnand_set_prp_map(cmd->dfc_io.prp_index, 0x0);
    cmd_nvm->status = NVM_IO_FAIL;
    return -1;
}

int dfcnand_write_page (struct nvm_mmgr_io_cmd *cmd_nvm)
{
    int c;
    io_cmd *cmd = (struct io_cmd *) cmd_nvm->fpga_io;

    uint32_t sec_sz = NAND_SECTOR_SIZE;
    uint32_t pg_sz  = NAND_PAGE_SIZE;
    uint32_t oob_sz = NAND_OOB_SIZE;

    /* For now we only accept up to 16K page size + 1K OOB */
    if (cmd_nvm->pg_sz > pg_sz || cmd_nvm->sec_sz != sec_sz ||
                                                     cmd_nvm->md_sz > oob_sz)
        return -1;

    memset(cmd, 0, sizeof(cmd));

    uint16_t phytl = dfcnand_vir_to_phy_lun(cmd_nvm->ppa.g.lun);
    uint32_t prp_map = dfcnand_get_next_prp(cmd);
    uint16_t vec_index = prp_map >> 16;
    uint32_t offset = (prp_map & 0x0000ffff) * sec_sz * 5;

    cmd->dfc_io.nvm_mmgr_io = cmd_nvm;

    /*Page Write -16K + 1K OOB*/
    cmd->dfc_io.virt_addr = virt_addr[vec_index] + offset;

    cmd->lun = phytl & 0x00ff;
    cmd->chip = cmd_nvm->ppa.g.ch;
    cmd->target = phytl >> 8;
    cmd->block = cmd_nvm->ppa.g.blk * 2 + cmd_nvm->ppa.g.pl;
    cmd->page = cmd_nvm->ppa.g.pg;
    cmd->dfc_io.cmd_type = MMGR_WRITE_PG;

    cmd_nvm->n_sectors = cmd_nvm->pg_sz / sec_sz;

    if (dfcnand_dma_helper (cmd))
        goto CLEAN;

    for (c = 0; c < 5; c++) {
        cmd->len[c] = sec_sz;
        cmd->host_addr[c] = (uint64_t)phy_addr[vec_index] + offset + sec_sz * c;
    }
    cmd->len[4] = cmd_nvm->md_sz;

    if (nand_page_prog(cmd))
        goto CLEAN;

    return 0;

CLEAN:
    log_err("[MMGR Write ERROR: DMA or FPGA library returned -1]\n");
    dfcnand_set_prp_map(cmd->dfc_io.prp_index, 0x0);
    cmd_nvm->status = NVM_IO_FAIL;
    return -1;
}

int dfcnand_erase_blk (struct nvm_mmgr_io_cmd *cmd_nvm)
{
    io_cmd *cmd = (struct io_cmd *) cmd_nvm->fpga_io;
    uint16_t phytl = dfcnand_vir_to_phy_lun(cmd_nvm->ppa.g.lun);
    memset(cmd, 0, sizeof(cmd));
    cmd->dfc_io.nvm_mmgr_io = cmd_nvm;
    cmd->chip = cmd_nvm->ppa.g.ch;
    cmd->target = phytl >> 8;
    cmd->lun = phytl & 0x00ff;
    cmd->block = cmd_nvm->ppa.g.blk * 2 + cmd_nvm->ppa.g.pl;
    cmd->host_addr[0] = 0;
    cmd->len[0] = 0;
    cmd->dfc_io.cmd_type = MMGR_ERASE_BLK;

    if (nand_block_erase(cmd))
        goto CLEAN;

    return 0;

CLEAN:
    log_err("[MMGR Erase ERROR: FPGA library returned -1]\n");
    cmd_nvm->status = NVM_IO_FAIL;
    return -1;
}

void dfcnand_exit (struct nvm_mmgr *mmgr)
{
    int i;
    nand_dm_deinit();
    pthread_mutex_destroy(&prp_mutex);
    pthread_mutex_destroy(&prpmap_mutex);
    free(prp_map);
    for (i = 0; i < mmgr->geometry->n_of_ch; i++) {
        free(mmgr->ch_info->mmgr_rsv_list);
        free(mmgr->ch_info->ftl_rsv_list);
    }
}

static int dfcnand_io_rsv_blk (struct nvm_channel *ch, uint8_t cmdtype,
                                                   void **buf_vec, uint16_t pg)
{
    int ret, pl;
    void *buf = NULL;
    struct nvm_mmgr_io_cmd *cmd = malloc(sizeof(struct nvm_mmgr_io_cmd));
    if (!cmd)
        return EMEM;

    for (pl = 0; pl < ch->geometry->n_of_planes; pl++) {
        memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
        cmd->ppa.g.blk = DFCNAND_RESV_BLK;
        cmd->ppa.g.pl = pl;
        cmd->ppa.g.ch = ch->ch_mmgr_id;
        cmd->ppa.g.lun = 0;
        cmd->ppa.g.pg = pg;

        if (cmdtype != MMGR_ERASE_BLK)
            buf = buf_vec[pl];

        ret = nvm_submit_sync_io (ch, cmd, buf, cmdtype);
        if (ret)
            break;
    }
    free(cmd);

    return ret;
}

int dfcnand_read_nvminfo (struct nvm_channel *ch)
{
    int ret, pg, i;
    struct nvm_channel ch_a;
    void *buf_vec[ch->geometry->n_of_planes];
    void *buf;
    uint16_t buf_sz = ch->geometry->pg_size + ch->geometry->sec_oob_sz
                                                    * ch->geometry->sec_per_pg;

    buf = calloc(buf_sz * ch->geometry->n_of_planes, 1);
    if (!buf)
        return EMEM;

    for (i = 0; i < ch->geometry->n_of_planes; i++) {
        buf_vec[i] = buf + i * buf_sz;
    }

    pg = 0;
    do {
        memset (buf, 0, buf_sz * ch->geometry->n_of_planes);
        ret = dfcnand_io_rsv_blk (ch, MMGR_READ_PG, buf_vec, pg);
        memcpy (&ch_a.nvm_info, buf, sizeof (ch_a.nvm_info));

        if (ret || ch_a.i.in_use != NVM_CH_IN_USE)
            break;

        memcpy (&ch->nvm_info, &ch_a.nvm_info, sizeof (ch_a.nvm_info));
        pg++;
    } while (pg < ch->geometry->pg_per_blk);

    if (ret)
        goto OUT;

    if (!pg)
        ret = dfcnand_io_rsv_blk (ch, MMGR_ERASE_BLK, NULL, 0);

OUT:
    free(buf);
    return ret;
}

static int dfcnand_flush_nvminfo (struct nvm_channel *ch)
{
    int ret, pg, i;
    struct nvm_channel ch_a;
    void *buf_vec[ch->geometry->n_of_planes];
    void *buf;
    uint16_t buf_sz = ch->geometry->pg_size + ch->geometry->sec_oob_sz
                                                    * ch->geometry->sec_per_pg;

    buf = calloc(buf_sz * ch->geometry->n_of_planes, 1);
    if (!buf)
        return EMEM;

    for (i = 0; i < ch->geometry->n_of_planes; i++) {
        buf_vec[i] = buf + i * buf_sz;
    }

    pg = 0;
    do {
        memset (buf, 0, buf_sz * ch->geometry->n_of_planes);
        ret = dfcnand_io_rsv_blk (ch, MMGR_READ_PG, buf_vec, pg);
        memcpy (&ch_a.nvm_info, buf, sizeof (ch_a.nvm_info));

        if (ret || ch_a.i.in_use != NVM_CH_IN_USE)
            break;

        pg++;
    } while (pg < ch->geometry->pg_per_blk);

    if (ret)
        goto OUT;

    if (pg == ch->geometry->pg_per_blk) {
        if (dfcnand_io_rsv_blk (ch, MMGR_ERASE_BLK, NULL, 0))
            goto OUT;
        pg = 0;
    }

    memset (buf, 0, buf_sz * ch->geometry->n_of_planes);
    memcpy (buf, &ch->nvm_info, sizeof (ch->nvm_info));

    ret = dfcnand_io_rsv_blk (ch, MMGR_WRITE_PG, buf_vec, pg);

OUT:
    free(buf);
    return ret;
}

int dfcnand_set_ch_info (struct nvm_channel *ch, uint16_t nc)
{
    int i;

    for(i = 0; i < nc; i++){
        if(dfcnand_flush_nvminfo (&ch[i]))
            return -1;
    }

    return 0;
}

int dfcnand_get_ch_info (struct nvm_channel *ch, uint16_t nc)
{
    int i, n, nsp = 0;

    for(i = 0; i < nc; i++){
        ch[i].ch_mmgr_id = i;
        ch[i].mmgr = &dfcnand;
        ch[i].geometry = dfcnand.geometry;

        if (dfcnand_read_nvminfo (&ch[i]))
            return -1;

        if (ch[i].i.in_use != NVM_CH_IN_USE) {
            ch[i].i.ns_id = 0x0;
            ch[i].i.ns_part = 0x0;
            ch[i].i.ftl_id = 0x0;
            ch[i].i.in_use = 0x0;
        }

        ch[i].ns_pgs = NAND_VIRTUAL_LUNS *
                       NAND_BLOCK_COUNT *
                       NAND_PLANE_COUNT *
                       NAND_PAGE_COUNT;

        ch[i].mmgr_rsv = 3;
        ch[i].mmgr_rsv_list = malloc (ch[i].mmgr_rsv *
                                                sizeof(struct nvm_ppa_addr));
        if (!ch[i].mmgr_rsv_list)
            return EMEM;

        for (n = 0; n < ch[i].mmgr_rsv; n++)
            ch[i].mmgr_rsv_list[n].ppa = (uint64_t) (n & 0xffffffffffffffff);

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

int dfcnand_callback(io_cmd *cmd)
{
    int ret;
    struct nvm_mmgr_io_cmd *nvm_cmd =
                           (struct nvm_mmgr_io_cmd *) cmd->dfc_io.nvm_mmgr_io;

    if (nvm_memcheck(nvm_cmd) || nvm_cmd->status == NVM_IO_TIMEOUT)
        goto OUT;

    if (cmd->status) {
        if (cmd->dfc_io.cmd_type == MMGR_READ_PG)
            ret = dfcnand_dma_helper (cmd);

        nvm_cmd->status = (ret) ? NVM_IO_FAIL : NVM_IO_SUCCESS;
    } else {
        nvm_cmd->status = NVM_IO_FAIL;
    }

OUT:
    if (cmd->dfc_io.cmd_type == MMGR_WRITE_PG || cmd->dfc_io.cmd_type ==
                                                                  MMGR_READ_PG)
        dfcnand_set_prp_map(cmd->dfc_io.prp_index, 0x0);

    nvm_callback(nvm_cmd);

    return 0;
}

struct nvm_mmgr_ops dfcnand_ops = {
    .write_pg       = dfcnand_write_page,
    .read_pg        = dfcnand_read_page,
    .erase_blk      = dfcnand_erase_blk,
    .exit           = dfcnand_exit,
    .get_ch_info    = dfcnand_get_ch_info,
    .set_ch_info    = dfcnand_set_ch_info,
};

struct nvm_mmgr_geometry dfcnand_geo = {
    .n_of_ch        = NAND_CHIP_COUNT,
    .lun_per_ch     = NAND_VIRTUAL_LUNS,
    .blk_per_lun    = NAND_BLOCK_COUNT,
    .pg_per_blk     = NAND_PAGE_COUNT,
    .sec_per_pg     = NAND_SECTOR_COUNT,
    .n_of_planes    = NAND_PLANE_COUNT,
    .pg_size        = NAND_PAGE_SIZE,
    .sec_oob_sz     = NAND_OOB_SIZE / NAND_SECTOR_COUNT
};

int mmgr_dfcnand_init()
{
    int ret;

    dfcnand.name     = "DFC_NAND";
    dfcnand.ops      = &dfcnand_ops;
    dfcnand.geometry = &dfcnand_geo;

    ret = nand_dm_init();
    if(ret) {
        syslog(LOG_ERR, "dfcnand: Not possible to start NAND manager.");
        return -1;
    }
    ret = dfcnand_start_prp_map();
    if (ret) return ret;

    return nvm_register_mmgr(&dfcnand);
}