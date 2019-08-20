/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - Common Flash Translation Layer functions
 *
 * Copyright 2018 IT University of Copenhagen
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

#include <stdlib.h>
#include <string.h>
#include <libox.h>

void ftl_pg_io_prepare (struct nvm_channel *ch, struct nvm_io_data *data)
{
    uint16_t sec, pl;
    struct nvm_mmgr_geometry *g = ch->geometry;

    for (pl = 0; pl < data->n_pl; pl++) {
        data->pl_vec[pl] = data->buf +
                      ((uint32_t) pl * (data->buf_sz / (uint32_t) data->n_pl));

        for (sec = 0; sec < g->sec_per_pg; sec++) {
            data->oob_vec[g->sec_per_pg * pl + sec] =
                        data->pl_vec[pl] + data->pg_sz + (g->sec_oob_sz * sec);
            data->sec_vec[pl][sec] = data->pl_vec[pl] + (g->sec_size * sec);
        }

        data->sec_vec[pl][g->sec_per_pg] = data->oob_vec[g->sec_per_pg * pl];
    }
}

void ftl_pl_pg_io_prepare (struct nvm_channel *ch, struct nvm_io_data *data)
{
    uint16_t sec, pl;
    struct nvm_mmgr_geometry *g = ch->geometry;

    uint8_t *oob = data->buf + (data->pg_sz * data->n_pl);

    for (pl = 0; pl < data->n_pl; pl++) {
        data->pl_vec[pl] = data->buf + ((uint32_t) pl * data->pg_sz);

        for (sec = 0; sec < g->sec_per_pg; sec++) {
            data->oob_vec[g->sec_per_pg * pl + sec] = oob +
                                (g->pg_oob_sz * pl) + (g->sec_oob_sz * sec);
            data->sec_vec[pl][sec] = data->pl_vec[pl] + (g->sec_size * sec);
        }

        data->sec_vec[pl][g->sec_per_pg] = data->oob_vec[g->sec_per_pg * pl];
    }
}

struct nvm_io_data *ftl_alloc_pg_io (struct nvm_channel *ch)
{
    struct nvm_io_data *data;
    struct nvm_mmgr_geometry *g;
    uint16_t pl;

    data = ox_malloc (sizeof (struct nvm_io_data), OX_MEM_FTL);
    if (!data)
        return NULL;

    data->ch = ch;
    g = data->ch->geometry;
    data->n_pl = g->n_of_planes;
    data->pg_sz = g->pg_size;
    data->meta_sz = g->sec_oob_sz * g->sec_per_pg;
    data->buf_sz = (data->pg_sz + data->meta_sz) * data->n_pl;

    data->buf = ox_calloc(data->buf_sz, 1, OX_MEM_FTL);
    if (!data->buf)
        goto FREE;

    data->pl_vec = ox_malloc (sizeof (uint8_t *) * data->n_pl, OX_MEM_FTL);
    if (!data->pl_vec)
        goto FREE_BUF;

    data->oob_vec = ox_malloc (sizeof (uint8_t *) * g->sec_per_pl_pg, OX_MEM_FTL);
    if (!data->oob_vec)
        goto FREE_VEC;

    data->sec_vec = ox_malloc (sizeof (void *) * data->n_pl, OX_MEM_FTL);
    if (!data->sec_vec)
        goto FREE_OOB;

    for (pl = 0; pl < data->n_pl; pl++) {
        data->sec_vec[pl] = ox_malloc (sizeof (uint8_t *) * (g->sec_per_pg + 1),
                                                                    OX_MEM_FTL);
        if (!data->sec_vec[pl])
            goto FREE_SEC;
    }

    data->mod_oob = ox_malloc (data->meta_sz * data->n_pl, OX_MEM_FTL);
    if (!data->mod_oob)
        goto FREE_SEC;

    if (ch->mmgr->flags & MMGR_FLAG_PL_CMD)
        ftl_pl_pg_io_prepare (ch, data);
    else
        ftl_pg_io_prepare (ch, data);

    return data;

FREE_SEC:
    while (pl) {
        pl--;
        ox_free (data->sec_vec[pl], OX_MEM_FTL);
    }
    ox_free (data->sec_vec, OX_MEM_FTL);
FREE_OOB:
    ox_free (data->oob_vec, OX_MEM_FTL);
FREE_VEC:
    ox_free (data->pl_vec, OX_MEM_FTL);
FREE_BUF:
    ox_free (data->buf, OX_MEM_FTL);
FREE:
    ox_free (data, OX_MEM_FTL);
    return NULL;
}

void ftl_free_pg_io (struct nvm_io_data *data)
{
    uint32_t pl;

    for (pl = 0; pl < data->n_pl; pl++)
        ox_free (data->sec_vec[pl], OX_MEM_FTL);

    ox_free (data->sec_vec, OX_MEM_FTL);
    ox_free (data->mod_oob, OX_MEM_FTL);
    ox_free (data->oob_vec, OX_MEM_FTL);
    ox_free (data->pl_vec, OX_MEM_FTL);
    ox_free (data->buf, OX_MEM_FTL);
    ox_free (data, OX_MEM_FTL);
}

int ftl_blk_current_page (struct nvm_channel *ch, struct nvm_io_data *io,
                                              uint16_t blk_id, uint16_t offset)
{
    int pg, ret, fr = 0;
    uint8_t oob[ch->geometry->pg_oob_sz];

    if (io == NULL) {
        io = ftl_alloc_pg_io (ch);
        if (io == NULL)
            return -1;
        fr++;
    }

    /* Finds the location of the newest data by checking NVM_MAGIC */
    pg = 0;
    do {
        memset (io->buf, 0, io->buf_sz);

        if (ch->mmgr->flags & MMGR_FLAG_PL_CMD) {
            ret = ftl_io_rsv_pl_blk (ch, MMGR_READ_PL_PG,
                                                (void *) io->buf, blk_id, pg);
        } else {
            ret = ftl_io_rsv_blk (ch, MMGR_READ_PG,
                                             (void **) io->pl_vec, blk_id, pg);
        }

        if (ret || (
                ((struct nvm_magic *) io->oob_vec[0])->magic != NVM_MAGIC ) )
            break;

        /* get info from OOB area of first plane */
        memcpy(&oob, io->oob_vec[0], ch->geometry->pg_oob_sz);

        pg += offset;
    } while (pg < io->ch->geometry->pg_per_blk - offset);

    if (fr)
        ftl_free_pg_io (io);
    else
        memcpy (io->oob_vec[0], &oob, ch->geometry->pg_oob_sz);

    return (!ret) ? pg : -1;
}

int ftl_pg_io_switch (struct nvm_channel *ch, uint8_t cmdtype,
                    void **pl_vec, struct nvm_ppa_addr *ppa, uint8_t type)
{
    if (cmdtype == MMGR_READ_PG && (ch->mmgr->flags & MMGR_FLAG_PL_CMD))
        cmdtype = MMGR_READ_PL_PG;

    if (cmdtype == MMGR_WRITE_PG && (ch->mmgr->flags & MMGR_FLAG_PL_CMD))
        cmdtype = MMGR_WRITE_PL_PG;

    switch (type) {
        case NVM_IO_NORMAL:
            if (ch->mmgr->flags & MMGR_FLAG_PL_CMD)
                return ftl_pl_pg_io (ch, cmdtype, pl_vec[0], ppa);
            else
                return ftl_pg_io (ch, cmdtype, pl_vec, ppa);
        case NVM_IO_RESERVED:
            if (ch->mmgr->flags & MMGR_FLAG_PL_CMD)
                return ftl_io_rsv_pl_blk (ch, cmdtype,
                                             pl_vec[0], ppa->g.blk, ppa->g.pg);
            else
                return ftl_io_rsv_blk (ch, cmdtype,
                                             pl_vec, ppa->g.blk, ppa->g.pg);
                
        default:
            return -1;
    }
}

/**
 *  Transfers a table of user specific entries to/from a NVM unique block
 *  This function mount/unmount the multi-plane I/O buffers to a flat table
 *
 *  The table can cross multiple pages in the block
 *  For now, the maximum table size is the flash block
 *
 * @param io - struct created by alloc_alloc_pg_io
 * @param ppa - lun, block and first page number
 * @param user_buf - table buffer to be transfered
 * @param pgs - number of flash pages the table crosses (multi-plane pages)
 * @param ent_per_pg - number of entries per flash page (multi-plane pages)
 * @param ent_left - number of entries to be transfered
 * @param entry_sz - size of an entry
 * @param direction - NVM_TRANS_FROM_NVM or NVM_TRANS_TO_NVM
 * @param reserved - NVM_IO_NORMAL or NVM_IO_RESERVED
 * @return 0 on success, -1 on failure
 */
int ftl_nvm_seq_transfer (struct nvm_io_data *io, struct nvm_ppa_addr *ppa,
        uint8_t *user_buf, uint16_t pgs, uint16_t ent_per_pg, uint32_t ent_left,
        size_t entry_sz, uint8_t direction, uint8_t reserved)
{
    uint32_t i, pl, start_pg;
    size_t trf_sz, pg_ent_sz;
    uint8_t *from, *to;
    struct nvm_ppa_addr ppa_io;

    pg_ent_sz = ent_per_pg * entry_sz;
    memcpy (&ppa_io, ppa, sizeof(struct nvm_ppa_addr));
    start_pg = ppa_io.g.pg;

    /* Transfer page by page from/to NVM */
    for (i = 0; i < pgs; i++) {
        ppa_io.g.pg = start_pg + i;
        if (direction == NVM_TRANS_FROM_NVM)
            if (ftl_pg_io_switch (io->ch, MMGR_READ_PG, (void **)io->pl_vec,
                                                            &ppa_io, reserved))
                    return -1;

        /* Copy page entries from/to I/O buffer */
        for (pl = 0; pl < io->n_pl; pl++) {

            trf_sz = (ent_left >= ent_per_pg / io->n_pl) ?
                    (ent_per_pg / io->n_pl) * entry_sz : ent_left * entry_sz;

            from = (direction == NVM_TRANS_TO_NVM) ?
                user_buf + (pg_ent_sz * i) + (pl * (pg_ent_sz / io->n_pl)) :
                io->pl_vec[pl];

            to = (direction == NVM_TRANS_TO_NVM) ?
                io->pl_vec[pl] :
                user_buf + (pg_ent_sz * i) + (pl * (pg_ent_sz / io->n_pl));

            memcpy(to, from, trf_sz);

            ent_left = (ent_left >= ent_per_pg / io->n_pl) ?
                ent_left - (ent_per_pg / io->n_pl) : 0;

            if (!ent_left)
                break;
        }

        if (direction == NVM_TRANS_TO_NVM)
            if (ftl_pg_io_switch (io->ch, MMGR_WRITE_PG,
                                      (void **) io->pl_vec, &ppa_io, reserved))
                    return -1;
    }

    return 0;
}

int ftl_pg_io (struct nvm_channel *ch, uint8_t cmdtype,
                                      void **pl_vec, struct nvm_ppa_addr *ppa)
{
    int pl, ret = -1;
    void *buf = NULL;
    struct nvm_mmgr_io_cmd *cmd = ox_malloc(sizeof(struct nvm_mmgr_io_cmd),
                                                                    OX_MEM_FTL);
    if (!cmd)
        return EMEM;

    for (pl = 0; pl < ch->geometry->n_of_planes; pl++) {
        memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
        cmd->ppa.g.blk = ppa->g.blk;
        cmd->ppa.g.pl = pl;
        cmd->ppa.g.ch = ch->ch_mmgr_id;
        cmd->ppa.g.lun = ppa->g.lun;
        cmd->ppa.g.pg = ppa->g.pg;

        if (cmdtype != MMGR_ERASE_BLK)
            buf = pl_vec[pl];

        ret = ox_submit_sync_io (ch, cmd, buf, cmdtype);
        if (ret)
            break;
    }
    ox_free (cmd, OX_MEM_FTL);

    return ret;
}

int ftl_pl_pg_io (struct nvm_channel *ch, uint8_t cmdtype,
                                      void *buf, struct nvm_ppa_addr *ppa)
{
    int ret = -1;
    struct nvm_mmgr_io_cmd *cmd = ox_malloc(sizeof(struct nvm_mmgr_io_cmd),
                                                                    OX_MEM_FTL);
    if (!cmd)
        return EMEM;

    memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
    cmd->ppa.g.blk = ppa->g.blk;
    cmd->ppa.g.ch = ch->ch_mmgr_id;
    cmd->ppa.g.lun = ppa->g.lun;
    cmd->ppa.g.pg = ppa->g.pg;

    ret = ox_submit_sync_io (ch, cmd, buf, cmdtype);

    ox_free (cmd, OX_MEM_FTL);

    return ret;
}

int ftl_io_rsv_blk (struct nvm_channel *ch, uint8_t cmdtype,
                                     void **pl_vec, uint16_t blk, uint16_t pg)
{
    int pl, ret = -1;
    void *buf = NULL;
    struct nvm_mmgr_io_cmd *cmd = ox_malloc(sizeof(struct nvm_mmgr_io_cmd),
                                                                   OX_MEM_FTL);
    if (!cmd)
        return EMEM;

    for (pl = 0; pl < ch->geometry->n_of_planes; pl++) {
        memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
        cmd->ppa.g.blk = blk;
        cmd->ppa.g.pl = pl;
        cmd->ppa.g.ch = ch->ch_mmgr_id;

        /* TODO: Replicate among all LUNs in the channel */
        cmd->ppa.g.lun = 0;

        cmd->ppa.g.pg = pg;

        if (cmdtype != MMGR_ERASE_BLK)
            buf = pl_vec[pl];

        ret = ox_submit_sync_io (ch, cmd, buf, cmdtype);
        if (ret)
            break;
    }
    ox_free (cmd, OX_MEM_FTL);

    return ret;
}

int ftl_io_rsv_pl_blk (struct nvm_channel *ch, uint8_t cmdtype,
                                        void *buf, uint16_t blk, uint16_t pg)
{
    int ret = -1;
    struct nvm_mmgr_io_cmd *cmd = ox_malloc(sizeof(struct nvm_mmgr_io_cmd),
                                                                    OX_MEM_FTL);
    if (!cmd)
        return EMEM;

    memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
    cmd->ppa.g.blk = blk;
    cmd->ppa.g.ch = ch->ch_mmgr_id;

    /* TODO: Replicate among all LUNs in the channel */
    cmd->ppa.g.lun = 0;

    cmd->ppa.g.pg = pg;

    ret = ox_submit_sync_io (ch, cmd, buf, cmdtype);

    ox_free (cmd, OX_MEM_FTL);

    return ret;
}