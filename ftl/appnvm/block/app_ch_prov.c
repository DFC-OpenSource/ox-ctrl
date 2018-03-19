/* OX: Open-Channel NVM Express SSD Controller
 *  - AppNVM Flash Translation Layer (Channel Provisioning)
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

#include <stdlib.h>
#include <stdio.h>
#include "../appnvm.h"
#include <sys/queue.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include "../../../include/ssd.h"

#define APP_PROV_LINE   4

struct ch_prov_blk {
    struct nvm_ppa_addr             addr;
    struct app_blk_md_entry         *blk_md;
    uint8_t                         *state;
    CIRCLEQ_ENTRY(ch_prov_blk)      entry;
    TAILQ_ENTRY(ch_prov_blk)        open_entry;
};

struct ch_prov_lun {
    struct nvm_ppa_addr     addr;
    struct ch_prov_blk      *vblks;
    uint32_t                nfree_blks;
    uint32_t                nused_blks;
    uint32_t                nopen_blks;
    pthread_mutex_t         l_mutex;
    CIRCLEQ_HEAD(free_blk_list, ch_prov_blk) free_blk_head;
    CIRCLEQ_HEAD(used_blk_list, ch_prov_blk) used_blk_head;
    TAILQ_HEAD(open_blk_list, ch_prov_blk) open_blk_head;
};

struct ch_prov_line {
    uint16_t              nblks;
    uint16_t              current_blk;
    struct ch_prov_blk  **vblks;
};

struct ch_prov {
    struct ch_prov_lun  *luns;
    struct ch_prov_blk  **prov_vblks;
    struct ch_prov_line line;
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
        vblk = CIRCLEQ_FIRST(&(prov->luns[lun].free_blk_head));

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
    int pl;
    int bad_blk = 0;
    struct ch_prov_blk *rnd_vblk;
    int n_pl = lch->ch->geometry->n_of_planes;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    struct ch_prov_blk *vblk = &(prov->prov_vblks[lun][blk]);

    uint8_t *bbt = appnvm()->bbt->get_fn (lch, lun);

    vblk->state = malloc(sizeof (uint8_t) * n_pl);
    if (vblk->state == NULL)
        return -1;

    vblk->addr.ppa = prov->luns[lun].addr.ppa;
    vblk->addr.g.blk = blk;
    vblk->blk_md = &appnvm()->md->get_fn (lch, lun)[blk];

    for (pl = 0; pl < lch->ch->geometry->n_of_planes; pl++) {
        vblk->state[pl] = bbt[n_pl * blk + pl];
        bad_blk += vblk->state[pl];
    }

    if (!bad_blk) {

        vblk->blk_md->flags |= APP_BLK_MD_AVLB;

        if (vblk->blk_md->flags & APP_BLK_MD_USED) {
            CIRCLEQ_INSERT_HEAD(&(prov->luns[lun].used_blk_head), vblk, entry);
            prov->luns[lun].nused_blks++;

            if (vblk->blk_md->flags & APP_BLK_MD_OPEN) {
                TAILQ_INSERT_HEAD(&(prov->luns[lun].open_blk_head),
                                                              vblk, open_entry);
                prov->luns[lun].nopen_blks++;
            }

            return 0;
        }

        rnd_vblk = ch_prov_blk_rand(lch, lun);

        if (rnd_vblk == NULL) {
            CIRCLEQ_INSERT_HEAD(&(prov->luns[lun].free_blk_head), vblk, entry);
        } else {

            if (rand() % 2)
                CIRCLEQ_INSERT_BEFORE(&(prov->luns[lun].free_blk_head),
                                                rnd_vblk, vblk, entry);
            else
                CIRCLEQ_INSERT_AFTER(&(prov->luns[lun].free_blk_head),
                                                rnd_vblk, vblk, entry);
        }
        prov->luns[lun].nfree_blks++;
    } else {

        if (vblk->blk_md->flags & APP_BLK_MD_AVLB)
            vblk->blk_md->flags ^= APP_BLK_MD_AVLB;
    }

    return 0;
}

static int ch_prov_list_create(struct app_channel *lch, int lun)
{
    int blk;
    int nblk;
    struct nvm_ppa_addr addr;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    addr.ppa = 0x0;
    addr.g.ch = lch->ch->ch_id;

    addr.g.lun = lun;
    prov->luns[lun].addr.ppa = addr.ppa;
    nblk = lch->ch->geometry->blk_per_lun;

    prov->luns[lun].nfree_blks = 0;
    prov->luns[lun].nused_blks = 0;
    prov->luns[lun].nopen_blks = 0;
    CIRCLEQ_INIT(&(prov->luns[lun].free_blk_head));
    CIRCLEQ_INIT(&(prov->luns[lun].used_blk_head));
    TAILQ_INIT(&(prov->luns[lun].open_blk_head));
    pthread_mutex_init(&(prov->luns[lun].l_mutex), NULL);

    for (blk = 0; blk < nblk; blk++)
        ch_prov_blk_alloc(lch, lun, blk);

    return 0;
}

static int ch_prov_blk_free (struct app_channel *lch, int lun, int blk)
{
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    free (prov->prov_vblks[lun][blk].state);

    return 0;
}


static void ch_prov_blk_list_free(struct app_channel *lch, int lun)
{
    int nblk;
    int blk;
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
        vblk = CIRCLEQ_FIRST(&(prov->luns[lun].used_blk_head));

        for (blk = 0; blk < prov->luns[lun].nused_blks; blk++) {
            tmp = CIRCLEQ_NEXT(vblk, entry);
            CIRCLEQ_REMOVE(&(prov->luns[lun].used_blk_head), vblk, entry);
            vblk = tmp;
        }
    }
    prov->luns[lun].nused_blks = 0;

    if (prov->luns[lun].nopen_blks > 0) {
        vblk = TAILQ_FIRST(&(prov->luns[lun].open_blk_head));

        for (blk = 0; blk < prov->luns[lun].nopen_blks; blk++) {
            tmp = TAILQ_NEXT(vblk, open_entry);
            TAILQ_REMOVE(&(prov->luns[lun].open_blk_head), vblk, open_entry);
            vblk = tmp;
        }
    }
    prov->luns[lun].nopen_blks = 0;

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

    prov->luns = malloc (sizeof (struct ch_prov_lun) *
                                                lch->ch->geometry->lun_per_ch);
    if (!prov->luns)
        return -1;

    prov->prov_vblks = malloc(nluns * sizeof(struct ch_prov_blk *));
    if (!prov->prov_vblks)
        goto FREE_LUNS;

    for (lun = 0; lun < nluns; lun++) {
        prov->prov_vblks[lun] = malloc(nblocks * sizeof (struct ch_prov_blk));
        if (!prov->prov_vblks[lun]) {
            for (err_lun = 0; err_lun < lun; err_lun++)
                free (prov->prov_vblks[err_lun]);
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
        free (prov->prov_vblks[err_lun]);

  FREE_VBLKS:
    free (prov->prov_vblks);

  FREE_LUNS:
    free (prov->luns);
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
        free (prov->prov_vblks[lun]);
    }

    free (prov->prov_vblks);
    free (prov->luns);

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
    if (free_blk < APPNVM_GC_MIN_FREE_BLKS)
        appnvm_ch_active_unset (lch);

    if (free_blk / tot_blk < APPNVM_GC_THRESD)
        appnvm_ch_need_gc_set (lch);
}

/**
 * Gets a new block from a LUN and mark it as open.
 * If the block fails to erase, mark it as bad and try next block.
 * @return the pointer to the new open block
 */
static struct ch_prov_blk *ch_prov_blk_get (struct app_channel *lch,
                                                                  uint16_t lun)
{
    int ret, pl;
    int n_pl = lch->ch->geometry->n_of_planes;
    struct nvm_mmgr_io_cmd *cmd;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    struct ch_prov_lun *p_lun = &prov->luns[lun];

NEXT:
    if (p_lun->nfree_blks > 0) {
        struct ch_prov_blk *vblk = CIRCLEQ_FIRST(&p_lun->free_blk_head);

        CIRCLEQ_REMOVE(&(p_lun->free_blk_head), vblk, entry);
        CIRCLEQ_INSERT_TAIL(&(p_lun->used_blk_head), vblk, entry);
        TAILQ_INSERT_HEAD(&(p_lun->open_blk_head), vblk, open_entry);

        p_lun->nfree_blks--;
        p_lun->nused_blks++;
        p_lun->nopen_blks++;

        /* Erase the block, if it fails, mark as bad and try next block */
        cmd = malloc (sizeof(struct nvm_mmgr_io_cmd));
        if (!cmd)
            return NULL;
        for (pl = 0; pl < n_pl; pl++) {
            memset (cmd, 0x0, sizeof(struct nvm_mmgr_io_cmd));
            cmd->ppa.ppa = vblk->addr.ppa;
            cmd->ppa.g.pl = pl;

            ret = nvm_submit_sync_io (lch->ch, cmd, NULL, MMGR_ERASE_BLK);
            if (ret)
                break;
        }
        free (cmd);

        vblk->blk_md->erase_count++;

        if (ret) {
            for (pl = 0; pl < n_pl; pl++)
                lch->ch->ftl->ops->set_bbtbl (&vblk->addr, NVM_BBT_BAD);

            CIRCLEQ_REMOVE(&(p_lun->used_blk_head), vblk, entry);
            TAILQ_REMOVE(&(p_lun->open_blk_head), vblk, open_entry);
            p_lun->nused_blks--;
            p_lun->nopen_blks--;

            goto NEXT;
        }

        vblk->blk_md->current_pg = 0;
        vblk->blk_md->invalid_sec = 0;
        vblk->blk_md->flags |= (APP_BLK_MD_USED | APP_BLK_MD_OPEN);
        if (vblk->blk_md->flags & APP_BLK_MD_LINE)
            vblk->blk_md->flags ^= APP_BLK_MD_LINE;

        memset (vblk->blk_md->pg_state, 0x0, 1024);

        appnvm()->ch_prov->check_gc_fn (lch);

        if (APPNVM_DEBUG_CH_PROV) {
            printf("[appnvm (ch_prov): blk GET: (%d/%d/%d/%d) - Free: %d,"
                    " Used: %d, Open: %d]\n", vblk->addr.g.ch, vblk->addr.g.lun,
                    vblk->addr.g.blk, vblk->blk_md->erase_count,
                    p_lun->nfree_blks, p_lun->nused_blks, p_lun->nopen_blks);
        }

        return vblk;
    }

    return NULL;
}

/**
 * Mark a block as free when it is no longer used (recycled by GC).
 * @return 0 in success, negative if block is still open or still in use
 */
static int ch_prov_blk_put(struct app_channel *lch, uint16_t lun, uint16_t blk)
{
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    struct ch_prov_lun *p_lun = &prov->luns[lun];
    struct ch_prov_blk *vblk = &prov->prov_vblks[lun][blk];

    if (!(vblk->blk_md->flags & APP_BLK_MD_USED))
        return -1;

    if (vblk->blk_md->flags & APP_BLK_MD_OPEN)
        return -2;

    pthread_mutex_lock(&prov->ch_mutex);

    if (vblk->blk_md->flags & APP_BLK_MD_USED)
        vblk->blk_md->flags ^= APP_BLK_MD_USED;

    if (vblk->blk_md->flags & APP_BLK_MD_LINE)
        vblk->blk_md->flags ^= APP_BLK_MD_LINE;

    CIRCLEQ_REMOVE(&(p_lun->used_blk_head), &prov->prov_vblks[lun][blk], entry);
    CIRCLEQ_INSERT_TAIL(&(p_lun->free_blk_head),
                                           &prov->prov_vblks[lun][blk], entry);
    p_lun->nfree_blks++;
    p_lun->nused_blks--;

    pthread_mutex_unlock(&prov->ch_mutex);

    if (APPNVM_DEBUG_CH_PROV) {
        printf("[appnvm (ch_prov): blk PUT: (%d %d %d) - Free: %d,"
                " Used: %d, Open: %d]\n", vblk->addr.g.ch, vblk->addr.g.lun,
                vblk->addr.g.blk, p_lun->nfree_blks, p_lun->nused_blks,
                                                     p_lun->nopen_blks);
    }

    return 0;
}

/**
 * This function collects open blocks from all LUNs using round-robin.
 * It sets the line blocks (used by selecting pages to be written).
 * If a LUN has no open block, it opens a new block.
 * If a LUN has no blocks left, the line will be filled with available LUNs.
 * @return 0 in success, negative in failure (channel is full)
 */
static int ch_prov_renew_line (struct app_channel *lch)
{
    uint32_t lun, targets, i, found, j;
    struct ch_prov_blk *vblk;
    struct ch_prov_blk *line[APP_PROV_LINE];
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    uint32_t lflag = 0x0;

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

        if (TAILQ_EMPTY(&prov->luns[lun].open_blk_head)) {
GET_BLK:
            vblk = ch_prov_blk_get (lch, lun);

            /* LUN has no available blocks, unset flag */
            if (vblk == NULL) {
                if (lflag & (1 << lun))
                    lflag ^= (1 << lun);
                goto NEXT_LUN;
            }
        }

        /* Avoid picking a block already present in the line */
        TAILQ_FOREACH(vblk, &prov->luns[lun].open_blk_head, open_entry) {
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
        line[targets]->blk_md->flags |= APP_BLK_MD_LINE;
        targets++;

NEXT_LUN:
        lun = (lun == lch->ch->geometry->lun_per_ch - 1) ? 0 : lun + 1;
    } while (targets < APP_PROV_LINE);

    prov->line.nblks = targets;

    /* Check if line has at least 1 block available */
    if (targets == 0) {
        log_err("[appnvm (ch_prov): CH %d has no blocks left.]",lch->ch->ch_id);
        if (APPNVM_DEBUG_CH_PROV)
            printf("appnvm(ch_prv): CH %d has no blocks left\n",lch->ch->ch_id);
        return -1;
    }

    /* set the line pointers */
    for (i = 0; i < targets; i++)
        prov->line.vblks[i] = line[i];

    if (prov->line.current_blk >= targets)
        prov->line.current_blk = 0;

    if (APPNVM_DEBUG_CH_PROV) {
        printf ("[appnvm (ch_prov): Line is renewed: ");
        for (j = 0; j < targets; j++)
            printf ("(%d %d %d)", prov->line.vblks[j]->addr.g.ch,
                                   prov->line.vblks[j]->addr.g.lun,
                                   prov->line.vblks[j]->addr.g.blk);
        printf("]\n");
    }

    return 0;
}

static int ch_prov_init (struct app_channel *lch)
{
    struct ch_prov *prov = malloc (sizeof (struct ch_prov));
    if (!prov)
        return -1;

    if (pthread_mutex_init(&prov->ch_mutex, NULL))
        goto FREE_PROV;

    lch->ch_prov = prov;
    if (ch_prov_init_luns (lch))
        goto MUTEX;

    prov->line.current_blk = 0;
    prov->line.vblks = malloc (sizeof (struct ch_prov_blk *) * APP_PROV_LINE);
    if (!prov->line.vblks)
        goto FREE_LUNS;

    if (ch_prov_renew_line (lch)) {
        log_err ("[appnvm (ch_prov): CHANNEL %d is FULL!]\n",lch->ch->ch_id);
        if (APPNVM_DEBUG_CH_PROV)
            printf ("[appnvm (ch_prov): CHANNEL %d is FULL!]\n",lch->ch->ch_id);
        goto FREE_BLKS;
    }

    log_info("    [appnvm: Channel Provisioning started. Ch %d]\n",
                                                                lch->ch->ch_id);
    return 0;

FREE_BLKS:
    free (prov->line.vblks);
FREE_LUNS:
    ch_prov_exit_luns (lch);
MUTEX:
    pthread_mutex_destroy (&prov->ch_mutex);
FREE_PROV:
    free (prov);
    return -1;
}

static void ch_prov_exit (struct app_channel *lch)
{
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;

    free (prov->line.vblks);
    ch_prov_exit_luns (lch);
    pthread_mutex_destroy (&prov->ch_mutex);
    free (prov);
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
                                                                uint16_t pgs)
{
    uint16_t *li;
    uint32_t sec, pl, renew, pgs_left;
    struct ch_prov_blk *blk;
    struct nvm_ppa_addr *ppa_off;
    struct nvm_ppa_addr tppa;
    struct ch_prov_lun *p_lun;
    struct ch_prov *prov = (struct ch_prov *) lch->ch_prov;
    struct nvm_mmgr_geometry *g = lch->ch->geometry;

    /* Check if device is full (no blocks in the line) */
    if (prov->line.nblks < 1)
        return -1;

    pthread_mutex_lock(&prov->ch_mutex);

    li = &prov->line.current_blk;
    renew = 0;
    pgs_left = pgs;
    while (pgs_left) {
        blk = prov->line.vblks[*li];
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
        blk->blk_md->current_pg++;

        /* Close block if this is the last page */
        if (blk->blk_md->current_pg == g->pg_per_blk) {
            p_lun = &prov->luns[tppa.g.lun];

            TAILQ_REMOVE(&p_lun->open_blk_head, blk, open_entry);
            p_lun->nopen_blks--;

            if (blk->blk_md->flags & APP_BLK_MD_LINE)
                blk->blk_md->flags ^= APP_BLK_MD_LINE;
            if (blk->blk_md->flags & APP_BLK_MD_OPEN)
                blk->blk_md->flags ^= APP_BLK_MD_OPEN;

            renew = 1;
        }

        *li = (*li < prov->line.nblks - 1) ? *li + 1 : 0;

        /* Renew the line if a block has been closed */
        if ((*li == 0 || pgs_left == 0) && renew) {
            renew = 0;
            if (ch_prov_renew_line (lch)) {
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

static struct app_blk_md_entry *ch_prov_get_blk (struct app_channel *lch,
                                                                  uint16_t lun)
{
    struct ch_prov_blk *blk = ch_prov_blk_get (lch, lun);
    if (!blk)
        return NULL;

    return blk->blk_md;
}

static struct app_ch_prov appftl_ch_prov = {
    .mod_id       = APPFTL_CH_PROV,
    .init_fn      = ch_prov_init,
    .exit_fn      = ch_prov_exit,
    .check_gc_fn  = ch_prov_check_gc,
    .put_blk_fn   = ch_prov_blk_put,
    .get_blk_fn   = ch_prov_get_blk,
    .get_ppas_fn  = ch_prov_get_ppas
};

void ch_prov_register (void)
{
    appnvm_mod_register (APPMOD_CH_PROV, APPFTL_CH_PROV, &appftl_ch_prov);
}