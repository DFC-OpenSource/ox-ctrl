/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Flash Translation Layer (Channel Provisioning)
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
#include <stdio.h>
#include <ox-app.h>
#include <sys/queue.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include <libox.h>

#define APP_PROV_LINE_SZ    8
#define APP_PROV_LINES      3

struct ch_prov_blk {
    struct nvm_ppa_addr             addr;
    struct app_blk_md_entry         *blk_md;
    uint8_t                         *state;
    uint32_t                        blk_i;
    CIRCLEQ_ENTRY(ch_prov_blk)      entry;
    TAILQ_ENTRY(ch_prov_blk)        open_entry;
    LIST_ENTRY(ch_prov_blk)         waiting_entry;
};

struct ch_prov_lun {
    struct nvm_ppa_addr     addr;
    struct ch_prov_blk      *vblks;
    uint32_t                nfree_blks;
    uint32_t                nused_blks;
    uint32_t                nopen_blks[APP_PROV_LINES];
    pthread_mutex_t         l_mutex;
    CIRCLEQ_HEAD(free_blk_list, ch_prov_blk) free_blk_head;
    CIRCLEQ_HEAD(used_blk_list, ch_prov_blk) used_blk_head;

    /* Open blocks for separated user, cold and meta lines */
    TAILQ_HEAD(open_blk_list, ch_prov_blk) open_blk_head[APP_PROV_LINES];

    /* Blocks to be put after checkpoint */
    LIST_HEAD(waiting_cp, ch_prov_blk) waiting_cp_head;
};

struct ch_prov_line {
    uint16_t              nblks;
    uint16_t              current_blk;
    struct ch_prov_blk  **vblks;
};

struct ch_prov {
    struct ch_prov_lun  *luns;
    struct ch_prov_blk  **prov_vblks;
    struct ch_prov_line line[APP_PROV_LINES];
    pthread_mutex_t     ch_mutex;
};

static struct ch_prov_blk *ch_prov_blk_rand (struct app_channel *lch,
                                                                       int lun)
{
    int blk, blk_idx;
    struct ch_prov_blk *vblk, *tmp;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    if (prov->luns[lun].nfree_blks > 0) {
        blk_idx = rand() % prov->luns[lun].nfree_blks;
        vblk = CIRCLEQ_FIRST(&prov->luns[lun].free_blk_head);

        for (blk = 0; blk < blk_idx; blk++) {
            tmp = CIRCLEQ_NEXT(vblk, entry);
            vblk = tmp;
        }

        return vblk;
    }
    return NULL;
}

static int ch_prov_blk_alloc(struct app_channel *lch, int lun, int blk)
{
    int pl, ltype;
    int bad_blk = 0;
    struct ch_prov_blk *rnd_vblk;
    int n_pl = lch->ch->geometry->n_of_planes;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    struct ch_prov_blk *vblk = &(prov->prov_vblks[lun][blk]);

    uint8_t *bbt = oxapp()->bbt->get_fn (lch, lun);

    vblk->state = ox_malloc(sizeof (uint8_t) * n_pl, OX_MEM_OXBLK_CPR);
    if (vblk->state == NULL)
        return -1;

    vblk->addr.ppa = prov->luns[lun].addr.ppa;
    vblk->addr.g.blk = blk;
    vblk->blk_i = lun * lch->ch->geometry->blk_per_lun + blk;
    vblk->blk_md = &oxapp()->md->get_fn (lch, lun)[blk];

    for (pl = 0; pl < lch->ch->geometry->n_of_planes; pl++) {
        vblk->state[pl] = bbt[n_pl * blk + pl];
        bad_blk += vblk->state[pl];
    }

    if (!bad_blk) {

        if (!(vblk->blk_md->flags & APP_BLK_MD_AVLB)) {
            oxapp()->md->mark_fn (lch, vblk->blk_i / lch->blk_md->ent_per_pg);
            vblk->blk_md->flags |= APP_BLK_MD_AVLB;
        }

        if (vblk->blk_md->flags & APP_BLK_MD_USED) {
            CIRCLEQ_INSERT_HEAD(&(prov->luns[lun].used_blk_head), vblk, entry);
            prov->luns[lun].nused_blks++;

            /* Add open block to the correct line type */
            if (vblk->blk_md->flags & APP_BLK_MD_OPEN) {

                ltype = (vblk->blk_md->flags & APP_BLK_MD_META) ?
                                                              APP_LINE_META :
                        (vblk->blk_md->flags & APP_BLK_MD_COLD) ?
                                             APP_LINE_COLD : APP_LINE_USER;

                TAILQ_INSERT_HEAD(&prov->luns[lun].open_blk_head[ltype],
                                                             vblk, open_entry);
                prov->luns[lun].nopen_blks[ltype]++;
            }

            return 0;
        }

        rnd_vblk = ch_prov_blk_rand(lch, lun);

        if (rnd_vblk == NULL) {
            CIRCLEQ_INSERT_HEAD(&prov->luns[lun].free_blk_head, vblk, entry);
        } else {

            if (rand() % 2)
                CIRCLEQ_INSERT_BEFORE(&prov->luns[lun].free_blk_head,
                                                rnd_vblk, vblk, entry);
            else
                CIRCLEQ_INSERT_AFTER(&prov->luns[lun].free_blk_head,
                                                rnd_vblk, vblk, entry);
        }
        prov->luns[lun].nfree_blks++;

    } else {

        if (vblk->blk_md->flags & APP_BLK_MD_AVLB) {
            oxapp()->md->mark_fn (lch, vblk->blk_i / lch->blk_md->ent_per_pg);
            vblk->blk_md->flags ^= APP_BLK_MD_AVLB;
        }
    }

    return 0;
}

static int ch_prov_list_create(struct app_channel *lch, int lun)
{
    int blk, nblk, line;
    struct nvm_ppa_addr addr;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    addr.ppa = 0x0;
    addr.g.ch = lch->ch->ch_id;

    addr.g.lun = lun;
    prov->luns[lun].addr.ppa = addr.ppa;
    nblk = lch->ch->geometry->blk_per_lun;

    prov->luns[lun].nfree_blks = 0;
    prov->luns[lun].nused_blks = 0;
    memset (prov->luns[lun].nopen_blks, 0, sizeof(uint32_t) * APP_PROV_LINES);

    CIRCLEQ_INIT(&prov->luns[lun].free_blk_head);
    CIRCLEQ_INIT(&prov->luns[lun].used_blk_head);
    LIST_INIT(&prov->luns[lun].waiting_cp_head);

    for (line = 0; line < APP_PROV_LINES; line++) {
        TAILQ_INIT(&prov->luns[lun].open_blk_head[line]);
    }

    pthread_mutex_init(&prov->luns[lun].l_mutex, NULL);

    for (blk = 0; blk < nblk; blk++)
        ch_prov_blk_alloc(lch, lun, blk);

    return 0;
}

static int ch_prov_blk_free (struct app_channel *lch, int lun, int blk)
{
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    ox_free (prov->prov_vblks[lun][blk].state, OX_MEM_OXBLK_CPR);

    return 0;
}


static void ch_prov_blk_list_free(struct app_channel *lch, int lun)
{
    int nblk, blk, ltype;
    struct ch_prov_blk *vblk, *tmp;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    nblk = lch->ch->geometry->blk_per_lun;

    if (prov->luns[lun].nfree_blks > 0) {
        vblk = CIRCLEQ_FIRST(&(prov->luns[lun].free_blk_head));

        for (blk = 0; blk < prov->luns[lun].nfree_blks; blk++) {
            tmp = CIRCLEQ_NEXT(vblk, entry);
            CIRCLEQ_REMOVE(&(prov->luns[lun].free_blk_head), vblk, entry);
            vblk = tmp;
        }
    }
    prov->luns[lun].nfree_blks = 0;

    if (prov->luns[lun].nused_blks > 0) {
        vblk = CIRCLEQ_FIRST(&prov->luns[lun].used_blk_head);

        for (blk = 0; blk < prov->luns[lun].nused_blks; blk++) {
            tmp = CIRCLEQ_NEXT(vblk, entry);
            CIRCLEQ_REMOVE(&prov->luns[lun].used_blk_head, vblk, entry);
            vblk = tmp;

            if (CIRCLEQ_EMPTY (&prov->luns[lun].used_blk_head))
                break;
        }
    }
    prov->luns[lun].nused_blks = 0;

    for (ltype = 0; ltype < APP_PROV_LINES; ltype++) {

        if (prov->luns[lun].nopen_blks[ltype] > 0) {
            vblk = TAILQ_FIRST(&prov->luns[lun].open_blk_head[ltype]);

            for (blk = 0; blk < prov->luns[lun].nopen_blks[ltype]; blk++) {

                tmp = TAILQ_NEXT(vblk, open_entry);
                TAILQ_REMOVE(&prov->luns[lun].open_blk_head[ltype],
                                                             vblk, open_entry);
                vblk = tmp;

            }
        }
        prov->luns[lun].nopen_blks[ltype] = 0;
    }

    for (blk = 0; blk < nblk; blk++) {
        ch_prov_blk_free(lch, lun, blk);
    }

    pthread_mutex_destroy(&(prov->luns[lun].l_mutex));
}

static int ch_prov_init_luns (struct app_channel *lch)
{
    int lun, err_lun;
    int nluns;
    int nblocks;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    srand(time(NULL));

    nluns = lch->ch->geometry->lun_per_ch;
    nblocks = lch->ch->geometry->blk_per_lun;

    prov->luns = ox_malloc (sizeof (struct ch_prov_lun) *
                            lch->ch->geometry->lun_per_ch, OX_MEM_OXBLK_CPR);
    if (!prov->luns)
        return -1;

    prov->prov_vblks = ox_malloc(nluns * sizeof(struct ch_prov_blk *),
                                                            OX_MEM_OXBLK_CPR);
    if (!prov->prov_vblks)
        goto FREE_LUNS;

    for (lun = 0; lun < nluns; lun++) {
        prov->prov_vblks[lun] = ox_malloc(nblocks * sizeof (struct ch_prov_blk),
                                                            OX_MEM_OXBLK_CPR);
        if (!prov->prov_vblks[lun]) {
            for (err_lun = 0; err_lun < lun; err_lun++)
                ox_free (prov->prov_vblks[err_lun], OX_MEM_OXBLK_CPR);
            goto FREE_VBLKS;
        }

        if (ch_prov_list_create(lch, lun) < 0) {
            for (err_lun = 0; err_lun < lun; err_lun++)
                ch_prov_blk_list_free(lch, err_lun);
            goto FREE_VBLKS_LUN;
        }
    }

    return 0;

FREE_VBLKS_LUN:
    for (err_lun = 0; err_lun < nluns; err_lun++)
        ox_free (prov->prov_vblks[err_lun], OX_MEM_OXBLK_CPR);

FREE_VBLKS:
    ox_free (prov->prov_vblks, OX_MEM_OXBLK_CPR);

FREE_LUNS:
    ox_free (prov->luns, OX_MEM_OXBLK_CPR);
    return -1;
}

static int ch_prov_exit_luns (struct app_channel *lch)
{
    int lun;
    int nluns;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    nluns = lch->ch->geometry->lun_per_ch;

    for (lun = 0; lun < nluns; lun++) {
        ch_prov_blk_list_free(lch, lun);
        ox_free (prov->prov_vblks[lun], OX_MEM_OXBLK_CPR);
    }

    ox_free (prov->prov_vblks, OX_MEM_OXBLK_CPR);
    ox_free (prov->luns, OX_MEM_OXBLK_CPR);

    return 0;
}

static void ch_prov_check_gc (struct app_channel *lch)
{
    uint16_t lun_i;
    float tot_blk = 0, free_blk = 0;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    struct ch_prov_lun *p_lun;

    for (lun_i = 0; lun_i < lch->ch->geometry->lun_per_ch; lun_i++) {
        p_lun = &prov->luns[lun_i];
        tot_blk += p_lun->nfree_blks + p_lun->nused_blks;
        free_blk += p_lun->nfree_blks;
    }

    /* If the channel runs out of blocks, disable channel and leave
                                                    last blocks for GC usage*/
    if (free_blk < APP_GC_MIN_FREE_BLKS)
        app_ch_active_unset (lch);

    if ((float) 1 - (free_blk / tot_blk) > APP_GC_THRESD)
        app_ch_need_gc_set (lch);
}

/**
 * Gets a new block from a LUN and mark it as open.
 * The parameter 'type' defines the line type that the block will be added.
 * If the block fails to erase, mark it as bad and try next block.
 * @return the pointer to the new open block
 */
static struct ch_prov_blk *ch_prov_blk_get (struct app_channel *lch,
                                                    uint16_t lun, uint8_t type)
{
    int ret, pl;
    int n_pl = lch->ch->geometry->n_of_planes;
    struct nvm_mmgr_io_cmd *cmd;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    struct ch_prov_lun *p_lun = &prov->luns[lun];
    uint64_t index;

NEXT:
    if (p_lun->nfree_blks > 0) {
        struct ch_prov_blk *vblk = CIRCLEQ_FIRST(&p_lun->free_blk_head);

        CIRCLEQ_REMOVE(&p_lun->free_blk_head, vblk, entry);
        CIRCLEQ_INSERT_TAIL(&p_lun->used_blk_head, vblk, entry);
        TAILQ_INSERT_HEAD(&p_lun->open_blk_head[type], vblk, open_entry);

        p_lun->nfree_blks--;
        p_lun->nused_blks++;
        p_lun->nopen_blks[type]++;

        /* Erase the block, if it fails, mark as bad and try next block */
        cmd = ox_malloc (sizeof(struct nvm_mmgr_io_cmd), OX_MEM_OXBLK_CPR);
        if (!cmd)
            return NULL;
        for (pl = 0; pl < n_pl; pl++) {
            memset (cmd, 0x0, sizeof(struct nvm_mmgr_io_cmd));
            cmd->ppa.ppa = vblk->addr.ppa;
            cmd->ppa.g.pl = pl;

            ret = ox_submit_sync_io (lch->ch, cmd, NULL, MMGR_ERASE_BLK);
            if (ret)
                break;
        }
        ox_free (cmd, OX_MEM_OXBLK_CPR);

        index = vblk->blk_i / lch->blk_md->ent_per_pg;
        oxapp()->md->mark_fn (lch, index);

        pthread_spin_lock (&lch->blk_md->entry_spin[index]);
        vblk->blk_md->erase_count++;
        pthread_spin_unlock (&lch->blk_md->entry_spin[index]);

        if (ret) {
            for (pl = 0; pl < n_pl; pl++)
                lch->ch->ftl->ops->set_bbtbl (&vblk->addr, APP_BBT_BAD);

            CIRCLEQ_REMOVE(&(p_lun->used_blk_head), vblk, entry);
            TAILQ_REMOVE(&(p_lun->open_blk_head[type]), vblk, open_entry);
            p_lun->nused_blks--;
            p_lun->nopen_blks[type]--;

            goto NEXT;
        }

        pthread_spin_lock (&lch->blk_md->entry_spin[index]);
        vblk->blk_md->current_pg = 0;
        vblk->blk_md->invalid_sec = 0;
        vblk->blk_md->flags |= (APP_BLK_MD_USED | APP_BLK_MD_OPEN);
        if (vblk->blk_md->flags & APP_BLK_MD_LINE)
            vblk->blk_md->flags ^= APP_BLK_MD_LINE;
        if (vblk->blk_md->flags & APP_BLK_MD_META)
            vblk->blk_md->flags ^= APP_BLK_MD_META;
        if (vblk->blk_md->flags & APP_BLK_MD_COLD)
            vblk->blk_md->flags ^= APP_BLK_MD_COLD;

        memset (vblk->blk_md->pg_state, 0x0, BLK_MD_BYTE_VEC_SZ);
        pthread_spin_unlock (&lch->blk_md->entry_spin[index]);

        oxapp()->ch_prov->check_gc_fn (lch);

        if (APP_DEBUG_CH_PROV) {
            printf("[ox-blk (ch_prov): blk GET: (%d/%d/%d/%d) - Free: %d,"
                " Used: %d, Open(0x%d): %d]\n",vblk->addr.g.ch,vblk->addr.g.lun,
                vblk->addr.g.blk, vblk->blk_md->erase_count, p_lun->nfree_blks,
                p_lun->nused_blks, type, p_lun->nopen_blks[type]);
        }

        return vblk;
    }

    return NULL;
}

static void __ch_prov_blk_put (struct app_channel *lch, struct ch_prov_blk *vblk)
{
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    struct ch_prov_lun *p_lun = &prov->luns[vblk->addr.g.lun];
    uint64_t index;

    index = vblk->blk_i / lch->blk_md->ent_per_pg;
    oxapp()->md->mark_fn (lch, index);

    pthread_spin_lock (&lch->blk_md->entry_spin[index]);
    if (vblk->blk_md->flags & APP_BLK_MD_USED)
        vblk->blk_md->flags ^= APP_BLK_MD_USED;

    if (vblk->blk_md->flags & APP_BLK_MD_LINE)
        vblk->blk_md->flags ^= APP_BLK_MD_LINE;

    if (vblk->blk_md->flags & APP_BLK_MD_META)
        vblk->blk_md->flags ^= APP_BLK_MD_META;

    if (vblk->blk_md->flags & APP_BLK_MD_COLD)
        vblk->blk_md->flags ^= APP_BLK_MD_COLD;
    pthread_spin_unlock (&lch->blk_md->entry_spin[index]);

    CIRCLEQ_REMOVE(&p_lun->used_blk_head, vblk, entry);
    CIRCLEQ_INSERT_TAIL(&p_lun->free_blk_head, vblk, entry);

    p_lun->nfree_blks++;
    p_lun->nused_blks--;

    if (APP_DEBUG_CH_PROV) {
        printf("[2 - ox-blk (ch_prov): blk PUT: (%d %d %d) - Free: %d,"
                " Used: %d]\n", vblk->addr.g.ch, vblk->addr.g.lun,
                vblk->addr.g.blk, p_lun->nfree_blks, p_lun->nused_blks);
    }
}

/**
 * Mark a block as free when it is no longer used (recycled by GC).
 * Put blocks will be kept in a list until this function is called with the
 * following values: (struct app_channel *, 0xffff, 0xffff)
 * @return 0 in success, negative if block is still open or still in use
 */
static int ch_prov_blk_put(struct app_channel *lch, uint16_t lun, uint16_t blk)
{
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    struct ch_prov_blk *vblk;
    struct ch_prov_lun *p_lun;
    uint32_t lun_i;

    if (lun == 0xffff && blk == 0xffff) {
        for (lun_i = 0; lun_i < lch->ch->geometry->lun_per_ch; lun_i++) {
            p_lun = &prov->luns[lun_i];

            pthread_mutex_lock(&prov->ch_mutex);
            while (!LIST_EMPTY(&p_lun->waiting_cp_head)) {
                vblk = LIST_FIRST (&p_lun->waiting_cp_head);
                LIST_REMOVE (vblk, waiting_entry);
                __ch_prov_blk_put (lch, vblk);
            }
            pthread_mutex_unlock(&prov->ch_mutex);

        }

        return 0;
    }

    vblk = &prov->prov_vblks[lun][blk];
    p_lun = &prov->luns[vblk->addr.g.lun];

    if (!(vblk->blk_md->flags & APP_BLK_MD_USED))
        return -1;

    if (vblk->blk_md->flags & APP_BLK_MD_OPEN)
        return -2;

    vblk->blk_md->flags ^= APP_BLK_MD_USED;

    pthread_mutex_lock(&prov->ch_mutex);
    LIST_INSERT_HEAD (&p_lun->waiting_cp_head, vblk, waiting_entry);
    pthread_mutex_unlock(&prov->ch_mutex);

    return 0;
}

/**
 * This function collects open blocks from all LUNs using round-robin.
 * It sets the line blocks (used by selecting pages to be written).
 * If a LUN has no open block, it opens a new block.
 * If a LUN has no blocks left, the line will be filled with available LUNs.
 * @return 0 in success, negative in failure (channel is full)
 */
static int ch_prov_renew_line (struct app_channel *lch, uint8_t type)
{
    uint32_t lun, targets, i, found, j;
    struct ch_prov_blk *vblk;
    struct ch_prov_blk *line[APP_PROV_LINE_SZ];
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    uint32_t lflag = 0x0;
    uint64_t index;

    /* set all LUNS as available by setting all flags */
    for (lun = 0; lun < lch->ch->geometry->lun_per_ch; lun++)
        lflag |= (uint32_t)(1 << lun);

    lun = 0;
    targets = 0;
    do {
        /* There is no available LUN */
        if (lflag == 0x0)
            break;

        /* Check if LUN is available */
        if (!(lflag & (1 << lun)))
            goto NEXT_LUN;

        if (TAILQ_EMPTY(&prov->luns[lun].open_blk_head[type])) {
GET_BLK:
            vblk = ch_prov_blk_get (lch, lun, type);

            /* LUN has no available blocks, unset flag */
            if (vblk == NULL) {
                if (lflag & (1 << lun))
                    lflag ^= (1 << lun);
                goto NEXT_LUN;
            }
        }

        /* Avoid picking a block already present in the line */
        TAILQ_FOREACH(vblk, &prov->luns[lun].open_blk_head[type], open_entry) {
            if (!(vblk->blk_md->flags & APP_BLK_MD_LINE))
                break;

            /* Keep current line blocks in the line when renew is recalled */
            found = 0;
            for (i = 0; i < targets; i++) {
                if (line[i] == vblk) {
                    found = 1;
                    break;
                }
            }
            if (!found)
                break;
        }

        if (!vblk)
            goto GET_BLK;

        line[targets] = vblk;

        index = line[targets]->blk_i / lch->blk_md->ent_per_pg;
        oxapp()->md->mark_fn (lch, index);

        /* Set line related flags */
        pthread_spin_lock (&lch->blk_md->entry_spin[index]);
        line[targets]->blk_md->flags |= APP_BLK_MD_LINE;

        if (type == APP_LINE_META)
            line[targets]->blk_md->flags |= APP_BLK_MD_META;
        else if (type == APP_LINE_COLD)
            line[targets]->blk_md->flags |= APP_BLK_MD_COLD;
        pthread_spin_unlock (&lch->blk_md->entry_spin[index]);

        targets++;

NEXT_LUN:
        lun = (lun == lch->ch->geometry->lun_per_ch - 1) ? 0 : lun + 1;
    } while (targets < APP_PROV_LINE_SZ);

    prov->line[type].nblks = targets;

    /* Check if line has at least 1 block available */
    if (targets == 0) {
        log_err("[ox-blk (ch_prov): CH %d has no blocks left.]",lch->ch->ch_id);
        if (APP_DEBUG_CH_PROV)
            printf("ox-blk (ch_prv): CH %d has no blocks left\n",lch->ch->ch_id);
        return -1;
    }

    /* set the line pointers */
    for (i = 0; i < targets; i++)
        prov->line[type].vblks[i] = line[i];

    if (prov->line[type].current_blk >= targets)
        prov->line[type].current_blk = 0;

    if (APP_DEBUG_CH_PROV) {
        printf ("[ox-blk (ch_prov): Line (0x%d) is renewed: ", type);
        for (j = 0; j < targets; j++)
            printf ("(%d %d %d)", prov->line[type].vblks[j]->addr.g.ch,
                                   prov->line[type].vblks[j]->addr.g.lun,
                                   prov->line[type].vblks[j]->addr.g.blk);
        printf("]\n");
    }

    return 0;
}

static int ch_prov_init (struct app_channel *lch)
{
    int line;

    struct ch_prov *prov = ox_malloc(sizeof (struct ch_prov), OX_MEM_OXBLK_CPR);
    if (!prov)
        return -1;

    if (pthread_mutex_init(&prov->ch_mutex, NULL))
        goto FREE_PROV;

    lch->ch_prov = prov;
    if (ch_prov_init_luns (lch))
        goto MUTEX;

    for (line = 0; line < APP_PROV_LINES; line++) {
        prov->line[line].current_blk = 0;
        prov->line[line].vblks = ox_malloc (sizeof(struct ch_prov_blk *) *
                                            APP_PROV_LINE_SZ, OX_MEM_OXBLK_CPR);
        if (!prov->line[line].vblks)
            goto FREE_BLKS;

        if (ch_prov_renew_line (lch, line)) {
            log_err ("[ox-blk (ch_prov): CHANNEL %d is FULL!]\n",
                                                                lch->ch->ch_id);
            if (APP_DEBUG_CH_PROV)
                printf ("[ox-blk (ch_prov): CHANNEL %d is FULL!]\n",
                                                                lch->ch->ch_id);

            ox_free (prov->line[line].vblks, OX_MEM_OXBLK_CPR);
            goto FREE_BLKS;
        }
    }

    log_info("    [ox-blk: Channel Provisioning started. Ch %d]\n",
                                                                lch->ch->ch_id);
    return 0;

FREE_BLKS:
    while (line) {
        line--;
        ox_free (prov->line[line].vblks, OX_MEM_OXBLK_CPR);
    }
    ch_prov_exit_luns (lch);
MUTEX:
    pthread_mutex_destroy (&prov->ch_mutex);
FREE_PROV:
    ox_free (prov, OX_MEM_OXBLK_CPR);
    return -1;
}

static void ch_prov_exit (struct app_channel *lch)
{
    int line;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    for (line = 0; line < APP_PROV_LINES; line++)
        ox_free (prov->line[line].vblks, OX_MEM_OXBLK_CPR);

    ch_prov_exit_luns (lch);
    pthread_mutex_destroy (&prov->ch_mutex);
    ox_free (prov, OX_MEM_OXBLK_CPR);
}

static void ch_prov_blk_close (struct app_channel *lch,
                                          struct ch_prov_blk *blk, uint8_t type)
{
    struct ch_prov_lun *p_lun;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    struct timespec ts;
    uint32_t blk_id;
    uint64_t index;

    p_lun = &prov->luns[blk->addr.g.lun];

    TAILQ_REMOVE(&p_lun->open_blk_head[type], blk, open_entry);
    p_lun->nopen_blks[type]--;

    /* Set timestamp. This avoids garbage collecting blocks before checkpoint */
    blk_id = (blk->addr.g.lun * lch->ch->geometry->blk_per_lun) + blk->addr.g.blk;
    GET_NANOSECONDS (lch->blk_md->closed_ts[blk_id], ts);

    index = blk->blk_i / lch->blk_md->ent_per_pg;
    oxapp()->md->mark_fn (lch, index);

    pthread_spin_lock (&lch->blk_md->entry_spin[index]);
    if (blk->blk_md->flags & APP_BLK_MD_LINE)
        blk->blk_md->flags ^= APP_BLK_MD_LINE;
    if (blk->blk_md->flags & APP_BLK_MD_OPEN)
        blk->blk_md->flags ^= APP_BLK_MD_OPEN;
    pthread_spin_unlock (&lch->blk_md->entry_spin[index]);
}

static int ch_prov_blk_amend (struct app_channel *lch,
                                        struct nvm_ppa_addr *ppa, uint8_t type)
{
    uint32_t blk_i;
    struct ch_prov_blk *blk = NULL;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    uint64_t index;

    for (blk_i = 0; blk_i < prov->line[type].nblks; blk_i++) {
        blk = prov->line[type].vblks[blk_i];
        if (blk->addr.g.lun == ppa->g.lun && blk->addr.g.blk == ppa->g.blk)
            break;
    }

    if (blk_i == prov->line[type].nblks || !prov->line[type].nblks) {
        log_info ("[ch_prov (close_blk): Block [%d/%d/%d] not found in line.\n",
                                             ppa->g.ch, ppa->g.lun, ppa->g.blk);
        return -1;
    }

    ch_prov_blk_close (lch, blk, type);
    ch_prov_renew_line (lch, type);

    index = blk->blk_i / lch->blk_md->ent_per_pg;
    oxapp()->md->mark_fn (lch, index);

    pthread_spin_lock (&lch->blk_md->entry_spin[index]);
    blk->blk_md->current_pg = lch->ch->geometry->pg_per_blk;
    pthread_spin_unlock (&lch->blk_md->entry_spin[index]);

    log_info ("[ch_prov (close_blk): Block [%d/%d/%d] closed by force.\n",
                                            ppa->g.ch, ppa->g.lun, ppa->g.blk);
    return 0;
}

/**
 * Returns a list of PPAs to be written based in the current block line. The
 * pages are collected using round-robin, and the PPAs in a page are collected
 * following write constraints (sequential sectors and planes). The minimum
 * number of PPAs returned is 'pgs * sec_per_pg * n_planes'. We assume 1 thread
 * per channel and no form of locking is needed.
 *
 * @param list - Vector of PPAs. Needs enough allocated memory for
 *                                          'pgs * sec_per_pg * n_planes' PPAs.
 * @param pgs - Number of PPAs requested.
 * @return 0 in success, negative in failure (channel is full).
 */
static int ch_prov_get_ppas (struct app_channel *lch, struct nvm_ppa_addr *list,
                                                    uint16_t pgs, uint8_t type)
{
    uint16_t *li;
    uint32_t sec, pl, renew, pgs_left;
    struct ch_prov_blk *blk;
    struct nvm_ppa_addr *ppa_off;
    struct nvm_ppa_addr tppa;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    struct nvm_mmgr_geometry *g = lch->ch->geometry;
    uint64_t index;

    /* Check if device is full (no blocks in the line) */
    if (prov->line[type].nblks < 1)
        return -1;

    pthread_mutex_lock(&prov->ch_mutex);

    li = &prov->line[type].current_blk;
    renew = 0;
    pgs_left = pgs;
    while (pgs_left) {
        blk = prov->line[type].vblks[*li];
        ppa_off = &list[g->n_of_planes * g->sec_per_pg * (pgs - pgs_left)];

        tppa.ppa = blk->addr.ppa;
        tppa.g.pg = blk->blk_md->current_pg;

        for (pl = 0; pl < g->n_of_planes; pl++) {
            for (sec = 0; sec < g->sec_per_pg; sec++) {
                tppa.g.pl = pl;
                tppa.g.sec = sec;
                ppa_off[sec + pl * g->sec_per_pg].ppa = tppa.ppa;
            }
        }

        /* All page states are set to 0 when block is new, 0 means valid page,
         so, we don't need to update the page state here */

        pgs_left--;
        index = blk->blk_i / lch->blk_md->ent_per_pg;
        oxapp()->md->mark_fn (lch, index);

        pthread_spin_lock (&lch->blk_md->entry_spin[index]);
        blk->blk_md->current_pg++;
        pthread_spin_unlock (&lch->blk_md->entry_spin[index]);

        /* Close block if this is the last page */
        if (blk->blk_md->current_pg == g->pg_per_blk) {
            ch_prov_blk_close (lch, blk, type);
            renew = 1;
        }

        *li = (*li < prov->line[type].nblks - 1) ? *li + 1 : 0;

        /* Renew the line if a block has been closed */
        if ((*li == 0 || pgs_left == 0) && renew) {
            renew = 0;
            if (ch_prov_renew_line (lch, type)) {
                if (pgs_left > 0)
                    goto FULL;
            }
        }
    }

    pthread_mutex_unlock(&prov->ch_mutex);
    return 0;

FULL:
    pthread_mutex_unlock(&prov->ch_mutex);
    return -1;
}

static struct app_ch_prov oxblk_ch_prov = {
    .mod_id       = OXBLK_CH_PROV,
    .name         = "OX-BLOCK-UPROV",
    .init_fn      = ch_prov_init,
    .exit_fn      = ch_prov_exit,
    .check_gc_fn  = ch_prov_check_gc,
    .put_blk_fn   = ch_prov_blk_put,
    .close_blk_fn = ch_prov_blk_amend,
    .get_ppas_fn  = ch_prov_get_ppas
};

void oxb_ch_prov_register (void)
{
    app_mod_register (APPMOD_CH_PROV, OXBLK_CH_PROV, &oxblk_ch_prov);
}