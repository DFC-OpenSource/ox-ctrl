/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX-App Flash Translation Layer (Physical Page Address I/O)
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

#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/queue.h>
#include <libox.h>
#include <ox-app.h>

extern struct core_struct core;

static int ppa_io_submit (struct nvm_io_cmd *);

static void ppa_io_set_pgmap(uint8_t *pgmap, uint8_t index, uint8_t flag,
                                                        pthread_mutex_t *mutex)
{
    pthread_mutex_lock (mutex);
    pgmap[index / 8] = (flag)
            ? pgmap[index / 8] | (1 << (index % 8))
            : pgmap[index / 8] ^ (1 << (index % 8));
    pthread_mutex_unlock (mutex);
}

static int ppa_io_check_pgmap_complete (uint8_t *pgmap, uint8_t ni) {
    int sum = 0, i;
    for (i = 0; i < ni; i++)
        sum += pgmap[i];
    return sum;
}

static inline int ppa_io_pg_read (struct nvm_mmgr_io_cmd *cmd)
{
    cmd->callback.cb_fn = NULL;
    return ox_submit_mmgr (cmd);
}

static inline int ppa_io_pg_write (struct nvm_mmgr_io_cmd *cmd)
{
    cmd->callback.cb_fn = NULL;
    return ox_submit_mmgr (cmd);
}

static inline int ppa_io_erase_blk (struct nvm_mmgr_io_cmd *cmd)
{
    cmd->callback.cb_fn = NULL;
    return ox_submit_mmgr (cmd);
}

/* If all pages were processed, it checks for errors. If all succeed, finish
 * cmd. Otherwise, retry only pages with error.
 *
 * Calls to this fn come from submit_io or from end_pg_io
 */
static int ppa_io_check_end (struct nvm_io_cmd *cmd)
{
    if (cmd->status.pgs_p == cmd->status.total_pgs) {

        pthread_mutex_lock (&cmd->mutex);
        /* if true, some pages failed */
        if ( ppa_io_check_pgmap_complete (cmd->status.pg_map,
                                      ((cmd->status.total_pgs - 1) / 8) + 1)) {

            cmd->status.ret_t++;
            if (cmd->status.ret_t <= APP_IO_RETRY) {
                log_err ("[FTL WARNING: Cmd resubmitted due failed pages]\n");
                goto SUBMIT;
            } else {
                log_err ("[FTL WARNING: Completing FAILED command]\n");
                cmd->status.status = NVM_IO_FAIL;
                cmd->status.nvme_status = NVME_DATA_TRAS_ERROR;
                goto COMPLETE;
            }

        } else {
            if (cmd->status.status == NVM_IO_PROCESS) {
                cmd->status.status = NVM_IO_SUCCESS;
                cmd->status.nvme_status = NVME_SUCCESS;
                goto COMPLETE;
            } else {
                goto RETURN;
            }
        }
    }
    return 0;

SUBMIT:
    pthread_mutex_unlock (&cmd->mutex);
    ppa_io_submit (cmd);
    return 0;;

COMPLETE:
    pthread_mutex_unlock (&cmd->mutex);
    cmd->callback.cb_fn (cmd->callback.opaque);
    return 0;
RETURN:
    pthread_mutex_unlock (&cmd->mutex);
    return 0;
}

static int ppa_io_check_pg (struct nvm_io_cmd *cmd, uint8_t index)
{
    int c;
    struct nvm_ppa_addr *ppa;
    struct nvm_mmgr_geometry *g;
    struct nvm_mmgr_io_cmd *mio = &cmd->mmgr_io[index];

    mio->nvm_io = cmd;

    if (cmd->cmdtype == MMGR_ERASE_BLK) {
        mio->ppa = cmd->ppalist[index];
        mio->ch = cmd->channel[index];
        return 0;
    }

    mio->ppa = cmd->ppalist[mio->sec_offset];
    mio->ch = cmd->channel[mio->sec_offset];

    g = mio->ch->geometry;

    /* Writes must follow a correct page ppa sequence, while reads are allowed
     * at sector granularity */
    if (cmd->cmdtype == MMGR_WRITE_PG) {
        for (c = 1; c < g->sec_per_pg; c++) {
            ppa = &cmd->ppalist[mio->sec_offset + c];
            if (ppa->g.ch != mio->ppa.g.ch || ppa->g.lun != mio->ppa.g.lun ||
                    ppa->g.blk != mio->ppa.g.blk ||
                    ppa->g.pg != mio->ppa.g.pg   ||
                    ppa->g.pl != mio->ppa.g.pl   ||
                    ppa->g.sec != mio->ppa.g.sec + c) {
                log_err ("[ERROR ftl_lnvm: Wrong write ppa sequence. "
                                                         "Aborting IO cmd.\n");
                return -1;
            }
        }
    }

    if (cmd->sec_sz != g->sec_size)
        return -1;

    /* Build the MMGR command at page granularity, but PRP for empty sectors
     * are kept 0. The empty PRPs are checked in the MMGR for DMA. */
    mio->sec_sz = cmd->sec_sz;
    mio->md_sz = g->pg_oob_sz;

    memset(mio->prp, 0x0, sizeof(uint64_t) * g->sec_per_pg);
    for (c = 0; c < mio->n_sectors; c++)
        mio->prp[cmd->ppalist[mio->sec_offset + c].g.sec] =
                                                cmd->prp[mio->sec_offset + c];

    mio->pg_sz = g->pg_size;
    mio->n_sectors = mio->pg_sz / mio->sec_sz;

    mio->md_prp = cmd->md_prp[index];

    return 0;
}

static int ppa_io_check (struct nvm_io_cmd *cmd)
{
    int i;

    if (cmd->status.pgs_p == 0) {
        for (i = 0; i < cmd->status.total_pgs; i++)
            ppa_io_set_pgmap (cmd->status.pg_map, i, FTL_PGMAP_ON, &cmd->mutex);
    }

    cmd->status.pgs_p = cmd->status.pgs_s;

    if (cmd->cmdtype == MMGR_ERASE_BLK)
        return 0;

    if (cmd->status.total_pgs > 64 || cmd->status.total_pgs == 0){
        cmd->status.status = NVM_IO_FAIL;
        return cmd->status.nvme_status = NVME_INVALID_FORMAT;
    }

    return 0;
}

static void ppa_io_decompact_write (struct nvm_mmgr_io_cmd *cmd)
{
    struct nvm_mmgr_geometry *g = cmd->ch->geometry;

    for (int pl = 1; pl < g->n_of_planes; pl++) {
        for (int sec = 0; sec < g->sec_per_pg; sec++)
            cmd->prp[g->sec_per_pg * pl + sec] = 0x0;

        if (cmd->status == NVM_IO_SUCCESS) {
            ppa_io_set_pgmap(cmd->nvm_io->status.pg_map,
                        cmd->pg_index + pl, FTL_PGMAP_OFF, &cmd->nvm_io->mutex);
            pthread_mutex_lock(&cmd->nvm_io->mutex);
            cmd->nvm_io->status.pgs_s++;
        } else {
            pthread_mutex_lock(&cmd->nvm_io->mutex);
            cmd->status = NVM_IO_FAIL;
            cmd[pl].status = NVM_IO_FAIL;
            cmd->nvm_io->status.pg_errors++;
        }

        cmd->nvm_io->status.pgs_p++;
        pthread_mutex_unlock(&cmd->nvm_io->mutex);
    }

    cmd->n_sectors = g->sec_per_pg;
}

static void ppa_io_callback (struct nvm_mmgr_io_cmd *cmd)
{
    if (cmd->cmdtype == MMGR_WRITE_PG &&
                                    (cmd->ch->mmgr->flags & MMGR_FLAG_PL_CMD))
        ppa_io_decompact_write (cmd);

    if (cmd->status == NVM_IO_SUCCESS) {
        ppa_io_set_pgmap (cmd->nvm_io->status.pg_map,
                            cmd->pg_index, FTL_PGMAP_OFF, &cmd->nvm_io->mutex);
        pthread_mutex_lock (&cmd->nvm_io->mutex);
        cmd->nvm_io->status.pgs_s++;
    } else {
        pthread_mutex_lock (&cmd->nvm_io->mutex);
        cmd->nvm_io->status.pg_errors++;
    }

    cmd->nvm_io->status.pgs_p++;
    pthread_mutex_unlock (&cmd->nvm_io->mutex);

    ppa_io_check_end (cmd->nvm_io);
}

static void ppa_io_compact_write (struct nvm_io_cmd *cmd, uint16_t i)
{
    struct nvm_mmgr_geometry *g = cmd->mmgr_io[i].ch->geometry;

    for (int pl = 1; pl < g->n_of_planes; pl++) {
        for (int sec = 0; sec < g->sec_per_pg; sec++)
            cmd->mmgr_io[i].prp[g->sec_per_pg * pl + sec] =
                                                cmd->mmgr_io[i + pl].prp[sec];
        cmd->mmgr_io[i + pl].status = NVM_IO_PROCESS;
    }
    cmd->mmgr_io[i].n_sectors = g->sec_per_pl_pg;
}

static int ppa_io_submit (struct nvm_io_cmd *cmd)
{
    uint8_t compact;
    int ret, i, pl;
    struct nvm_mmgr_geometry *g;

    ret = ppa_io_check (cmd);
    if (ret) return ret;

    for (i = 0; i < cmd->status.total_pgs; i++) {

        /* if true, page not processed yet */
        if ( cmd->status.pg_map[i / 8] & (1 << (i % 8)) ) {
            if (ppa_io_check_pg (cmd, i)) {
                cmd->status.status = NVM_IO_FAIL;
                cmd->status.nvme_status = NVME_INVALID_FORMAT;
                return -1;
            }
        }
    }

    for (i = 0; i < cmd->status.total_pgs; i++) {
        g = cmd->mmgr_io[i].ch->geometry;

        /* if true, page not processed yet */
        if ( cmd->status.pg_map[i / 8] & (1 << (i % 8)) ) {

            compact = (cmd->mmgr_io[i].ch->mmgr->flags & MMGR_FLAG_PL_CMD) ?
                                                                         1 : 0;
            cmd->mmgr_io[i].status = NVM_IO_PROCESS;
            switch (cmd->cmdtype) {
                case MMGR_WRITE_PG:

                    if (compact)
                        ppa_io_compact_write(cmd, i);

                    ret = ppa_io_pg_write(&cmd->mmgr_io[i]);

                    if (compact && ret) {
                        ppa_io_decompact_write (&cmd->mmgr_io[i]);
                        for (pl = 1; pl < g->n_of_planes; pl++)
                            cmd->mmgr_io[i + pl].status = NVM_IO_FAIL;
                    }

                    break;
                case MMGR_READ_PG:
                    ret = ppa_io_pg_read(&cmd->mmgr_io[i]);
                    break;
                case MMGR_ERASE_BLK:
                    ret = ppa_io_erase_blk(&cmd->mmgr_io[i]);
                    break;
                default:
                    ret = -1;
            }
            if (ret) {
                pthread_mutex_lock (&cmd->mutex);
                cmd->status.pg_errors++;
                cmd->status.pgs_p++;
                pthread_mutex_unlock (&cmd->mutex);
                cmd->mmgr_io[i].status = NVM_IO_FAIL;
                ppa_io_check_end (cmd);
            }

            if (compact && (cmd->cmdtype == MMGR_WRITE_PG))
                i += g->n_of_planes - 1;
        }
    }

    return 0;
}

static struct app_ppa_io oxblk_ppa_io = {
    .mod_id      = OXBLK_PPA_IO,
    .name        = "OX-BLOCK-PPA",
    .submit_fn   = ppa_io_submit,
    .callback_fn = ppa_io_callback
};

void oxb_ppa_io_register (void) {
    app_mod_register (APPMOD_PPA_IO, OXBLK_PPA_IO, &oxblk_ppa_io);
}
