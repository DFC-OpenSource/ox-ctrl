/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Flash Translation Layer (Physical Page Address I/O)
 *
 * Copyright 2018 IT University of Copenhagen.
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
 * Written by Ivan Luiz Picoli <ivpi@itu.dk>
 *
 * Partially supported by CAPES Foundation, Ministry of Education
 * of Brazil, Brasilia - DF 70040-020, Brazil.
 */

#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/queue.h>
#include "../../../include/ssd.h"
#include "../appnvm.h"

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
    return nvm_submit_mmgr (cmd);
}

static inline int ppa_io_pg_write (struct nvm_mmgr_io_cmd *cmd)
{
    return nvm_submit_mmgr (cmd);
}

static inline int ppa_io_erase_blk (struct nvm_mmgr_io_cmd *cmd)
{
    return nvm_submit_mmgr (cmd);
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
            cmd->status.status = NVM_IO_SUCCESS;
            cmd->status.nvme_status = NVME_SUCCESS;
            goto COMPLETE;
        }
    }
    goto RETURN;

SUBMIT:
    pthread_mutex_unlock (&cmd->mutex);
    ppa_io_submit (cmd);
    goto RETURN;

COMPLETE:
    pthread_mutex_unlock (&cmd->mutex);
    appnvm()->lba_io->callback_fn (cmd);

RETURN:
    return 0;
}

static int ppa_io_check_pg (struct nvm_io_cmd *cmd, uint8_t index)
{
    int c;
    struct nvm_ppa_addr *ppa;
    struct nvm_mmgr_io_cmd *mio = &cmd->mmgr_io[index];

    mio->nvm_io = cmd;

    if (cmd->cmdtype == MMGR_ERASE_BLK) {
        mio->ppa = cmd->ppalist[index];
        mio->ch = cmd->channel[index];
        return 0;
    }

    mio->ppa = cmd->ppalist[mio->sec_offset];
    mio->ch = cmd->channel[mio->sec_offset];

    /* Writes must follow a correct page ppa sequence, while reads are allowed
     * at sector granularity */
    if (cmd->cmdtype == MMGR_WRITE_PG) {
        for (c = 1; c < LNVM_SEC_PG; c++) {
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

    if (cmd->sec_sz != LNVM_SECSZ)
        return -1;

    /* Build the MMGR command at page granularity, but PRP for empty sectors
     * are kept 0. The empty PRPs are checked in the MMGR for DMA. */
    mio->sec_sz = cmd->sec_sz;
    mio->md_sz = LNVM_SEC_OOBSZ * LNVM_SEC_PG;

    for (c = 0; c < mio->n_sectors; c++)
        mio->prp[cmd->ppalist[mio->sec_offset + c].g.sec] =
                                                cmd->prp[mio->sec_offset + c];

    mio->pg_sz = LNVM_PG_SIZE;
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

static void ppa_io_callback (struct nvm_mmgr_io_cmd *cmd)
{
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

static int ppa_io_submit (struct nvm_io_cmd *cmd)
{
    int ret, i;

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
        /* if true, page not processed yet */
        if ( cmd->status.pg_map[i / 8] & (1 << (i % 8)) ) {
            switch (cmd->cmdtype) {
                case MMGR_WRITE_PG:
                    ret = ppa_io_pg_write(&cmd->mmgr_io[i]);
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
                ppa_io_check_end (cmd);
            }
        }
    }

    return 0;
}

static struct app_ppa_io appftl_ppa_io = {
    .mod_id      = APPFTL_PPA_IO,
    .submit_fn   = ppa_io_submit,
    .callback_fn = ppa_io_callback
};

void ppa_io_register (void) {
    appnvm_mod_register (APPMOD_PPA_IO, APPFTL_PPA_IO, &appftl_ppa_io);
}