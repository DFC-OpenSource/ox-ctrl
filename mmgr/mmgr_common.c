/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - Common Media Manager functions
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

#include <string.h>
#include <stdlib.h>
#include <libox.h>

static int mmgr_io_rsv_blk (struct nvm_channel *ch, uint8_t cmdtype,
                            void *buf, uint16_t blk, uint16_t pl, uint16_t pg)
{
    int ret = -1;
    struct nvm_mmgr_io_cmd *cmd = ox_malloc(sizeof(struct nvm_mmgr_io_cmd),
                                                                  OX_MEM_MMGR);
    if (!cmd)
        return EMEM;

    memset (cmd, 0, sizeof (struct nvm_mmgr_io_cmd));
    cmd->ppa.g.blk = blk;
    cmd->ppa.g.pl = pl;
    cmd->ppa.g.ch = ch->ch_mmgr_id;

    /* TODO: Replicate among all LUNs in the channel */
    cmd->ppa.g.lun = 0;

    cmd->ppa.g.pg = pg;

    ret = ox_submit_sync_io (ch, cmd, buf, cmdtype);
    
    ox_free (cmd, OX_MEM_MMGR);

    return ret;
}

static int mmgr_read_nvminfo (struct nvm_channel *ch)
{
    int ret, pg;
    struct nvm_channel ch_a;
    void *buf;
    uint16_t buf_sz = ch->geometry->pg_size + ch->geometry->sec_oob_sz
                                                    * ch->geometry->sec_per_pg;

    buf = ox_calloc(buf_sz * ch->geometry->n_of_planes, 1, OX_MEM_MMGR);
    if (!buf)
        return EMEM;

    pg = 0;
    do {
        memset (buf, 0, buf_sz * ch->geometry->n_of_planes);
        ret = mmgr_io_rsv_blk (ch, MMGR_READ_PL_PG, buf, 0, 0, pg);
        memcpy (&ch_a.nvm_info, buf, sizeof (ch_a.nvm_info));

        if (ret || ch_a.i.in_use != NVM_CH_IN_USE)
            break;

        memcpy (&ch->nvm_info, &ch_a.nvm_info, sizeof (ch_a.nvm_info));
        pg++;
    } while (pg < ch->geometry->pg_per_blk);

    if (ret)
        goto OUT;

    if (!pg)
        ret = mmgr_io_rsv_blk (ch, MMGR_ERASE_BLK, NULL, 0, 0, 0);

OUT:
    ox_free(buf, OX_MEM_MMGR);
    return ret;
}

static int mmgr_flush_nvminfo (struct nvm_channel *ch)
{
    int ret, pg;
    struct nvm_channel ch_a;
    void *buf;
    uint16_t buf_sz = ch->geometry->pg_size + ch->geometry->sec_oob_sz
                                                    * ch->geometry->sec_per_pg;

    buf = ox_calloc(buf_sz * ch->geometry->n_of_planes, 1, OX_MEM_MMGR);
    if (!buf)
        return EMEM;

    pg = 0;
    do {
        memset (buf, 0, buf_sz * ch->geometry->n_of_planes);
        ret = mmgr_io_rsv_blk (ch, MMGR_READ_PL_PG, buf, 0, 0, pg);
        memcpy (&ch_a.nvm_info, buf, sizeof (ch_a.nvm_info));

        if (ret || ch_a.i.in_use != NVM_CH_IN_USE)
            break;

        pg++;
    } while (pg < ch->geometry->pg_per_blk);

    if (ret)
        goto OUT;

    if (pg == ch->geometry->pg_per_blk) {
        if (mmgr_io_rsv_blk (ch, MMGR_ERASE_BLK, NULL, 0, 0, 0))
            goto OUT;
        pg = 0;
    }

    memset (buf, 0, buf_sz * ch->geometry->n_of_planes);
    memcpy (buf, &ch->nvm_info, sizeof (ch->nvm_info));

    ret = mmgr_io_rsv_blk (ch, MMGR_WRITE_PL_PG, buf, 0, 0, pg);
    if (ret) {
        pg = 0;
        if (mmgr_io_rsv_blk (ch, MMGR_ERASE_BLK, NULL, 0, 0, 0))
            goto OUT;
        ret = mmgr_io_rsv_blk (ch, MMGR_WRITE_PL_PG, buf, 0, 0, pg);
    }

OUT:
    ox_free(buf, OX_MEM_MMGR);
    return ret;
}

int mmgr_set_ch_info (struct nvm_channel *ch, uint16_t nc)
{
    int i;

    for(i = 0; i < nc; i++) {
        if(mmgr_flush_nvminfo (&ch[i]))
            return -1;
    }

    return 0;
}

int mmgr_get_ch_info (struct nvm_mmgr *mmgr, struct nvm_channel *ch,
        uint16_t nc, uint16_t rsv_blks)
{
    int i, n, pl, nsp = 0, trsv;
    struct nvm_ppa_addr *ppa;
    uint16_t npl;

    for(i = 0; i < nc; i++){
        ch[i].ch_mmgr_id = i;
        ch[i].mmgr = mmgr;
        ch[i].geometry = mmgr->geometry;

        ch[i].ns_pgs = ch[i].geometry->lun_per_ch *
                       ch[i].geometry->blk_per_lun *
                       ch[i].geometry->n_of_planes *
                       ch[i].geometry->pg_per_blk;

        npl = ch[i].geometry->n_of_planes;
        ch[i].mmgr_rsv = rsv_blks;
        trsv = ch[i].mmgr_rsv * npl;
        ch[i].mmgr_rsv_list = ox_malloc (trsv * sizeof(struct nvm_ppa_addr),
                                                                  OX_MEM_MMGR);

        if (!ch[i].mmgr_rsv_list)
            return EMEM;

        memset (ch[i].mmgr_rsv_list, 0, trsv * sizeof(struct nvm_ppa_addr));

        for (n = 0; n < ch[i].mmgr_rsv; n++) {
            for (pl = 0; pl < npl; pl++) {
                ppa = &ch[i].mmgr_rsv_list[npl * n + pl];
                ppa->g.ch = ch[i].ch_mmgr_id;
                ppa->g.lun = 0;
                ppa->g.blk = n;
                ppa->g.pl = pl;
            }
        }

        ch[i].ftl_rsv = 0;
        ch[i].ftl_rsv_list = ox_malloc (sizeof(struct nvm_ppa_addr),
                                                                  OX_MEM_MMGR);
        if (!ch[i].ftl_rsv_list) {
            ox_free (ch[i].mmgr_rsv_list, OX_MEM_MMGR);
            return EMEM;
        }

        if (mmgr_read_nvminfo (&ch[i])) {
            ox_free (ch[i].ftl_rsv_list, OX_MEM_MMGR);
            ox_free (ch[i].mmgr_rsv_list, OX_MEM_MMGR);
            return -1;
        }

        if (ch[i].i.in_use != NVM_CH_IN_USE) {
            ch[i].i.ns_id = 0x0;
            ch[i].i.ns_part = 0x0;
            ch[i].i.ftl_id = 0x0;
            ch[i].i.in_use = 0x0;
        }

        ch[i].tot_bytes = 0;
        ch[i].slba = 0;
        ch[i].elba = 0;
        nsp++;
    }

    return 0;
}